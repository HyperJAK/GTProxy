#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "world.hpp"
#include "../control/event_hub.hpp"
#include "../control/world_view.hpp"
#include "../event/event.hpp"

namespace world {
struct VendingEntry {
    int x{ 0 }, y{ 0 };
    std::uint32_t machine_id{ 0 };
    std::string machine_name;
    std::uint32_t item_id{ 0 }; // 0 = empty machine
    std::string item_name;
    std::uint32_t price{ 0 };   // raw wire value; its unit is not interpreted here
};

struct DisplayEntry {
    int x{ 0 }, y{ 0 };
    std::uint32_t machine_id{ 0 };
    std::string machine_name;
    std::uint32_t item_id{ 0 };
    std::string item_name;
};

struct DropEntry {
    std::uint32_t object_id{ 0 };
    std::uint32_t item_id{ 0 };
    std::string item_name;
    int pos_x{ 0 }, pos_y{ 0 }; // raw world coordinates
    int tile_x{ 0 }, tile_y{ 0 }; // pos / 32
    std::uint32_t amount{ 0 };
};

struct DropTotal {
    std::uint32_t item_id{ 0 };
    std::string item_name;
    std::uint64_t total{ 0 };
    std::uint32_t stacks{ 0 };
};

struct WorldSnapshot {
    std::string name;
    std::uint16_t version{ 0 };
    int width{ 0 }, height{ 0 };
    int players{ 0 };
    std::int64_t first_seen_us{ 0 }, last_seen_us{ 0 };
    std::uint64_t revision{ 0 };
    std::vector<VendingEntry> vending;
    std::vector<DisplayEntry> displays;
    std::vector<DropEntry> drops;
    std::vector<DropTotal> drop_totals; // per item, largest first
};

using NameResolver = std::function<std::string(std::uint32_t)>;

// Pure function over the tracked World: unit-testable with synthetic data.
[[nodiscard]] WorldSnapshot build_snapshot(World& world, const NameResolver& names);

// Remembers every world seen during this proxy run (in memory only, newest
// 100) so vending machines and dropped items can be browsed and searched
// across worlds. Main-thread only.
class WorldInspector final : public control::IWorldView {
public:
    using Clock = std::function<std::int64_t()>;

    WorldInspector(control::EventHub& hub, NameResolver names, Clock clock = {});

    // Listen for map data and drop changes. Runs after the existing WorldHandler.
    void attach(event::Dispatcher& dispatcher);
    // Re-read the tracked World (also called by the listeners).
    void refresh(World& world);
    void left_world() { current_.clear(); }

    [[nodiscard]] std::string list_json() const override;
    [[nodiscard]] std::string get_json(const std::string& name) const override;
    [[nodiscard]] std::string search_json(const std::string& query) const override;
    void clear() override;

    [[nodiscard]] const std::map<std::string, WorldSnapshot>& worlds() const { return worlds_; }
    [[nodiscard]] const std::string& current() const { return current_; }

private:
    control::EventHub& hub_;
    NameResolver names_;
    Clock clock_;
    std::map<std::string, WorldSnapshot> worlds_;
    std::string current_;
    std::vector<event::ScopedHandle> handles_;
    static constexpr std::size_t kMaxWorlds = 100;
};
}
