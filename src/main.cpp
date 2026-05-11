// main.cpp — TotalReaper REAPER extension entry point.
//
// REAPER loads this plugin by calling ReaperPluginEntry with rec->caller_version
// matching its own. We:
//   1) Resolve API function pointers
//   2) Register every TotalReaper action (Toggle OSC Dump, Routing Mirror,
//      2-Way Control, preamp/global commands, snapshot save/load slots 1-8)
//   3) Hook the action dispatcher so REAPER calls back when the user runs them
//   4) Stand up the OSC client/server objects
//
// On unload, tear everything down cleanly.

#include "actions/Actions.h"
#include "csurf/TotalReaperCSurf.h"
#include "osc/OscClient.h"
#include "osc/OscServer.h"
#include "osc/TotalMixState.h"
#include "reaper/ChannelMap.h"
#include "reaper/Console.h"
#include "reaper/ReaperAPI.h"

#include "reaper_plugin.h"

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

constexpr std::uint16_t kTotalMixRxPort = 7001;
constexpr std::uint16_t kTotalReaperListenPort = 7002;

std::unique_ptr<totalreaper::osc::Client> g_client;
std::unique_ptr<totalreaper::osc::Server> g_server;
std::unique_ptr<totalreaper::osc::TotalMixState> g_state;
std::unique_ptr<totalreaper::csurf::TotalReaperCSurf> g_csurf;

// hookcommand2 is required for actions registered via "custom_action" (per
// REAPER SDK reaper_plugin.h). Old "hookcommand" only fires for built-in /
// gaccel actions and never sees our custom command IDs.
bool onAction2(KbdSectionInfo* /*sec*/, int command, int /*val*/, int /*val2*/,
               int /*relmode*/, HWND /*hwnd*/) {
    if (totalreaper::actions::runDumpOsc(command)) return true;
    if (totalreaper::actions::runToggleRoutingMirror(command)) return true;
    if (totalreaper::actions::runToggleTwoWay(command)) return true;
    if (totalreaper::actions::runToggleAutoTalkback(command)) return true;
    if (totalreaper::actions::runPreampAction(command)) return true;
    if (totalreaper::actions::runGlobalAction(command)) return true;
    return false;
}

