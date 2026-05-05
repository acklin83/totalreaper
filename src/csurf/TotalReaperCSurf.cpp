// TotalReaperCSurf.cpp — IReaperControlSurface implementation.

#include "TotalReaperCSurf.h"

#include "../osc/OscMessage.h"
#include "../reaper/ChannelMap.h"

// reaper_plugin_functions.h declares the API function pointers as extern.
// They're defined exactly once via REAPERAPI_IMPLEMENT in ReaperAPI.cpp.
#include "reaper_plugin_functions.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_set>

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

// Find a track's first hardware-output destination, translated through the
// output channel map (reaper.ini [alias_out_*]). Returns -1 if the track has
// no HW send. The returned value is the device hardware channel index, which
// equals TotalMix's bus index for /mix/in/<n>/<bus>/...
int resolveHwOutBus(MediaTrack* tr) {
    if (tr == nullptr) return -1;
    const int hwSendCount = GetTrackNumSends(tr, 1);
    if (hwSendCount <= 0) return -1;
    const int reaperDst = static_cast<int>(
        GetTrackSendInfo_Value(tr, 1, 0, "I_DSTCHAN"));
    // I_DSTCHAN low 10 bits are the start channel; mask to ignore any
    // multichannel/stereo-pair flags before passing through the alias map.
    return reaper::reaperOutputToHardware(reaperDst & 0x3FF);
}

int resolveMainBusFromMaster() {
    return resolveHwOutBus(GetMasterTrack(nullptr));
}

float linToClampedDb(double lin) {
    if (lin <= 0.0) return kMinusInfDb;
    float db = static_cast<float>(20.0 * std::log10(lin));
    if (db > kMaxDb) db = kMaxDb;
    if (db < kMinusInfDb) db = kMinusInfDb;
    return db;
}

// Compute the per-input balpan values from REAPER's pan model. Returns
// (panL, panR) where panR is meaningful only for stereo inputs.
struct PanPair { float L; float R; };
PanPair computeInputPan(int recInput, double pan, double width,
                        double dualPanL, double dualPanR, int panMode) {
    if (!isStereoInput(recInput)) {
        return {static_cast<float>(pan), 0.0f};
    }
    float L, R;
    if (panMode == 6) {
        L = static_cast<float>(dualPanL);
        R = static_cast<float>(dualPanR);
    } else {
        const double effectiveWidth = (panMode == 5) ? width : 1.0;
        L = static_cast<float>(pan - effectiveWidth);
        R = static_cast<float>(pan + effectiveWidth);
    }
    if (L < -1.0f) L = -1.0f; if (L > 1.0f) L = 1.0f;
    if (R < -1.0f) R = -1.0f; if (R > 1.0f) R = 1.0f;
    return {L, R};
}

// Recursively walk a track's audio sends and return every (bus, gain) pair
// the source's signal eventually reaches via a hardware output. Intermediate
// track faders are folded into the gain (post-fader sends use trackVol *
// sendVol; pre-fader uses sendVol). The destination track's own fader is
// excluded — TotalMix's /output/<bus>/volume stays user-controlled.
//
// Pan: we record the pan of the FIRST send in each chain (the closest to the
// source), since that's where the user typically intends a routing-specific
// pan. Multi-hop pan composition is not modelled.
struct WalkRouting { int bus; double linGain; double firstPan; bool firstPanSet; };

