// main.cpp — TotalReaper REAPER extension entry point.
//
// REAPER loads this plugin by calling ReaperPluginEntry with rec->caller_version
// matching its own. We:
//   1) Resolve API function pointers
//   2) Register two actions (Toggle OSC Dump, Send Test Mute Input 1)
//   3) Hook the action dispatcher so REAPER calls back when the user runs them
//   4) Stand up the OSC client/server objects
//
// On unload, tear everything down cleanly.

#include "actions/Actions.h"
#include "osc/OscClient.h"
#include "osc/OscServer.h"
#include "reaper/Console.h"
#include "reaper/ReaperAPI.h"

#include "reaper_plugin.h"

#include <memory>
#include <string>

namespace {

std::unique_ptr<totalreaper::osc::Client> g_client;
std::unique_ptr<totalreaper::osc::Server> g_server;

// hookcommand2 is required for actions registered via "custom_action" (per
// REAPER SDK reaper_plugin.h). Old "hookcommand" only fires for built-in /
// gaccel actions and never sees our custom command IDs.
bool onAction2(KbdSectionInfo* /*sec*/, int command, int /*val*/, int /*val2*/,
               int /*relmode*/, HWND /*hwnd*/) {
    if (totalreaper::actions::runDumpOsc(command)) return true;
    if (totalreaper::actions::runTestSend(command)) return true;
    return false;
}

void registerAction(reaper_plugin_info_t* rec,
                    const char* idStr,
                    const char* description,
                    int& outCommandId) {
    custom_action_register_t action{};
    action.uniqueSectionId = 0; // main section
    action.idStr = idStr;
    action.name = description;
    outCommandId = rec->Register("custom_action", &action);
    if (outCommandId == 0) {
        // Fall back to plain command registration (older API path)
        outCommandId = rec->Register("command_id", const_cast<char*>(idStr));
        gaccel_register_t accel{};
        accel.accel.cmd = static_cast<WORD>(outCommandId);
        accel.desc = description;
        rec->Register("gaccel", &accel);
    }
}

} // namespace

extern "C" {

REAPER_PLUGIN_DLL_EXPORT int ReaperPluginEntry(REAPER_PLUGIN_HINSTANCE /*hInstance*/,
                                               reaper_plugin_info_t* rec) {
    if (rec == nullptr) {
        // Plugin is being unloaded
        if (g_server) g_server->stop();
        g_server.reset();
        g_client.reset();
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) {
        return 0;
    }

    if (!totalreaper::reaper::initFunctionPointers(rec->GetFunc)) {
        return 0;
    }

    // Stand up OSC objects (not yet connected)
    g_client = std::make_unique<totalreaper::osc::Client>();
    g_server = std::make_unique<totalreaper::osc::Server>();
    totalreaper::actions::setOscClient(g_client.get());
    totalreaper::actions::setOscServer(g_server.get());

    // Register actions
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_OSC_DUMP",
                   "TotalReaper: Toggle OSC Dump",
                   totalreaper::actions::dumpOscCommandId());
    registerAction(rec,
                   "TOTALREAPER_TEST_SEND",
                   "TotalReaper: Send Test Mute Input 1",
                   totalreaper::actions::testSendCommandId());

    // hookcommand2 (not hookcommand) — required for custom_action IDs.
    rec->Register("hookcommand2", reinterpret_cast<void*>(onAction2));

    totalreaper::reaper::log("[TotalReaper] v0.1.0 loaded — "
                             "find actions in Action List by typing 'TotalReaper'");
    return 1;
}

} // extern "C"
