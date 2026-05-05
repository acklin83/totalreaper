// PreampActions.cpp — REAPER actions that drive TotalMix's per-input
// channel-strip controls (preamp gain, 48V phantom, pad, phase).
//
// REAPER doesn't have native concepts for these, so we store the desired
// state in per-track P_EXT:* properties and push the corresponding OSC
// path on every action invocation. State persists with the project file.
//
// All actions operate on the user's currently selected tracks, which is
// REAPER's standard convention. Toggles use the "if any is off, turn all
// on; otherwise turn all off" pattern that matches stock REAPER actions
// like Track: Toggle mute on selected tracks.

#include "Actions.h"

#include "../osc/OscMessage.h"
#include "../reaper/ChannelMap.h"

#include "reaper_plugin_functions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace totalreaper::actions {

namespace {

constexpr const char* kExtGain  = "P_EXT:totalreaper_gain";
constexpr const char* kExt48v   = "P_EXT:totalreaper_48v";
constexpr const char* kExtPad   = "P_EXT:totalreaper_pad";
constexpr const char* kExtPhase = "P_EXT:totalreaper_phase";

constexpr int kRecInputMidi         = 4096;
constexpr int kRecInputMultichannel = 2048;
constexpr int kRecInputStereo       = 1024;
constexpr int kRecInputChannelMask  = 0x3FF;

int s_gainInc = 0, s_gainDec = 0;
int s_t48v = 0, s_tPad = 0, s_tPhase = 0;

int hwStartChannel(int recInput) {
    if (recInput < 0) return -1;
    if (recInput & kRecInputMidi) return -1;
    if (recInput & kRecInputMultichannel) return -1;
    return recInput & kRecInputChannelMask;
}

bool isStereo(int recInput) {
    return (recInput & kRecInputStereo) != 0;
}

// Read a per-track ExtState string. Returns "" if missing.
std::string readExt(MediaTrack* tr, const char* key) {
    char buf[64] = {0};
    GetSetMediaTrackInfo_String(tr, key, buf, false);
    return std::string(buf);
}

void writeExt(MediaTrack* tr, const char* key, const char* value) {
    GetSetMediaTrackInfo_String(tr, key, const_cast<char*>(value), true);
}

// Gather all currently-selected tracks that have a hardware mono/stereo input.
std::vector<MediaTrack*> selectedHwInputTracks() {
    std::vector<MediaTrack*> out;
    const int n = CountSelectedTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaTrack* tr = GetSelectedTrack(nullptr, i);
        if (tr == nullptr) continue;
        const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
        if (hwStartChannel(recInput) < 0) continue;
        out.push_back(tr);
    }
    return out;
}

// Push an OSC float to /input/<hwIdx>/<param> for both the left channel and
// (for stereo inputs) the right channel of the track's hardware input. Only
// sends if the routing mirror is currently enabled — otherwise the value is
// stored in ExtState but TotalMix is not touched.
void pushPreampParam(MediaTrack* tr, const char* paramSuffix, float value) {
    if (csurfInstance() == nullptr || !csurfInstance()->isEnabled()) return;
    osc::Client* c = oscClient();
    if (c == nullptr) return;

    const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
    const int leftCh = hwStartChannel(recInput);
    if (leftCh < 0) return;

    auto sendOne = [&](int channel) {
        const int hwIdx = reaper::reaperInputToHardware(channel);
        char path[64];
        std::snprintf(path, sizeof(path), "/input/%d/%s", hwIdx, paramSuffix);
        osc::Message m(path);
        m.addFloat(value);
        c->send(m);
    };
    sendOne(leftCh);
    if (isStereo(recInput)) sendOne(leftCh + 1);
}

void runGainDelta(double deltaDb) {
    for (MediaTrack* tr : selectedHwInputTracks()) {
        const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
        const int leftCh = hwStartChannel(recInput);
        if (leftCh < 0) continue;
        const int hwIdx = reaper::reaperInputToHardware(leftCh);

        double dB = 0.0;
        bool haveBaseline = false;
        if (auto* st = totalMixState()) {
            float observed = 0.0f;
            if (st->getInputGain(hwIdx, &observed)) {
                dB = observed;
                haveBaseline = true;
            }
        }
        if (!haveBaseline) {
            const std::string stored = readExt(tr, kExtGain);
            if (!stored.empty()) {
                dB = std::strtod(stored.c_str(), nullptr);
                haveBaseline = true;
            }
        }
        if (!haveBaseline) {
            // No idea what TotalMix's current value is — applying a delta
            // would land somewhere arbitrary (likely 0, which TotalMix
            // clamps to). Silently skip; the next OSC echo from TotalMix
            // (which fires on any gain knob movement) seeds the cache.
            continue;
        }

        dB += deltaDb;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", dB);
        writeExt(tr, kExtGain, buf);
        pushPreampParam(tr, "gain", static_cast<float>(dB));

        if (auto* st = totalMixState()) {
            st->setInputGain(hwIdx, static_cast<float>(dB));
            if (isStereo(recInput)) {
                st->setInputGain(reaper::reaperInputToHardware(leftCh + 1),
                                 static_cast<float>(dB));
            }
        }
    }
}