void walkSendChain(MediaTrack* sourceTr, MediaTrack* current, double pathLin,
                   double firstPan, bool firstPanSet,
                   std::unordered_set<MediaTrack*>& visited,
                   std::vector<WalkRouting>& out) {
    if (visited.count(current)) return;
    visited.insert(current);

    // Terminal: current track has a hardware output (and isn't the source).
    if (current != sourceTr) {
        const int hwBus = resolveHwOutBus(current);
        if (hwBus >= 0) {
            out.push_back({hwBus, pathLin, firstPan, firstPanSet});
            visited.erase(current);
            return;
        }
    }

    const double currFader = GetMediaTrackInfo_Value(current, "D_VOL");
    const int sendCount = GetTrackNumSends(current, 0);
    for (int i = 0; i < sendCount; ++i) {
        const bool muted = GetTrackSendInfo_Value(current, 0, i, "B_MUTE") != 0;
        if (muted) continue;
        MediaTrack* dest = reinterpret_cast<MediaTrack*>(static_cast<intptr_t>(
            static_cast<std::int64_t>(GetTrackSendInfo_Value(
                current, 0, i, "P_DESTTRACK"))));
        if (dest == nullptr) continue;
        const double sendVol = GetTrackSendInfo_Value(current, 0, i, "D_VOL");
        const double sendPan = GetTrackSendInfo_Value(current, 0, i, "D_PAN");
        const int sendMode = static_cast<int>(
            GetTrackSendInfo_Value(current, 0, i, "I_SENDMODE"));

        const double outGain = (sendMode == 0) ? currFader * sendVol : sendVol;
        const double nextLin = pathLin * outGain;
        const double nextPan = firstPanSet ? firstPan : sendPan;
        walkSendChain(sourceTr, dest, nextLin, nextPan, true, visited, out);
    }
    visited.erase(current);
}

} // namespace

TotalReaperCSurf::TotalReaperCSurf(osc::Client* client, std::uint16_t txPort)
    : client_(client), txPort_(txPort) {}

void TotalReaperCSurf::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    SetExtState("TotalReaper", "RoutingMirrorEnabled",
                enabled ? "1" : "0", /*persist*/ true);

    if (!enabled) {
        // Drive every routing we've ever pushed to -∞ in TotalMix so REAPER
        // stops affecting monitoring, and restore each track's main send so
        // REAPER's own monitoring works again. We use the cached recInput
        // because the track may have been deleted by now.
        const int mainBus = resolveMainBusFromMaster();
        for (const auto& [tr, state] : states_) {
            if (mainBus >= 0) {
                pushInputRouting(state.recInput, mainBus, kMinusInfDb, 0, 0, false);
            }
            for (const auto& r : state.sendRoutings) {
                pushInputRouting(state.recInput, r.bus, kMinusInfDb, 0, 0, false);
            }
            if (state.savedMainSend != -1) {
                SetMediaTrackInfo_Value(tr, "B_MAINSEND",
                                        static_cast<double>(state.savedMainSend));
            }
        }
        states_.clear();
        return;
    }

    // Enabling: prime by pushing every currently-active track. Run() would
    // catch up on its next tick (~33 ms), but doing it eagerly avoids the
    // user-visible lag between enabling the mirror and TotalMix reflecting
    // state. Tracks that aren't actively monitoring (default-input or
    // explicitly muted-monitor) are left out of the cache so they don't
    // push spurious -∞ values that overwrite the active tracks.
    states_.clear();
    const int trackCount = CountTracks(nullptr);
    for (int i = 0; i < trackCount; ++i) {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (tr == nullptr) continue;
        const int recMon = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECMON"));
        const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
        if (recMon == 0 || hwStartChannel(recInput) < 0) continue;
        updateTrackRouting(tr);
    }
}

void TotalReaperCSurf::SetSurfaceVolume(MediaTrack* tr, double volume) {
    // SetSurfaceVolume can fire as a state heartbeat. Only react for tracks
    // we're already tracking (i.e. that have been seen active) — otherwise
    // a default-input-but-monitor-off track would push -∞ here on its first
    // heartbeat, overwriting whatever's currently driving that channel.
    //
    // Important: don't write cached.linVol here. processTrack does its own
    // change detection by re-reading D_VOL and comparing against the cache;
    // priming linVol up front would mask the change and processTrack would
    // skip the update.
    if (!enabled_ || tr == nullptr) return;
    auto it = states_.find(tr);
    if (it == states_.end()) return;
    if (it->second.linVol == volume) return;
    processTrack(tr);
}

int TotalReaperCSurf::Extended(int call, void* parm1, void* /*parm2*/,
                               void* /*parm3*/) {
    if (call == CSURF_EXT_SETINPUTMONITOR) {
        processTrack(static_cast<MediaTrack*>(parm1));
        return 1;
    }
    return 0;
}

