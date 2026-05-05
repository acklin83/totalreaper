// TotalReaperCSurf.cpp — IReaperControlSurface implementation.

#include "TotalReaperCSurf.h"

#include "../osc/OscMessage.h"
#include "../reaper/ChannelMap.h"

// reaper_plugin_functions.h declares the API function pointers as extern.
// They're defined exactly once via REAPERAPI_IMPLEMENT in ReaperAPI.cpp.
#include "reaper_plugin_functions.h"

#include <cmath>
#include <cstdio>

namespace totalreaper::csurf {

namespace {
constexpr float kMinusInfDb = -300.0f; // TotalMix's -∞ sentinel value
constexpr float kMaxDb = 6.0f;    // TotalMix input fader ceiling

// I_RECINPUT bit layout (per reaper_plugin_functions.h):
//   bit 4096 → MIDI input (we skip)
//   bit 2048 → multichannel input (we skip for now)
//   bit 1024 → stereo input (otherwise mono)
//   low 10 bits → start channel
constexpr int kRecInputMidi         = 4096;
constexpr int kRecInputMultichannel = 2048;
constexpr int kRecInputStereo       = 1024;
constexpr int kRecInputChannelMask  = 0x3FF;

// Returns the left/start channel of a hardware audio input. -1 if the input
// is MIDI, multichannel, or absent.
int hwStartChannel(int recInput) {
    if (recInput < 0) return -1;
    if (recInput & kRecInputMidi) return -1;
    if (recInput & kRecInputMultichannel) return -1;
    return recInput & kRecInputChannelMask;
}

bool isStereoInput(int recInput) {
    return (recInput & kRecInputStereo) != 0;
}

// Find the REAPER master track's first hardware-output destination channel,
// translated through the output channel map (reaper.ini [alias_out_*]).
// Returns -1 if the master has no HW send. The returned value is the
// device hardware channel index, which equals TotalMix's bus index for
// /mix/in/<n>/<bus>/...
int resolveMainBusFromMaster() {
    MediaTrack* master = GetMasterTrack(nullptr);
    if (!master) return -1;
    const int hwSendCount = GetTrackNumSends(master, 1);
    if (hwSendCount <= 0) return -1;
    const int reaperDst = static_cast<int>(
        GetTrackSendInfo_Value(master, 1, 0, "I_DSTCHAN"));
    return reaper::reaperOutputToHardware(reaperDst);
}

} // namespace

TotalReaperCSurf::TotalReaperCSurf(osc::Client* client, std::uint16_t txPort)
    : client_(client), txPort_(txPort) {}

void TotalReaperCSurf::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;

    if (!enabled) {
        // Drive every previously-mirrored input to -∞ in TotalMix so REAPER
        // stops affecting monitoring, and restore each track's main send so
        // REAPER's own monitoring works again. We use the cached recInput
        // because the track may have been deleted by now.
        for (const auto& [tr, state] : states_) {
            sendFaderForInput(state.recInput, kMinusInfDb);
            if (state.savedMainSend != -1) {
                SetMediaTrackInfo_Value(tr, "B_MAINSEND",
                                        static_cast<double>(state.savedMainSend));
            }
        }
        states_.clear();
        return;
    }

    // Enabling: prime by pushing every track. Run() would catch up on its
    // next tick (~33 ms), but doing it eagerly avoids the user-visible lag
    // between "I enabled the mirror" and "TotalMix actually reflects state."
    states_.clear();
    const int trackCount = CountTracks(nullptr);
    for (int i = 0; i < trackCount; ++i) {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (tr == nullptr) continue;
        updateTrackRouting(tr);
    }
}

void TotalReaperCSurf::SetSurfaceVolume(MediaTrack* tr, double volume) {
    // SetSurfaceVolume can fire as a state heartbeat with the same value, so
    // dedupe via the same cache Run() uses to avoid spamming TotalMix.
    if (!enabled_ || tr == nullptr) return;
    TrackState& cached = states_[tr];
    if (cached.linVol == volume) return;
    // Run() will refresh the full cache entry on its next tick. We only set
    // linVol here so identical immediate repeats are short-circuited.
    cached.linVol = volume;
    updateTrackRouting(tr);
}

