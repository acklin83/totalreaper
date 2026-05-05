// TotalReaperCSurf.h — REAPER control surface that mirrors track state to TotalMix.
//
// Phase 1.0 behaviour: when a REAPER track has a hardware input assigned and
// input monitoring is on, mirror the track's fader value to that input's send
// to the main output bus in TotalMix. When monitoring is off, send the fader
// to -∞ (we use the fader, not the mute path, because mute is reserved for
// later user-facing features).
//
// Hooked callbacks:
//   - SetSurfaceVolume: fires on every fader move, tiny user latency.
//   - Extended(CSURF_EXT_SETINPUTMONITOR): fires on monitor toggle.
//
// Limitations (Phase 1.0):
//   - Mono hardware inputs only (I_RECINPUT in 0..1023). Stereo / MIDI ignored.
//   - The destination bus in TotalMix is derived from the REAPER master
//     track's first hardware send (its I_DSTCHAN). If the master has no HW
//     send, falls back to bus 0.

#pragma once

#include "../osc/OscClient.h"

#include "reaper_plugin.h"

#include <cstdint>
#include <unordered_map>

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
    void updateTrackRouting(MediaTrack* tr);

    // Send a single fader-value OSC message. `reaperChannel` is REAPER's
    // mono input channel index (I_RECINPUT for mono); the function applies
    // the channel-name-to-hardware translation. The bus is resolved from
    // REAPER's master track on each call. Used by updateTrackRouting and
    // also by the close-out paths (input reassignment, toggle off).
    void sendFader(int reaperChannel, float db);

    // Per-track state cache so Run() can detect changes and avoid re-sending
    // identical values every tick.
    struct TrackState {
        int recInput = -2;   // -2 = "never seen" (distinct from -1 = no input)
        int recMon = -1;
        double linVol = -1.0;
    };
    std::unordered_map<MediaTrack*, TrackState> states_;

    osc::Client* client_;
    std::uint16_t txPort_;
    bool enabled_ = false; // off by default — user opts in via the toggle action
};

} // namespace totalreaper::csurf
