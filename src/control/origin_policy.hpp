#pragma once
#include <string_view>

namespace control {
// Browsers always send Origin on WebSocket handshakes and cross-origin
// fetches. The control plane only serves pages loaded from the local machine,
// so any other origin (a random website trying to talk to localhost, or a
// DNS-rebinding page) is refused. Non-browser clients send no Origin.
[[nodiscard]] inline bool is_loopback_host(std::string_view host)
{
    if (host.starts_with('[')) {
        const auto end = host.find(']');
        return end != std::string_view::npos && host.substr(0, end + 1) == "[::1]";
    }
    if (const auto colon = host.find(':'); colon != std::string_view::npos) {
        host = host.substr(0, colon);
    }
    return host == "localhost" || host == "127.0.0.1";
}

[[nodiscard]] inline bool origin_allowed(std::string_view origin)
{
    if (origin.empty()) {
        return true;
    }
    for (const std::string_view scheme : { std::string_view{ "http://" }, std::string_view{ "https://" } }) {
        if (origin.starts_with(scheme)) {
            const auto host = origin.substr(scheme.size());
            return !host.empty() && host.find('/') == std::string_view::npos && is_loopback_host(host);
        }
    }
    return false; // includes the literal "null" origin
}
}