int TotalReaperCSurf::Extended(int call, void* parm1, void* /*parm2*/,
                               void* /*parm3*/) {
    if (call == CSURF_EXT_SETINPUTMONITOR) {
        updateTrackRouting(static_cast<MediaTrack*>(parm1));
        return 1;
    }
    return 0;
}

void TotalReaperCSurf::Run() {
    if (!enabled_) return;

    const int trackCount = CountTracks(nullptr);
    for (int i = 0; i < trackCount; ++i) {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (tr == nullptr) continue;

        const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
        const int recMon = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECMON"));
        const double linVol = GetMediaTrackInfo_Value(tr, "D_VOL");
        const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
        const double width = GetMediaTrackInfo_Value(tr, "D_WIDTH");
        const double dualPanL = GetMediaTrackInfo_Value(tr, "D_DUALPANL");
        const double dualPanR = GetMediaTrackInfo_Value(tr, "D_DUALPANR");
        const int panMode = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_PANMODE"));

        const TrackState& cached = states_[tr];
        if (cached.recInput == recInput &&
            cached.recMon == recMon &&
            cached.linVol == linVol &&
            cached.pan == pan &&
            cached.width == width &&
            cached.dualPanL == dualPanL &&
            cached.dualPanR == dualPanR &&
            cached.panMode == panMode) {
            continue;
        }

        // If the input was reassigned, close out the OLD input first so it
        // doesn't stay stuck at the last fader value in TotalMix. Then fall
        // through to the normal update for the new input.
        if (cached.recInput != recInput && hwStartChannel(cached.recInput) >= 0) {
            sendFaderForInput(cached.recInput, kMinusInfDb);
        }

        if (hwStartChannel(recInput) < 0) {
            // No valid hardware input now — leave cache reflecting current
            // state so we don't re-send the close-out next tick.
            TrackState& s = states_[tr];
            s.recInput = recInput;
            s.recMon = recMon;
            s.linVol = linVol;
            s.pan = pan;
            s.width = width;
            s.dualPanL = dualPanL;
            s.dualPanR = dualPanR;
            s.panMode = panMode;
            continue;
        }

        updateTrackRouting(tr);
    }
}

