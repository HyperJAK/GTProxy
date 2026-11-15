#pragma once
// Synthetic packet fixtures, assembled byte-by-byte (not via the codec under
// test) so decoder tests are not circular. Layout per ARCHITECTURE.md.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "control/encoding.hpp"
#include "network/connection.hpp"

namespace fixtures {
using Bytes = std::vector<std::byte>;

template <typename T>
inline void put(Bytes& b, T v)
{
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}

inline void put_str(Bytes& b, const std::string& s)
{
    for (char c : s) b.push_back(static_cast<std::byte>(c));
}

inline Bytes text_packet(std::uint32_t msg_type, const std::string& text)
{
    Bytes b;
    put<std::uint32_t>(b, msg_type);
    put_str(b, text);
    b.push_back(std::byte{ 0 });
    return b;
}

// 4-byte message type + 56-byte header + extra.
inline Bytes game_packet(std::uint8_t type, std::uint32_t net_id, std::uint32_t flags, const Bytes& extra,
    float x = 0, float y = 0, std::int32_t int_data = 0)
{
    Bytes b;
    put<std::uint32_t>(b, 4);
    put<std::uint8_t>(b, type);
    for (int i = 0; i < 3; ++i) put<std::uint8_t>(b, 0);
    put<std::uint32_t>(b, net_id);       // 4
    put<std::int32_t>(b, 0);             // 8 item_net_id
    put<std::uint32_t>(b, flags);        // 12
    put<float>(b, 0.0f);                 // 16 float_var
    put<std::int32_t>(b, int_data);      // 20
    put<float>(b, x);                    // 24 pos_x
    put<float>(b, y);                    // 28 pos_y
    for (int i = 0; i < 5; ++i) put<std::uint32_t>(b, 0); // 32..51
    put<std::uint32_t>(b, static_cast<std::uint32_t>(extra.size())); // 52 data_size
    b.insert(b.end(), extra.begin(), extra.end());
    return b;
}

inline Bytes variant_extra_onspawn()
{
    Bytes e;
    put<std::uint8_t>(e, 2);                    // count
    put<std::uint8_t>(e, 0); put<std::uint8_t>(e, 2);   // idx0 string
    put<std::uint32_t>(e, 7); put_str(e, "OnSpawn");
    put<std::uint8_t>(e, 1); put<std::uint8_t>(e, 9);   // idx1 signed
    put<std::int32_t>(e, 42);
    return e;
}

inline Bytes call_function_packet() { return game_packet(1, 0xFFFFFFFF, 8, variant_extra_onspawn()); }
inline Bytes state_packet() { return game_packet(0, 7, 0, {}, 12.5f, 30.0f); }
inline Bytes input_packet() { return text_packet(2, "action|input\n|text|hello"); }

inline Bytes hex(const std::string& s) { return *control::encoding::from_hex(s); }

// Records writes; stands in for an ENet peer.
class FakeConnection final : public network::IConnection {
public:
    mutable std::vector<Bytes> writes;
    bool connected{ true };
    bool write(std::span<const std::byte> d, int) const override
    {
        if (!connected) return false;
        writes.emplace_back(d.begin(), d.end());
        return true;
    }
    bool write(const std::vector<std::byte>& d, int ch) const override { return write(std::span{ d }, ch); }
    [[nodiscard]] bool is_connected() const override { return connected; }
    [[nodiscard]] std::uint32_t round_trip_ms() const override { return 17; }
    [[nodiscard]] std::string peer_description() const override { return "fake:1"; }
};
}
