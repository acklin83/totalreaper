// OscMessage.cpp — Encode/decode for OSC 1.0 wire format

#include "OscMessage.h"

#include <cstring>
#include <sstream>

namespace totalreaper::osc {

namespace {

// OSC pads strings and blobs to multiples of 4 bytes.
constexpr std::size_t kAlignment = 4;

inline std::size_t paddedSize(std::size_t size) noexcept {
    return ((size + kAlignment - 1) / kAlignment) * kAlignment;
}

// ── Big-endian helpers ──────────────────────────────────────────────────────
inline void writeBE32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

inline std::uint32_t readBE32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24)
         | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)
         | static_cast<std::uint32_t>(p[3]);
}

// Append a NUL-terminated string and pad to 4-byte boundary.
void appendOscString(std::vector<std::uint8_t>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0); // mandatory NUL
    while (out.size() % kAlignment != 0) {
        out.push_back(0);
    }
}

// Read an OSC string starting at `offset`. Returns the string and advances
// `offset` past the padded length, or returns false on malformed input.
bool readOscString(const std::uint8_t* data, std::size_t size,
                   std::size_t& offset, std::string& out) {
    if (offset >= size) {
        return false;
    }
    const auto start = offset;
    while (offset < size && data[offset] != 0) {
        ++offset;
    }
    if (offset >= size) {
        return false; // no NUL terminator found
    }
    out.assign(reinterpret_cast<const char*>(data + start), offset - start);
    ++offset; // skip NUL
    // Pad to 4-byte boundary
    while ((offset - start) % kAlignment != 0) {
        if (offset >= size) {
            return false;
        }
        ++offset;
    }
    return true;
}

// IEEE 754 single-precision float ↔ uint32 punning (without UB).
inline std::uint32_t floatToBits(float f) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

inline float bitsToFloat(std::uint32_t bits) noexcept {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

} // namespace

// ── Builder API ─────────────────────────────────────────────────────────────
Message& Message::addFloat(float value) {
    arguments_.emplace_back(value);
    return *this;
}

Message& Message::addInt(std::int32_t value) {
    arguments_.emplace_back(value);
    return *this;
}

Message& Message::addString(std::string value) {
    arguments_.emplace_back(std::move(value));
    return *this;
}

Message& Message::addBlob(Blob value) {
    arguments_.emplace_back(std::move(value));
    return *this;
}

// ── Encode ──────────────────────────────────────────────────────────────────
std::vector<std::uint8_t> Message::encode() const {
    std::vector<std::uint8_t> out;
    out.reserve(64);

    // Address pattern
    appendOscString(out, address_);

    // Type tag string starts with ','
    std::string typeTag = ",";
    typeTag.reserve(arguments_.size() + 1);
    for (const auto& arg : arguments_) {
        if (std::holds_alternative<float>(arg)) {
            typeTag += 'f';
        } else if (std::holds_alternative<std::int32_t>(arg)) {
            typeTag += 'i';
        } else if (std::holds_alternative<std::string>(arg)) {
            typeTag += 's';
        } else if (std::holds_alternative<Blob>(arg)) {
            typeTag += 'b';
        }
    }
    appendOscString(out, typeTag);

    // Arguments in the order they appear in the type tag
    for (const auto& arg : arguments_) {
        if (const auto* f = std::get_if<float>(&arg)) {
            writeBE32(out, floatToBits(*f));
        } else if (const auto* i = std::get_if<std::int32_t>(&arg)) {
            writeBE32(out, static_cast<std::uint32_t>(*i));
        } else if (const auto* s = std::get_if<std::string>(&arg)) {
            appendOscString(out, *s);
        } else if (const auto* b = std::get_if<Blob>(&arg)) {
            writeBE32(out, static_cast<std::uint32_t>(b->size()));
            out.insert(out.end(), b->begin(), b->end());
            const auto padded = paddedSize(b->size());
            for (std::size_t i = b->size(); i < padded; ++i) {
                out.push_back(0);
            }
        }
    }
    return out;
}

// ── Decode ──────────────────────────────────────────────────────────────────
bool Message::decode(const std::uint8_t* data, std::size_t size, Message& out) {
    out = Message{}; // reset

    std::size_t offset = 0;
    if (!readOscString(data, size, offset, out.address_)) {
        return false;
    }

    // Type tag
    std::string typeTag;
    if (!readOscString(data, size, offset, typeTag)) {
        return false;
    }
    if (typeTag.empty() || typeTag.front() != ',') {
        return false;
    }

    // Read arguments according to type tag
    for (std::size_t t = 1; t < typeTag.size(); ++t) {
        const char tag = typeTag[t];
        switch (tag) {
            case 'f': {
                if (offset + 4 > size) return false;
                const auto bits = readBE32(data + offset);
                offset += 4;
                out.arguments_.emplace_back(bitsToFloat(bits));
                break;
            }
            case 'i': {
                if (offset + 4 > size) return false;
                const auto bits = readBE32(data + offset);
                offset += 4;
                out.arguments_.emplace_back(static_cast<std::int32_t>(bits));
                break;
            }
            case 's': {
                std::string s;
                if (!readOscString(data, size, offset, s)) return false;
                out.arguments_.emplace_back(std::move(s));
                break;
            }
            case 'b': {
                if (offset + 4 > size) return false;
                const auto length = readBE32(data + offset);
                offset += 4;
                if (offset + length > size) return false;
                Blob blob(data + offset, data + offset + length);
                offset += length;
                // Skip padding
                const auto padded = paddedSize(length);
                offset += padded - length;
                out.arguments_.emplace_back(std::move(blob));
                break;
            }
            default:
                // Unknown type tag — bail out rather than corrupt parse state
                return false;
        }
    }
    return true;
}

// ── toString (for logging) ──────────────────────────────────────────────────
std::string Message::toString() const {
    std::ostringstream oss;
    oss << address_;
    if (arguments_.empty()) {
        return oss.str();
    }
    oss << " ,";
    // Type tags
    for (const auto& arg : arguments_) {
        if (std::holds_alternative<float>(arg))            oss << 'f';
        else if (std::holds_alternative<std::int32_t>(arg)) oss << 'i';
        else if (std::holds_alternative<std::string>(arg))  oss << 's';
        else if (std::holds_alternative<Blob>(arg))         oss << 'b';
    }
    // Values
    for (const auto& arg : arguments_) {
        oss << ' ';
        if (const auto* f = std::get_if<float>(&arg)) {
            oss << *f;
        } else if (const auto* i = std::get_if<std::int32_t>(&arg)) {
            oss << *i;
        } else if (const auto* s = std::get_if<std::string>(&arg)) {
            oss << '"' << *s << '"';
        } else if (const auto* b = std::get_if<Blob>(&arg)) {
            oss << "<blob " << b->size() << " bytes>";
        }
    }
    return oss.str();
}

} // namespace totalreaper::osc
