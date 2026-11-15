#include "web_server.hpp"

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <enet/enet.h>

#include "../utils/network.hpp"
#include "../utils/text_parse.hpp"
// ReSharper disable once CppUnusedIncludeDirective
#include "../utils/formatter/text_parse_formatter.hpp"

namespace core {
WebServer::WebServer(
    Config& config,
    event::Dispatcher& dispatcher,
    network::Client& client,
    network::Server& server
)
    : config_{ config }
    , dispatcher_{ dispatcher }
    , client_{ client }
    , server_{ server }
    , dns_resolver_{ network::create_dns_provider(
        config_.get_client_config().dns_server == "system" ? "google" : config_.get_client_config().dns_server
    ) }
    , https_server_{ "./resources/cert.pem", "./resources/key.pem" }
    , pending_port_{ 65535 }
{
    setup_server();

    if (!https_server_.bind_to_port("0.0.0.0", 443)) {
        spdlog::error("Failed to bind to port 443");
        return;
    }

    dispatcher_.append_listener(event::Type::ClientConnect, [this](const event::Event& e) { on_client_connect(e); });
    dispatcher_.append_listener(event::Type::ClientDisconnect, [this](const event::Event& e) { on_client_disconnect(e); });

    spdlog::info("HTTPS server listening on port 443");
    server_thread_ = std::thread{ &WebServer::listen_internal, this };
}

WebServer::~WebServer()
{
    https_server_.stop();
    server_thread_.join();
}

std::string WebServer::resolve_upstream(const std::string& host) const
{
    if (host == "localhost") {
        return "127.0.0.1";
    }

    if (utils::network::classify_host(host) != utils::network::HostType::Hostname) {
        return host;
    }

    if (config_.get_client_config().dns_server == "system") {
        ENetAddress address{};
        if (enet_address_set_host(&address, host.c_str()) != 0) {
            spdlog::error("System DNS could not resolve {}", host);
            return {};
        }

        return utils::network::format_ip_address(address.host);
    }

    return dns_resolver_.resolve_ip(host);
}

void WebServer::setup_server()
{
    https_server_.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        spdlog::info("{} {} {}", req.method, req.path, res.status);
    });

    https_server_.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            fmt::format("{} ({})", httplib::status_message(res.status), res.status),
            "text/plain"
        );
    });

    https_server_.set_exception_handler([](const httplib::Request&, httplib::Response& res, const std::exception_ptr& ep) {
        res.status = 500;
        try {
            std::rethrow_exception(ep);
        }
        catch (const std::exception& e) {
            res.set_content(fmt::format("{}", e.what()), "text/plain");
        }
        catch (...) { // This should never happen, but just in case
            res.set_content("Unknown Exception", "text/plain");
        }
    });
}

void WebServer::listen_internal()
{
    https_server_.Post("/growtopia/server_data.php", [this](
        const httplib::Request& req,
        httplib::Response& res
    ) -> bool {
        if (!req.headers.empty()) {
            spdlog::info("Headers:");
            for (const auto& [key, value] : req.headers) {
                spdlog::info("\t{}: {}", key, value);
            }
        }

        if (!req.params.empty()) {
            spdlog::info("Params:");
            spdlog::info("\t{}", httplib::detail::params_to_query_str(req.params));
        }

        if (!req.body.empty()) {
            spdlog::info("Body:");
            spdlog::info("\t{}", req.body);
        }

        const auto profile{ config_.get_active_profile() };
        if (!profile) {
            res.status = 500;
            res.set_content("No active server profile", "text/plain");
            return true;
        }

        if (profile->direct) {
            // Direct profile: do not fetch server_data.php upstream. Point the
            // client at this proxy and remember the ENet endpoint to connect to.
            // NOTE: this body is the minimum needed to redirect; it is untested
            // against a real client (see ARCHITECTURE.md, Assumptions).
            pending_address_ = profile->enet_address;
            pending_port_ = static_cast<uint16_t>(profile->enet_port);
            spdlog::info("Profile \"{}\" is direct: ENet target {}:{}", profile->name, pending_address_, pending_port_);

            res.set_content(
                fmt::format(
                    "server|127.0.0.1\nport|{}\ntype|1\ntype2|1\n",
                    config_.get_server_config().port
                ),
                "text/html"
            );
            return true;
        }

        const std::string server_address{ profile->address };
        const std::string resolved_ip{ resolve_upstream(server_address) };

        if (resolved_ip.empty()) {
            res.status = 500;
            res.set_content("Failed to resolve server address", "text/plain");
            return true;
        }

        httplib::Client cli{ fmt::format("{}://{}:{}", profile->tls ? "https" : "http", resolved_ip, profile->port) };
        cli.enable_server_certificate_verification(false); // private servers commonly use self-signed certs
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);

        httplib::Headers headers{
            { "User-Agent", req.get_header_value("User-Agent") },
            { "Host", server_address }
        };

        httplib::Result response{ cli.Post("/growtopia/server_data.php", headers, req.params) };
        if (!response) {
            spdlog::error(
                "Response is null: httplib::Error::{}",
                magic_enum::enum_name(response.error())
            );

            res.status = 502;
            res.set_content("Failed to get response from server", "text/plain");
            return true;
        }

        if (response.error() != httplib::Error::Success || response->status != 200) {
            spdlog::error(
                "Failed to get response: {}",
                response.error() == httplib::Error::Success
                    ? fmt::format("HTTP status: {}", response->status)
                    : fmt::format("HTTP error: {}", httplib::to_string(response.error()))
            );

            res.status = 502;
            res.set_content("Failed to get response from server", "text/plain");
            return true;
        }

        if (response->body.empty()) {
            res.status = 502;
            res.set_content("Empty response from server", "text/plain");
            return true;
        }

        utils::TextParse text_parse{ response->body };
        if (text_parse.empty()) {
            spdlog::error("Failed to parse server_data.php response");
            res.status = 500;
            return true;
        }

        spdlog::info("Original server_data.php response:\n{}", text_parse);

        pending_address_ = text_parse.get("server", 0);

        std::string port{ text_parse.get("port", 0) };
        if (
            auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), pending_port_);
            ec != std::errc{}
        ) {
            spdlog::error("Failed to parse port from server_data.php response");
            res.status = 500;
            return true;
        }

        text_parse.set("server", { "127.0.0.1" });
        text_parse.set("port", { std::to_string(config_.get_server_config().port) });
        text_parse.set("type2", { "1" });

        spdlog::debug("Modified server_data.php response:\n{}", text_parse);

        res.set_content(text_parse.get_raw(), "text/html");
        return true;
    });

    https_server_.listen_after_bind();
}

void WebServer::on_client_connect(const event::Event& e)
{
    if (pending_port_ == 65535) {
        return;
    }

    spdlog::info("Connecting to Growtopia server at {}:{}", pending_address_, pending_port_);

    client_.connect(pending_address_, pending_port_);

    pending_address_.clear();
    pending_port_ = 65535;
}

void WebServer::on_client_disconnect(const event::Event& e)
{
    pending_address_.clear();
    pending_port_ = 65535;
}
}
