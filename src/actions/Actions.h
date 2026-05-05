// Actions.h — Shared state and registration helpers for TotalReaper actions.
//
// The two MVP actions (DumpOscAction, TestSendAction) both operate on the
// same OSC client/server pair. Rather than passing them around, the actions
// pull from this shared accessor that main.cpp owns.

#pragma once

#include "../osc/OscClient.h"
#include "../osc/OscServer.h"

namespace totalreaper::actions {

// Set once during plugin init, used by all actions.
void setOscClient(osc::Client* client);
void setOscServer(osc::Server* server);

osc::Client* oscClient();
osc::Server* oscServer();

// Action handlers — return true if the command was handled.
bool runDumpOsc(int command);
bool runTestSend(int command);

// Command IDs are assigned dynamically by REAPER and stored after
// registration. Actions check incoming command IDs against these.
int& dumpOscCommandId();
int& testSendCommandId();

} // namespace totalreaper::actions
