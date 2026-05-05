// ReaperAPI.h — Function pointer table for the REAPER C API.
//
// REAPER extensions receive a `reaper_plugin_info_t` struct on load with a
// `GetFunc` callback. We use it to resolve only the API functions
// TotalReaper actually calls. As we touch more of the API, add entries here
// rather than scattering GetFunc lookups throughout the codebase.

#pragma once

// Default include — gives extern declarations of every REAPER API function
// pointer. Exactly one .cpp (ReaperAPI.cpp) defines REAPERAPI_IMPLEMENT
// before this header to allocate storage for those pointers.
#include "reaper_plugin_functions.h"

namespace totalreaper::reaper {

// Resolve the API entry points we use. Returns false if any required
// function is missing — in that case the extension should refuse to load.
bool initFunctionPointers(void* (*getFunc)(const char* name));

} // namespace totalreaper::reaper
