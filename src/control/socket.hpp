#pragma once
// Thin cross-platform wrapper over BSD / Winsock sockets, used by the
// WebSocket server and by the test client. Windows path is compiled but
// untested in the authoring sandbox.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace control::net {
#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalid = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalid = -1;
#endif

inline void init()
{
#ifdef _WIN32
    static const bool once = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)once;
#endif
}

inline void close_socket(socket_t s)
{
    if (s == kInvalid) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

inline bool set_non_blocking(socket_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Wait until readable (or writable). Returns >0 ready, 0 timeout, <0 error.
inline int wait(socket_t s, bool for_write, int timeout_ms)
{
    pollfd fd{};
    fd.fd = s;
    fd.events = for_write ? POLLOUT : POLLIN;
#ifdef _WIN32
    return WSAPoll(&fd, 1, timeout_ms);
#else
    return ::poll(&fd, 1, timeout_ms);
#endif
}

// Returns bytes read, 0 on orderly close, -1 on error, -2 if it would block.
inline long recv_some(socket_t s, char* buf, std::size_t len)
{
    const auto n = ::recv(s, buf, static_cast<int>(len), 0);
    if (n >= 0) return static_cast<long>(n);
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK ? -2 : -1;
#else
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -2 : -1;
#endif
}

inline bool send_all(socket_t s, const char* data, std::size_t len, int timeout_ms = 3000)
{
#ifdef _WIN32
    constexpr int flags = 0;
#elif defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    std::size_t sent = 0;
    while (sent < len) {
        const auto n = ::send(s, data + sent, static_cast<int>(len - sent), flags);
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        if (n < 0) {
#ifdef _WIN32
            const bool would_block = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool would_block = errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
            if (!would_block) return false;
        }
        if (wait(s, true, timeout_ms) <= 0) return false;
    }
    return true;
}

inline bool send_all(socket_t s, std::string_view data, int timeout_ms = 3000)
{
    return send_all(s, data.data(), data.size(), timeout_ms);
}

// Listening socket bound to bind_addr:port (port 0 = ephemeral). Returns the
// socket and writes the actual port; kInvalid on failure.
inline socket_t listen_on(const std::string& bind_addr, std::uint16_t port, std::uint16_t& actual_port)
{
    init();
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalid) return kInvalid;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1 ||
        ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 16) != 0) {
        close_socket(s);
        return kInvalid;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    actual_port = ntohs(addr.sin_port);
    set_non_blocking(s);
    return s;
}

inline socket_t connect_to(const std::string& host, std::uint16_t port)
{
    init();
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalid) return kInvalid;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        return kInvalid;
    }
    return s;
}
}
