#include "world_inspector.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <spdlog/spdlog.h>

#include "../control/json_writer.hpp"
#include "../packet/game/world.hpp"

namespace world {
namespace {
std::int64_t system_now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void write_vending(control::JsonWriter& w, const VendingEntry& v, const std::string* world = nullptr)
{
    w.begin_object();
    if (world) w.field("world", std::string_view{ *world });
    w.field("kind", "vending").field("x", v.x).field("y", v.y).field("machine", std::string_view{ v.machine_name })
        .field("machine_id", v.machine_id).field("item_id", v.item_id).field("item", std::string_view{ v.item_name })
        .field("price", v.price).end_object();
}
}

WorldSnapshot build_snapshot(World& world, const NameResolver& names)
{
    WorldSnapshot s;
    s.name = world.get_name();
    s.version = world.get_version();
    s.players = static_cast<int>(world.get_players().size());

    auto& tile_map = world.get_tile_map();
    s.width = tile_map.get_size().x;
    s.height = tile_map.get_size().y;
    const auto& tiles = tile_map.get_tiles();
    for (std::size_t i = 0; i < tiles.size() && s.width > 0; ++i) {
        const auto& t = tiles[i];
        const int x = static_cast<int>(i % static_cast<std::size_t>(s.width));
        const int y = static_cast<int>(i / static_cast<std::size_t>(s.width));
        if (const auto* v = t.extra.get_as<tile_extra::Vending>()) {
            s.vending.push_back({ x, y, t.foreground, names(t.foreground), v->item_id, v->item_id ? names(v->item_id) : std::string{}, v->price });
        } else if (const auto* d = t.extra.get_as<tile_extra::DisplayBlock>()) {
            s.displays.push_back({ x, y, t.foreground, names(t.foreground), d->item_id, d->item_id ? names(d->item_id) : std::string{} });
        }
    }

    std::map<std::uint32_t, DropTotal> totals;
    for (const auto& o : world.get_object_map().get_objects()) {
        DropEntry e;
        e.object_id = o.object_id;
        e.item_id = o.item_id;
        e.item_name = names(o.item_id);
        e.pos_x = o.pos.x;
        e.pos_y = o.pos.y;
        e.tile_x = o.pos.x / 32;
        e.tile_y = o.pos.y / 32;
        e.amount = o.amount;
        s.drops.push_back(e);
        auto& t = totals[o.item_id];
        t.item_id = o.item_id;
        t.item_name = e.item_name;
        t.total += o.amount;
        t.stacks += 1;
    }
    for (auto& [id, t] : totals) s.drop_totals.push_back(t);
    std::ranges::sort(s.drop_totals, [](const DropTotal& a, const DropTotal& b) { return a.total > b.total; });
    return s;
}

WorldInspector::WorldInspector(control::EventHub& hub, NameResolver names, Clock clock)
    : hub_{ hub }, names_{ std::move(names) }, clock_{ clock ? std::move(clock) : Clock{ system_now_us } }
{ }

void WorldInspector::attach(event::Dispatcher& dispatcher)
{
    // Lowest priority: run after WorldHandler has updated the World singleton.
    constexpr auto map_type = event::packet_event_type(packet::PacketId::SendMapData);
    handles_.emplace_back(dispatcher, map_type, dispatcher.append_listener(map_type, [this](const event::Event& e) {
        const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::SendMapData>*>(&e);
        if (evt && evt->direction == event::Direction::ClientBound) refresh(World::instance());
    }, event::Priority::Lowest));

    constexpr auto obj_type = event::packet_event_type(packet::PacketId::ItemChangeObject);
    handles_.emplace_back(dispatcher, obj_type, dispatcher.append_listener(obj_type, [this](const event::Event& e) {
        const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::ItemChangeObject>*>(&e);
        if (evt && evt->direction == event::Direction::ClientBound) refresh(World::instance());
    }, event::Priority::Lowest));

    constexpr auto join_type = event::packet_event_type(packet::PacketId::JoinRequest);
    handles_.emplace_back(dispatcher, join_type, dispatcher.append_listener(join_type, [this](const event::Event& e) {
        const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::JoinRequest>*>(&e);
        if (evt && evt->direction == event::Direction::ServerBound) left_world();
    }, event::Priority::Lowest));
}

void WorldInspector::refresh(World& world)
{
    auto snap = build_snapshot(world, names_);
    if (snap.name.empty()) {
        return; // no map received yet
    }
    const auto now = clock_();
    if (const auto it = worlds_.find(snap.name); it != worlds_.end()) {
        snap.first_seen_us = it->second.first_seen_us;
        snap.revision = it->second.revision + 1;
    } else {
        snap.first_seen_us = now;
        snap.revision = 1;
    }
    snap.last_seen_us = now;
    current_ = snap.name;

    control::JsonWriter w;
    w.begin_object().field("t", "world").field("name", std::string_view{ snap.name }).field("revision", snap.revision)
        .field("vending", static_cast<std::uint64_t>(snap.vending.size()))
        .field("drops", static_cast<std::uint64_t>(snap.drops.size())).end_object();
    worlds_[snap.name] = std::move(snap);

    while (worlds_.size() > kMaxWorlds) {
        const auto oldest = std::ranges::min_element(worlds_, {}, [](const auto& kv) { return kv.second.last_seen_us; });
        worlds_.erase(oldest);
    }
    hub_.publish(w.take(), false);
}

std::string WorldInspector::list_json() const
{
    std::vector<const WorldSnapshot*> ordered;
    for (const auto& [n, s] : worlds_) ordered.push_back(&s);
    std::ranges::sort(ordered, [](auto* a, auto* b) { return a->last_seen_us > b->last_seen_us; });

    control::JsonWriter w;
    w.begin_array();
    for (const auto* s : ordered) {
        w.begin_object().field("name", std::string_view{ s->name }).field("current", s->name == current_)
            .field("width", s->width).field("height", s->height).field("players", s->players)
            .field("vending", static_cast<std::uint64_t>(s->vending.size()))
            .field("displays", static_cast<std::uint64_t>(s->displays.size()))
            .field("drops", static_cast<std::uint64_t>(s->drops.size()))
            .field("last_seen", s->last_seen_us).field("revision", s->revision).end_object();
    }
    w.end_array();
    return w.take();
}

std::string WorldInspector::get_json(const std::string& name) const
{
    const auto it = worlds_.find(name);
    if (it == worlds_.end()) return {};
    const auto& s = it->second;

    control::JsonWriter w;
    w.begin_object().field("name", std::string_view{ s.name }).field("current", s.name == current_).field("version", static_cast<unsigned>(s.version))
        .field("width", s.width).field("height", s.height).field("players", s.players)
        .field("first_seen", s.first_seen_us).field("last_seen", s.last_seen_us).field("revision", s.revision);
    w.key("vending").begin_array();
    for (const auto& v : s.vending) write_vending(w, v);
    w.end_array();
    w.key("displays").begin_array();
    for (const auto& d : s.displays) {
        w.begin_object().field("x", d.x).field("y", d.y).field("machine", std::string_view{ d.machine_name })
            .field("item_id", d.item_id).field("item", std::string_view{ d.item_name }).end_object();
    }
    w.end_array();
    w.key("drops").begin_array();
    for (const auto& d : s.drops) {
        w.begin_object().field("object_id", d.object_id).field("item_id", d.item_id).field("item", std::string_view{ d.item_name })
            .field("amount", d.amount).field("x", d.tile_x).field("y", d.tile_y).field("pos_x", d.pos_x).field("pos_y", d.pos_y).end_object();
    }
    w.end_array();
    w.key("drop_totals").begin_array();
    for (const auto& t : s.drop_totals) {
        w.begin_object().field("item_id", t.item_id).field("item", std::string_view{ t.item_name })
            .field("total", t.total).field("stacks", t.stacks).end_object();
    }
    w.end_array().end_object();
    return w.take();
}

std::string WorldInspector::search_json(const std::string& query) const
{
    const auto needle = lower(query);
    const bool numeric = !needle.empty() && std::ranges::all_of(needle, [](unsigned char c) { return std::isdigit(c); });
    const auto id_needle = numeric ? std::stoull(needle.substr(0, 9)) : 0ull;
    const auto matches = [&](std::uint32_t id, const std::string& name) {
        if (id == 0 || needle.empty()) return false;
        return (numeric && id == id_needle) || lower(name).find(needle) != std::string::npos;
    };

    control::JsonWriter w;
    w.begin_array();
    std::size_t count = 0;
    constexpr std::size_t kMax = 500;
    for (const auto& [name, s] : worlds_) {
        for (const auto& v : s.vending) {
            if (count < kMax && matches(v.item_id, v.item_name)) { write_vending(w, v, &name); ++count; }
        }
        for (const auto& d : s.displays) {
            if (count < kMax && matches(d.item_id, d.item_name)) {
                w.begin_object().field("world", std::string_view{ name }).field("kind", "display").field("x", d.x).field("y", d.y)
                    .field("machine", std::string_view{ d.machine_name }).field("item_id", d.item_id).field("item", std::string_view{ d.item_name }).end_object();
                ++count;
            }
        }
        for (const auto& d : s.drops) {
            if (count < kMax && matches(d.item_id, d.item_name)) {
                w.begin_object().field("world", std::string_view{ name }).field("kind", "drop").field("x", d.tile_x).field("y", d.tile_y)
                    .field("item_id", d.item_id).field("item", std::string_view{ d.item_name }).field("amount", d.amount).end_object();
                ++count;
            }
        }
    }
    w.end_array();
    return w.take();
}

void WorldInspector::clear()
{
    worlds_.clear();
    current_.clear();
}
}
