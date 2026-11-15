#include "plugin_manager.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>

#include "../control/json_value.hpp"
#include "../control/json_writer.hpp"

namespace plugin {
PluginManager::PluginManager(std::filesystem::path dir, control::EventHub& hub, const Limits limits)
    : dir_{ std::move(dir) }, hub_{ hub }, limits_{ limits }
{ }

PluginManager::Entry* PluginManager::find(const std::string& name)
{
    const auto it = std::ranges::find_if(entries_, [&](const Entry& e) { return e.plugin->id() == name; });
    return it == entries_.end() ? nullptr : &*it;
}

const PluginManager::Entry* PluginManager::find(const std::string& name) const
{
    return const_cast<PluginManager*>(this)->find(name);
}

std::string PluginManager::rescan()
{
    std::vector<std::string> enabled_names;
    {
        std::ifstream in{ dir_ / "state.json", std::ios::binary };
        std::stringstream ss;
        ss << in.rdbuf();
        if (const auto doc = control::JsonValue::parse(ss.str())) {
            if (const auto* arr = doc->find("enabled"); arr && arr->is_array()) {
                for (const auto& v : arr->array) if (v.is_string()) enabled_names.push_back(v.string);
            }
        }
    }

    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator{ dir_, ec }) {
        if (e.is_directory() && std::filesystem::exists(e.path() / "main.lua")) {
            found.push_back(e.path().filename().string());
        }
    }
    std::ranges::sort(found);

    // drop plugins whose folder disappeared
    std::erase_if(entries_, [&](const Entry& e) { return !std::ranges::contains(found, e.plugin->id()); });

    for (const auto& id : found) {
        if (find(id)) continue;
        Entry entry;
        entry.plugin = std::make_unique<Plugin>(id, dir_ / id, hub_, limits_);
        entry.enabled = std::ranges::contains(enabled_names, id);
        if (entry.enabled) std::ignore = entry.plugin->load();
        entries_.push_back(std::move(entry));
    }
    std::ranges::sort(entries_, {}, [](const Entry& e) { return e.plugin->id(); });
    publish_state();
    return {};
}

void PluginManager::save_state() const
{
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    control::JsonWriter w;
    w.begin_object().key("enabled").begin_array();
    for (const auto& e : entries_) if (e.enabled) w.value(std::string_view{ e.plugin->id() });
    w.end_array().end_object();
    std::ofstream{ dir_ / "state.json", std::ios::binary | std::ios::trunc } << w.str();
}

void PluginManager::publish_state()
{
    hub_.publish(R"({"t":"plugin_state"})", false);
}

std::vector<control::PluginInfo> PluginManager::list() const
{
    std::vector<control::PluginInfo> out;
    for (const auto& e : entries_) {
        auto i = e.plugin->info();
        i.enabled = e.enabled;
        out.push_back(std::move(i));
    }
    return out;
}

std::string PluginManager::set_enabled(const std::string& name, const bool enabled)
{
    auto* e = find(name);
    if (!e) return "no plugin named " + name;
    e->enabled = enabled;
    std::string err;
    if (enabled) err = e->plugin->load();
    else e->plugin->unload();
    save_state();
    publish_state();
    return err.empty() ? std::string{} : "plugin failed to load: " + err;
}

std::string PluginManager::reload(const std::string& name)
{
    auto* e = find(name);
    if (!e) return "no plugin named " + name;
    if (!e->enabled) return "plugin is disabled; enable it first";
    const auto err = e->plugin->load();
    publish_state();
    return err.empty() ? std::string{} : "plugin failed to load: " + err;
}

std::vector<control::PluginLogLine> PluginManager::logs(const std::string& name, const std::size_t limit) const
{
    const auto* e = find(name);
    return e ? e->plugin->logs(limit) : std::vector<control::PluginLogLine>{};
}

void PluginManager::enforce_trips()
{
    bool changed = false;
    for (auto& e : entries_) {
        if (e.enabled && e.plugin->tripped()) {
            e.enabled = false;
            e.plugin->unload();
            changed = true;
        }
    }
    if (changed) {
        save_state();
        publish_state();
    }
}

control::HookAction PluginManager::on_packet(control::PacketFlow& flow, const event::Direction dir, const std::uint64_t id,
    const control::DecodedPacket& decoded, std::vector<std::byte>& bytes)
{
    auto action = control::HookAction::Pass;
    const control::DecodedPacket* current = &decoded;
    control::DecodedPacket redecoded;
    for (auto& e : entries_) {
        if (!e.enabled || !e.plugin->loaded()) continue;
        const auto before = bytes;
        action = e.plugin->on_packet(flow, dir, id, *current, bytes);
        if (action == control::HookAction::Drop) break; // first drop wins; later plugins never see it
        if (bytes != before) {
            redecoded = control::decode_packet(bytes);
            current = &redecoded;
        }
    }
    enforce_trips();
    return action;
}

void PluginManager::on_connection(control::PacketFlow& flow, const std::string_view peer, const bool connected)
{
    for (auto& e : entries_) {
        if (e.enabled && e.plugin->loaded()) e.plugin->on_connection(flow, peer, connected);
    }
    enforce_trips();
}
}
