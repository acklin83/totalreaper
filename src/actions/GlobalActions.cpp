// GlobalActions.cpp — REAPER actions for global TotalMix state (not
// per-track). Talkback toggle and snapshot save/load.
//
// Talkback state lives in REAPER's global ExtState ("TotalReaper" namespace),
// because TotalMix's talkback is one global flag — no per-track meaning.
//
// Snapshots map a TotalMix snapshot slot to "the state of the studio for
// this REAPER project". By convention slot 8 is reserved for TotalReaper —
// users keep slots 1-7 for their own. Save action writes slot 8 from the
// current TotalMix state; load action reads slot 8 back. The slot number
// itself is hardcoded for now; if the convention bites, we can promote it
// to a Project ExtState later without breaking existing flows.

#include "Actions.h"

#include "../osc/OscMessage.h"

#include "reaper_plugin_functions.h"

#include <cstdio>

namespace totalreaper::actions {

namespace {

constexpr const char* kExtNamespace = "TotalReaper";
constexpr const char* kExtTalkback  = "TalkbackOn";

constexpr int kProjectSnapshotSlot = 8;

int s_tTalkback = 0;
int s_snapSave = 0;
int s_snapLoad = 0;

// Returns true if the routing mirror is active and we have a working OSC
// path to TotalMix. Global actions are no-ops without a live mirror because
// they assume the user has consciously wired TotalReaper to TotalMix.
bool oscReady() {
    if (csurfInstance() == nullptr || !csurfInstance()->isEnabled()) return false;
    return oscClient() != nullptr;
}

void runToggleTalkback() {
    if (!oscReady()) return;

    bool currentlyOn = false;
    if (HasExtState(kExtNamespace, kExtTalkback)) {
        const char* v = GetExtState(kExtNamespace, kExtTalkback);
        currentlyOn = (v != nullptr && v[0] == '1');
    }
    const bool newOn = !currentlyOn;

    osc::Message m("/controlroom/talkback");
    m.addFloat(newOn ? 1.0f : 0.0f);
    oscClient()->send(m);

    SetExtState(kExtNamespace, kExtTalkback,
                newOn ? "1" : "0", /*persist*/ true);
}

void runSnapshotSave() {
    if (!oscReady()) return;
    char path[48];
    std::snprintf(path, sizeof(path), "/snapshot/save/%d",
                  kProjectSnapshotSlot);
    osc::Message m(path);
    m.addFloat(1.0f);
    oscClient()->send(m);
}

void runSnapshotLoad() {
    if (!oscReady()) return;
    char path[48];
    std::snprintf(path, sizeof(path), "/snapshot/load/%d",
                  kProjectSnapshotSlot);
    osc::Message m(path);
    m.addFloat(1.0f);
    oscClient()->send(m);
}

} // namespace

int& toggleTalkbackCommandId() { return s_tTalkback; }
int& snapshotSaveCommandId()   { return s_snapSave; }
int& snapshotLoadCommandId()   { return s_snapLoad; }

bool isTalkbackOn() {
    if (!HasExtState(kExtNamespace, kExtTalkback)) return false;
    const char* v = GetExtState(kExtNamespace, kExtTalkback);
    return v != nullptr && v[0] == '1';
}

bool runGlobalAction(int command) {
    if (command == s_tTalkback) { runToggleTalkback(); return true; }
    if (command == s_snapSave)  { runSnapshotSave();   return true; }
    if (command == s_snapLoad)  { runSnapshotLoad();   return true; }
    return false;
}

} // namespace totalreaper::actions
