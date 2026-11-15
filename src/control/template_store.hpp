#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "json_value.hpp"

namespace control {
struct PacketTemplate {
    std::string name;
    std::string description;
    std::string dir{ "server_bound" };
    JsonValue packet; // an encode_packet() spec
};

// Saved inject templates, persisted as a small JSON file. Main-thread only.
class TemplateStore {
public:
    explicit TemplateStore(std::filesystem::path file);

    [[nodiscard]] const std::vector<PacketTemplate>& all() const { return items_; }
    [[nodiscard]] std::optional<PacketTemplate> find(const std::string& name) const;
    // Returns an error message or empty on success.
    std::string save(PacketTemplate t);
    std::string remove(const std::string& name);
    [[nodiscard]] std::string to_json() const;

private:
    std::string persist() const;

    std::filesystem::path file_;
    std::vector<PacketTemplate> items_;
};
}
