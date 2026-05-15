// TotalReaperCSurf.h — REAPER control surface that mirrors track state to TotalMix.
//
// What we mirror, while the routing mirror is engaged:
//   1. Input monitoring → /mix/in/<n>/<main>/fader. The main bus is the
//      master track's first HW send.
//   2. Track-to-track sends that eventually reach a hardware output, which
//      add per-bus routings: /mix/in/<n>/<destbus>/fader. Walks send chains
//      recursively, multiplying intermediate track faders for post-fader
//      sends, so Track A → Track B → Track C (HW out) projects A's input
//      onto C's bus at gain f_A * sendVol_AB * f_B * sendVol_BC.
//
// We do NOT mirror destination track faders to TotalMix output bus volumes —
// those are user/external-controller territory (e.g. more_me.html).
//
// Hooked callbacks: SetSurfaceVolume, Extended(CSURF_EXT_SETINPUTMONITOR),
// plus a 30 Hz Run() poll that catches changes other surfaces (UF8,
// automation, scripts) don't propagate to us.
//
// Limitations:
//   - Hardware inputs only — mono and stereo. MIDI inputs (bit 4096) and
//     multichannel inputs (bit 2048) are ignored.
//   - Send pan only uses the first send in the chain (closest to source).
//     Multi-hop pan composition is not modelled.

#pragma once

#include "../osc/OscClient.h"

#include "reaper_plugin.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace totalreaper::csurf {

class TotalReaperCSurf : public IReaperControlSurface {
public:
    TotalReaperCSurf(osc::Client* client, std::uint16_t txPort);
    ~TotalReaperCSurf() override = default;

    const char* GetTypeString() override { return "TOTALREAPER"; }
    const char* GetDescString() override { return "TotalReaper"; }
    const char* GetConfigString() override { return ""; }

    void SetSurfaceVolume(MediaTrack* tr, double volume) override;
    void SetPlayState(bool play, bool pause, bool rec) override;
    int Extended(int call, void* parm1, void* parm2, void* parm3) override;

    // Run() fires ~30×/sec. We poll track state here as a fallback because
    // some change sources (other control surfaces like UF8, automation,
    // scripts) don't always reach SetSurfaceVolume on our instance.
    void Run() override;

    // Master enable/disable. Enabling pushes current state of every monitored
    // track immediately. Disabling drives every previously-mirrored input to
    // -∞ in TotalMix so REAPER no longer affects monitoring; the user's
    // pre-engagement TotalMix state is not restored (we don't snapshot).
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return enabled_.load(); }

    // 2-Way Control: when on, incoming fader/balpan changes from TotalMix
    // are mirrored back into REAPER track volume / send volume. Only takes
    // effect while isEnabled() — without the outgoing direction active, the
    // echo-suppression cache stays empty and we'd loop. Persisted in
    // ExtState so the user's preference survives REAPER restarts.
    void setTwoWayEnabled(bool enabled);
    bool isTwoWayEnabled() const noexcept { return twoWayEnabled_.load(); }

    // Auto-Talkback on Stop: when on, REAPER's transport state drives
    // TotalMix talkback — stopped/paused opens talkback, play/record closes
    // it. Persisted in ExtState. Like the other csurf-side toggles, only
    // takes effect while isEnabled().
    void setAutoTalkbackEnabled(bool enabled);
    bool isAutoTalkbackEnabled() const noexcept { return autoTalkbackEnabled_.load(); }

    // Called from the OSC receive thread when TotalMix reports a fader or
    // balpan value for /mix/in/<hwIdx>/<bus>/{fader,balpan}. Performs
    // echo-suppression against our own most-recent send and, if the value
    // looks like a genuine TotalMix-side change, queues a main-thread
    // callback to apply it to REAPER track state.
    void onIncomingFader(int hwIdx, int bus, float db);
    void onIncomingBalpan(int hwIdx, int bus, float balpan);

    // Called from the OSC receive thread when TotalMix reports a preamp
    // value at /input/<hwIdx>/{48v,pad,phase,gain}. Mirrors the value
    // into the matching REAPER track's P_EXT:totalreaper_<leaf> so any
    // surface or script reading that cache (e.g. reaper-uf8's REC + RME
    // value-line) reflects TotalMix-side changes immediately. Always
    // ungated by twoWayEnabled_ — preamp state is descriptive metadata
    // that doesn't conflict with REAPER state. `leaf` must be one of
    // "48v" / "pad" / "phase" / "gain". Queues a main-thread apply.
    void onIncomingPreamp(int hwIdx, const char* leaf, float value);

    // Schedule a callback to fire from Run() after `delayMs` milliseconds.
    // Used for sequencing operations like "mute → toggle pad → unmute" so
    // a brief audio mute hides hardware pop noise around a pad change.
    // Fires on REAPER's main thread, so REAPER API calls inside are safe.
    void scheduleAfter(int delayMs, std::function<void()> action);

