#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

#include "control/socket.hpp"
#include "control/encoding.hpp"

namespace support {
struct TempDir {
    std::filesystem::path path;
    TempDir()
    {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() / ("gtproxy_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    [[nodiscard]] std::string str(const std::string& sub = {}) const { return (sub.empty() ? path : path / sub).string(); }
};

// Writes a config.json that points every control-plane resource into `dir`
// and uses ephemeral ports.
inline std::string write_config(const TempDir& dir, int proxy_port = 16999)
{
    const auto file = dir.str("config.json");
    std::ofstream out{ file };
    const auto q = [&](const std::string& s) { std::string r = "\""; for (char c : s) { if (c == '\\') r += "\\\\"; else r += c; } return r + "\""; };
    out << R"({"server":{"port":)" << proxy_port << R"(,"address":"127.0.0.1"},"control":{"http_port":0,"ws_port":0,"ui_dir":)"
        << q(dir.str("ui")) << R"(,"plugins_dir":)" << q(dir.str("plugins")) << R"(,"sessions_dir":)" << q(dir.str("sessions"))
        << R"(,"templates_file":)" << q(dir.str("templates.json")) << "}}";
    return file;
}

inline bool wait_until(const std::function<bool()>& pred, const std::function<void()>& pump, int timeout_ms = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// Blocking WebSocket test client (masked frames, as a browser would send).
class WsClient {
public:
    std::string handshake_response;

    bool connect(std::uint16_t port, const std::string& origin = {})
    {
        sock_ = control::net::connect_to("127.0.0.1", port);
        if (sock_ == control::net::kInvalid) return false;
        std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
        if (!origin.empty()) req += "Origin: " + origin + "\r\n";
        req += "\r\n";
        control::net::send_all(sock_, req);
        while (handshake_response.find("\r\n\r\n") == std::string::npos) {
            char c;
            if (!read_exact(&c, 1, 2000)) return false;
            handshake_response += c;
        }
        return handshake_response.find("101") != std::string::npos;
    }

    ~WsClient() { control::net::close_socket(sock_); }

    void send_text(const std::string& msg)
    {
        std::string f;
        f += static_cast<char>(0x81);
        const unsigned char mask[4] = { 1, 2, 3, 4 };
        if (msg.size() < 126) f += static_cast<char>(0x80 | msg.size());
        else { f += static_cast<char>(0x80 | 126); f += static_cast<char>(msg.size() >> 8); f += static_cast<char>(msg.size() & 0xFF); }
        for (auto m : mask) f += static_cast<char>(m);
        for (std::size_t i = 0; i < msg.size(); ++i) f += static_cast<char>(msg[i] ^ mask[i % 4]);
        control::net::send_all(sock_, f);
    }

    // Returns false on timeout/close.
    bool recv_text(std::string& out, int timeout_ms)
    {
        unsigned char h[2];
        if (!read_exact(reinterpret_cast<char*>(h), 2, timeout_ms)) return false;
        std::uint64_t len = h[1] & 0x7F;
        if (len == 126) { unsigned char e[2]; if (!read_exact(reinterpret_cast<char*>(e), 2, 1000)) return false; len = (e[0] << 8) | e[1]; }
        out.assign(len, '\0');
        if (len && !read_exact(out.data(), len, 1000)) return false;
        last_opcode = h[0] & 0x0F;
        return true;
    }

    int last_opcode{ 0 };

private:
    bool read_exact(char* buf, std::size_t n, int timeout_ms)
    {
        std::size_t got = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < n) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            if (control::net::wait(sock_, false, static_cast<int>(left)) <= 0) continue;
            const auto r = control::net::recv_some(sock_, buf + got, n - got);
            if (r == 0 || r == -1) return false;
            if (r > 0) got += static_cast<std::size_t>(r);
        }
        return true;
    }

    control::net::socket_t sock_{ control::net::kInvalid };
};
}
