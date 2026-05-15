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
#include <cstdio>
#include <cstring>

namespace totalreaper::actions {

namespace {
osc::Client* s_client = nullptr;
osc::Server* s_server = nullptr;
osc::TotalMixState* s_state = nullptr;
int s_dumpId = 0;
std::atomic<bool> s_dumpToConsole{false};
} // namespace

void setOscClient(osc::Client* client) { s_client = client; }
void setOscServer(osc::Server* server) { s_server = server; }
void setTotalMixState(osc::TotalMixState* state) { s_state = state; }
osc::Client* oscClient() { return s_client; }
osc::Server* oscServer() { return s_server; }
osc::TotalMixState* totalMixState() { return s_state; }

int& dumpOscCommandId() { return s_dumpId; }

namespace {

// Parse "/mix/in/<hwIdx>/<bus>/<leaf>" where <leaf> is "fader" or "balpan".
// Returns true on match and fills outputs. The OSC server thread calls
// this for every message; keep it allocation-free.
bool parseMixInLeaf(const std::string& addr, int* outHwIdx, int* outBus,
                    const char** outLeaf) {
    static constexpr char kPrefix[] = "/mix/in/";
    static constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (addr.compare(0, kPrefixLen, kPrefix) != 0) return false;

    const char* p = addr.c_str() + kPrefixLen;
    char* endHw = nullptr;
    const long hwIdx = std::strtol(p, &endHw, 10);
    if (endHw == p || *endHw != '/') return false;

    const char* q = endHw + 1;
    char* endBus = nullptr;
    const long bus = std::strtol(q, &endBus, 10);
    if (endBus == q || *endBus != '/') return false;

    const char* leaf = endBus + 1;
    if (std::strcmp(leaf, "fader") != 0 && std::strcmp(leaf, "balpan") != 0) {
        return false;
    }
    *outHwIdx = static_cast<int>(hwIdx);
    *outBus = static_cast<int>(bus);
    *outLeaf = leaf;
    return true;
}

// Parse "/input/<hwIdx>/<leaf>" for the preamp leaves we mirror to P_EXT
// (48v / pad / phase / gain). Returns true on a known leaf, fills hwIdx +
// pointer-into-addr for `leaf`. Anything else (mute, stereo, …) returns
// false so we don't churn rxQueue with irrelevant traffic.
bool parseInputPreampLeaf(const std::string& addr, int* outHwIdx,
                          const char** outLeaf) {
    static constexpr char kPrefix[] = "/input/";
    static constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (addr.compare(0, kPrefixLen, kPrefix) != 0) return false;

    const char* p = addr.c_str() + kPrefixLen;
    char* end = nullptr;
    const long hwIdx = std::strtol(p, &end, 10);
    if (end == p || *end != '/') return false;
    const char* leaf = end + 1;
    if (std::strcmp(leaf, "48v")   != 0 &&
        std::strcmp(leaf, "pad")   != 0 &&
        std::strcmp(leaf, "phase") != 0 &&
        std::strcmp(leaf, "gain")  != 0) return false;
    *outHwIdx = static_cast<int>(hwIdx);
    *outLeaf = leaf;
    return true;
}

// Extract the first float argument; returns false if the message has no
// float in slot 0. TotalMix fader/balpan messages are always single-float.
bool firstFloat(const osc::Message& m, float* out) {
    const auto& args = m.arguments();
    if (args.empty()) return false;
    if (const float* f = std::get_if<float>(&args[0])) {
        *out = *f;
        return true;
    }
    return false;
}

} // namespace

// Single handler installed by main.cpp on the always-running OSC server.
// Always feeds the state cache; logs to console only when the dump action
// is currently toggled on; forwards /mix/in/.../fader|balpan to the csurf
// when 2-Way Control is engaged.
void rxHandler(const osc::Message& m) {
    if (s_state) s_state->onMessage(m);
    if (s_dumpToConsole.load(std::memory_order_relaxed)) {
        reaper::log("[RX] " + m.toString());
    }

    int hwIdx = 0;
    int bus = 0;
    const char* leaf = nullptr;
    if (csurfInstance() != nullptr && csurfInstance()->isTwoWayEnabled() &&
        parseMixInLeaf(m.address(), &hwIdx, &bus, &leaf)) {
        float v = 0.0f;
        if (!firstFloat(m, &v)) return;
        if (leaf[0] == 'f') {
            csurfInstance()->onIncomingFader(hwIdx, bus, v);
        } else {
            csurfInstance()->onIncomingBalpan(hwIdx, bus, v);
        }
        return;
    }

    // Preamp leaves are mirrored to P_EXT regardless of 2-Way Control —
    // they're metadata, not routing state. Any surface or script that
    // reads P_EXT:totalreaper_<flag> stays in sync with TotalMix-side
    // changes (e.g. user toggles 48V in TotalMix's UI directly).
    int hwIdx2 = 0;
    const char* preampLeaf = nullptr;
    if (csurfInstance() != nullptr &&
        parseInputPreampLeaf(m.address(), &hwIdx2, &preampLeaf)) {
        float v = 0.0f;
        if (!firstFloat(m, &v)) return;
        csurfInstance()->onIncomingPreamp(hwIdx2, preampLeaf, v);
    }
}

bool isDumpToConsoleActive() { return s_dumpToConsole.load(); }

bool runDumpOsc(int command) {
    if (command != s_dumpId) return false;
    const bool nowOn = !s_dumpToConsole.load();
    s_dumpToConsole.store(nowOn);
    reaper::debugLog(nowOn
                     ? "[TotalReaper] OSC dump → console ENABLED"
                     : "[TotalReaper] OSC dump → console disabled");
    return true;
}

} // namespace totalreaper::actions