void TotalReaperCSurf::Run() {
    // Fire any deferred actions whose time has come, regardless of enabled
    // state — they were scheduled by code that already gated on enabled.
    if (!deferred_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = deferred_.begin(); it != deferred_.end(); ) {
            if (it->fireAt <= now) {
                auto action = std::move(it->action);
                it = deferred_.erase(it);
                action();
            } else {
                ++it;
            }
        }
    }

    if (!enabled_) return;
    const int trackCount = CountTracks(nullptr);
    for (int i = 0; i < trackCount; ++i) {
        processTrack(GetTrack(nullptr, i));
    }
}

void TotalReaperCSurf::scheduleAfter(int delayMs,
                                     std::function<void()> action) {
    deferred_.push_back({
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs),
        std::move(action)
    });
}

void TotalReaperCSurf::processTrack(MediaTrack* tr) {
    if (tr == nullptr || tr == GetMasterTrack(nullptr)) return;

    const int recInput = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
    const int recMon = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECMON"));
    const bool currActive = (recMon != 0 && hwStartChannel(recInput) >= 0);

    auto it = states_.find(tr);
    const bool wasTracked = (it != states_.end());

    // Don't pollute TotalMix for tracks the user never engaged. New tracks
    // arrive with default inputs (e.g. MADI 1 on Frank's UFX+) and would
    // otherwise immediately push -∞ to that channel, overwriting whatever
    // an actually-monitoring track is putting there.
    if (!wasTracked && !currActive) return;

    // Note: the scalar "no-change" shortcut from earlier versions is gone.
    // Send routings depend on other tracks' faders / sends, which can change
    // independently of this track's scalars, so we always re-walk the chain.
    // updateTrackRouting still avoids redundant pushes per routing via the
    // cached sendRoutings list.

    if (wasTracked && it->second.recInput != recInput &&
        hwStartChannel(it->second.recInput) >= 0) {
        // Input was reassigned — close out everything on the OLD input
        // (main bus + every cached send routing).
        const int mainBus = resolveMainBusFromMaster();
        const TrackState& c = it->second;
        if (mainBus >= 0) {
            pushInputRouting(c.recInput, mainBus, kMinusInfDb, 0, 0, false);
        }
        for (const auto& r : c.sendRoutings) {
            pushInputRouting(c.recInput, r.bus, kMinusInfDb, 0, 0, false);
        }
    }

    if (currActive) {
        updateTrackRouting(tr);
    } else {
        // Transition out of active. Drive the channel(s) to -∞ on every bus
        // we've been pushing to, then restore the track's main send.
        const int mainBus = resolveMainBusFromMaster();
        if (hwStartChannel(recInput) >= 0 && mainBus >= 0) {
            pushInputRouting(recInput, mainBus, kMinusInfDb, 0, 0, false);
        }
        if (wasTracked) {
            for (const auto& r : it->second.sendRoutings) {
                pushInputRouting(it->second.recInput, r.bus,
                                 kMinusInfDb, 0, 0, false);
            }
            if (it->second.savedMainSend != -1) {
                SetMediaTrackInfo_Value(tr, "B_MAINSEND",
                    static_cast<double>(it->second.savedMainSend));
            }
        }
        states_.erase(tr);
    }
}

