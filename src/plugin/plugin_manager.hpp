#pragma once
#include <filesystem>
#include <string_view>

#include "../control/event_hub.hpp"
#include "../control/packet_flow.hpp"
#include "../control/plugin_admin.hpp"

namespace plugin {
// Placeholder until the plugin runtime lands (phase 5): loads nothing.
class PluginManager final : public control::IPacketHooks, public control::IPluginAdmin {
public:
    PluginManager(std::filesystem::path dir, control::EventHub& hub);

    control::HookAction on_packet(control::PacketFlow&, event::Direction, std::uint64_t, const control::DecodedPacket&, std::vector<std::byte>&) override
    {
        return control::HookAction::Pass;
    }
    void on_connection(control::PacketFlow&, std::string_view, bool) { }
    void poll() { }

    [[nodiscard]] std::vector<control::PluginInfo> list() const override { return {}; }
    std::string set_enabled(const std::string&, bool) override { return "plugin runtime not available"; }
    std::string reload(const std::string&) override { return "plugin runtime not available"; }
    std::string rescan() override { return {}; }
    [[nodiscard]] std::vector<control::PluginLogLine> logs(const std::string&, std::size_t) const override { return {}; }

private:
    std::filesystem::path dir_;
    control::EventHub& hub_;
};
}
