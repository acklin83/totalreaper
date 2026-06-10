// SettingsWindow.h — TotalReaper settings UI (ReaImGui).
//
// A small, optional GUI for things that previously had no front-end:
//   - OSC ports (send-to-TotalMix / listen) — applied live, no restart.
//   - Stereo-pair link: ask TotalMix to link the two hardware channels of a
//     REAPER stereo input into a stereo strip.
//   - 2-Way Control / Routing Mirror / Auto-Talkback toggles, mirrored from
//     and to the same state the Action-List toggles use (checkmarks follow).
//
// ReaImGui is a RUNTIME dependency the user installs via ReaPack. It is not
// bundled with REAPER. initImGui() degrades gracefully when it's absent: the
// window simply becomes unavailable and everything else keeps working.

#pragma once

namespace totalreaper::osc { class Client; class Server; }
namespace totalreaper::csurf { class TotalReaperCSurf; }

namespace totalreaper::ui {

// Stash REAPER's GetFunc for LAZY ReaImGui resolution. ReaImGui's API is not
// reliably registered yet at plugin-load time (inter-extension load order is
// not guaranteed), so we must NOT resolve it in ReaperPluginEntry — we resolve
// on first window open instead, by which point every extension is loaded.
void setGetFunc(void* (*getFunc)(const char*));

// True once ReaImGui has been resolved successfully.
bool imguiAvailable();

// Wire the objects the window reads and controls. Called once at plugin init.
void setContext(osc::Client* client, osc::Server* server,
                csurf::TotalReaperCSurf* surf);

// Action plumbing — mirrors the other TotalReaper actions.
int& openSettingsCommandId();
bool runOpenSettings(int command);

// Toggle-state for the "toggleaction" callback (Action-List checkmark).
bool isSettingsWindowOpen();

// Pumped from a registered REAPER timer (~30 Hz, main thread). Draws one frame
// while the window is open; cheap no-op when closed.
void tick();

// Release the ImGui context on plugin unload.
void shutdown();

} // namespace totalreaper::ui