void TotalReaperCSurf::updateTrackRouting(MediaTrack* tr) {
    // Master is excluded by processTrack (and fires here only via direct
    // calls during enable priming, where the caller has already filtered).
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

    const float mainDb = (recMon != 0) ? linToClampedDb(linVol) : kMinusInfDb;
    const bool nowActive = (mainDb > kMinusInfDb);

    // Mute REAPER's software monitor while we're driving the channel from
    // TotalMix — otherwise the user hears the input twice (TotalMix direct +
    // REAPER through-the-DAW with buffer-size latency = comb filter).
    TrackState& state = states_[tr];
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

    state.recInput = recInput;
    state.recMon = recMon;
    state.linVol = linVol;
    state.pan = pan;
    state.width = width;
    state.dualPanL = dualPanL;
    state.dualPanR = dualPanR;
    state.panMode = panMode;

    // Main bus: REAPER's master track HW out.
    const int mainBus = resolveMainBusFromMaster();
    if (mainBus >= 0) {
        const PanPair p = computeInputPan(recInput, pan, width,
                                          dualPanL, dualPanR, panMode);
        // Pan only meaningful while monitoring; when we're pushing -∞ we
        // don't fight any user adjustments to balpan in TotalMix.
        pushInputRouting(recInput, mainBus, mainDb, p.L, p.R, nowActive);
    }

    // Send routings: walk the source's audio sends to find every HW bus its
    // signal eventually reaches, with cumulative gain. Diff against cached
    // routings, push changes, close out routings that no longer apply.
    std::vector<WalkRouting> walked;
    if (nowActive) {
        std::unordered_set<MediaTrack*> visited;
        walkSendChain(tr, tr, 1.0, 0.0, false, visited, walked);
    }

    // Convert walked → CachedRouting (db, panL, panR) and push to TotalMix
    // anything that's new or changed.
    std::vector<CachedRouting> next;
    next.reserve(walked.size());
    for (const auto& w : walked) {
        if (w.bus == mainBus) continue; // main bus handled above; avoid duplicate
        CachedRouting r;
        r.bus = w.bus;
        r.db = linToClampedDb(w.linGain);
        r.hasPan = w.firstPanSet;
        if (isStereoInput(recInput)) {
            // Stereo source on a stereo dest bus: treat the (single) send pan
            // as a balance offset, full-width by default.
            const double sp = w.firstPanSet ? w.firstPan : 0.0;
            float L = static_cast<float>(sp - 1.0);
            float R = static_cast<float>(sp + 1.0);
            if (L < -1.0f) L = -1.0f; if (L > 1.0f) L = 1.0f;
            if (R < -1.0f) R = -1.0f; if (R > 1.0f) R = 1.0f;
            r.panL = L;
            r.panR = R;
        } else {
            r.panL = static_cast<float>(w.firstPanSet ? w.firstPan : 0.0);
            r.panR = 0.0f;
        }
        next.push_back(r);
    }

    // Close out routings that disappeared between cache and current.
    for (const auto& cached : state.sendRoutings) {
        const bool stillPresent = [&] {
            for (const auto& n : next) if (n.bus == cached.bus) return true;
            return false;
        }();
        if (!stillPresent) {
            pushInputRouting(recInput, cached.bus, kMinusInfDb, 0, 0, false);
        }
    }

    // Push current set: new entries OR entries with changed values.
    for (const auto& r : next) {
        const CachedRouting* prev = nullptr;
        for (const auto& cached : state.sendRoutings) {
            if (cached.bus == r.bus) { prev = &cached; break; }
        }
        if (prev == nullptr || prev->db != r.db ||
            prev->panL != r.panL || prev->panR != r.panR ||
            prev->hasPan != r.hasPan) {
            pushInputRouting(recInput, r.bus, r.db, r.panL, r.panR, r.hasPan);
        }
    }

    state.sendRoutings = std::move(next);
}

void TotalReaperCSurf::pushInputRouting(int recInput, int bus, float db,
                                        float panL, float panR, bool sendPan) {
    const int leftCh = hwStartChannel(recInput);
    if (leftCh < 0 || bus < 0) return;
    sendFader(leftCh, bus, db);
    const bool stereo = isStereoInput(recInput);
    if (stereo) {
        sendFader(leftCh + 1, bus, db);
    }
    if (sendPan) {
        sendBalpan(leftCh, bus, panL);
        if (stereo) {
            sendBalpan(leftCh + 1, bus, panR);
        }
    }
}

void TotalReaperCSurf::sendFader(int reaperChannel, int bus, float db) {
    if (client_ == nullptr || bus < 0) return;
    if (reaperChannel < 0 || reaperChannel > kRecInputChannelMask) return;

    const int hwIdx = reaper::reaperInputToHardware(reaperChannel);

    if (!client_->isConnected()) {
        client_->connect("127.0.0.1", txPort_);
    }

    char path[64];
    std::snprintf(path, sizeof(path), "/mix/in/%d/%d/fader", hwIdx, bus);
    osc::Message msg(path);
    msg.addFloat(db);
    client_->send(msg);
}

void TotalReaperCSurf::sendBalpan(int reaperChannel, int bus, float balpan) {
    if (client_ == nullptr || bus < 0) return;
    if (reaperChannel < 0 || reaperChannel > kRecInputChannelMask) return;

    const int hwIdx = reaper::reaperInputToHardware(reaperChannel);

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
