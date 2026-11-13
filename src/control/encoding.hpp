#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace control::encoding {
[[nodiscard]] inline std::string to_hex(std::span<const std::byte> data)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const auto b : data) {
        const auto v = static_cast<unsigned>(b);
        out += digits[v >> 4];
        out += digits[v & 0xF];
    }
    return out;
}

// Accepts "de ad be ef", "DEADBEEF", "0xde,0xad", "de:ad" etc.
// Returns nullopt on an odd number of nibbles or a non-hex character.
[[nodiscard]] inline std::optional<std::vector<std::byte>> from_hex(std::string_view text)
{
    std::vector<std::byte> out;
    int pending = -1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':' || c == ',' || c == '-') {
            if (pending != -1) return std::nullopt; // separator in the middle of a byte
            continue;
        }
        if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && pending == -1) {
            ++i;
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return std::nullopt;

        if (pending == -1) {
            pending = v;
        } else {
            out.push_back(static_cast<std::byte>((pending << 4) | v));
            pending = -1;
        }
    }
    if (pending != -1) return std::nullopt;
    return out;
}

[[nodiscard]] inline std::string base64_encode(std::span<const std::byte> data)
{
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16)
            | (static_cast<std::uint32_t>(data[i + 1]) << 8)
            | static_cast<std::uint32_t>(data[i + 2]);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63]; out += tbl[n & 63];
    }
    if (i + 1 == data.size()) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16)
            | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += '=';
    }
    return out;
}

[[nodiscard]] inline std::optional<std::vector<std::byte>> base64_decode(std::string_view text)
{
    std::vector<std::byte> out;
    std::uint32_t acc = 0;
    int bits = 0;
    std::size_t pad = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') { ++pad; continue; }
        else if (c == '\n' || c == '\r' || c == ' ') continue;
        else return std::nullopt;
        if (pad) return std::nullopt; // data after padding
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// SHA-1 is used only for the RFC 6455 WebSocket handshake (not for security).
[[nodiscard]] inline std::array<std::uint8_t, 20> sha1(std::span<const std::byte> data)
{
    std::uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::vector<std::uint8_t> msg;
    msg.reserve(data.size() + 72);
    for (const auto b : data) msg.push_back(static_cast<std::uint8_t>(b));
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bit_len >> (i * 8)));

    const auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(msg[off + i * 4]) << 24)
                | (static_cast<std::uint32_t>(msg[off + i * 4 + 1]) << 16)
                | (static_cast<std::uint32_t>(msg[off + i * 4 + 2]) << 8)
                | static_cast<std::uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view s)
{
    return { reinterpret_cast<const std::byte*>(s.data()), s.size() };
}
}
