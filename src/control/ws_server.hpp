#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "event_hub.hpp"
#include "socket.hpp"

namespace control {
// Minimal RFC 6455 server: text frames, ping/pong, close, fragmentation.
// No extensions (no permessage-deflate), no subprotocols. Server -> client
// frames are sent unmasked; client -> server frames must be masked.
class WsServer {
public:
    // Called on the client's thread for each inbound text message; the
    // returned string (if non-empty) is sent back to that client.
    using MessageHandler = std::function<std::string(const std::string&)>;

    WsServer(std::string bind_addr, std::uint16_t port, EventHub& hub, MessageHandler handler);
    ~WsServer();

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // Returns an error message or empty on success.
    std::string start();
    void stop();

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::size_t client_count() const { return clients_alive_.load(); }

    [[nodiscard]] static std::string accept_key(const std::string& client_key);
    // Encode one unmasked server text frame (exposed for tests).
    [[nodiscard]] static std::string encode_text_frame(std::string_view payload);

private:
    struct Client {
        net::socket_t sock{ net::kInvalid };
        std::mutex mutex;
        std::deque<std::string> outbox;
        std::atomic<bool> done{ false };
        std::thread thread;
    };

    void accept_loop();
    void serve(Client& c);
    bool handshake(Client& c);
    void enqueue(Client& c, std::string frame);
    void reap();

    std::string bind_addr_;
    std::uint16_t port_;
    EventHub& hub_;
    MessageHandler handler_;
    net::socket_t listen_sock_{ net::kInvalid };
    std::atomic<bool> running_{ false };
    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::list<std::unique_ptr<Client>> clients_;
    std::atomic<std::size_t> clients_alive_{ 0 };

    static constexpr std::size_t kMaxClients = 8;
    static constexpr std::size_t kMaxOutbox = 20000;
    static constexpr std::size_t kMaxMessage = 1u << 20;
};
}
