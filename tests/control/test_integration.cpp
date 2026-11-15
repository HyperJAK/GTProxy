// Control-plane integration: ControlPlane + real WebSocket/REST sockets on
// loopback, with fake connections standing in for the ENet peers.
#include <future>
#include <gtest/gtest.h>
#include <httplib.h>

#include "fixtures.hpp"
#include "support.hpp"
#include "control/control_plane.hpp"

using namespace control;
using namespace fixtures;
using event::Direction;

namespace {
struct Plane {
    support::TempDir dir;
    core::Config config{ support::write_config(dir) };
    FakeConnection client, upstream;
    ControlPlane plane{ config, client, upstream };
    void pump() { plane.poll(); }

    // Read WS messages until `pred` accepts one, pumping the main loop.
    bool wait_msg(support::WsClient& ws, const std::function<bool(const JsonValue&)>& pred, int ms = 3000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            pump();
            std::string m;
            if (ws.recv_text(m, 20)) {
                if (const auto v = JsonValue::parse(m); v && pred(*v)) return true;
            }
        }
        return false;
    }
};
}

TEST(ControlIntegration, StreamsPacketsOverWebSocket)
{
    Plane p;
    ASSERT_NE(p.plane.ws_port(), 0);
    support::WsClient ws;
    ASSERT_TRUE(ws.connect(p.plane.ws_port())) << ws.handshake_response;
    EXPECT_NE(ws.handshake_response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);

    p.plane.flow().process(Direction::ClientBound, call_function_packet());
    EXPECT_TRUE(p.wait_msg(ws, [](const JsonValue& v) {
        return v.str("t") == "packet" && v.str("name") == "OnSpawn" && v.str("dir") == "client_bound";
    }));
    EXPECT_EQ(p.client.writes.size(), 1u); // and it was forwarded
}

TEST(ControlIntegration, CommandsOverWebSocketRunOnMainThread)
{
    Plane p;
    support::WsClient ws;
    ASSERT_TRUE(ws.connect(p.plane.ws_port()));
    ws.send_text(R"({"id":7,"op":"inject","params":{"dir":"server_bound","packet":{"kind":"text","msg_type":2,"text":"action|quit"}}})");
    EXPECT_TRUE(p.wait_msg(ws, [](const JsonValue& v) { return v.str("t") == "reply" && v.integer("id") == 7 && v.flag("ok"); }));
    ASSERT_EQ(p.upstream.writes.size(), 1u);
    EXPECT_EQ(p.upstream.writes[0], text_packet(2, "action|quit"));

    ws.send_text("garbage");
    EXPECT_TRUE(p.wait_msg(ws, [](const JsonValue& v) { return v.str("t") == "reply" && !v.flag("ok"); }));
    ws.send_text(R"({"id":"x","op":"nope"})");
    EXPECT_TRUE(p.wait_msg(ws, [](const JsonValue& v) { return v.str("id") == "x" && !v.flag("ok"); }));
}

TEST(ControlIntegration, InterceptEditRoundTripOverWebSocket)
{
    Plane p;
    support::WsClient ws;
    ASSERT_TRUE(ws.connect(p.plane.ws_port()));
    ws.send_text(R"({"id":1,"op":"intercept.set","params":{"enabled":true}})");
    ASSERT_TRUE(p.wait_msg(ws, [](const JsonValue& v) { return v.integer("id") == 1 && v.flag("ok"); }));

    p.plane.flow().process(Direction::ServerBound, input_packet());
    std::int64_t held_id = 0;
    ASSERT_TRUE(p.wait_msg(ws, [&](const JsonValue& v) {
        if (v.str("t") == "verdict" && v.str("state") == "held") { held_id = v.integer("id"); return true; }
        return false;
    }));
    EXPECT_TRUE(p.upstream.writes.empty());

    ws.send_text(R"({"id":2,"op":"intercept.forward","params":{"id":)" + std::to_string(held_id) +
        R"(,"hex":"02000000616374696f6e7c717569740a00"}})");
    ASSERT_TRUE(p.wait_msg(ws, [](const JsonValue& v) { return v.integer("id") == 2 && v.flag("ok"); }));
    ASSERT_EQ(p.upstream.writes.size(), 1u);
    EXPECT_EQ(control::encoding::to_hex(p.upstream.writes[0]), "02000000616374696f6e7c717569740a00");
}

TEST(ControlIntegration, RejectsForeignOrigins)
{
    Plane p;
    support::WsClient evil;
    EXPECT_FALSE(evil.connect(p.plane.ws_port(), "http://evil.example"));
    EXPECT_NE(evil.handshake_response.find("403"), std::string::npos);
    support::WsClient local;
    EXPECT_TRUE(local.connect(p.plane.ws_port(), "http://localhost:4200"));
}

TEST(ControlIntegration, RestEndpoints)
{
    Plane p;
    httplib::Client cli{ "127.0.0.1", p.plane.rest_port() };
    cli.set_read_timeout(5);

    const auto call = [&](const std::function<httplib::Result(httplib::Client&)>& fn) {
        auto fut = std::async(std::launch::async, [&] { return fn(cli); });
        while (fut.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) p.pump();
        return fut.get();
    };

    auto r = call([](httplib::Client& c) { return c.Get("/api/status"); });
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(JsonValue::parse(r->body)->flag("ok"));

    r = call([](httplib::Client& c) { return c.Post("/api/call/inject", R"({"dir":"client_bound","hex":"0300000041"})", "application/json"); });
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(p.client.writes.size(), 1u);

    r = call([](httplib::Client& c) { return c.Post("/api/call/inject", "not json", "application/json"); });
    EXPECT_EQ(r->status, 400);
    r = call([](httplib::Client& c) { return c.Post("/api/call/bogus", "{}", "application/json"); });
    EXPECT_EQ(r->status, 404);

    // cross-origin browser request is refused
    r = call([](httplib::Client& c) { return c.Get("/api/status", httplib::Headers{ { "Origin", "https://evil.example" } }); });
    EXPECT_EQ(r->status, 403);
    // DNS-rebinding style Host header is refused
    r = call([](httplib::Client& c) { return c.Get("/api/status", httplib::Headers{ { "Host", "evil.example" } }); });
    EXPECT_EQ(r->status, 403);

    r = call([](httplib::Client& c) { return c.Get("/api/packets?limit=1"); });
    EXPECT_EQ(JsonValue::parse(r->body)->find("result")->array.size(), 1u);
}

TEST(ControlIntegration, LateMainLoopStillAnswersQueuedRequest)
{
    // The main loop starts draining only after the request is already waiting.
    Plane p;
    httplib::Client cli{ "127.0.0.1", p.plane.rest_port() };
    cli.set_read_timeout(10);
    auto fut = std::async(std::launch::async, [&] { return cli.Get("/api/status"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    while (fut.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) p.pump();
    EXPECT_EQ(fut.get()->status, 200);
}
