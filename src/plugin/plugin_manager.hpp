#pragma once
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "plugin.hpp"

namespace plugin {
// Discovers plugins in <dir>/<name>/main.lua, tracks which are enabled
// (<dir>/state.json) and runs their hooks in alphabetical order.
// New plugins start DISABLED: nothing runs until you switch it on.
class PluginManager final : public control::IPacketHooks, public control::IPluginAdmin {
public:
    PluginManager(std::filesystem::path dir, control::EventHub& hub, Limits limits = {});

    control::HookAction on_packet(control::PacketFlow& flow, event::Direction dir, std::uint64_t id,
        const control::DecodedPacket& decoded, std::vector<std::byte>& bytes) override;
    void on_connection(control::PacketFlow& flow, std::string_view peer, bool connected);
    void poll() { }

    [[nodiscard]] std::vector<control::PluginInfo> list() const override;
    std::string set_enabled(const std::string& name, bool enabled) override;
    std::string reload(const std::string& name) override;
    std::string rescan() override;
    [[nodiscard]] std::vector<control::PluginLogLine> logs(const std::string& name, std::size_t limit) const override;

private:
    struct Entry {
        std::unique_ptr<Plugin> plugin;
        bool enabled{ false };
    };
    Entry* find(const std::string& name);
    [[nodiscard]] const Entry* find(const std::string& name) const;
    void save_state() const;
    void publish_state();
    void enforce_trips();

    std::filesystem::path dir_;
    control::EventHub& hub_;
    Limits limits_;
    std::vector<Entry> entries_; // sorted by id
};
}
