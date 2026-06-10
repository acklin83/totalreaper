// ReaperAPI.h — Function pointer table for the REAPER C API.
//
// REAPER extensions receive a `reaper_plugin_info_t` struct on load with a
// `GetFunc` callback. The SDK's reaper_plugin_functions.h declares one
// extern function pointer per REAPER API function and provides a loader.
//
// We define REAPERAPI_MINIMAL + REAPERAPI_WANT_<name> for *only* the
// functions TotalReaper actually calls. Without this, the loader tries to
// resolve every function in the SDK header, and any function added in a
// newer REAPER than the user runs (e.g. SDK pinned at 7.70 vs REAPER 7.66
// in the field) makes our strict load check fail and the plugin gets
// silently rejected — no actions show up.
//
// As we touch more of the API, add a REAPERAPI_WANT_<funcname> line below
// rather than scattering GetFunc lookups throughout the codebase.

#pragma once

#define REAPERAPI_MINIMAL

#define REAPERAPI_WANT_CountSelectedTracks
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_get_ini_file
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_GetMasterTrack
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetTrackNumSends
#define REAPERAPI_WANT_GetTrackSendInfo_Value
#define REAPERAPI_WANT_HasExtState
#define REAPERAPI_WANT_RefreshToolbar2
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_SetMediaTrackInfo_Value
#define REAPERAPI_WANT_SetTrackSendInfo_Value
#define REAPERAPI_WANT_ShowConsoleMsg

// Default include — gives extern declarations of every REAPER API function
// pointer the WANT macros above selected. Exactly one .cpp (ReaperAPI.cpp)
// defines REAPERAPI_IMPLEMENT before this header to allocate storage.
#include "reaper_plugin_functions.h"

namespace totalreaper::reaper {

// Resolve the API entry points we use. Returns false if any required
// function is missing — in that case the extension should refuse to load.
bool initFunctionPointers(void* (*getFunc)(const char* name));

} // namespace totalreaper::reaper
