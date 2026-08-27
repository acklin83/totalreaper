// SettingsWindow.cpp — ReaImGui-backed settings panel for TotalReaper.
//
// This translation unit OWNS the ReaImGui API binding: it defines
// REAIMGUIAPI_IMPLEMENT so the function-pointer storage lives here, and
// ImGui::init() (called from initImGui) resolves them via REAPER's GetFunc.
// All ImGui::* calls therefore happen in this file.

#define REAIMGUIAPI_IMPLEMENT
#include "reaper_imgui_functions.h"

#include "SettingsWindow.h"

#include "../actions/Actions.h"
#include "../csurf/TotalReaperCSurf.h"
#include "../osc/OscClient.h"
#include "../osc/OscServer.h"
#include "../reaper/Console.h" // pulls ReaperAPI.h → SetExtState/GetExtState/…

#include <cstdint>
#include <cstdlib>
#include <string>

namespace totalreaper::ui {

namespace {

ImGui_Context* g_ctx = nullptr;
bool g_open = false;
bool g_available = false;
int  g_cmdId = 0;
void* (*g_getFunc)(const char*) = nullptr;

osc::Client* g_client = nullptr;
osc::Server* g_server = nullptr;
csurf::TotalReaperCSurf* g_surf = nullptr;

// Editable copies of the port settings. Seeded from ExtState in setContext()
// and only written back to ExtState (+ applied live) when the user hits Apply.
int g_sendPort = 7001;       // we send to TotalMix here (TotalMix's RX port)
int g_listenPort = 7002;     // edited value for the listen port
int g_activeListenPort = 7002; // last port the server successfully bound
std::string g_status;

int readExtInt(const char* key, int def) {
    if (!HasExtState("TotalReaper", key)) return def;
    const char* v = GetExtState("TotalReaper", key);
    if (v == nullptr || v[0] == '\0') return def;
    const int n = std::atoi(v);
    return (n >= 1 && n <= 65535) ? n : def;
}

void applyPorts() {
    if (g_sendPort < 1 || g_sendPort > 65535 ||
        g_listenPort < 1 || g_listenPort > 65535) {
        g_status = "Ports must be between 1 and 65535.";
        return;
    }
    // Send side: reconnect the shared client and tell the control surface its
    // new TX port (it reconnects lazily on the next flush using that port).
    if (g_client != nullptr) {
        g_client->connect("127.0.0.1", static_cast<std::uint16_t>(g_sendPort));
    }
    if (g_surf != nullptr) {
        g_surf->setTxPort(static_cast<std::uint16_t>(g_sendPort));
    }
    // Listen side: rebind the receive server. stop() joins the rx thread, so
    // this must run on the main thread (it does — driven from the timer).
    bool ok = true;
    if (g_server != nullptr) {
        g_server->stop();
        ok = g_server->start(static_cast<std::uint16_t>(g_listenPort),
                             &totalreaper::actions::rxHandler);
        if (!ok) {
            // Bind failed (port in use?). Don't leave the user without a
            // listener — fall back to the previously-working port.
            g_server->start(static_cast<std::uint16_t>(g_activeListenPort),
                            &totalreaper::actions::rxHandler);
        }
    }
    if (ok) {
        g_activeListenPort = g_listenPort;
        SetExtState("TotalReaper", "OscSendPort",
                    std::to_string(g_sendPort).c_str(), true);
        SetExtState("TotalReaper", "OscListenPort",
                    std::to_string(g_listenPort).c_str(), true);
        g_status = "Applied — send " + std::to_string(g_sendPort) +
                   ", listen " + std::to_string(g_listenPort) + ".";
    } else {
        g_listenPort = g_activeListenPort; // reflect the port we fell back to
        g_status = "Could not bind listen port — kept " +
                   std::to_string(g_activeListenPort) + ".";
    }
}

void drawContents() {
    ImGui::Text(g_ctx, "OSC Ports");
    ImGui::Separator(g_ctx);
    ImGui::InputInt(g_ctx, "Send to TotalMix", &g_sendPort);
    ImGui::InputInt(g_ctx, "Listen / receive", &g_listenPort);
    if (ImGui::Button(g_ctx, "Apply ports")) {
        applyPorts();
    }
    if (!g_status.empty()) {
        ImGui::TextWrapped(g_ctx, g_status.c_str());
    }

    ImGui::Separator(g_ctx);
    ImGui::Text(g_ctx, "Behavior");

    if (g_surf != nullptr) {
        // After changing state from here we must tell REAPER to re-query the
        // matching action's toggle state, otherwise the Action-List checkmark
        // and toolbar icons keep showing the old state (REAPER only polls
        // toggleaction when prompted). RefreshToolbar2(section 0, command id).
        bool mirror = g_surf->isEnabled();
        if (ImGui::Checkbox(g_ctx, "Routing Mirror (REAPER -> TotalMix)", &mirror)) {
            g_surf->setEnabled(mirror);
            RefreshToolbar2(0, actions::routingMirrorCommandId());
        }
        bool onlyMain = g_surf->isOnlyMainSubmix();
        if (ImGui::Checkbox(g_ctx, "Only set the main submix", &onlyMain)) {
            g_surf->setOnlyMainSubmix(onlyMain);
        }
        ImGui::TextWrapped(g_ctx,
            "The mirror writes the master track's hardware bus and nothing "
            "else. Phones and cue submixes keep whatever you built in "
            "TotalMix, including when you switch the mirror off.");

        bool twoWay = g_surf->isTwoWayEnabled();
        if (ImGui::Checkbox(g_ctx, "2-Way Control (TotalMix -> REAPER)", &twoWay)) {
            g_surf->setTwoWayEnabled(twoWay);
            RefreshToolbar2(0, actions::twoWayCommandId());
        }
        bool autoTb = g_surf->isAutoTalkbackEnabled();
        if (ImGui::Checkbox(g_ctx, "Auto-Talkback on Stop", &autoTb)) {
            g_surf->setAutoTalkbackEnabled(autoTb);
            RefreshToolbar2(0, actions::autoTalkbackCommandId());
        }
        bool stereo = g_surf->isStereoPairLink();
        if (ImGui::Checkbox(g_ctx, "Link REAPER stereo inputs as TotalMix pairs", &stereo)) {
            g_surf->setStereoPairLink(stereo);
        }
        ImGui::TextWrapped(g_ctx,
            "Stereo link is one-way: turning it off does not unlink existing "
            "pairs in TotalMix.");
    }
}

// Resolve ReaImGui on demand. Safe to call repeatedly: returns true once
// resolved, and re-attempts while unresolved (so it recovers if ReaImGui was
// installed/loaded after us). Must run after plugin-load — call from the
// action handler, never from ReaperPluginEntry.
bool ensureImGui() {
    if (g_available) return true;
    if (g_getFunc == nullptr) return false;
    try {
        ImGui::init(g_getFunc);
        g_available = true;
    } catch (...) {
        g_available = false; // ReaImGui genuinely absent / too old
    }
    return g_available;
}

} // namespace

void setGetFunc(void* (*getFunc)(const char*)) { g_getFunc = getFunc; }

bool imguiAvailable() { return g_available; }

void setContext(osc::Client* client, osc::Server* server,
                csurf::TotalReaperCSurf* surf) {
    g_client = client;
    g_server = server;
    g_surf = surf;
    g_sendPort = readExtInt("OscSendPort", 7001);
    g_listenPort = readExtInt("OscListenPort", 7002);
    g_activeListenPort = g_listenPort; // matches the port main.cpp bound at load
}

int& openSettingsCommandId() { return g_cmdId; }

bool runOpenSettings(int command) {
    if (command != g_cmdId) return false;
    if (!ensureImGui()) {
        reaper::log("[TotalReaper] Settings window needs ReaImGui. Install it "
                    "via Extensions -> ReaPack -> Browse packages (search "
                    "\"ReaImGui\"), then restart REAPER.");
        return true;
    }
    g_open = !g_open;
    return true;
}

bool isSettingsWindowOpen() { return g_open; }

void tick() {
    if (!g_open) {
        // Stop referencing the context so ReaImGui garbage-collects it; a
        // fresh one is created next time the window opens.
        g_ctx = nullptr;
        return;
    }
    if (!g_available) { g_open = false; return; }

    try {
        if (g_ctx == nullptr) {
            g_ctx = ImGui::CreateContext("TotalReaper Settings");
        }
        ImGui::SetNextWindowSize(g_ctx, 480, 360, ImGui::Cond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin(g_ctx, "TotalReaper Settings", &open)) {
            drawContents();
            ImGui::End(g_ctx); // ReaImGui: End only when Begin returned true
        }
        if (!open) { g_open = false; g_ctx = nullptr; }
    } catch (const ImGui_Error& e) {
        // Context invalidated (e.g. REAPER GC'd it after a hiccup). Reset and
        // let the user reopen cleanly.
        reaper::debugLog(std::string("[TotalReaper] ImGui error: ") + e.what());
        g_ctx = nullptr;
        g_open = false;
    }
}

void shutdown() {
    g_ctx = nullptr;
    g_open = false;
}

} // namespace totalreaper::ui
