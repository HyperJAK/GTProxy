#include "ws_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <spdlog/spdlog.h>

#include "encoding.hpp"
#include "origin_policy.hpp"

namespace control {
namespace {
constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string header_value(const std::string& request, std::string_view name)
{
    // case-insensitive header lookup on a raw HTTP request
    std::size_t pos = 0;
    while ((pos = request.find("\r\n", pos)) != std::string::npos) {
        pos += 2;
        const auto end = request.find("\r\n", pos);
        if (end == std::string::npos || end == pos) return {};
        const auto line = std::string_view{ request }.substr(pos, end - pos);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon != name.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i]))) { match = false; break; }
        }
        if (!match) continue;
        auto v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        return std::string{ v };
    }
    return {};
}

bool contains_token(std::string value, std::string_view token)
{
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.find(token) != std::string::npos;
}

std::string frame(std::uint8_t opcode, std::string_view payload)
{
    std::string out;
    out += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126) {
        out += static_cast<char>(payload.size());
    } else if (payload.size() <= 0xFFFF) {
        out += static_cast<char>(126);
        out += static_cast<char>(payload.size() >> 8);
        out += static_cast<char>(payload.size() & 0xFF);
    } else {
        out += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) out += static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (i * 8)) & 0xFF);
    }
    out.append(payload);
    return out;
}
}

WsServer::WsServer(std::string bind_addr, const std::uint16_t port, EventHub& hub, MessageHandler handler)
    : bind_addr_{ std::move(bind_addr) }, port_{ port }, hub_{ hub }, handler_{ std::move(handler) }
{ }

WsServer::~WsServer() { stop(); }

std::string WsServer::accept_key(const std::string& client_key)
{
    const std::string joined = client_key + std::string{ kGuid };
    const auto digest = encoding::sha1(encoding::as_bytes(joined));
    return encoding::base64_encode(std::as_bytes(std::span{ digest }));
}

std::string WsServer::encode_text_frame(const std::string_view payload) { return frame(0x1, payload); }

std::string WsServer::start()
{
    std::uint16_t actual = 0;
    listen_sock_ = net::listen_on(bind_addr_, port_, actual);
    if (listen_sock_ == net::kInvalid) {
        return fmt::format("cannot listen on {}:{} (port in use?)", bind_addr_, port_);
    }
    port_ = actual;
    running_ = true;
    accept_thread_ = std::thread{ &WsServer::accept_loop, this };
    spdlog::info("Control-plane WebSocket listening on ws://{}:{}", bind_addr_, port_);
    return {};
}

void WsServer::stop()
{
    if (!running_.exchange(false)) return;
    if (accept_thread_.joinable()) accept_thread_.join();
    std::list<std::unique_ptr<Client>> clients;
    {
        std::scoped_lock lock{ clients_mutex_ };
        clients.swap(clients_);
    }
    for (auto& c : clients) {
        c->done = true;
        if (c->thread.joinable()) c->thread.join();
    }
    net::close_socket(listen_sock_);
    listen_sock_ = net::kInvalid;
}

