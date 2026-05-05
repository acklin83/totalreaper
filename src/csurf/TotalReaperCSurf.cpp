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
constexpr int kMonoInputMax = 1024; // I_RECINPUT < 1024 means mono HW input

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
        // stops affecting monitoring. We use the cached recInput (not a fresh
        // re-read) because the track may have been deleted by now.
        for (const auto& [tr, state] : states_) {
            sendFader(state.recInput, kMinusInfDb);
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

        const TrackState& cached = states_[tr];
        if (cached.recInput == recInput &&
            cached.recMon == recMon &&
            cached.linVol == linVol) {
            continue;
        }

        // If the input was reassigned, close out the OLD input first so it
        // doesn't stay stuck at the last fader value in TotalMix. Then fall
        // through to the normal update for the new input.
        if (cached.recInput >= 0 && cached.recInput < kMonoInputMax &&
            cached.recInput != recInput) {
            sendFader(cached.recInput, kMinusInfDb);
        }

        if (recInput < 0 || recInput >= kMonoInputMax) {
            // No valid mono input now — leave cache reflecting current state
            // (so we don't re-send the close-out next tick).
            states_[tr] = {recInput, recMon, linVol};
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
    if (recInput < 0 || recInput >= kMonoInputMax) return;

    const int recMon = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECMON"));
    const double linVol = GetMediaTrackInfo_Value(tr, "D_VOL");

    // Refresh cache so Run()'s no-change shortcut applies on the next tick
    // and SetSurfaceVolume's heartbeat dedupe sees the latest value.
    states_[tr] = {recInput, recMon, linVol};

    float targetDb = kMinusInfDb;
    if (recMon != 0 && linVol > 0.0) {
        targetDb = static_cast<float>(20.0 * std::log10(linVol));
        if (targetDb > kMaxDb) targetDb = kMaxDb;
        if (targetDb < kMinusInfDb) targetDb = kMinusInfDb;
    }

    sendFader(recInput, targetDb);
}

void TotalReaperCSurf::sendFader(int reaperChannel, float db) {
    if (client_ == nullptr) return;
    if (reaperChannel < 0 || reaperChannel >= kMonoInputMax) return;

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

} // namespace totalreaper::csurf
