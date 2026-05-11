// AutoTalkbackAction.cpp — Action: "TotalReaper: Toggle Auto-Talkback on Stop"
//
// Drives TotalMix talkback from REAPER's transport state. With this on:
//   - Transport stops or pauses → talkback opens (engineer can talk to room).
//   - Transport starts (play / record) → talkback closes.
//
// The actual work happens in TotalReaperCSurf::SetPlayState, which REAPER
// calls on every transport-state change. This file is just the toggle.

#include "Actions.h"
#include "../reaper/Console.h"

namespace totalreaper::actions {

namespace {
int s_autoTalkbackId = 0;
} // namespace

int& autoTalkbackCommandId() { return s_autoTalkbackId; }

bool isAutoTalkbackEnabled() {
    return csurfInstance() != nullptr && csurfInstance()->isAutoTalkbackEnabled();
}

bool runToggleAutoTalkback(int command) {
    if (command != s_autoTalkbackId || csurfInstance() == nullptr) {
        return false;
    }
    const bool nowOn = !csurfInstance()->isAutoTalkbackEnabled();
    csurfInstance()->setAutoTalkbackEnabled(nowOn);
    reaper::debugLog(nowOn
        ? "[TotalReaper] Auto-Talkback ENABLED — transport stop opens talkback"
        : "[TotalReaper] Auto-Talkback disabled");
    return true;
}

} // namespace totalreaper::actions
