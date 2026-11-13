#pragma once
#include <cstddef>
#include <span>

namespace network {
// Observer attached to a Server/Client. `on_receive_begin`/`end` bracket the
// synchronous handling of one inbound ENet packet (so everything the proxy
// does in response happens "inside" the scope), and `on_write` reports every
// packet this side successfully queued to its peer.
class IWireTap {
public:
    virtual ~IWireTap() = default;
    virtual void on_receive_begin(std::span<const std::byte> data) = 0;
    virtual void on_receive_end() = 0;
    virtual void on_write(std::span<const std::byte> data, int channel) = 0;
};

class WireTapGuard {
public:
    WireTapGuard(IWireTap* tap, const std::span<const std::byte> data)
        : tap_{ tap }
    {
        if (tap_) {
            tap_->on_receive_begin(data);
        }
    }

    ~WireTapGuard()
    {
        if (tap_) {
            tap_->on_receive_end();
        }
    }

    WireTapGuard(const WireTapGuard&) = delete;
    WireTapGuard& operator=(const WireTapGuard&) = delete;

private:
    IWireTap* tap_;
};
}
