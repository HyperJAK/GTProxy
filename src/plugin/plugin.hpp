#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "../control/event_hub.hpp"
#include "../control/packet_flow.hpp"
#include "../control/plugin_admin.hpp"

namespace plugin {
struct Limits {
    std::size_t memory_bytes{ 32u * 1024 * 1024 }; // per plugin
    std::uint64_t hook_instructions{ 2'000'000 };  // per hook call
    std::chrono::milliseconds hook_time{ 50 };      // per hook call
    std::chrono::milliseconds load_time{ 500 };     // running main.lua
    std::size_t injects_per_call{ 32 };
    std::uint32_t max_consecutive_errors{ 10 };     // then the plugin is switched off
};

// One plugin = one isolated Lua state with a restricted standard library, a
// memory cap and an execution budget. Every call into Lua is protected: a
// plugin that errors, loops forever or exhausts memory is stopped without
// affecting the proxy or other plugins. Main-thread only.
class Plugin {
public:
    Plugin(std::string id, std::filesystem::path dir, control::EventHub& hub, Limits limits = {});
    ~Plugin();

    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    // Returns an error message, or empty on success.
    std::string load();
    void unload();

    [[nodiscard]] bool loaded() const { return L_ != nullptr; }
    // True once the plugin hit max_consecutive_errors; the manager then disables it.
    [[nodiscard]] bool tripped() const { return tripped_; }

    control::HookAction on_packet(control::PacketFlow& flow, event::Direction dir, std::uint64_t id,
        const control::DecodedPacket& decoded, std::vector<std::byte>& bytes);
    void on_connection(control::PacketFlow& flow, std::string_view peer, bool connected);

    [[nodiscard]] control::PluginInfo info() const;
    [[nodiscard]] std::vector<control::PluginLogLine> logs(std::size_t limit) const;
    void log(std::string_view level, std::string_view message);

    [[nodiscard]] const std::string& id() const { return id_; }

    // Used by the Lua budget hook; not part of the public contract.
    bool budget_exceeded();

private:
    struct Mem {
        std::size_t used{ 0 };
        std::size_t limit{ 0 };
    };
    struct LuaPacket;

    static void* limited_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);
    void read_manifest();
    void setup_sandbox();
    void setup_api();
    void begin_budget(std::chrono::milliseconds time);
    bool protected_call(const char* hook, const std::function<sol::protected_function_result(sol::protected_function&)>& invoke, bool* returned_false = nullptr);
    void record_error(std::string_view where, std::string_view what);

    std::string id_;
    std::filesystem::path dir_;
    control::EventHub& hub_;
    Limits limits_;

    Mem mem_;
    lua_State* L_{ nullptr };
    std::unique_ptr<sol::state_view> lua_;

    std::string name_, version_, description_;
    std::string last_error_;
    std::uint64_t errors_{ 0 }, packets_{ 0 };
    std::uint32_t consecutive_errors_{ 0 };
    bool tripped_{ false };

    std::chrono::steady_clock::time_point deadline_;
    std::uint64_t instructions_left_{ 0 };
    control::PacketFlow* current_flow_{ nullptr };
    std::size_t injects_this_call_{ 0 };

    std::deque<control::PluginLogLine> log_;
    static constexpr std::size_t kLogCap = 500;
};
}