private:
    // Send /controlroom/talkback to TotalMix and update the TalkbackOn
    // ExtState. Used by SetPlayState (auto fires) and setAutoTalkbackEnabled
    // (immediate apply on toggle).
    void sendTalkback(bool on);

    // Reconcile one track's TotalMix routing with its current REAPER state.
    // Implements the state machine: only tracks that have been seen "active"
    // (monitor on with a hardware input) are kept in the cache; tracks that
    // never had monitor on are ignored entirely so they don't push spurious
    // -∞ values that overwrite legitimately-active channels.
    void processTrack(MediaTrack* tr);

    // Lower-level helper called by processTrack for tracks that should
    // currently be reflected in TotalMix. Pushes fader, balpan and manages
    // the B_MAINSEND override.
    void updateTrackRouting(MediaTrack* tr);

    // Push a fader value (+ optional balpan) for an entire I_RECINPUT to a
    // specific bus. Stereo inputs get fader on both channels but no balpan —
    // TotalMix already lays out the pair correctly (hard L/R when split,
    // balance when linked); two balpan sends would overwrite each other on a
    // linked strip and fight the user's hand-set values on split strips.
    // sendPan=false means "don't touch balpan" (also honored for mono).
    void pushInputRouting(int recInput, int bus, float db,
                          float panL, float panR, bool sendPan);

    // Low-level: send a single fader OSC message for one device channel on
    // a specific bus. Applies the reaper.ini input alias translation.
    void sendFader(int reaperChannel, int bus, float db);

    // Low-level: send a pan/balance OSC message (-1.0 hard left … +1.0 hard
    // right) for one device channel on a specific bus.
    void sendBalpan(int reaperChannel, int bus, float balpan);

    // One TotalMix routing this track currently drives (besides the main
    // bus). Cached so we know what to close out when a routing disappears.
    struct CachedRouting {
        int bus = -1;
        float db = 0.0f;
        float panL = 0.0f;
        float panR = 0.0f;
        bool hasPan = false;
    };

    // Per-track state cache so Run() can detect changes and avoid re-sending
    // identical values every tick.
    struct TrackState {
        int recInput = -2;   // -2 = "never seen" (distinct from -1 = no input)
        int recMon = -1;
        double linVol = -1.0;
        double pan = 0.0;
        double width = 1.0;     // stereo pan mode (5)
        double dualPanL = 0.0;  // dual pan mode (6)
        double dualPanR = 0.0;
        int panMode = -1;
        // -1 = we haven't overridden this track's main send. Otherwise the
        // value B_MAINSEND had before we set it to 0 — restored on disengage.
        // Overriding silences REAPER's software monitor so the user doesn't
        // hear the input doubled (TotalMix direct + REAPER through-the-DAW).
        int savedMainSend = -1;

        // Routings to non-main buses, derived from this track's audio sends.
        // Recomputed every processTrack call; entries no longer present get
        // closed out (push -∞) and removed.
        std::vector<CachedRouting> sendRoutings;
    };
    std::unordered_map<MediaTrack*, TrackState> states_;

    struct Deferred {
        std::chrono::steady_clock::time_point fireAt;
        std::function<void()> action;
    };
    std::vector<Deferred> deferred_;

    // Main-thread apply helpers used by drainRxQueue. Both run with REAPER
    // API thread guarantees and re-check enabled_/twoWayEnabled_ since state
    // can change between rx-thread enqueue and main-thread drain.
    void applyIncomingFader(int hwIdx, int bus, float db);
    void applyIncomingBalpan(int hwIdx, int bus, float balpan);

    // Main-thread apply for preamp 48v/pad/phase/gain. Writes the
    // formatted value (gain as decimal dB, flags as "0"/"1") to
    // P_EXT:totalreaper_<leaf> on every REAPER track whose I_RECINPUT
    // resolves to `hwIdx`. Iterates all tracks because multiple tracks
    // can share the same hardware input — a 1-knob change in TotalMix
    // should reflect on each of them.
    void applyIncomingPreamp(int hwIdx, std::string extKey,
                             std::string value);

    // Find the first track whose I_RECINPUT maps to exactly `hwIdx` as its
    // left/start channel. Returns nullptr if none. "First" = lowest index in
    // REAPER's track order — matches user expectation when multiple tracks
    // share an input.
    MediaTrack* findFirstTrackForHwIdx(int hwIdx);

    // Look up the direct-send index on `source` whose destination track has
    // a hardware output to `targetBus`. Returns -1 if no direct match
    // (multi-hop sends are not addressed by 2-way for now).
    int findDirectSendToBus(MediaTrack* source, int targetBus);

    // Cache of the most recent fader/balpan value we sent on each (hwIdx,
    // bus). Keyed by (hwIdx<<16)|bus. Read from rx thread, written from
    // main thread → guarded by rxMu_.
    using EchoKey = std::int32_t;
    std::unordered_map<EchoKey, float> lastSentFader_;
    std::unordered_map<EchoKey, float> lastSentBalpan_;

    // Queue of main-thread callbacks produced by onIncoming*. Drained at
    // the top of every Run() tick. Guarded by rxMu_.
    std::vector<std::function<void()>> rxQueue_;

    mutable std::mutex rxMu_;

    // Echo-detection thresholds. TotalMix typically echoes our values back
    // exactly, but we leave some slop for float quantisation paths.
    static constexpr float kEchoFaderEpsilonDb = 0.05f;
    static constexpr float kEchoBalpanEpsilon = 0.005f;

    osc::Client* client_;
    std::uint16_t txPort_;
    std::atomic<bool> enabled_{false};            // routing-mirror direction (REAPER → TotalMix)
    std::atomic<bool> twoWayEnabled_{false};      // additionally enable TotalMix → REAPER
    std::atomic<bool> autoTalkbackEnabled_{false}; // drive talkback from transport state

    // Held true during the routing-mirror enable window so 2-Way's rx path
    // doesn't snap REAPER faders to TotalMix's pre-push state. While set,
    // onIncomingFader/Balpan only seed the echo cache (push values win via
    // try_emplace), no apply happens. Cleared by a scheduleAfter ~500 ms
    // after the push so the bulk of /sendall responses have time to arrive
    // and be absorbed without disturbing REAPER track state.
    std::atomic<bool> priming_{false};

    // Last "rolling" state SetPlayState was called with, so we only act on
    // genuine transitions (play→stop, stop→play). REAPER tends to fire
    // SetPlayState only on changes but it's cheap to dedupe and avoids any
    // surprise OSC chatter.
    bool lastRolling_ = false;
    bool lastRollingValid_ = false;
};

} // namespace totalreaper::csurf