// "Toggle on selected tracks" semantics: if any selected track has the flag
// off, set all to on; otherwise turn all off.
void runToggleFlag(const char* extKey, const char* paramSuffix) {
    auto tracks = selectedHwInputTracks();
    if (tracks.empty()) return;
    bool anyOff = false;
    for (MediaTrack* tr : tracks) {
        if (readExt(tr, extKey) != "1") { anyOff = true; break; }
    }
    const float newValue = anyOff ? 1.0f : 0.0f;
    const char* newStr = anyOff ? "1" : "0";
    for (MediaTrack* tr : tracks) {
        writeExt(tr, extKey, newStr);
        pushPreampParam(tr, paramSuffix, newValue);
    }
}

// Pad toggle with a mute window around the change. Hardware preamps pop on a
// pad switch — we mute the channel for ~1 second total and toggle the pad
// roughly in the middle, so the click happens while the channel is silent.
//
// Sequence (per track):
//   T=0 ms     mute = 1
//   T=100 ms   pad  = newValue   (let the mute settle into hardware first)
//   T=1000 ms  mute = 0
//
// Stages 2 and 3 use the csurf's deferred queue, which fires from Run() on
// REAPER's main thread, so REAPER API calls in the captured lambdas are safe.
void runTogglePadWithMutePulse() {
    auto tracks = selectedHwInputTracks();
    if (tracks.empty()) return;
    bool anyOff = false;
    for (MediaTrack* tr : tracks) {
        if (readExt(tr, kExtPad) != "1") { anyOff = true; break; }
    }
    const float newPadValue = anyOff ? 1.0f : 0.0f;
    const char* newStr = anyOff ? "1" : "0";

    constexpr int kPadDelayMs = 100;
    constexpr int kUnmuteDelayMs = 1000;

    auto* surf = csurfInstance();
    std::vector<MediaTrack*> snapshot(tracks);

    // Stage 1: mute every selected track immediately.
    for (MediaTrack* tr : snapshot) {
        pushPreampParam(tr, "mute", 1.0f);
    }

    // Stage 2: after the mute settles, write the new pad state.
    if (surf) {
        surf->scheduleAfter(kPadDelayMs, [snapshot, newPadValue, newStr]() {
            for (MediaTrack* tr : snapshot) {
                writeExt(tr, kExtPad, newStr);
                pushPreampParam(tr, "pad", newPadValue);
            }
        });

        // Stage 3: unmute after the hardware pop has fully decayed.
        surf->scheduleAfter(kUnmuteDelayMs, [snapshot]() {
            for (MediaTrack* tr : snapshot) {
                pushPreampParam(tr, "mute", 0.0f);
            }
        });
    } else {
        // No csurf available — fall back to immediate, no-mute path.
        for (MediaTrack* tr : snapshot) {
            writeExt(tr, kExtPad, newStr);
            pushPreampParam(tr, "pad", newPadValue);
        }
    }
}

} // namespace

int& gainIncCommandId()  { return s_gainInc; }
int& gainDecCommandId()  { return s_gainDec; }
int& toggle48vCommandId() { return s_t48v; }
int& togglePadCommandId() { return s_tPad; }
int& togglePhaseCommandId() { return s_tPhase; }

bool runPreampAction(int command) {
    if (command == s_gainInc)   { runGainDelta(+1.0); return true; }
    if (command == s_gainDec)   { runGainDelta(-1.0); return true; }
    if (command == s_t48v)      { runToggleFlag(kExt48v, "48v"); return true; }
    if (command == s_tPad)      { runTogglePadWithMutePulse(); return true; }
    if (command == s_tPhase)    { runToggleFlag(kExtPhase, "phase"); return true; }
    return false;
}

} // namespace totalreaper::actions