void WsServer::reap()
{
    std::scoped_lock lock{ clients_mutex_ };
    for (auto it = clients_.begin(); it != clients_.end();) {
        if ((*it)->done) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void WsServer::accept_loop()
{
    while (running_) {
        reap();
        if (net::wait(listen_sock_, false, 100) <= 0) continue;
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const auto s = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (s == net::kInvalid) continue;
        net::set_non_blocking(s);

        auto client = std::make_unique<Client>();
        client->sock = s;
        auto* raw = client.get();
        {
            std::scoped_lock lock{ clients_mutex_ };
            if (clients_.size() >= kMaxClients) {
                net::send_all(s, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                net::close_socket(s);
                continue;
            }
            clients_.push_back(std::move(client));
        }
        raw->thread = std::thread{ &WsServer::serve, this, std::ref(*raw) };
    }
}

bool WsServer::handshake(Client& c)
{
    std::string req;
    char buf[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (req.find("\r\n\r\n") == std::string::npos) {
        if (req.size() > 8192 || std::chrono::steady_clock::now() > deadline || !running_) return false;
        if (net::wait(c.sock, false, 100) <= 0) continue;
        const auto n = net::recv_some(c.sock, buf, sizeof(buf));
        if (n == 0 || n == -1) return false;
        if (n > 0) req.append(buf, static_cast<std::size_t>(n));
    }

    const auto reject = [&](std::string_view status) {
        net::send_all(c.sock, fmt::format("HTTP/1.1 {}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", status));
        return false;
    };

    if (!req.starts_with("GET ")) return reject("405 Method Not Allowed");
    if (!origin_allowed(header_value(req, "Origin"))) {
        spdlog::warn("Rejected WebSocket from disallowed origin \"{}\"", header_value(req, "Origin"));
        return reject("403 Forbidden");
    }
    if (const auto host = header_value(req, "Host"); !host.empty() && !is_loopback_host(host)) {
        return reject("403 Forbidden"); // DNS-rebinding guard
    }
    const auto key = header_value(req, "Sec-WebSocket-Key");
    if (!contains_token(header_value(req, "Upgrade"), "websocket") || !contains_token(header_value(req, "Connection"), "upgrade") ||
        key.empty() || header_value(req, "Sec-WebSocket-Version") != "13") {
        return reject("400 Bad Request");
    }
    return net::send_all(c.sock, fmt::format(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        accept_key(key)));
}

void WsServer::enqueue(Client& c, std::string f)
{
    std::scoped_lock lock{ c.mutex };
    if (c.outbox.size() >= kMaxOutbox) {
        c.done = true; // client too slow: disconnect instead of stalling the proxy
        return;
    }
    c.outbox.push_back(std::move(f));
}

void WsServer::serve(Client& c)
{
    clients_alive_++;
    std::uint64_t sub = 0;
    if (handshake(c)) {
        sub = hub_.subscribe([this, &c](const std::string& json) { enqueue(c, frame(0x1, json)); });

        std::string in;      // unparsed bytes
        std::string message; // fragmented message being assembled
        bool open = true;
        char buf[4096];
        while (open && running_ && !c.done) {
            // flush outbox
            std::deque<std::string> batch;
            {
                std::scoped_lock lock{ c.mutex };
                batch.swap(c.outbox);
            }
            for (const auto& f : batch) {
                if (!net::send_all(c.sock, f)) { open = false; break; }
            }
            if (!open) break;

            if (net::wait(c.sock, false, 25) > 0) {
                const auto n = net::recv_some(c.sock, buf, sizeof(buf));
                if (n == 0 || n == -1) break;
                if (n > 0) in.append(buf, static_cast<std::size_t>(n));
            }

            // parse frames
            while (in.size() >= 2) {
                const auto b0 = static_cast<std::uint8_t>(in[0]);
                const auto b1 = static_cast<std::uint8_t>(in[1]);
                const bool fin = b0 & 0x80;
                const auto opcode = static_cast<std::uint8_t>(b0 & 0x0F);
                const bool masked = b1 & 0x80;
                std::uint64_t len = b1 & 0x7F;
                std::size_t off = 2;
                if (len == 126) {
                    if (in.size() < 4) break;
                    len = (static_cast<std::uint8_t>(in[2]) << 8) | static_cast<std::uint8_t>(in[3]);
                    off = 4;
                } else if (len == 127) {
                    if (in.size() < 10) break;
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<std::uint8_t>(in[2 + i]);
                    off = 10;
                }
                if (!masked || len > kMaxMessage || (b0 & 0x70)) { open = false; break; } // protocol error
                if (in.size() < off + 4 + len) break;
                const char* mask = in.data() + off;
                std::string payload(in.data() + off + 4, static_cast<std::size_t>(len));
                for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
                in.erase(0, off + 4 + static_cast<std::size_t>(len));

                if (opcode == 0x8) { // close
                    net::send_all(c.sock, frame(0x8, {}));
                    open = false;
                    break;
                }
                if (opcode == 0x9) { enqueue(c, frame(0xA, payload)); continue; } // ping -> pong
                if (opcode == 0xA) continue;
                if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
                    if (opcode != 0x0) message.clear();
                    message += payload;
                    if (message.size() > kMaxMessage) { open = false; break; }
                    if (fin) {
                        try {
                            if (const auto reply = handler_(message); !reply.empty()) enqueue(c, frame(0x1, reply));
                        } catch (const std::exception& e) {
                            spdlog::error("WebSocket handler threw: {}", e.what());
                        }
                        message.clear();
                    }
                    continue;
                }
                open = false; // unknown opcode
                break;
            }
        }
    }
    if (sub) hub_.unsubscribe(sub);
    net::close_socket(c.sock);
    c.sock = net::kInvalid;
    clients_alive_--;
    c.done = true;
}
}
