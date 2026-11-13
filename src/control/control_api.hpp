#pragma once
#include <chrono>
#include <functional>
#include <string>

#include "event_hub.hpp"
#include "json_value.hpp"
#include "packet_flow.hpp"
#include "plugin_admin.hpp"
#include "session.hpp"
#include "template_store.hpp"
#include "../core/config.hpp"
#include "../network/connection.hpp"

namespace control {
struct ApiReply {
    bool ok{ true };
    int http_status{ 200 };
    std::string body; // full JSON document: {"ok":true,"result":...} or {"ok":false,"error":"..."}
};

// Every operation the UI (WebSocket or REST) can ask for. One implementation
// serves both transports. NOT thread-safe: run on the proxy main thread.
class ControlApi {
public:
    ControlApi(
        core::Config& config,
        EventHub& hub,
        PacketFlow& flow,
        network::IConnection& game_client,
        network::IConnection& upstream,
        SessionRecorder& recorder,
        TemplateStore& templates
    );

    void set_plugin_admin(IPluginAdmin* admin) { plugins_ = admin; }
    void set_replay_hooks(IPacketHooks* hooks) { replay_hooks_ = hooks; }
    void set_ws_client_counter(std::function<std::size_t()> fn) { ws_clients_ = std::move(fn); }
    // Called after the active profile changed (so the core can drop connections etc.).
    void set_on_profile_changed(std::function<void()> fn) { on_profile_changed_ = std::move(fn); }

    [[nodiscard]] ApiReply handle(const std::string& op, const JsonValue& params);
    [[nodiscard]] std::string status_json() const;
    [[nodiscard]] static std::vector<std::string> operations();

private:
    ApiReply op_inject(const JsonValue& p);
    ApiReply op_intercept_set(const JsonValue& p);
    ApiReply op_intercept_release(const JsonValue& p, bool drop);
    ApiReply op_templates_save(const JsonValue& p);
    ApiReply op_profile_save(const JsonValue& p);
    ApiReply op_plugins(const std::string& op, const JsonValue& p);
    ApiReply op_session(const std::string& op, const JsonValue& p);
    [[nodiscard]] std::string profiles_json() const;

    core::Config& config_;
    EventHub& hub_;
    PacketFlow& flow_;
    network::IConnection& game_client_;
    network::IConnection& upstream_;
    SessionRecorder& recorder_;
    TemplateStore& templates_;
    IPluginAdmin* plugins_{ nullptr };
    IPacketHooks* replay_hooks_{ nullptr };
    std::function<std::size_t()> ws_clients_;
    std::function<void()> on_profile_changed_;
    std::chrono::steady_clock::time_point started_{ std::chrono::steady_clock::now() };
};

[[nodiscard]] ApiReply api_ok(std::string result_json = "null");
[[nodiscard]] ApiReply api_error(int http_status, std::string_view message);
}