// Toggle-state callback — gives REAPER the current on/off state for actions
// that have one, so the Action List shows a checkmark.
int onToggleAction(int command) {
    return totalreaper::actions::toggleActionState(command);
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
        if (g_csurf) {
            // REAPER doesn't expose an unregister-by-instance API; the cleanest
            // we can do on unload is destroy the object. REAPER stops calling
            // into it once the dylib is unloaded.
            g_csurf.reset();
        }
        if (g_server) g_server->stop();
        g_server.reset();
        g_client.reset();
        g_state.reset();
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) {
        return 0;
    }

    if (!totalreaper::reaper::initFunctionPointers(rec->GetFunc)) {
        return 0;
    }

    // Load channel mapping table from reaper.ini so we can translate REAPER
    // input slot indices to device hardware indices (which is what TotalMix
    // addresses in /mix/in/<n>/<bus>/...).
    totalreaper::reaper::loadChannelMap();

    // Stand up OSC objects. Connect TX eagerly so the control surface can
    // start sending updates immediately. The receive server runs from now
    // on, populating the TotalMix state cache that relative actions (like
    // preamp gain delta) depend on for an accurate baseline.
    g_client = std::make_unique<totalreaper::osc::Client>();
    g_client->connect("127.0.0.1", kTotalMixRxPort);
    g_server = std::make_unique<totalreaper::osc::Server>();
    g_state = std::make_unique<totalreaper::osc::TotalMixState>();
    totalreaper::actions::setOscClient(g_client.get());
    totalreaper::actions::setOscServer(g_server.get());
    totalreaper::actions::setTotalMixState(g_state.get());
    g_server->start(kTotalReaperListenPort, &totalreaper::actions::rxHandler);

    // Install the control surface that mirrors REAPER track state to TotalMix.
    g_csurf = std::make_unique<totalreaper::csurf::TotalReaperCSurf>(
        g_client.get(), kTotalMixRxPort);
    rec->Register("csurf_inst", g_csurf.get());
    totalreaper::actions::setCsurf(g_csurf.get());

    // Register actions
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_OSC_DUMP",
                   "TotalReaper: Toggle OSC Dump",
                   totalreaper::actions::dumpOscCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_ROUTING_MIRROR",
                   "TotalReaper: Toggle Routing Mirror",
                   totalreaper::actions::routingMirrorCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_TWO_WAY",
                   "TotalReaper: Toggle 2-Way Control",
                   totalreaper::actions::twoWayCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_AUTO_TALKBACK",
                   "TotalReaper: Toggle Auto-Talkback on Stop",
                   totalreaper::actions::autoTalkbackCommandId());

    registerAction(rec,
                   "TOTALREAPER_GAIN_INC",
                   "TotalReaper: Increase preamp gain on selected tracks (+1 dB)",
                   totalreaper::actions::gainIncCommandId());
    registerAction(rec,
                   "TOTALREAPER_GAIN_DEC",
                   "TotalReaper: Decrease preamp gain on selected tracks (-1 dB)",
                   totalreaper::actions::gainDecCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_48V",
                   "TotalReaper: Toggle 48V phantom on selected tracks",
                   totalreaper::actions::toggle48vCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_PAD",
                   "TotalReaper: Toggle pad on selected tracks",
                   totalreaper::actions::togglePadCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_PHASE",
                   "TotalReaper: Toggle phase invert on selected tracks",
                   totalreaper::actions::togglePhaseCommandId());
    registerAction(rec,
                   "TOTALREAPER_TOGGLE_AUTOLEVEL",
                   "TotalReaper: Toggle AutoLevel on selected tracks",
                   totalreaper::actions::toggleAutolevelCommandId());

    registerAction(rec,
                   "TOTALREAPER_TOGGLE_TALKBACK",
                   "TotalReaper: Toggle Talkback",
                   totalreaper::actions::toggleTalkbackCommandId());

    // Snapshot save/load — one action per TotalMix snapshot slot (1..8).
    // Static buffers because registerAction stores the id-string pointer
    // for the lifetime of the plugin.
    static char snapSaveIds[8][32];
    static char snapSaveNames[8][64];
    static char snapLoadIds[8][32];
    static char snapLoadNames[8][64];
    for (int slot = 1; slot <= 8; ++slot) {
        std::snprintf(snapSaveIds[slot - 1], sizeof(snapSaveIds[slot - 1]),
                      "TOTALREAPER_SNAPSHOT_SAVE_%d", slot);
        std::snprintf(snapSaveNames[slot - 1], sizeof(snapSaveNames[slot - 1]),
                      "TotalReaper: Save TotalMix Snapshot Slot %d", slot);
        registerAction(rec,
                       snapSaveIds[slot - 1],
                       snapSaveNames[slot - 1],
                       totalreaper::actions::snapshotSaveCommandId(slot));

        std::snprintf(snapLoadIds[slot - 1], sizeof(snapLoadIds[slot - 1]),
                      "TOTALREAPER_SNAPSHOT_LOAD_%d", slot);
        std::snprintf(snapLoadNames[slot - 1], sizeof(snapLoadNames[slot - 1]),
                      "TotalReaper: Load TotalMix Snapshot Slot %d", slot);
        registerAction(rec,
                       snapLoadIds[slot - 1],
                       snapLoadNames[slot - 1],
                       totalreaper::actions::snapshotLoadCommandId(slot));
    }

    // hookcommand2 (not hookcommand) — required for custom_action IDs.
    rec->Register("hookcommand2", reinterpret_cast<void*>(onAction2));
    rec->Register("toggleaction", reinterpret_cast<void*>(onToggleAction));

    // Restore the routing mirror's persisted on/off state so the user's
    // last choice survives REAPER restarts.
    if (HasExtState("TotalReaper", "RoutingMirrorEnabled")) {
        const char* v = GetExtState("TotalReaper", "RoutingMirrorEnabled");
        if (v && v[0] == '1') {
            g_csurf->setEnabled(true);
        }
    }
    if (HasExtState("TotalReaper", "TwoWayEnabled")) {
        const char* v = GetExtState("TotalReaper", "TwoWayEnabled");
        if (v && v[0] == '1') {
            g_csurf->setTwoWayEnabled(true);
        }
    }
    if (HasExtState("TotalReaper", "AutoTalkbackEnabled")) {
        const char* v = GetExtState("TotalReaper", "AutoTalkbackEnabled");
        if (v && v[0] == '1') {
            g_csurf->setAutoTalkbackEnabled(true);
        }
    }

    totalreaper::reaper::debugLog("[TotalReaper] loaded — "
                                  "find actions in Action List by typing 'TotalReaper'");
    return 1;
}

} // extern "C"
