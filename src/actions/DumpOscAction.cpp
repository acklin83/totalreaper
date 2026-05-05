// DumpOscAction.cpp — Action: "TotalReaper: Toggle OSC Dump"
//
// Toggles whether incoming TotalMix OSC messages are logged to the REAPER
// console. The receive server itself runs unconditionally from plugin load
// so we can keep a state cache populated for relative actions like preamp
// gain delta — this action is just a console firehose for protocol
// exploration.

#include "Actions.h"
#include "../reaper/Console.h"

#include <atomic>

namespace totalreaper::actions {

namespace {
osc::Client* s_client = nullptr;
osc::Server* s_server = nullptr;
osc::TotalMixState* s_state = nullptr;
int s_dumpId = 0;
int s_testId = 0;
std::atomic<bool> s_dumpToConsole{false};
} // namespace

void setOscClient(osc::Client* client) { s_client = client; }
void setOscServer(osc::Server* server) { s_server = server; }
void setTotalMixState(osc::TotalMixState* state) { s_state = state; }
osc::Client* oscClient() { return s_client; }
osc::Server* oscServer() { return s_server; }
osc::TotalMixState* totalMixState() { return s_state; }

int& dumpOscCommandId() { return s_dumpId; }
int& testSendCommandId() { return s_testId; }

// Single handler installed by main.cpp on the always-running OSC server.
// Always feeds the state cache; logs to console only when the dump action
// is currently toggled on.
void rxHandler(const osc::Message& m) {
    if (s_state) s_state->onMessage(m);
    if (s_dumpToConsole.load(std::memory_order_relaxed)) {
        reaper::log("[RX] " + m.toString());
    }
}

bool isDumpToConsoleActive() { return s_dumpToConsole.load(); }

bool runDumpOsc(int command) {
    if (command != s_dumpId) return false;
    const bool nowOn = !s_dumpToConsole.load();
    s_dumpToConsole.store(nowOn);
    reaper::log(nowOn
                ? "[TotalReaper] OSC dump → console ENABLED"
                : "[TotalReaper] OSC dump → console disabled");
    return true;
}

} // namespace totalreaper::actions
