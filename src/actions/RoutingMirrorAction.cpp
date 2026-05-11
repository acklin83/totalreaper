// RoutingMirrorAction.cpp — Action: "TotalReaper: Toggle Routing Mirror"
//
// Master enable/disable for the track-fader-to-TotalMix mirror behaviour.
// Off by default; flipping it on causes track volume / monitor changes to
// drive the corresponding TotalMix input fader. Flipping it off freezes
// TotalMix in place — we stop sending updates but don't reset what's been
// written.

#include "Actions.h"
#include "../reaper/Console.h"

namespace totalreaper::actions {

namespace {
csurf::TotalReaperCSurf* s_surf = nullptr;
int s_toggleId = 0;
} // namespace

void setCsurf(csurf::TotalReaperCSurf* surf) { s_surf = surf; }
csurf::TotalReaperCSurf* csurfInstance() { return s_surf; }

int& routingMirrorCommandId() { return s_toggleId; }

bool runToggleRoutingMirror(int command) {
    if (command != s_toggleId || s_surf == nullptr) {
        return false;
    }

    const bool nowEnabled = !s_surf->isEnabled();
    s_surf->setEnabled(nowEnabled);
    reaper::debugLog(nowEnabled
                     ? "[TotalReaper] Routing mirror ENABLED — REAPER track faders now drive TotalMix"
                     : "[TotalReaper] Routing mirror DISABLED — TotalMix retains current state");
    return true;
}

int toggleActionState(int command) {
    if (command == s_toggleId && s_surf != nullptr) {
        return s_surf->isEnabled() ? 1 : 0;
    }
    if (command == dumpOscCommandId()) {
        return isDumpToConsoleActive() ? 1 : 0;
    }
    if (command == toggleTalkbackCommandId()) {
        return isTalkbackOn() ? 1 : 0;
    }
    if (command == twoWayCommandId()) {
        return isTwoWayEnabled() ? 1 : 0;
    }
    if (command == autoTalkbackCommandId()) {
        return isAutoTalkbackEnabled() ? 1 : 0;
    }
    return -1;
}

} // namespace totalreaper::actions