void TotalReaperCSurf::updateTrackRouting(MediaTrack* tr) {
    // The master track also receives csurf callbacks (volume + monitor) and
    // returns I_RECINPUT=0 by default — without this guard we'd spuriously
    // treat master as a recording track on Analog 1.
    if (!enabled_ || tr == nullptr || tr == GetMasterTrack(nullptr) ||
        client_ == nullptr) {
        return;
    }

    const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
    if (hwStartChannel(recInput) < 0) return;

    const int recMon = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECMON"));
    const double linVol = GetMediaTrackInfo_Value(tr, "D_VOL");
    const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
    const double width = GetMediaTrackInfo_Value(tr, "D_WIDTH");
    const double dualPanL = GetMediaTrackInfo_Value(tr, "D_DUALPANL");
    const double dualPanR = GetMediaTrackInfo_Value(tr, "D_DUALPANR");
    const int panMode = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_PANMODE"));

    float targetDb = kMinusInfDb;
    if (recMon != 0 && linVol > 0.0) {
        targetDb = static_cast<float>(20.0 * std::log10(linVol));
        if (targetDb > kMaxDb) targetDb = kMaxDb;
        if (targetDb < kMinusInfDb) targetDb = kMinusInfDb;
    }

    // Mute REAPER's software monitor on this track while we're driving its
    // input from TotalMix. Otherwise the user hears the input twice — once
    // direct via TotalMix (zero latency) and once through REAPER's master
    // (with buffer-size latency, causing a comb filter).
    TrackState& state = states_[tr];
    const bool nowActive = (targetDb > kMinusInfDb);
    const bool wasOverridden = (state.savedMainSend != -1);
    if (nowActive && !wasOverridden) {
        const int currentMainSend = static_cast<int>(
            GetMediaTrackInfo_Value(tr, "B_MAINSEND"));
        if (currentMainSend != 0) {
            state.savedMainSend = currentMainSend;
            SetMediaTrackInfo_Value(tr, "B_MAINSEND", 0.0);
        }
    } else if (!nowActive && wasOverridden) {
        SetMediaTrackInfo_Value(tr, "B_MAINSEND",
                                static_cast<double>(state.savedMainSend));
        state.savedMainSend = -1;
    }

    // Refresh cache so the various no-change shortcuts catch up.
    state.recInput = recInput;
    state.recMon = recMon;
    state.linVol = linVol;
    state.pan = pan;
    state.width = width;
    state.dualPanL = dualPanL;
    state.dualPanR = dualPanR;
    state.panMode = panMode;

    sendFaderForInput(recInput, targetDb);

    // Pan/width: only meaningful while the channel is actually monitoring.
    // When inactive (-∞), don't touch TotalMix's pan — the fader silences
    // the channel anyway, and we don't fight any manual user adjustments
    // they might have made in TotalMix.
    if (nowActive) {
        const int leftCh = hwStartChannel(recInput);
        if (isStereoInput(recInput)) {
            float L, R;
            if (panMode == 6) {
                // Dual pan: each channel has its own independent pan.
                L = static_cast<float>(dualPanL);
                R = static_cast<float>(dualPanR);
            } else {
                // Stereo pan (mode 5) gives a width control; balance modes
                // (0, 3) don't, so we default to full width and let pan act
                // as a position offset.
                const double effectiveWidth = (panMode == 5) ? width : 1.0;
                L = static_cast<float>(pan - effectiveWidth);
                R = static_cast<float>(pan + effectiveWidth);
            }
            if (L < -1.0f) L = -1.0f; if (L > 1.0f) L = 1.0f;
            if (R < -1.0f) R = -1.0f; if (R > 1.0f) R = 1.0f;
            sendBalpan(leftCh,     L);
            sendBalpan(leftCh + 1, R);
        } else {
            sendBalpan(leftCh, static_cast<float>(pan));
        }
    }
}

void TotalReaperCSurf::sendFaderForInput(int recInput, float db) {
    const int leftCh = hwStartChannel(recInput);
    if (leftCh < 0) return;
    sendFader(leftCh, db);
    if (isStereoInput(recInput)) {
        sendFader(leftCh + 1, db);
    }
}

void TotalReaperCSurf::sendFader(int reaperChannel, float db) {
    if (client_ == nullptr) return;
    if (reaperChannel < 0 || reaperChannel > kRecInputChannelMask) return;

    const int hwIdx = reaper::reaperInputToHardware(reaperChannel);
    const int mainBus = resolveMainBusFromMaster();
    const int bus = mainBus >= 0 ? mainBus : 0;

    if (!client_->isConnected()) {
        client_->connect("127.0.0.1", txPort_);
    }

    char path[64];
    std::snprintf(path, sizeof(path), "/mix/in/%d/%d/fader", hwIdx, bus);
    osc::Message msg(path);
    msg.addFloat(db);
    client_->send(msg);
}

void TotalReaperCSurf::sendBalpan(int reaperChannel, float balpan) {
    if (client_ == nullptr) return;
    if (reaperChannel < 0 || reaperChannel > kRecInputChannelMask) return;

    const int hwIdx = reaper::reaperInputToHardware(reaperChannel);
    const int mainBus = resolveMainBusFromMaster();
    const int bus = mainBus >= 0 ? mainBus : 0;

    if (!client_->isConnected()) {
        client_->connect("127.0.0.1", txPort_);
    }

    char path[64];
    std::snprintf(path, sizeof(path), "/mix/in/%d/%d/balpan", hwIdx, bus);
    osc::Message msg(path);
    msg.addFloat(balpan);
    client_->send(msg);
}

} // namespace totalreaper::csurf
