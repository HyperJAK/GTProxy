#pragma once
#include <vector>

#include "../../event/event.hpp"
#include "../../network/client.hpp"
#include "../../network/server.hpp"

namespace control { class PacketFlow; }

namespace core::handlers {
class ForwardingHandler {
public:
    ForwardingHandler(
        event::Dispatcher& dispatcher,
        network::Client& client,
        network::Server& server,
        control::PacketFlow* flow = nullptr
    );

private:
    void setup_raw_packet_handlers();

private:
    event::Dispatcher& dispatcher_;
    network::Client& client_;
    network::Server& server_;
    control::PacketFlow* flow_; // optional control-plane pipeline (null = plain forwarding)

    std::vector<event::ScopedHandle> handles_;
};
}
