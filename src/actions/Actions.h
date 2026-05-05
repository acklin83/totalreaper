// Actions.h — Shared state and registration helpers for TotalReaper actions.
//
// The two MVP actions (DumpOscAction, TestSendAction) both operate on the
// same OSC client/server pair. Rather than passing them around, the actions
// pull from this shared accessor that main.cpp owns.

#pragma once

#include "../csurf/TotalReaperCSurf.h"
#include "../osc/OscClient.h"
#include "../osc/OscServer.h"

namespace totalreaper::actions {

// Set once during plugin init, used by all actions.
void setOscClient(osc::Client* client);
void setOscServer(osc::Server* server);
void setCsurf(csurf::TotalReaperCSurf* surf);

osc::Client* oscClient();
osc::Server* oscServer();
csurf::TotalReaperCSurf* csurfInstance();

// Action handlers — return true if the command was handled.
bool runDumpOsc(int command);
bool runTestSend(int command);
bool runToggleRoutingMirror(int command);

// Toggle-state callback for "toggleaction" registration. Returns 1 if the
// command's feature is currently on, 0 if off, -1 if the command isn't ours
// or has no toggle state.
int toggleActionState(int command);

// Command IDs are assigned dynamically by REAPER and stored after
// registration. Actions check incoming command IDs against these.
int& dumpOscCommandId();
int& testSendCommandId();
int& routingMirrorCommandId();

} // namespace totalreaper::actions
