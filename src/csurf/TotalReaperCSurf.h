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

#include <cstdint>
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
    bool isEnabled() const noexcept { return enabled_; }

private:
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

    // Push a fader value (+ optional balpans) for an entire I_RECINPUT to
    // a specific bus. Splits stereo inputs into left/right channels.
    // panL == panR == NaN_marker means "don't touch balpan".
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

    osc::Client* client_;
    std::uint16_t txPort_;
    bool enabled_ = false; // off by default — user opts in via the toggle action
};

} // namespace totalreaper::csurf
