// OscMessage.h — Minimal OSC 1.0 message encoding/decoding
//
// Spec reference: http://opensoundcontrol.org/spec-1_0
//
// Scope of this implementation:
//   - Type tags supported: 'f' (float32), 'i' (int32), 's' (string), 'b' (blob)
//   - Big-endian wire format, 4-byte aligned
//   - No bundle support yet (TotalMix Alpha 4 sends /status/* in bundles —
//     we'll add that when we hit it)
//   - No address pattern matching — TotalMix uses concrete paths, simple
//     string compare is enough
//
// Why no liblo / oscpack: TotalReaper needs ~200 lines of OSC. A dependency
// would be 10–100x more code than the actual feature. Spec is small enough
// to implement directly, avoids LGPL/license drama for a MIT-licensed
// REAPER extension.

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace totalreaper::osc {

using Blob = std::vector<std::uint8_t>;
using Argument = std::variant<float, std::int32_t, std::string, Blob>;

class Message {
public:
    Message() = default;
    explicit Message(std::string address) : address_(std::move(address)) {}

    // ── Builder API ─────────────────────────────────────────────────────────
    Message& addFloat(float value);
    Message& addInt(std::int32_t value);
    Message& addString(std::string value);
    Message& addBlob(Blob value);

    // ── Accessors ───────────────────────────────────────────────────────────
    const std::string& address() const noexcept { return address_; }
    const std::vector<Argument>& arguments() const noexcept { return arguments_; }

    // ── Wire format ─────────────────────────────────────────────────────────
    // Encode this message into the OSC wire format.
    std::vector<std::uint8_t> encode() const;

    // Decode a single OSC message (no bundle handling). Returns false on
    // malformed input. On success, `out` contains the parsed message.
    static bool decode(const std::uint8_t* data, std::size_t size, Message& out);

    // Human-readable representation for logging — e.g.
    //   "/input/11/mute ,f 1.0"
    std::string toString() const;

private:
    std::string address_;
    std::vector<Argument> arguments_;
};

} // namespace totalreaper::osc
