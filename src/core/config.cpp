#include "config.hpp"

#include <algorithm>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

namespace core {
namespace {
std::vector<Config::ProfileConfig> default_profiles()
{
    // Both are examples for servers YOU run. Edit them in config.json or in the UI.
    Config::ProfileConfig local;
    local.name = "localhost";
    local.direct = true;
    local.enet_address = "127.0.0.1";
    local.enet_port = 17091;

    Config::ProfileConfig lan;
    lan.name = "lan-server";
    lan.address = "192.168.1.100";
    lan.port = 443;
    lan.tls = true;

    return { local, lan };
}
}

Config::Config(std::string path)
    : path_{ std::move(path) }
{
    if (const auto read_ec = glz::read_file_json(config_, path_, std::string{})) {
        if (read_ec.ec != glz::error_code::file_open_failure) {
            throw std::runtime_error{
                fmt::format("Failed to load configuration file: {}", glz::format_error(read_ec))
            };
        }

        spdlog::warn("Configuration file \"{}\" not found. Creating default configuration file...", path_);
        config_.profiles = default_profiles();
        config_.active_profile = config_.profiles.front().name;
        normalise_locked();

        if (const auto err = save(); !err.empty()) {
            throw std::runtime_error{ fmt::format("Failed to create default configuration file: {}", err) };
        }

        spdlog::info("Default configuration file \"{}\" created successfully.", path_);
        return;
    }

    normalise_locked();
    spdlog::info("Config file \"{}\" is all loaded up and ready to go!", path_);
}

void Config::normalise_locked()
{
    // Files written before profiles existed only have server.address.
    if (config_.profiles.empty()) {
        ProfileConfig legacy;
        legacy.name = "default";
        legacy.address = config_.server.address;
        config_.profiles.push_back(legacy);
    }

    const auto it = std::ranges::find(config_.profiles, config_.active_profile, &ProfileConfig::name);
    if (it == config_.profiles.end()) {
        if (!config_.active_profile.empty()) {
            spdlog::warn("Active profile \"{}\" not found, using \"{}\"", config_.active_profile, config_.profiles.front().name);
        }
        config_.active_profile = config_.profiles.front().name;
    }

    if (const auto active = std::ranges::find(config_.profiles, config_.active_profile, &ProfileConfig::name);
        active != config_.profiles.end()) {
        config_.server.address = active->address;
    }
}

Config::ServerConfig Config::get_server_config() const
{
    std::scoped_lock lock{ mutex_ };
    return config_.server;
}

Config::ClientConfig Config::get_client_config() const
{
    std::scoped_lock lock{ mutex_ };
    return config_.client;
}

Config::ControlConfig Config::get_control_config() const
{
    std::scoped_lock lock{ mutex_ };
    return config_.control;
}

std::vector<Config::ProfileConfig> Config::get_profiles() const
{
    std::scoped_lock lock{ mutex_ };
    return config_.profiles;
}

std::string Config::get_active_profile_name() const
{
    std::scoped_lock lock{ mutex_ };
    return config_.active_profile;
}

std::optional<Config::ProfileConfig> Config::get_active_profile() const
{
    std::scoped_lock lock{ mutex_ };
    const auto it = std::ranges::find(config_.profiles, config_.active_profile, &ProfileConfig::name);
    if (it == config_.profiles.end()) {
        return std::nullopt;
    }
    return *it;
}

std::string Config::validate_profile(const ProfileConfig& p)
{
    if (p.name.empty() || p.name.size() > 64) {
        return "profile name must be 1-64 characters";
    }
    if (p.direct) {
        if (p.enet_address.empty()) return "direct profiles need enet_address";
        if (p.enet_port < 1 || p.enet_port > 65535) return "enet_port must be 1-65535";
    } else {
        if (p.address.empty()) return "profile needs an address";
        if (p.port < 1 || p.port > 65535) return "port must be 1-65535";
    }
    return {};
}

std::string Config::set_active_profile(const std::string& name)
{
    std::scoped_lock lock{ mutex_ };
    const auto it = std::ranges::find(config_.profiles, name, &ProfileConfig::name);
    if (it == config_.profiles.end()) {
        return fmt::format("no profile named \"{}\"", name);
    }
    config_.active_profile = name;
    config_.server.address = it->address;
    return {};
}

std::string Config::upsert_profile(const ProfileConfig& profile)
{
    if (auto err = validate_profile(profile); !err.empty()) {
        return err;
    }
    std::scoped_lock lock{ mutex_ };
    const auto it = std::ranges::find(config_.profiles, profile.name, &ProfileConfig::name);
    if (it == config_.profiles.end()) {
        config_.profiles.push_back(profile);
    } else {
        *it = profile;
    }
    if (config_.active_profile == profile.name) {
        config_.server.address = profile.address;
    }
    return {};
}

std::string Config::remove_profile(const std::string& name)
{
    std::scoped_lock lock{ mutex_ };
    if (config_.profiles.size() <= 1) {
        return "cannot remove the last profile";
    }
    if (config_.active_profile == name) {
        return "cannot remove the active profile; switch to another one first";
    }
    const auto removed = std::erase_if(config_.profiles, [&](const ProfileConfig& p) { return p.name == name; });
    return removed ? std::string{} : fmt::format("no profile named \"{}\"", name);
}

std::string Config::save() const
{
    std::scoped_lock lock{ mutex_ };
    if (const auto ec = glz::write_file_json<glz::opts{ .prettify = true }>(config_, path_, std::string{})) {
        return glz::format_error(ec);
    }
    return {};
}
}
