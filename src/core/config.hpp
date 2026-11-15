#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace core {
class Config {
public:
    struct ServerConfig {
        // `port` is the port the proxy itself listens on for the game client (ENet).
        // `address` is the upstream HTTP host; it is kept in sync with the active profile.
        int port{ 16999 };
        std::string address{ "127.0.0.1" };
    };

    struct ClientConfig {
        std::string game_version{ "5.39" };
        int protocol{ 225 };
        // "system" resolves with the OS resolver (default: no outside lookups).
        // "cloudflare" / "google" use DNS-over-HTTPS.
        std::string dns_server{ "system" };
    };

    struct LogConfig {
        bool print_message{ true };
        bool print_game_update_packet{ false };
        bool print_variant{ true };
        bool print_extra{ true };
    };

    struct CommandConfig {
        char prefix{ '/' };
    };

    // A server the proxy can point at. Switchable at runtime from the UI.
    struct ProfileConfig {
        std::string name;
        // Host serving /growtopia/server_data.php (ignored when `direct`).
        std::string address{ "127.0.0.1" };
        int port{ 443 };
        bool tls{ true };
        // direct: do not fetch server_data.php upstream; connect the ENet
        // peer straight to enet_address:enet_port.
        bool direct{ false };
        std::string enet_address;
        int enet_port{ 17091 };
    };

    struct ControlConfig {
        bool enabled{ true };
        // The control plane is local by design. Binding anything other than
        // loopback is refused unless allow_remote is set explicitly.
        std::string bind{ "127.0.0.1" };
        bool allow_remote{ false };
        int http_port{ 7301 };
        int ws_port{ 7302 };
        std::string ui_dir{ "ui" };
        std::string plugins_dir{ "plugins" };
        std::string sessions_dir{ "sessions" };
        std::string templates_file{ "templates.json" };
    };

    struct WrapperConfig {
        ServerConfig server;
        ClientConfig client;
        LogConfig log;
        CommandConfig command;
        ControlConfig control;
        std::vector<ProfileConfig> profiles;
        std::string active_profile;
    };

public:
    // Loads `path`; writes a default file if it does not exist.
    explicit Config(std::string path = "config.json");
    ~Config() = default;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

public:
    // Mutable at runtime (profile switching) and read from several threads,
    // so these return copies taken under a lock.
    [[nodiscard]] ServerConfig get_server_config() const;
    [[nodiscard]] ClientConfig get_client_config() const;
    [[nodiscard]] const LogConfig& get_log_config() const { return config_.log; }
    [[nodiscard]] const CommandConfig& get_command_config() const { return config_.command; }
    [[nodiscard]] ControlConfig get_control_config() const;

    [[nodiscard]] std::vector<ProfileConfig> get_profiles() const;
    [[nodiscard]] std::string get_active_profile_name() const;
    [[nodiscard]] std::optional<ProfileConfig> get_active_profile() const;

    // Return an error message, or an empty string on success.
    std::string set_active_profile(const std::string& name);
    std::string upsert_profile(const ProfileConfig& profile);
    std::string remove_profile(const std::string& name);

    // Persist to the file this Config was loaded from.
    std::string save() const;

    [[nodiscard]] static std::string validate_profile(const ProfileConfig& p);

private:
    void normalise_locked();

private:
    std::string path_;
    mutable std::mutex mutex_;
    WrapperConfig config_;
};
}
