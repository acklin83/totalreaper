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
//   - Hardware inputs only — mono and stereo. MIDI inputs (bit 4096) and
//     multichannel inputs (bit 2048) are ignored.
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

    // Send the fader value to TotalMix for every channel of a given
    // I_RECINPUT — one message for mono, two for stereo (left + right).
    // Skips MIDI and multichannel inputs.
    void sendFaderForInput(int recInput, float db);

    // Low-level: send a single fader OSC message for one device channel.
    // Applies the reaper.ini input alias translation. The bus is resolved
    // from the REAPER master track on each call.
    void sendFader(int reaperChannel, float db);

    // Low-level: send a pan/balance OSC message (-1.0 hard left … +1.0 hard
    // right) to one device channel's send-to-Main routing.
    void sendBalpan(int reaperChannel, float balpan);

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
    };
    std::unordered_map<MediaTrack*, TrackState> states_;

    osc::Client* client_;
    std::uint16_t txPort_;
    bool enabled_ = false; // off by default — user opts in via the toggle action
};

} // namespace totalreaper::csurf
