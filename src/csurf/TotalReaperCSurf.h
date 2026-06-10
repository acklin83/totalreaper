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
//   3. Direct hardware-output sends on the track itself (e.g. a cue sent
//      straight to the RME phones channels with no intermediate bus track).
//      Same per-bus routing as (2); gain folds the track fader in per the
//      send mode (post = f * sendVol, pre = sendVol).
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
#include <unordered_set>
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

    // Live OSC send-port change (settings window). The next flush reconnects
    // the client to this port; the caller also reconnects the shared client.
    // Main-thread only.
    void setTxPort(std::uint16_t port) noexcept { txPort_ = port; }

    // Stereo-pair link: when on, REAPER stereo inputs ask TotalMix to link
    // their two hardware channels into one stereo strip. Link-only — turning
    // it off does NOT auto-unlink (that would rearrange the user's layout).
    // Persisted in ExtState.
    void setStereoPairLink(bool enabled);
    bool isStereoPairLink() const noexcept { return stereoPairLink_.load(); }

    // Called from the OSC receive thread when TotalMix reports a fader or
    // balpan value for /mix/in/<hwIdx>/<bus>/{fader,balpan}. Performs
    // echo-suppression against our own most-recent send and, if the value
    // looks like a genuine TotalMix-side change, queues a main-thread
    // callback to apply it to REAPER track state.
    void onIncomingFader(int hwIdx, int bus, float db);
    void onIncomingBalpan(int hwIdx, int bus, float balpan);

    // Called from the OSC receive thread for /input/<hwIdx>/width — the
    // stereo-width control of a linked pair. Echo-suppressed and queued like
    // fader/balpan; only acted on for linked stereo inputs.
    void onIncomingWidth(int hwIdx, float width);

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

    // Strip-level: send /input/<hwIdx>/stereo (1 = link the pair, 0 = unlink).
    // Sent immediately, not tick-staged — stereo-link changes are rare.
    void sendStripStereo(int hwIdx, bool on);

    // Strip-level: send /input/<hwIdx>/width (0.0 = mono … 1.0 = full stereo).
    void sendStripWidth(int hwIdx, float width);

    // True if incoming echoes for hwIdx are within the post-stereo-toggle mute
    // window. MUST be called with rxMu_ held; lazily evicts expired entries.
    bool stereoEchoMutedLocked_(int hwIdx);

    // One TotalMix routing this track currently drives (besides the main
    // bus). Cached so we know what to close out when a routing disappears.
    struct CachedRouting {
        int bus = -1;
        float db = 0.0f;
        float panL = 0.0f;
        float panR = 0.0f;
        bool hasPan = false;
        // Single composed balance (track pan + send pan) for this bus, used
        // when the input is a LINKED stereo pair (one balpan, not L/R).
        float balance = 0.0f;
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
    // Main-thread apply for /input/<hwIdx>/width → REAPER track D_WIDTH. Only
    // affects tracks whose input is a linked stereo pair.
    void applyIncomingWidth(int hwIdx, float width);

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

    // Returns true if `tr` is the elected "primary owner" for `hwIdx`.
    // Election rule, in priority order:
    //   1. The first track (lowest REAPER index) that is rec-armed AND
    //      monitoring. Frank's convention: at most one such track per input
    //      at any moment. This is the "active recording / monitoring" track.
    //   2. Else the first track that is monitoring (recMon != 0). This
    //      covers ordinary "I'm just listening to this input" usage where
    //      the user hasn't rec-armed anything yet.
    //   3. Else no primary — every track for this hwIdx is dormant.
    // Only the primary drives the TotalMix matrix cells for its hwIdx;
    // other tracks sharing the input are skipped so we don't get
    // last-write-wins jitter or ambiguous mute semantics.
    bool isPrimaryOwnerForHwIdx_(MediaTrack* tr, int hwIdx);

    // A send located on a source track for the 2-way write-back path:
    // which category (0 = track→track, 1 = direct hardware output) and its
    // index. {-1, -1} when no match.
    struct LocatedSend { int category; int index; };

    // Look up a send on `source` that reaches `targetBus` — either a
    // track→track send whose destination track has a hardware output to that
    // bus, or a direct hardware-output send on `source` itself. Track→track
    // is preferred (checked first). Multi-hop sends are not addressed by
    // 2-way for now; returns {-1, -1} if no direct match.
    LocatedSend findDirectSendToBus(MediaTrack* source, int targetBus);

    // Cache of the most recent fader/balpan value we sent on each (hwIdx,
    // bus). Keyed by (hwIdx<<16)|bus. Read from rx thread, written from
    // main thread → guarded by rxMu_.
    using EchoKey = std::int32_t;
    std::unordered_map<EchoKey, float> lastSentFader_;
    std::unordered_map<EchoKey, float> lastSentBalpan_;
    // Last stereo-width we sent per left hw channel — doubles as the dedupe
    // (don't re-send unchanged) and the echo cache (ignore TotalMix echoing
    // our own value back). Keyed by hwIdx. Guarded by rxMu_.
    std::unordered_map<int, float> lastSentWidth_;

    // Tick-level dedupe so multiple REAPER tracks sharing the same
    // I_RECINPUT don't each push their own gain for the same (hwIdx, bus)
    // matrix cell in one Run() tick — TotalMix can only hold one value per
    // cell, and rapid-fire writes during a fader move visibly jitter the
    // strip. processTrack stages here; Run() flushes once at the end. Last
    // write wins so the user-visible cell stays at whatever the previous
    // last-write-wins steady state was (avoid jumping to a different track's
    // value on first tick after enable). Main thread only.
    struct PendingFader { float db; };
    struct PendingBalpan { float balpan; };
    std::unordered_map<EchoKey, PendingFader>  tickFader_;
    std::unordered_map<EchoKey, PendingBalpan> tickBalpan_;
    // Flush tickFader_ / tickBalpan_ to the OSC client + lastSent caches +
    // TX log. Called once at end of Run().
    void flushTick_();

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
    std::atomic<bool> stereoPairLink_{false};      // link REAPER stereo inputs in TotalMix

    // Left hardware channels we've already sent a stereo-link for, so we don't
    // re-send every tick. Cleared on mirror-disable and on link-off. Written
    // and read only on the main thread (Run/updateTrackRouting).
    std::unordered_set<int> stereoLinked_;

    // Per-hwIdx deadline until which incoming fader/balpan echoes are ignored.
    // Set when we toggle a stereo link (for the pair's both channels) so
    // TotalMix's hard-L/R reset cascade isn't written back into REAPER via
    // 2-Way. Written on the main thread, read on the rx thread — guarded by
    // rxMu_.
    std::unordered_map<int, std::chrono::steady_clock::time_point> stereoEchoMute_;

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
