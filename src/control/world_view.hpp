#pragma once
#include <string>

namespace control {
// Control-plane view of the world inspector. Methods return JSON documents.
class IWorldView {
public:
    virtual ~IWorldView() = default;
    [[nodiscard]] virtual std::string list_json() const = 0;
    // Empty string if the world is unknown.
    [[nodiscard]] virtual std::string get_json(const std::string& name) const = 0;
    [[nodiscard]] virtual std::string search_json(const std::string& query) const = 0;
    virtual void clear() = 0;
};
}
