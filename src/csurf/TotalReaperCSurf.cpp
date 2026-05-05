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
        // stops affecting monitoring. We need to do this BEFORE clearing the
        // cache because pushFaderToInput depends on the cached hardware
        // index (the actual track may have been deleted by now, and Run()
        // wouldn't have a chance to re-read it).
        const int mainBus = resolveMainBusFromMaster();
        const int bus = mainBus >= 0 ? mainBus : 0;
        if (client_ && !client_->isConnected()) {
            client_->connect("127.0.0.1", txPort_);
        }
        for (const auto& [tr, state] : states_) {
            if (state.recInput < 0 || state.recInput >= kMonoInputMax) continue;
            const int hwIdx = reaper::reaperInputToHardware(state.recInput);
            char path[64];
            std::snprintf(path, sizeof(path), "/mix/in/%d/%d/fader", hwIdx, bus);
            osc::Message msg(path);
            msg.addFloat(kMinusInfDb);
            if (client_) client_->send(msg);
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
        if (recInput < 0 || recInput >= kMonoInputMax) continue;

        const int recMon = static_cast<int>(GetMediaTrackInfo_Value(tr, "I_RECMON"));
        const double linVol = GetMediaTrackInfo_Value(tr, "D_VOL");

        const TrackState& cached = states_[tr];
        if (cached.recInput == recInput &&
            cached.recMon == recMon &&
            cached.linVol == linVol) {
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

    // REAPER's I_RECINPUT is the user-visible channel slot, which may differ
    // from the device's hardware channel index (and TotalMix's matrix index).
    // Translate via the reaper.ini channel map.
    const int hwIdx = reaper::reaperInputToHardware(recInput);
    const int mainBus = resolveMainBusFromMaster();
    const int bus = mainBus >= 0 ? mainBus : 0;

    float targetDb = kMinusInfDb;
    if (recMon != 0 && linVol > 0.0) {
        targetDb = static_cast<float>(20.0 * std::log10(linVol));
        if (targetDb > kMaxDb) targetDb = kMaxDb;
        if (targetDb < kMinusInfDb) targetDb = kMinusInfDb;
    }

    if (!client_->isConnected()) {
        client_->connect("127.0.0.1", txPort_);
    }

    char path[64];
    std::snprintf(path, sizeof(path), "/mix/in/%d/%d/fader", hwIdx, bus);
    osc::Message msg(path);
    msg.addFloat(targetDb);
    client_->send(msg);
}

} // namespace totalreaper::csurf
