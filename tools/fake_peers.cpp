// Development helper: plays BOTH ends of a session against a running GTProxy,
// so you can exercise the UI, plugins and record/replay with no game client
// and no real server.
//
//   1. starts a stand-in upstream server (ENet) on --upstream-port
//   2. asks the proxy's /growtopia/server_data.php (direct profile) where to go
//   3. connects a stand-in game client to the proxy
//   4. sends a small scripted conversation every second
//
// The proxy must be using a *direct* profile pointing at 127.0.0.1:<upstream-port>
// (the default "localhost" profile does, with 17091).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>
#include <httplib.h>

#include "core/config.hpp"
#include "event/event.hpp"
#include "network/client.hpp"
#include "network/server.hpp"

namespace {
std::atomic<bool> g_stop{ false };
using Bytes = std::vector<std::byte>;

template <typename T> void put(Bytes& b, T v) { const auto* p = reinterpret_cast<const std::byte*>(&v); b.insert(b.end(), p, p + sizeof(T)); }

Bytes text(std::uint32_t type, const std::string& s)
{
    Bytes b; put<std::uint32_t>(b, type);
    for (char c : s) b.push_back(static_cast<std::byte>(c));
    b.push_back(std::byte{ 0 });
    return b;
}

Bytes game(std::uint8_t type, std::uint32_t net_id, const Bytes& extra, float x = 0, float y = 0)
{
    Bytes b; put<std::uint32_t>(b, 4); put<std::uint8_t>(b, type);
    for (int i = 0; i < 3; ++i) put<std::uint8_t>(b, 0);
    put<std::uint32_t>(b, net_id); put<std::int32_t>(b, 0);
    put<std::uint32_t>(b, extra.empty() ? 0u : 8u); // extended flag
    put<float>(b, 0); put<std::int32_t>(b, 0); put<float>(b, x); put<float>(b, y);
    for (int i = 0; i < 5; ++i) put<std::uint32_t>(b, 0);
    put<std::uint32_t>(b, static_cast<std::uint32_t>(extra.size()));
    b.insert(b.end(), extra.begin(), extra.end());
    return b;
}

Bytes call(const std::string& fn, const std::string& arg, std::int32_t number)
{
    Bytes e; put<std::uint8_t>(e, 3);
    put<std::uint8_t>(e, 0); put<std::uint8_t>(e, 2); put<std::uint32_t>(e, static_cast<std::uint32_t>(fn.size())); for (char c : fn) e.push_back(static_cast<std::byte>(c));
    put<std::uint8_t>(e, 1); put<std::uint8_t>(e, 2); put<std::uint32_t>(e, static_cast<std::uint32_t>(arg.size())); for (char c : arg) e.push_back(static_cast<std::byte>(c));
    put<std::uint8_t>(e, 2); put<std::uint8_t>(e, 9); put<std::int32_t>(e, number);
    return game(1, 0xFFFFFFFF, e);
}

std::string write_cfg(int port)
{
    const auto path = (std::filesystem::temp_directory_path() / ("fake_peers_" + std::to_string(port) + ".json")).string();
    std::ofstream{ path } << R"({"server":{"port":)" << port << "}}";
    return path;
}
}

int main(int argc, char** argv)
{
    int proxy_port = 16999, upstream_port = 17091;
    std::string http_host = "127.0.0.1";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--proxy-port") proxy_port = std::atoi(argv[i + 1]);
        else if (k == "--upstream-port") upstream_port = std::atoi(argv[i + 1]);
        else if (k == "--http-host") http_host = argv[i + 1];
    }
    std::cout << std::unitbuf;
    std::signal(SIGINT, [](int) { g_stop = true; });
    if (enet_initialize() != 0) return 1;

    core::Config up_cfg{ write_cfg(upstream_port) };
    core::Config cl_cfg{ write_cfg(upstream_port + 1000) };
    event::Dispatcher up_d, cl_d;
    network::Server upstream{ up_cfg, up_d };
    network::Client client{ cl_cfg, cl_d };

    bool up_ok = false, cl_ok = false;
    auto h1 = up_d.append_listener(event::Type::ClientConnect, [&](const event::Event&) { up_ok = true; std::cout << "[upstream] proxy connected\n"; });
    auto h2 = cl_d.append_listener(event::Type::ServerConnect, [&](const event::Event&) { cl_ok = true; std::cout << "[client] connected to proxy\n"; });
    auto h3 = up_d.append_listener(event::Type::ServerBoundPacket, [&](const event::Event& e) {
        if (const auto* r = dynamic_cast<const event::RawPacketEvent*>(&e)) std::cout << "[upstream] got " << r->data.size() << " bytes\n";
    });
    auto h4 = cl_d.append_listener(event::Type::ClientBoundPacket, [&](const event::Event& e) {
        if (const auto* r = dynamic_cast<const event::RawPacketEvent*>(&e)) std::cout << "[client] got " << r->data.size() << " bytes\n";
    });

    httplib::SSLClient web{ http_host, 443 };
    web.enable_server_certificate_verification(false);
    const auto res = web.Post("/growtopia/server_data.php", "version=0&platform=0&protocol=0", "application/x-www-form-urlencoded");
    if (!res || res->status != 200) {
        std::cerr << "server_data.php request failed (is the proxy running and on a direct profile?)\n";
        return 1;
    }
    std::cout << "[client] server_data.php says:\n" << res->body << "\n";

    if (!client.connect("127.0.0.1", static_cast<std::uint16_t>(proxy_port))) { std::cerr << "cannot start ENet connect\n"; return 1; }

    int tick = 0;
    auto next = std::chrono::steady_clock::now();
    while (!g_stop) {
        upstream.process();
        client.process();
        if (up_ok && cl_ok && std::chrono::steady_clock::now() >= next) {
            next += std::chrono::seconds(1);
            ++tick;
            std::ignore = client.write(text(2, "action|input\n|text|hello from the fake client " + std::to_string(tick)));
            std::ignore = upstream.write(call("OnConsoleMessage", "`2fake server says hi", tick));
            std::ignore = client.write(game(0, 7, {}, 10.0f + static_cast<float>(tick), 20.0f));
            std::ignore = upstream.write(text(3, "action|log\nmsg|server tick " + std::to_string(tick)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    enet_deinitialize();
    return 0;
}
