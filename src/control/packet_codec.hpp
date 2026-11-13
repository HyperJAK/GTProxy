#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "json_value.hpp"

namespace control {
// Human-readable view of a wire packet. `fields_json` is a JSON object
// (already serialised) that the UI renders; it never replaces the raw bytes.
struct DecodedPacket {
    std::string kind;       // hello | text | variant | game | raw
    std::string name;       // short label, e.g. "OnSpawn", "action|input", "PACKET_STATE"
    std::string summary;    // one-line description for list views
    std::string fields_json;
    bool malformed{ false };
};

struct EncodeResult {
    std::vector<std::byte> bytes;
    std::string error; // non-empty on failure
    [[nodiscard]] bool ok() const { return error.empty(); }
};

// Layout of a game packet header (bytes after the 4-byte message type).
inline constexpr std::size_t kGameHeaderSize = 56;
inline constexpr std::size_t kMaxPacketSize = 1u << 20;

// Decode wire bytes for display. Never throws; anything unknown or truncated
// is reported as kind "raw" / malformed=true so the caller falls back to hex.
[[nodiscard]] DecodedPacket decode_packet(std::span<const std::byte> data);

// Build wire bytes from an edit spec (the same shape decode_packet emits):
//   {"kind":"raw","hex":"..."}
//   {"kind":"text","msg_type":3,"text":"action|input\n|text|hi"}
//   {"kind":"variant","header":{...},"values":[{"type":"string","value":"OnSpawn"},...]}
//   {"kind":"game","header":{...},"extra_hex":"..."}
[[nodiscard]] EncodeResult encode_packet(const JsonValue& spec);

[[nodiscard]] std::string message_type_name(std::uint32_t t);
}
