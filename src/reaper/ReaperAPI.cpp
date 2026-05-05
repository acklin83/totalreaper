// ReaperAPI.cpp — Resolve REAPER API function pointers via GetFunc.
//
// The REAPER SDK header `reaper_plugin_functions.h` declares a long list of
// function pointers as extern. Including it here with REAPERAPI_IMPLEMENT
// causes the storage to be allocated, and `REAPERAPI_LoadAPI` (also defined
// by the header) walks the list calling GetFunc for each.

#define REAPERAPI_IMPLEMENT
#include "ReaperAPI.h"

namespace totalreaper::reaper {

bool initFunctionPointers(void* (*getFunc)(const char* name)) {
    if (getFunc == nullptr) {
        return false;
    }
    // Returns the count of REQUIRED functions that failed to resolve.
    // 0 means everything we marked as needed was found.
    const int missing = REAPERAPI_LoadAPI(getFunc);
    return missing == 0;
}

} // namespace totalreaper::reaper
