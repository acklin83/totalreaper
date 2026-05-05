// Console.h — Logging helper that writes to REAPER's console window.
//
// `ShowConsoleMsg` is the simplest cross-platform output channel for an
// extension. Wrap it so call sites can pass std::string and we centralize
// any future formatting / file logging.

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

} // namespace totalreaper::reaper
