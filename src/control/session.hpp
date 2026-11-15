#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "event_hub.hpp"
#include "packet_flow.hpp"
#include "../event/event.hpp"

namespace control {
// A recorded session is a JSON-Lines file:
//   line 1   {"t":"session","version":1,"started":<us>,"profile":"..."}
//   then     every "packet", "verdict" and "connection" event exactly as it
//            was streamed to the UI (so a file is also a readable log).
class SessionRecorder {
public:
    SessionRecorder(EventHub& hub, std::filesystem::path dir);
    ~SessionRecorder();

    // Returns an error message, or empty on success. `name` is sanitised.
    std::string start(const std::string& name, const std::string& profile);
    std::string stop();

    [[nodiscard]] bool active() const;
    [[nodiscard]] std::string current_name() const;
    [[nodiscard]] std::uint64_t lines_written() const;

    struct Info {
        std::string name;
        std::uint64_t bytes{ 0 };
    };
    [[nodiscard]] std::vector<Info> list() const;
    [[nodiscard]] std::filesystem::path path_for(const std::string& name) const;
    [[nodiscard]] static bool valid_name(const std::string& name);

private:
    void on_event(const std::string& json);

    EventHub& hub_;
    std::filesystem::path dir_;
    std::uint64_t subscription_{ 0 };
    mutable std::mutex mutex_;
    std::ofstream file_;
    std::string name_;
    std::uint64_t lines_{ 0 };
};

struct RecordedPacket {
    std::uint64_t id{ 0 };
    event::Direction dir{ event::Direction::ClientBound };
    std::vector<std::byte> bytes;
    std::string origin;
    std::int64_t ts{ 0 };
};

struct SessionData {
    std::vector<RecordedPacket> packets;
    std::size_t skipped_lines{ 0 };
    std::string error;
};

[[nodiscard]] SessionData load_session(const std::filesystem::path& file);

struct ReplayOutput {
    event::Direction dir;
    std::vector<std::byte> bytes;
};

struct ReplayOutcome {
    std::uint64_t id{ 0 };
    event::Direction dir{ event::Direction::ClientBound };
    std::vector<std::byte> input;
    std::vector<ReplayOutput> outputs; // everything written as a result, in order
    bool dropped{ false };
};

// Feed the recorded wire packets (origin == "wire") through a private
// PacketFlow with capture-only connections. Hooks (plugins) run for real;
// nothing is sent to live peers. Deterministic given the same hooks.
[[nodiscard]] std::vector<ReplayOutcome> replay_session(const SessionData& session, IPacketHooks* hooks);
}
