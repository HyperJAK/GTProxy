#pragma once
#include <chrono>
#include <memory>
#include <vector>

#include "control_api.hpp"
#include "event_hub.hpp"
#include "main_thread_queue.hpp"
#include "packet_flow.hpp"
#include "rest_server.hpp"
#include "session.hpp"
#include "template_store.hpp"
#include "ws_server.hpp"
#include "../core/config.hpp"
#include "../event/event.hpp"
#include "../network/connection.hpp"

namespace plugin { class PluginManager; }
namespace world { class WorldInspector; }

namespace control {
// Owns the whole control plane: event hub, packet pipeline, API, REST and
// WebSocket servers, session recorder, template store and the plugin manager.
// Construct and poll() on the proxy main thread.
class ControlPlane {
public:
    // game_client: the connection facing the Growtopia-compatible client
    // (network::Server); upstream: the connection facing your server (network::Client).
    ControlPlane(core::Config& config, network::IConnection& game_client, network::IConnection& upstream);
    ~ControlPlane(); // out of line: WorldInspector is incomplete here

    ControlPlane(const ControlPlane&) = delete;
    ControlPlane& operator=(const ControlPlane&) = delete;

    // Publish connection lifecycle events from the proxy's dispatcher.
    void attach(event::Dispatcher& dispatcher);

    // Call once per main-loop iteration.
    void poll();

    [[nodiscard]] PacketFlow& flow() { return flow_; }
    [[nodiscard]] EventHub& hub() { return hub_; }
    [[nodiscard]] ControlApi& api() { return api_; }
    [[nodiscard]] plugin::PluginManager* plugins() { return plugins_.get(); }
    [[nodiscard]] std::uint16_t rest_port() const { return rest_ ? rest_->port() : 0; }
    [[nodiscard]] std::uint16_t ws_port() const { return ws_ ? ws_->port() : 0; }

private:
    void publish_connection(std::string_view peer, bool connected);
    [[nodiscard]] std::string handle_ws_message(const std::string& text);

    core::Config& config_;
    network::IConnection& game_client_;
    network::IConnection& upstream_;

    EventHub hub_;
    MainThreadQueue queue_;
    PacketFlow flow_;
    SessionRecorder recorder_;
    TemplateStore templates_;
    ControlApi api_;
    std::unique_ptr<plugin::PluginManager> plugins_;
    std::unique_ptr<world::WorldInspector> worlds_;
    std::unique_ptr<RestServer> rest_;
    std::unique_ptr<WsServer> ws_;
    std::vector<event::ScopedHandle> handles_;
    std::chrono::steady_clock::time_point last_status_{};
};
}
