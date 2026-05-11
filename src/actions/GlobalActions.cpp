// GlobalActions.cpp — REAPER actions for global TotalMix state (not
// per-track). Talkback toggle and per-slot snapshot save/load.
//
// Talkback state lives in REAPER's global ExtState ("TotalReaper" namespace),
// because TotalMix's talkback is one global flag — no per-track meaning.
//
// Snapshot slots 1..8 map directly to TotalMix's 8 snapshot slots. Save fires
// /snapshot/save/<slot>, load fires /snapshot/load/<slot>. Slot numbering is
// 1-based to match TotalMix's UI labelling.

#include "Actions.h"

#include "../osc/OscMessage.h"

#include "reaper_plugin_functions.h"

#include <cstdio>

namespace totalreaper::actions {

namespace {

constexpr const char* kExtNamespace = "TotalReaper";
constexpr const char* kExtTalkback  = "TalkbackOn";

constexpr int kSnapshotSlotCount = 8;

int s_tTalkback = 0;
int s_snapSave[kSnapshotSlotCount] = {};
int s_snapLoad[kSnapshotSlotCount] = {};
int s_invalidId = -1;

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

void runSnapshotSave(int slot1Based) {
    if (!oscReady()) return;
    char path[48];
    std::snprintf(path, sizeof(path), "/snapshot/save/%d", slot1Based);
    osc::Message m(path);
    m.addFloat(1.0f);
    oscClient()->send(m);
}

void runSnapshotLoad(int slot1Based) {
    if (!oscReady()) return;
    char path[48];
    std::snprintf(path, sizeof(path), "/snapshot/load/%d", slot1Based);
    osc::Message m(path);
    m.addFloat(1.0f);
    oscClient()->send(m);
}

} // namespace

int& toggleTalkbackCommandId() { return s_tTalkback; }

int& snapshotSaveCommandId(int slot) {
    if (slot < 1 || slot > kSnapshotSlotCount) return s_invalidId;
    return s_snapSave[slot - 1];
}

int& snapshotLoadCommandId(int slot) {
    if (slot < 1 || slot > kSnapshotSlotCount) return s_invalidId;
    return s_snapLoad[slot - 1];
}

bool isTalkbackOn() {
    if (!HasExtState(kExtNamespace, kExtTalkback)) return false;
    const char* v = GetExtState(kExtNamespace, kExtTalkback);
    return v != nullptr && v[0] == '1';
}

bool runGlobalAction(int command) {
    if (command == 0) return false;
    if (command == s_tTalkback) { runToggleTalkback(); return true; }
    for (int i = 0; i < kSnapshotSlotCount; ++i) {
        if (command == s_snapSave[i]) { runSnapshotSave(i + 1); return true; }
        if (command == s_snapLoad[i]) { runSnapshotLoad(i + 1); return true; }
    }
    return false;
}

} // namespace totalreaper::actions
