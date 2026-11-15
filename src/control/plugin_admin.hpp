#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace control {
struct PluginInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string path;
    bool enabled{ false };
    bool loaded{ false };
    std::string error;          // last load/runtime error ("" = none)
    std::uint64_t errors{ 0 };  // runtime errors since load
    std::uint64_t packets{ 0 }; // packets passed through its hooks
};

struct PluginLogLine {
    std::int64_t ts{ 0 };
    std::string level;
    std::string message;
};

// Control-plane view of the plugin manager.
class IPluginAdmin {
public:
    virtual ~IPluginAdmin() = default;
    [[nodiscard]] virtual std::vector<PluginInfo> list() const = 0;
    // Each returns an error message, or empty on success.
    virtual std::string set_enabled(const std::string& name, bool enabled) = 0;
    virtual std::string reload(const std::string& name) = 0;
    virtual std::string rescan() = 0;
    [[nodiscard]] virtual std::vector<PluginLogLine> logs(const std::string& name, std::size_t limit) const = 0;
};
}
