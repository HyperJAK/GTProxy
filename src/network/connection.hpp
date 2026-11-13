#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../packet/packet_helper.hpp"

namespace network {
class IConnection {
public:
    virtual ~IConnection() = default;

    [[nodiscard]] virtual bool write(std::span<const std::byte> data, int channel = 0) const = 0;
    [[nodiscard]] virtual bool write(const std::vector<std::byte>& data, int channel = 0) const = 0;

    template <class Packet>
    bool write(Packet& packet) {
        return packet::PacketHelper::write(packet, *this);
    }

    [[nodiscard]] virtual bool is_connected() const = 0;

    // Optional diagnostics used by the control plane status view.
    [[nodiscard]] virtual std::uint32_t round_trip_ms() const { return 0; }
    [[nodiscard]] virtual std::string peer_description() const { return {}; }
};
}
