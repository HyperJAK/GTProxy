#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "event_hub.hpp"
#include "packet_codec.hpp"
#include "../event/event.hpp"
#include "../network/connection.hpp"
#include "../network/wire_tap.hpp"

namespace control {
class PacketFlow;

enum class HookAction { Pass, Drop };

// Implemented by the plugin manager. Called on the proxy main thread only.
class IPacketHooks {
public:
    virtual ~IPacketHooks() = default;
    // `bytes` may be modified in place. `name` is the decoded packet label.
    virtual HookAction on_packet(PacketFlow& flow, event::Direction dir, std::uint64_t id, const DecodedPacket& decoded, std::vector<std::byte>& bytes) = 0;
};

struct InterceptRule {
    std::string direction{ "any" }; // any | client_bound | server_bound
    std::string match;              // case-insensitive substring of name/summary; empty = everything
    std::string kind;               // optional: text | variant | game | hello | raw
};

struct InterceptConfig {
    bool enabled{ false };
    std::vector<InterceptRule> rules; // empty with enabled=true holds every packet
};

struct FlowStats {
    std::uint64_t packets[2]{ 0, 0 }; // indexed by event::Direction
    std::uint64_t bytes[2]{ 0, 0 };
    std::uint64_t dropped{ 0 };
    std::uint64_t edited{ 0 };
    std::uint64_t injected{ 0 };
    std::uint64_t held_total{ 0 };
    std::uint64_t consumed{ 0 };
    std::uint64_t write_failures{ 0 };
};

struct ReleaseResult {
    bool ok{ false };
    std::string error;
};

// The packet pipeline. Owns no sockets: it writes through two IConnections
// (`to_game_client`, `to_upstream`) so it can be driven by real ENet peers or
// by test fakes. Not thread-safe: call only from the proxy main thread
// (other threads go through MainThreadQueue).
class PacketFlow final {
public:
    using Clock = std::function<std::int64_t()>; // microseconds since Unix epoch

    PacketFlow(EventHub& hub, network::IConnection& to_game_client, network::IConnection& to_upstream, Clock clock = {});

    void set_hooks(IPacketHooks* hooks) { hooks_ = hooks; }

    // --- receive path -----------------------------------------------------
    // Wire taps for network::Server (receives ServerBound) and
    // network::Client (receives ClientBound).
    [[nodiscard]] network::IWireTap* server_tap() { return &server_tap_; }
    [[nodiscard]] network::IWireTap* client_tap() { return &client_tap_; }

    void begin_receive(event::Direction dir, std::span<const std::byte> data);
    void end_receive();
    // Called by ForwardingHandler for packets that survived the typed handlers.
    void forward(event::Direction dir, std::span<const std::byte> data);
    // begin + forward + end in one call (tests, replay).
    void process(event::Direction dir, std::span<const std::byte> data);

    // --- control ------------------------------------------------------------
    // dir = the way the packet travels: ClientBound goes to the game client,
    // ServerBound goes to the upstream server. Returns the event id (0 on failure).
    std::uint64_t inject(event::Direction dir, std::span<const std::byte> data, const std::string& origin = "inject");

    void set_intercept(const InterceptConfig& cfg, bool flush_forward = true);
    [[nodiscard]] const InterceptConfig& intercept() const { return intercept_; }
    ReleaseResult release(std::uint64_t id, bool drop, std::optional<std::vector<std::byte>> replacement = std::nullopt);
    std::size_t release_all(bool drop);
    [[nodiscard]] std::size_t held_count() const { return held_.size(); }
    [[nodiscard]] std::string intercept_json() const;

    [[nodiscard]] const FlowStats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

    [[nodiscard]] std::string packet_json(std::uint64_t id, event::Direction dir, std::int64_t ts, std::span<const std::byte> bytes,
        const DecodedPacket& decoded, std::string_view origin, std::string_view state) const;

    [[nodiscard]] std::int64_t now_us() const { return clock_(); }

private:
    struct Held {
        std::uint64_t id;
        event::Direction dir;
        std::vector<std::byte> bytes;
        std::string name;
        std::string summary;
        std::int64_t since_us;
        bool by_rule; // false = parked only to preserve ordering
    };

    struct Tap final : network::IWireTap {
        PacketFlow& flow;
        event::Direction recv_dir;
        Tap(PacketFlow& f, event::Direction d) : flow{ f }, recv_dir{ d } { }
        void on_receive_begin(std::span<const std::byte> data) override { flow.begin_receive(recv_dir, data); }
        void on_receive_end() override { flow.end_receive(); }
        void on_write(std::span<const std::byte> data, int) override
        {
            flow.note_written(recv_dir == event::Direction::ClientBound ? event::Direction::ServerBound : event::Direction::ClientBound, data);
        }
    };

    struct Current {
        std::uint64_t id{ 0 };
        event::Direction dir{ event::Direction::ClientBound };
        bool active{ false };
        bool resolved{ false };
        DecodedPacket decoded;
    };

    void note_written(event::Direction toward, std::span<const std::byte> data);
    void publish_verdict(std::uint64_t id, std::string_view state, std::string_view by, const std::vector<std::byte>* bytes);
    bool write_toward(event::Direction dir, std::span<const std::byte> data);
    [[nodiscard]] bool rule_matches(event::Direction dir, const DecodedPacket& d) const;
    void publish_intercept();
    void advance(event::Direction dir);
    [[nodiscard]] bool queue_has(event::Direction dir) const;
    std::uint64_t publish_new(event::Direction dir, std::span<const std::byte> data, const DecodedPacket& decoded, std::string_view origin, std::string_view state);

    EventHub& hub_;
    network::IConnection& to_game_client_;
    network::IConnection& to_upstream_;
    Clock clock_;
    IPacketHooks* hooks_{ nullptr };

    Tap server_tap_{ *this, event::Direction::ServerBound };
    Tap client_tap_{ *this, event::Direction::ClientBound };

    std::uint64_t next_id_{ 0 };
    Current current_;
    bool suppress_observer_{ false };

    InterceptConfig intercept_;
    std::deque<Held> held_;
    FlowStats stats_;

    static constexpr std::size_t kMaxHeld = 1000;
};

[[nodiscard]] const char* direction_name(event::Direction d);
}
