// End-to-end over REAL ENet sockets on 127.0.0.1:
//   fake game client (network::Client) -> proxy Server -> ForwardingHandler/PacketFlow
//   -> proxy Client -> fake upstream server (network::Server)
// No Growtopia client or server is involved.
#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "support.hpp"
#include "control/control_plane.hpp"
#include "core/handlers/forwarding_handler.hpp"
#include "network/client.hpp"
#include "network/server.hpp"
#include "packet/register_packets.hpp"

using namespace fixtures;

namespace {
constexpr int kProxyPort = 38917;
constexpr int kUpstreamPort = 38918;

struct Capture {
    std::vector<Bytes> got;
    event::ScopedHandle handle;
    Capture(event::Dispatcher& d, event::Type t)
        : handle{ d, t, d.append_listener(t, [this](const event::Event& e) {
            if (const auto* raw = dynamic_cast<const event::RawPacketEvent*>(&e)) got.emplace_back(raw->data.begin(), raw->data.end());
        }) }
    { }
};

struct Topology {
    support::TempDir dir;
    core::Config proxy_cfg{ support::write_config(dir, kProxyPort) };
    core::Config up_cfg{ [&] {
        const auto f = dir.str("up.json");
        std::ofstream{ f } << R"({"server":{"port":)" << kUpstreamPort << "}}";
        return f;
    }() };
    core::Config client_cfg{ [&] {
        const auto f = dir.str("cl.json");
        std::ofstream{ f } << R"({"server":{"port":38919}})";
        return f;
    }() };
    event::Dispatcher proxy_d, up_d, cl_d;
    std::unique_ptr<network::Server> upstream;          // fake upstream server
    std::unique_ptr<network::Server> proxy_server;
    std::unique_ptr<network::Client> proxy_client;
    std::unique_ptr<network::Client> fake_client;
    std::unique_ptr<control::ControlPlane> plane;
    std::unique_ptr<core::handlers::ForwardingHandler> fwd;
    bool up_connected{ false }, cl_connected{ false }, proxy_up{ false };
    std::vector<event::ScopedHandle> handles;

    Topology()
    {
        upstream = std::make_unique<network::Server>(up_cfg, up_d);
        proxy_server = std::make_unique<network::Server>(proxy_cfg, proxy_d);
        proxy_client = std::make_unique<network::Client>(proxy_cfg, proxy_d);
        fake_client = std::make_unique<network::Client>(client_cfg, cl_d);
        plane = std::make_unique<control::ControlPlane>(proxy_cfg, *proxy_server, *proxy_client);
        proxy_server->set_wire_tap(plane->flow().server_tap());
        proxy_client->set_wire_tap(plane->flow().client_tap());
        plane->attach(proxy_d);
        fwd = std::make_unique<core::handlers::ForwardingHandler>(proxy_d, *proxy_client, *proxy_server, &plane->flow());

        const auto watch = [&](event::Dispatcher& d, event::Type t, bool& flag) {
            handles.emplace_back(d, t, d.append_listener(t, [&flag](const event::Event&) { flag = true; }));
        };
        watch(up_d, event::Type::ClientConnect, up_connected);
        watch(cl_d, event::Type::ServerConnect, cl_connected);
        watch(proxy_d, event::Type::ServerConnect, proxy_up);
    }

    void pump()
    {
        upstream->process();
        proxy_server->process();
        proxy_client->process();
        fake_client->process();
        plane->poll();
    }

    bool connect()
    {
        if (!fake_client->connect("127.0.0.1", kProxyPort) || !proxy_client->connect("127.0.0.1", kUpstreamPort)) return false;
        return support::wait_until([&] { return up_connected && cl_connected && proxy_up; }, [&] { pump(); }, 5000);
    }
};
}

TEST(EnetLoopback, ForwardsBothDirectionsAndStreamsEvents)
{
    packet::register_all_packets();
    Topology t;
    ASSERT_TRUE(t.connect()) << "ENet loopback unavailable in this environment";

    Capture at_upstream{ t.up_d, event::Type::ServerBoundPacket };
    Capture at_client{ t.cl_d, event::Type::ClientBoundPacket };

    ASSERT_TRUE(t.fake_client->write(input_packet()));
    ASSERT_TRUE(support::wait_until([&] { return !at_upstream.got.empty(); }, [&] { t.pump(); }));
    EXPECT_EQ(at_upstream.got[0], input_packet());

    // the upstream server's reply travels back
    ASSERT_TRUE(t.upstream->write(call_function_packet()));
    ASSERT_TRUE(support::wait_until([&] { return !at_client.got.empty(); }, [&] { t.pump(); }));
    EXPECT_EQ(at_client.got[0], call_function_packet());

    // both crossed the control plane
    const auto events = t.plane->hub().backlog();
    int packets = 0;
    for (const auto& e : events) if (e.find("\"t\":\"packet\"") != std::string::npos) ++packets;
    EXPECT_EQ(packets, 2);
    EXPECT_EQ(t.plane->flow().stats().packets[0] + t.plane->flow().stats().packets[1], 2u);
}

TEST(EnetLoopback, InterceptHoldsOnRealPeersThenEditedPacketArrives)
{
    packet::register_all_packets();
    Topology t;
    ASSERT_TRUE(t.connect());
    Capture at_upstream{ t.up_d, event::Type::ServerBoundPacket };

    t.plane->api().handle("intercept.set", *control::JsonValue::parse(R"({"enabled":true})"));
    ASSERT_TRUE(t.fake_client->write(input_packet()));
    ASSERT_TRUE(support::wait_until([&] { return t.plane->flow().held_count() == 1; }, [&] { t.pump(); }));
    for (int i = 0; i < 50; ++i) { t.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    EXPECT_TRUE(at_upstream.got.empty()); // really held

    const auto q = control::JsonValue::parse(t.plane->flow().intercept_json());
    const auto id = q->find("held")->array[0].integer("id");
    t.plane->api().handle("intercept.forward", *control::JsonValue::parse(
        R"({"id":)" + std::to_string(id) + R"(,"packet":{"kind":"text","msg_type":2,"text":"action|input\n|text|edited"}})"));
    ASSERT_TRUE(support::wait_until([&] { return !at_upstream.got.empty(); }, [&] { t.pump(); }));
    EXPECT_EQ(at_upstream.got[0], text_packet(2, "action|input\n|text|edited"));
}
