#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <httplib.h>

#include "control_api.hpp"
#include "main_thread_queue.hpp"

namespace control {
// Loopback-only HTTP/REST front for ControlApi (+ static hosting of the built
// Angular UI). Deliberately a second cpp-httplib server rather than a route on
// core::WebServer: that one is a TLS listener on 0.0.0.0:443 for the game
// client, which is the wrong place for a debugging API.
class RestServer {
public:
    // `call` executes (op, params) on the proxy main thread and returns the reply.
    using Executor = std::function<ApiReply(const std::string& op, const JsonValue& params)>;

    RestServer(std::string bind_addr, std::uint16_t port, std::filesystem::path ui_dir, Executor executor);
    ~RestServer();

    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    std::string start(); // empty on success, else an error message
    void stop();
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void setup_routes();

    std::string bind_addr_;
    std::uint16_t port_;
    std::filesystem::path ui_dir_;
    Executor exec_;
    httplib::Server server_;
    std::thread thread_;
    std::atomic<bool> running_{ false };
};
}
