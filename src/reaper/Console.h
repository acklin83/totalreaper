// Console.h — Logging helpers that write to REAPER's console window.
//
// `ShowConsoleMsg` is the simplest cross-platform output channel for an
// extension. Two entry points:
//   log()      — always prints. Use for errors and direct user-action
//                feedback (toggle confirmations, test sends).
//   debugLog() — compiles to a no-op when NDEBUG is defined (Release
//                builds). Use for startup chatter, traces, and anything
//                the user did not explicitly ask to see.

#pragma once

#include "ReaperAPI.h"

#include <string>

namespace totalreaper::reaper {

inline void log(const std::string& message) {
    if (ShowConsoleMsg != nullptr) {
        ShowConsoleMsg((message + "\n").c_str());
    }
}

inline void log(const char* message) {
    if (ShowConsoleMsg != nullptr && message != nullptr) {
        std::string s = message;
        s += '\n';
        ShowConsoleMsg(s.c_str());
    }
}

inline void debugLog(const std::string& message) {
#ifndef NDEBUG
    log(message);
#else
    (void)message;
#endif
}

inline void debugLog(const char* message) {
#ifndef NDEBUG
    log(message);
#else
    (void)message;
#endif
}

} // namespace totalreaper::reaper
