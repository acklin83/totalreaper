// TwoWayControlAction.cpp — Action: "TotalReaper: Toggle 2-Way Control"
//
// On top of the routing mirror (REAPER → TotalMix), this toggle additionally
// enables TotalMix → REAPER. With both on, moving an input fader / balpan in
// TotalMix updates the corresponding REAPER track volume / pan, and moving a
// per-bus send fader in TotalMix scales the matching REAPER send.
//
// Echo-suppression and the actual rx → REAPER mapping live in
// TotalReaperCSurf — this file is just the toggle plumbing. The control
// surface gates its rx logic on both enabled_ (routing mirror) and
// twoWayEnabled_, so toggling 2-way without the routing mirror is harmless
// (and silently inert until the mirror is engaged).

#include "Actions.h"
#include "../reaper/Console.h"

namespace totalreaper::actions {

namespace {
int s_twoWayId = 0;
} // namespace

int& twoWayCommandId() { return s_twoWayId; }

bool isTwoWayEnabled() {
    return csurfInstance() != nullptr && csurfInstance()->isTwoWayEnabled();
}

bool runToggleTwoWay(int command) {
    if (command != s_twoWayId || csurfInstance() == nullptr) {
        return false;
    }
    const bool nowOn = !csurfInstance()->isTwoWayEnabled();
    csurfInstance()->setTwoWayEnabled(nowOn);
    reaper::debugLog(nowOn
        ? "[TotalReaper] 2-Way Control ENABLED — TotalMix faders now also drive REAPER"
        : "[TotalReaper] 2-Way Control disabled — back to one-way REAPER → TotalMix");
    return true;
}

} // namespace totalreaper::actions
