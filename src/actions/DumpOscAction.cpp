// DumpOscAction.cpp — Action: "TotalReaper: Toggle OSC Dump"
//
// First call: starts the OSC server on the configured port and logs every
// incoming message to the REAPER console.
// Second call: stops the server.
//
// This is the primary tool for Phase 0 protocol exploration — toggle on,
// click around in TotalMix, watch the console fill with the OSC paths the
// hardware is using.

#include "Actions.h"
#include "../reaper/Console.h"

namespace totalreaper::actions {

namespace {
osc::Client* s_client = nullptr;
osc::Server* s_server = nullptr;
int s_dumpId = 0;
int s_testId = 0;
bool s_dumping = false;
} // namespace

void setOscClient(osc::Client* client) { s_client = client; }
void setOscServer(osc::Server* server) { s_server = server; }
osc::Client* oscClient() { return s_client; }
osc::Server* oscServer() { return s_server; }

int& dumpOscCommandId() { return s_dumpId; }
int& testSendCommandId() { return s_testId; }

bool runDumpOsc(int command) {
    if (command != s_dumpId || s_server == nullptr) {
        return false;
    }

    if (!s_dumping) {
        // Default RX port for TotalMix Global OSC. Configurable later via
        // an extension preferences dialog.
        constexpr std::uint16_t kListenPort = 7002;
        const bool ok = s_server->start(kListenPort, [](const osc::Message& m) {
            // Handler runs on the receive thread — `log()` calls into
            // ShowConsoleMsg which is documented as thread-safe in the
            // REAPER SDK.
            reaper::log("[RX] " + m.toString());
        });
        if (ok) {
            s_dumping = true;
            reaper::log("[TotalReaper] OSC dump started — interact with TotalMix to see paths");
        } else {
            reaper::log("[TotalReaper] OSC dump start FAILED");
        }
    } else {
        s_server->stop();
        s_dumping = false;
        reaper::log("[TotalReaper] OSC dump stopped");
    }
    return true;
}

} // namespace totalreaper::actions
