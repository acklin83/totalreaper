// TotalMixState.cpp — parse interesting paths from incoming OSC and cache.

#include "TotalMixState.h"

#include <cstdio>
#include <cstring>

namespace totalreaper::osc {

namespace {

// Parse "/input/<idx>/gain" — return idx, or -1 if path doesn't match.
int parseInputGainIndex(const std::string& addr) {
    static constexpr const char* prefix = "/input/";
    static constexpr std::size_t prefixLen = 7;
    if (addr.size() < prefixLen + 1 + 5) return -1; // /input/N/gain minimum
    if (addr.compare(0, prefixLen, prefix) != 0) return -1;
    const char* p = addr.c_str() + prefixLen;
    char* end = nullptr;
    const long idx = std::strtol(p, &end, 10);
    if (end == p) return -1;
    if (std::strcmp(end, "/gain") != 0) return -1;
    return static_cast<int>(idx);
}

} // namespace

void TotalMixState::onMessage(const Message& msg) {
    const int idx = parseInputGainIndex(msg.address());
    if (idx >= 0 && !msg.arguments().empty()) {
        if (auto* f = std::get_if<float>(&msg.arguments()[0])) {
            std::lock_guard<std::mutex> lock(mu_);
            inputGain_[idx] = *f;
        }
    }
}

bool TotalMixState::getInputGain(int hwIdx, float* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = inputGain_.find(hwIdx);
    if (it == inputGain_.end()) return false;
    if (out) *out = it->second;
    return true;
}

void TotalMixState::setInputGain(int hwIdx, float dB) {
    std::lock_guard<std::mutex> lock(mu_);
    inputGain_[hwIdx] = dB;
}

} // namespace totalreaper::osc
