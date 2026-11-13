#include "control_plane.hpp"

#include <spdlog/spdlog.h>

#include "json_dump.hpp"
#include "json_writer.hpp"
#include "origin_policy.hpp"
#include "../plugin/plugin_manager.hpp"

namespace control {
ControlPlane::ControlPlane(core::Config& config, network::IConnection& game_client, network::IConnection& upstream)
    : config_{ config }
    , game_client_{ game_client }
    , upstream_{ upstream }
    , flow_{ hub_, game_client, upstream }
    , recorder_{ hub_, config.get_control_config().sessions_dir }
    , templates_{ config.get_control_config().templates_file }
    , api_{ config, hub_, flow_, game_client, upstream, recorder_, templates_ }
{
    const auto cfg = config.get_control_config();

    plugins_ = std::make_unique<plugin::PluginManager>(cfg.plugins_dir, hub_);
    flow_.set_hooks(plugins_.get());
    api_.set_plugin_admin(plugins_.get());
    api_.set_replay_hooks(plugins_.get());
    plugins_->rescan();

    if (!cfg.enabled) {
        spdlog::info("Control plane disabled in config (control.enabled = false)");
        return;
    }

    auto bind = cfg.bind;
    if (!cfg.allow_remote && !is_loopback_host(bind)) {
        spdlog::warn("control.bind \"{}\" is not loopback and control.allow_remote is false; using 127.0.0.1", bind);
        bind = "127.0.0.1";
    }

    const auto exec = [this](const std::string& op, const JsonValue& params) -> ApiReply {
        auto reply = queue_.call([&] { return api_.handle(op, params); });
        return reply ? std::move(*reply) : api_error(504, "the proxy main loop did not answer in time");
    };

    rest_ = std::make_unique<RestServer>(bind, static_cast<std::uint16_t>(cfg.http_port), cfg.ui_dir, exec);
    if (const auto err = rest_->start(); !err.empty()) {
        spdlog::error("Control-plane REST disabled: {}", err);
        rest_.reset();
    }

    ws_ = std::make_unique<WsServer>(bind, static_cast<std::uint16_t>(cfg.ws_port), hub_,
        [this](const std::string& text) { return handle_ws_message(text); });
    if (const auto err = ws_->start(); !err.empty()) {
        spdlog::error("Control-plane WebSocket disabled: {}", err);
        ws_.reset();
    }

    api_.set_ws_client_counter([this] { return ws_ ? ws_->client_count() : 0; });
}

ControlPlane::~ControlPlane()
{
    // Stop the threads first, then fail any call still waiting on the queue.
    ws_.reset();
    rest_.reset();
    queue_.close();
}

void ControlPlane::attach(event::Dispatcher& dispatcher)
{
    const auto add = [&](const event::Type type, const std::string_view peer, const bool connected) {
        handles_.emplace_back(dispatcher, type, dispatcher.append_listener(type, [this, peer, connected](const event::Event&) {
            publish_connection(peer, connected);
        }, event::Priority::Highest));
    };
    add(event::Type::ClientConnect, "game_client", true);
    add(event::Type::ClientDisconnect, "game_client", false);
    add(event::Type::ServerConnect, "upstream", true);
    add(event::Type::ServerDisconnect, "upstream", false);
}

void ControlPlane::publish_connection(const std::string_view peer, const bool connected)
{
    JsonWriter w;
    w.begin_object().field("t", "connection").field("peer", peer).field("state", connected ? "connected" : "disconnected")
        .field("ts", flow_.now_us()).end_object();
    hub_.publish(w.take(), true);
    if (plugins_) {
        plugins_->on_connection(flow_, peer, connected);
    }
}

void ControlPlane::poll()
{
    queue_.drain();
    if (plugins_) {
        plugins_->poll();
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_status_ >= std::chrono::seconds(1)) {
        last_status_ = now;
        hub_.publish(api_.status_json(), false);
    }
}

std::string ControlPlane::handle_ws_message(const std::string& text)
{
    // {"id": <any>, "op": "inject", "params": {...}}  ->  {"t":"reply","id":<same>,"ok":...}
    const auto msg = JsonValue::parse(text);
    if (!msg || !msg->is_object()) {
        return R"({"t":"reply","id":null,"ok":false,"error":"message must be a JSON object"})";
    }
    const auto* id = msg->find("id");
    const std::string id_json = id ? dump_json(*id) : "null";
    ApiReply reply;
    if (!msg->has("op")) {
        reply = api_error(400, "missing \"op\"");
    } else {
        const auto* params = msg->find("params");
        const JsonValue empty;
        auto r = queue_.call([&] { return api_.handle(msg->str("op"), params ? *params : empty); });
        reply = r ? std::move(*r) : api_error(504, "the proxy main loop did not answer in time");
    }
    // reply.body is {"ok":...}; splice the envelope fields in front.
    return R"({"t":"reply","id":)" + id_json + "," + reply.body.substr(1);
}
}
