#include "rest_server.hpp"

#include <cstdlib>
#include <spdlog/spdlog.h>

#include "json_writer.hpp"
#include "origin_policy.hpp"

namespace control {
RestServer::RestServer(std::string bind_addr, const std::uint16_t port, std::filesystem::path ui_dir, Executor executor)
    : bind_addr_{ std::move(bind_addr) }, port_{ port }, ui_dir_{ std::move(ui_dir) }, exec_{ std::move(executor) }
{
    setup_routes();
}

RestServer::~RestServer() { stop(); }

void RestServer::setup_routes()
{
    server_.set_payload_max_length(1u << 20);
    server_.set_keep_alive_max_count(50);

    // Origin + Host guard, applied to everything including static files.
    server_.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        const auto origin = req.get_header_value("Origin");
        const auto host = req.get_header_value("Host");
        if (!origin_allowed(origin) || (!host.empty() && !is_loopback_host(host))) {
            res.status = 403;
            res.set_content(R"({"ok":false,"error":"forbidden origin"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!origin.empty()) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Vary", "Origin");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        }
        if (req.method == "OPTIONS") {
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    const auto respond = [this](httplib::Response& res, const std::string& op, const JsonValue& params) {
        const auto reply = exec_(op, params);
        res.status = reply.http_status;
        res.set_content(reply.body, "application/json");
    };

    // Convenience GETs
    const std::pair<const char*, const char*> gets[] = {
        { "/api/status", "status" }, { "/api/profiles", "profiles.list" }, { "/api/plugins", "plugins.list" },
        { "/api/templates", "templates.list" }, { "/api/sessions", "sessions.list" }, { "/api/intercept", "intercept.get" },
    };
    for (const auto& [path, op] : gets) {
        server_.Get(path, [respond, op = std::string{ op }](const httplib::Request&, httplib::Response& res) {
            respond(res, op, JsonValue{});
        });
    }
    server_.Get("/api/packets", [respond](const httplib::Request& req, httplib::Response& res) {
        JsonValue p;
        p.type = JsonValue::Type::Object;
        JsonValue limit;
        limit.type = JsonValue::Type::Number;
        limit.number = req.has_param("limit") ? std::atof(req.get_param_value("limit").c_str()) : 0;
        p.object.emplace_back("limit", limit);
        respond(res, "packets.backlog", p);
    });
    server_.Get("/api/ops", [](const httplib::Request&, httplib::Response& res) {
        JsonWriter w;
        w.begin_array();
        for (const auto& o : ControlApi::operations()) w.value(std::string_view{ o });
        w.end_array();
        res.set_content(w.take(), "application/json");
    });

    // Generic call: POST /api/call/<op> with a JSON object body.
    server_.Post(R"(/api/call/([a-z_.]+))", [respond](const httplib::Request& req, httplib::Response& res) {
        JsonValue params;
        if (!req.body.empty()) {
            auto parsed = JsonValue::parse(req.body);
            if (!parsed || !parsed->is_object()) {
                res.status = 400;
                res.set_content(R"({"ok":false,"error":"body must be a JSON object"})", "application/json");
                return;
            }
            params = std::move(*parsed);
        }
        respond(res, req.matches[1], params);
    });

    if (std::filesystem::is_directory(ui_dir_)) {
        server_.set_mount_point("/", ui_dir_.string());
        spdlog::info("Serving UI from {}", ui_dir_.string());
    }
}

std::string RestServer::start()
{
    if (port_ == 0) {
        const int any = server_.bind_to_any_port(bind_addr_);
        if (any < 0) {
            return fmt::format("cannot bind REST API to {}", bind_addr_);
        }
        port_ = static_cast<std::uint16_t>(any);
    } else if (!server_.bind_to_port(bind_addr_, port_)) {
        return fmt::format("cannot bind REST API to {}:{} (port in use?)", bind_addr_, port_);
    }
    running_ = true;
    thread_ = std::thread{ [this] { server_.listen_after_bind(); } };
    spdlog::info("Control-plane REST listening on http://{}:{}", bind_addr_, port_);
    return {};
}

void RestServer::stop()
{
    if (!running_.exchange(false)) return;
    server_.stop();
    if (thread_.joinable()) thread_.join();
}
}
