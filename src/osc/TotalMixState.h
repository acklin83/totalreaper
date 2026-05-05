// TotalMixState.h — Caches TotalMix-side state we observe via incoming OSC.
//
// Why we need this: REAPER actions like "increase preamp gain by 1 dB" only
// work as relative if we know the current absolute value. TotalMix's gain
// path is absolute (no /+1 syntax), so we listen continuously and remember
// the latest value we saw. The first such action on a given input requires
// the user to have moved that control at least once, or for TotalMix to have
// pushed an update — typical in a session where the user has been adjusting
// things in TotalMix's UI before invoking REAPER actions.

#pragma once

#include "OscMessage.h"

#include <mutex>
#include <unordered_map>

namespace totalreaper::osc {

class TotalMixState {
public:
    // Update cache from a received OSC message. Thread-safe.
    void onMessage(const Message& msg);

    // Look up the last observed value for /input/<hwIdx>/gain, in dB. Returns
    // false (and leaves *out untouched) if no value has been observed yet.
    bool getInputGain(int hwIdx, float* out) const;

    // Manually seed the cache after we push a value ourselves, so subsequent
    // actions in the same session don't have to wait for TotalMix to echo
    // the state back.
    void setInputGain(int hwIdx, float dB);

private:
    mutable std::mutex mu_;
    std::unordered_map<int, float> inputGain_;
};

} // namespace totalreaper::osc
