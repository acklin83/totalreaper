// ChannelMap.h — Translates REAPER input channel indices to hardware indices.
//
// Background: REAPER lets users remap which device hardware channels map to
// REAPER's "input N" slots, via Audio Prefs → Channel naming/mapping. The
// mapping is stored per-device in reaper.ini under sections like
// `[alias_in_CoreAudio_Fireface_UFX+_(23802132)]` with `chN=M` entries —
// REAPER channel N → hardware channel M.
//
// Crucially for TotalReaper, M is also TotalMix's matrix-channel index, since
// both view the same hardware. So this map is the bridge from I_RECINPUT to
// `/mix/in/<n>/<bus>/...`.
//
// We parse reaper.ini once at plugin load. If the user changes audio devices
// or remaps after that, they need to restart REAPER (Phase 1.0 limitation).

#pragma once

namespace totalreaper::reaper {

// Load and cache REAPER input channel mappings from reaper.ini.
// Should be called once at plugin init, after the REAPER API is available.
void loadChannelMap();

// Translate a REAPER input channel index (I_RECINPUT for mono inputs) to its
// hardware/TotalMix-matrix index. Returns the input unchanged if no mapping
// exists for that channel (assumes identity).
int reaperInputToHardware(int reaperChannel);

// Translate a REAPER output channel index (I_DSTCHAN on a HW send) to its
// hardware/TotalMix-bus index. Same shape as the input map, parsed from
// `[alias_out_*]` sections.
int reaperOutputToHardware(int reaperChannel);

} // namespace totalreaper::reaper
