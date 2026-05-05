// TestSendAction.cpp — Action: "TotalReaper: Send Test Mute Input 1"
//
// Sends a single OSC message — `/input/0/mute 1.0f` — to TotalMix on the
// default Global OSC port. TotalMix OSC indexes are 0-based, so /input/0
// addresses what the UI labels as "Analog 1". Toggle off by running the
// action a second time (state alternates).

#include "Actions.h"
#include "../osc/OscMessage.h"
#include "../reaper/Console.h"

namespace totalreaper::actions {

namespace {
bool s_input1Muted = false;
constexpr std::uint16_t kSendPort = 7001; // TotalMix default RX port
} // namespace

bool runTestSend(int command) {
    if (command != testSendCommandId() || oscClient() == nullptr) {
        return false;
    }

    // Lazy connect on first invocation
    if (!oscClient()->isConnected()) {
        if (!oscClient()->connect("127.0.0.1", kSendPort)) {
            reaper::log("[TotalReaper] Could not open OSC client to TotalMix");
            return true;
        }
    }

    s_input1Muted = !s_input1Muted;
    osc::Message message("/input/0/mute");
    message.addFloat(s_input1Muted ? 1.0f : 0.0f);

    if (oscClient()->send(message)) {
        reaper::log(std::string("[TX] ") + message.toString());
    } else {
        reaper::log("[TotalReaper] OSC send FAILED — is TotalMix Global OSC enabled?");
    }
    return true;
}

} // namespace totalreaper::actions
