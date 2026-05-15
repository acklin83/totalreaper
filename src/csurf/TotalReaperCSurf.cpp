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
#include <cstring>
#include <unordered_set>
#include <utility>

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
    if (enabled == enabled_.load()) return;
    enabled_.store(enabled);
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

    // Hold priming_ across the push + /sendall window. While set, 2-Way's rx
    // path won't touch REAPER track state — it only seeds the echo cache.
    // Without this, TotalMix's pre-push state (which is -∞ for any channel
    // the user hadn't already routed) gets echoed back to us and 2-Way
    // happily applies it, slamming every active REAPER fader to -∞.
    priming_.store(true);

    // Push REAPER state to TotalMix FIRST so lastSentFader_ is populated
    // before any /sendall response arrives. Run() would catch up on its
    // next tick (~33 ms), but doing it eagerly avoids the user-visible lag
    // between enabling the mirror and TotalMix reflecting state. Tracks
    // that aren't actively monitoring (default-input or explicitly
    // muted-monitor) are left out so they don't push spurious -∞ values
    // that overwrite the active tracks.
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

    // Now ask TotalMix to re-emit every current parameter so our
    // TotalMixState cache gets seeded for channel-strip controls (gain,
    // 48v, pad, phase, mute) we haven't observed yet. Without this, the
    // first preamp gain delta on a fresh track had to wait for the user to
    // nudge the knob in TotalMix once. /sendall is the official Global OSC
    // trigger for this (see TotalMix FX 2.1 alpha 5 spec).
    //
    // Order matters: /sendall AFTER the push so for every (hwIdx, bus) we
    // care about, TotalMix's reported value is already the one we just
    // pushed — the echo cache matches and 2-Way doesn't try to feed it
    // back into REAPER.
    if (client_ != nullptr) {
        if (!client_->isConnected()) {
            client_->connect("127.0.0.1", txPort_);
        }
        osc::Message refresh("/sendall");
        refresh.addFloat(1.0f);
        client_->send(refresh);
    }

    // Release the priming flag once the /sendall response burst has had
    // time to land. 500 ms is heuristic but generous on localhost UDP.
    // After release, the rxQueue is cleared so any pre-priming straggler
    // doesn't fire on the next Run() tick.
    scheduleAfter(500, [this]() {
        {
            std::lock_guard<std::mutex> g(rxMu_);
            rxQueue_.clear();
        }
        priming_.store(false);
    });
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
    if (!enabled_.load() || tr == nullptr) return;
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
    // Drain rx-thread-queued 2-way work first so any track-state changes it
    // makes are visible to processTrack on the same tick. Holding rxMu_ only
    // for the swap keeps the rx thread unblocked while we execute.
    std::vector<std::function<void()>> rxJobs;
    {
        std::lock_guard<std::mutex> g(rxMu_);
        rxJobs.swap(rxQueue_);
    }
    for (auto& job : rxJobs) job();

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

    if (!enabled_.load()) return;
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
    if (!enabled_.load() || tr == nullptr || tr == GetMasterTrack(nullptr) ||
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
    //
    // Enforce B_MAINSEND=0 on every tick (not just first activation): project
    // load, undo, automation, or another control surface can restore the main
    // send behind our back. The cached savedMainSend captures the original
    // value the first time we see it non-zero, so we can restore it on
    // disengage.
    TrackState& state = states_[tr];
    if (nowActive) {
        const int currentMainSend = static_cast<int>(
            GetMediaTrackInfo_Value(tr, "B_MAINSEND"));
        if (state.savedMainSend == -1 && currentMainSend != 0) {
            state.savedMainSend = currentMainSend;
        }
        if (currentMainSend != 0) {
            SetMediaTrackInfo_Value(tr, "B_MAINSEND", 0.0);
        }
    } else if (state.savedMainSend != -1) {
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
        // Stereo inputs: don't touch balpan. TotalMix already handles the
        // pair correctly on its own — when split into 2 mono strips it sets
        // each strip's balpan to hard L/R; when linked it treats balpan as a
        // balance for the pair. Sending two balpan messages to the L and R
        // hardware channels would either overwrite each other on a linked
        // strip (panning the whole pair) or fight the user's manual setting
        // on split strips. (void) the unused params to silence warnings.
        (void)panL; (void)panR;
        return;
    }
    if (sendPan) {
        sendBalpan(leftCh, bus, panL);
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

    // Remember what we just sent so a TotalMix echo on this (hwIdx, bus) is
    // recognised as our own and ignored by onIncomingFader.
    {
        std::lock_guard<std::mutex> g(rxMu_);
        lastSentFader_[(hwIdx << 16) | bus] = db;
    }
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

    {
        std::lock_guard<std::mutex> g(rxMu_);
        lastSentBalpan_[(hwIdx << 16) | bus] = balpan;
    }
}

void TotalReaperCSurf::setTwoWayEnabled(bool enabled) {
    if (enabled == twoWayEnabled_.load()) return;
    twoWayEnabled_.store(enabled);
    SetExtState("TotalReaper", "TwoWayEnabled",
                enabled ? "1" : "0", /*persist*/ true);
}

void TotalReaperCSurf::setAutoTalkbackEnabled(bool enabled) {
    if (enabled == autoTalkbackEnabled_.load()) return;
    autoTalkbackEnabled_.store(enabled);
    SetExtState("TotalReaper", "AutoTalkbackEnabled",
                enabled ? "1" : "0", /*persist*/ true);
    // Reset the dedupe baseline so the next transport event fires regardless
    // of the previous transition direction — gives the user immediate
    // feedback when they enable auto-talkback during playback.
    lastRollingValid_ = false;

    if (!enabled_.load()) return;

    if (enabled) {
        // First-time enable: if the transport is currently stopped/paused,
        // open talkback right away so the user doesn't have to hit stop once
        // to trigger the rule. If currently playing, leave talkback alone —
        // the next stop will fire SetPlayState.
        const int ps = GetPlayState();
        const bool rolling = (ps & 1) != 0 && (ps & 2) == 0;
        if (!rolling) sendTalkback(true);
    } else {
        // Disabling closes talkback so the engineer doesn't end up stuck
        // broadcasting after toggling the feature off mid-session.
        sendTalkback(false);
    }
}

void TotalReaperCSurf::SetPlayState(bool play, bool pause, bool /*rec*/) {
    if (!autoTalkbackEnabled_.load() || !enabled_.load()) return;

    // "Rolling" = transport actively moving. Pause counts as not rolling so
    // the engineer can still talk to the live room while paused.
    const bool rolling = play && !pause;
    if (lastRollingValid_ && rolling == lastRolling_) return;
    lastRolling_ = rolling;
    lastRollingValid_ = true;

    sendTalkback(!rolling);
}

void TotalReaperCSurf::sendTalkback(bool on) {
    if (client_ == nullptr) return;
    if (!client_->isConnected()) {
        client_->connect("127.0.0.1", txPort_);
    }
    osc::Message m("/controlroom/talkback");
    m.addFloat(on ? 1.0f : 0.0f);
    client_->send(m);

    // Keep the manual-toggle ExtState in sync so the Toggle Talkback action's
    // checkmark reflects reality after we drive talkback automatically.
    SetExtState("TotalReaper", "TalkbackOn",
                on ? "1" : "0", /*persist*/ true);
}

void TotalReaperCSurf::onIncomingFader(int hwIdx, int bus, float db) {
    // Reject early on the rx thread so we don't allocate / queue work for
    // every fader echo TotalMix emits when the user moves things in REAPER.
    if (!twoWayEnabled_.load() || !enabled_.load()) return;
    if (hwIdx < 0 || bus < 0) return;

    const EchoKey key = (hwIdx << 16) | bus;
    {
        std::lock_guard<std::mutex> g(rxMu_);
        if (priming_.load()) {
            // Routing-mirror just enabled — TotalMix is dumping its old
            // state and our push is racing in parallel. Don't drive REAPER
            // off these values; only seed the echo cache for combos we
            // haven't pushed yet (try_emplace skips when push got there
            // first). Any genuine user moves during this ~500 ms window are
            // intentionally dropped.
            lastSentFader_.try_emplace(key, db);
            return;
        }
        auto it = lastSentFader_.find(key);
        if (it != lastSentFader_.end() &&
            std::fabs(it->second - db) < kEchoFaderEpsilonDb) {
            return; // our own echo
        }
        rxQueue_.push_back([this, hwIdx, bus, db]() {
            applyIncomingFader(hwIdx, bus, db);
        });
    }
}

void TotalReaperCSurf::onIncomingPreamp(int hwIdx, const char* leaf, float v) {
    // Ungated by enabled_ / twoWayEnabled_ — preamp readbacks are
    // metadata, not routing state. Caller wants P_EXT current whenever
    // TotalMix emits, regardless of whether the routing mirror is on.
    if (hwIdx < 0 || leaf == nullptr) return;

    std::string extKey;
    char valBuf[32];
    if (std::strcmp(leaf, "gain") == 0) {
        extKey = "P_EXT:totalreaper_gain";
        std::snprintf(valBuf, sizeof(valBuf), "%.6f", v);
    } else if (std::strcmp(leaf, "48v") == 0) {
        extKey = "P_EXT:totalreaper_48v";
        std::strcpy(valBuf, v >= 0.5f ? "1" : "0");
    } else if (std::strcmp(leaf, "pad") == 0) {
        extKey = "P_EXT:totalreaper_pad";
        std::strcpy(valBuf, v >= 0.5f ? "1" : "0");
    } else if (std::strcmp(leaf, "phase") == 0) {
        extKey = "P_EXT:totalreaper_phase";
        std::strcpy(valBuf, v >= 0.5f ? "1" : "0");
    } else {
        return;
    }
    std::string value(valBuf);
    std::lock_guard<std::mutex> g(rxMu_);
    rxQueue_.push_back([this, hwIdx,
                        ek = std::move(extKey),
                        val = std::move(value)]() {
        applyIncomingPreamp(hwIdx, ek, val);
    });
}

void TotalReaperCSurf::onIncomingBalpan(int hwIdx, int bus, float balpan) {
    if (!twoWayEnabled_.load() || !enabled_.load()) return;
    if (hwIdx < 0 || bus < 0) return;

    const EchoKey key = (hwIdx << 16) | bus;
    {
        std::lock_guard<std::mutex> g(rxMu_);
        if (priming_.load()) {
            lastSentBalpan_.try_emplace(key, balpan);
            return;
        }
        auto it = lastSentBalpan_.find(key);
        if (it != lastSentBalpan_.end() &&
            std::fabs(it->second - balpan) < kEchoBalpanEpsilon) {
            return;
        }
        rxQueue_.push_back([this, hwIdx, bus, balpan]() {
            applyIncomingBalpan(hwIdx, bus, balpan);
        });
    }
}

MediaTrack* TotalReaperCSurf::findFirstTrackForHwIdx(int hwIdx) {
    // TotalMix-side addresses are hardware indices, REAPER-side I_RECINPUT
    // stores REAPER-slot indices, and reaper.ini's [alias_in_*] map translates
    // between them. sendFader applies that map on the way out; for the rx
    // path we have to mirror it: translate each track's leftCh through the
    // same alias map before comparing to the incoming hwIdx. Without this,
    // every rx finds no matching track, applyIncomingFader no-ops, and
    // processTrack subsequently re-sends the unchanged D_VOL → user's move
    // in TotalMix snaps back.
    const int trackCount = CountTracks(nullptr);
    for (int i = 0; i < trackCount; ++i) {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (tr == nullptr) continue;
        const int recInput =
            static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
        const int leftCh = hwStartChannel(recInput);
        if (leftCh < 0) continue;
        // The track owns leftCh (and leftCh+1 for stereo). For 2-way we only
        // act on the primary (left) channel — rx for leftCh+1 falls through
        // and is silently ignored.
        if (reaper::reaperInputToHardware(leftCh) == hwIdx) return tr;
    }
    return nullptr;
}

int TotalReaperCSurf::findDirectSendToBus(MediaTrack* source, int targetBus) {
    if (source == nullptr || targetBus < 0) return -1;
    const int sendCount = GetTrackNumSends(source, 0);
    for (int i = 0; i < sendCount; ++i) {
        const bool muted =
            GetTrackSendInfo_Value(source, 0, i, "B_MUTE") != 0;
        if (muted) continue;
        MediaTrack* dest = reinterpret_cast<MediaTrack*>(static_cast<intptr_t>(
            static_cast<std::int64_t>(GetTrackSendInfo_Value(
                source, 0, i, "P_DESTTRACK"))));
        if (dest == nullptr) continue;
        if (resolveHwOutBus(dest) == targetBus) return i;
    }
    return -1;
}

void TotalReaperCSurf::applyIncomingPreamp(int hwIdx, std::string extKey,
                                            std::string value) {
    // Update P_EXT on every track that maps to this hwIdx (multiple
    // tracks can share an input). REAPER's track API is main-thread
    // only — this is called from drainRxQueue inside Run().
    const int trackCount = CountTracks(nullptr);
    for (int i = 0; i < trackCount; ++i) {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (tr == nullptr) continue;
        const int recInput =
            static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
        const int leftCh = hwStartChannel(recInput);
        if (leftCh < 0) continue;
        if (reaper::reaperInputToHardware(leftCh) != hwIdx) continue;
        // GetSetMediaTrackInfo_String's setNewValue=true wants a
        // mutable char*; copy from our owned string buffer.
        char buf[64];
        std::strncpy(buf, value.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        GetSetMediaTrackInfo_String(tr, extKey.c_str(), buf, true);
    }
}

void TotalReaperCSurf::applyIncomingFader(int hwIdx, int bus, float db) {
    if (!enabled_.load() || !twoWayEnabled_.load()) return;

    MediaTrack* tr = findFirstTrackForHwIdx(hwIdx);
    if (tr == nullptr) return;

    // Pre-set the echo cache to the value we're about to drive REAPER to.
    // processTrack will re-send the same value to TotalMix on the next tick;
    // TotalMix will echo it back; the echo will match this cache entry and
    // be suppressed inside onIncomingFader.
    {
        std::lock_guard<std::mutex> g(rxMu_);
        lastSentFader_[(hwIdx << 16) | bus] = db;
    }

    const float clampedDb = (db > kMaxDb) ? kMaxDb : db;
    const double linVol = (clampedDb <= kMinusInfDb + 1.0f)
        ? 0.0
        : std::pow(10.0, clampedDb / 20.0);

    const int mainBus = resolveMainBusFromMaster();
    if (bus == mainBus) {
        SetMediaTrackInfo_Value(tr, "D_VOL", linVol);
        return;
    }

    // Send-bus: scale the direct send leading from this track to `bus`. For
    // post-fader sends (sendMode==0), the on-bus level = trackFader *
    // sendVol; solving for sendVol so that the combined gain equals the
    // user's incoming fader value. Pre-fader (1/2) is direct.
    const int sendIdx = findDirectSendToBus(tr, bus);
    if (sendIdx < 0) return; // multi-hop or unmapped send: not handled in v1

    const int sendMode = static_cast<int>(
        GetTrackSendInfo_Value(tr, 0, sendIdx, "I_SENDMODE"));
    double sendVol;
    if (sendMode == 0) {
        const double trackVol = GetMediaTrackInfo_Value(tr, "D_VOL");
        if (trackVol <= 1e-9) return; // can't recover sendVol when fader is -∞
        sendVol = linVol / trackVol;
    } else {
        sendVol = linVol;
    }
    SetTrackSendInfo_Value(tr, 0, sendIdx, "D_VOL", sendVol);
}

void TotalReaperCSurf::applyIncomingBalpan(int hwIdx, int bus, float balpan) {
    if (!enabled_.load() || !twoWayEnabled_.load()) return;

    MediaTrack* tr = findFirstTrackForHwIdx(hwIdx);
    if (tr == nullptr) return;

    // For stereo inputs we don't send balpan to TotalMix (see pushInputRouting)
    // so accepting it back here would create asymmetry; skip.
    const int recInput =
        static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECINPUT"));
    if (isStereoInput(recInput)) return;

    {
        std::lock_guard<std::mutex> g(rxMu_);
        lastSentBalpan_[(hwIdx << 16) | bus] = balpan;
    }

    const double clamped = (balpan < -1.0f) ? -1.0
                          : (balpan > 1.0f) ?  1.0
                          : static_cast<double>(balpan);

    const int mainBus = resolveMainBusFromMaster();
    if (bus == mainBus) {
        SetMediaTrackInfo_Value(tr, "D_PAN", clamped);
        return;
    }

    const int sendIdx = findDirectSendToBus(tr, bus);
    if (sendIdx < 0) return;
    SetTrackSendInfo_Value(tr, 0, sendIdx, "D_PAN", clamped);
}

} // namespace totalreaper::csurf
