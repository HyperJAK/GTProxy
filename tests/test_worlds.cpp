#include <set>
// World inspector: vending machines and dropped items, per world and across worlds.
// The map bytes are assembled by hand following the layout the existing
// World::serialize reads (see src/world/*). No client or server involved.
#include <gtest/gtest.h>

#include "control/fixtures.hpp"
#include "control/support.hpp"
#include "control/control_api.hpp"
#include "core/handlers/world_handler.hpp"
#include "packet/game/world.hpp"
#include "packet/message/exit.hpp"
#include "world/world_inspector.hpp"

using namespace fixtures;
using control::JsonValue;

namespace {
struct TileSpec {
    std::uint16_t fg{ 0 };
    int vending_item{ -1 }; // >=0 => tile has a Vending extra
    std::uint32_t price{ 0 };
    int display_item{ -1 };
};

struct DropSpec {
    std::uint16_t item;
    float x, y;
    std::uint8_t amount;
    std::uint32_t id;
};

// version, 4 skipped, name, tile map, object map
Bytes map_bytes(const std::string& name, int w, int h, const std::vector<TileSpec>& specials, const std::vector<DropSpec>& drops)
{
    Bytes b;
    put<std::uint16_t>(b, 0x16);
    put<std::uint32_t>(b, 0);
    put<std::uint16_t>(b, static_cast<std::uint16_t>(name.size()));
    put_str(b, name);
    put<std::int32_t>(b, w);
    put<std::int32_t>(b, h);
    put<std::uint32_t>(b, static_cast<std::uint32_t>(w * h));
    for (int i = 0; i < 5; ++i) put<std::uint8_t>(b, 0);
    for (int i = 0; i < w * h; ++i) {
        const TileSpec* spec = i < static_cast<int>(specials.size()) ? &specials[i] : nullptr;
        put<std::uint16_t>(b, spec ? spec->fg : 0); // foreground
        put<std::uint16_t>(b, 0);                    // background
        put<std::uint16_t>(b, 0);                    // parent
        if (spec && spec->vending_item >= 0) {
            put<std::uint16_t>(b, 1); // Extra
            put<std::uint8_t>(b, 24); // tile_extra::Type::Vending
            put<std::uint32_t>(b, static_cast<std::uint32_t>(spec->vending_item));
            put<std::uint32_t>(b, spec->price);
        } else if (spec && spec->display_item >= 0) {
            put<std::uint16_t>(b, 1);
            put<std::uint8_t>(b, 23); // DisplayBlock
            put<std::uint32_t>(b, static_cast<std::uint32_t>(spec->display_item));
        } else {
            put<std::uint16_t>(b, 0);
        }
    }
    for (int i = 0; i < 12; ++i) put<std::uint8_t>(b, 0);
    put<std::uint32_t>(b, static_cast<std::uint32_t>(drops.size()));
    put<std::uint32_t>(b, 100); // drop id counter
    for (const auto& d : drops) {
        put<std::uint16_t>(b, d.item);
        put<float>(b, d.x);
        put<float>(b, d.y);
        put<std::uint8_t>(b, d.amount);
        put<std::uint8_t>(b, 0);
        put<std::uint32_t>(b, d.id);
    }
    return b;
}

std::string name_of(std::uint32_t id)
{
    switch (id) {
    case 0: return "Blank";
    case 2: return "Dirt";
    case 3: return "Lava";
    case 5: return "Cave Dirt";
    case 2978: return "Vending Machine";
    case 1008: return "Display Box";
    case 242: return "World Lock";
    default: return "item #" + std::to_string(id);
    }
}

struct Rig {
    control::EventHub hub;
    world::WorldInspector inspector{ hub, name_of };
    std::vector<std::string> events;
    Rig()
    {
        world::World::instance().clear();
        hub.subscribe([this](const std::string& e) { events.push_back(e); });
    }
    ~Rig() { world::World::instance().clear(); }

    void enter(const std::string& name, int w, int h, const std::vector<TileSpec>& tiles, const std::vector<DropSpec>& drops)
    {
        auto& world = world::World::instance();
        world.clear();
        const auto bytes = map_bytes(name, w, h, tiles, drops);
        world.serialize(bytes.data(), bytes.size());
        inspector.refresh(world);
    }
};
}

TEST(Worlds, ParsesVendingMachinesAndDisplayBlocksWithPositionsAndPrices)
{
    Rig r;
    // 4x3 world; tile index 1 -> (1,0), index 6 -> (2,1)
    std::vector<TileSpec> t(7);
    t[1] = { 2978, 242, 5, -1 };      // vending machine selling World Lock at raw price 5
    t[5] = { 2978, 0, 0, -1 };        // empty machine, (1,1)
    t[6] = { 1008, -1, 0, 3 };        // display box showing Lava, (2,1)
    r.enter("MYWORLD", 4, 3, t, {});

    const auto& s = r.inspector.worlds().at("MYWORLD");
    EXPECT_EQ(s.width, 4);
    EXPECT_EQ(s.height, 3);
    ASSERT_EQ(s.vending.size(), 2u);
    EXPECT_EQ(s.vending[0].x, 1); EXPECT_EQ(s.vending[0].y, 0);
    EXPECT_EQ(s.vending[0].item_name, "World Lock");
    EXPECT_EQ(s.vending[0].price, 5u);
    EXPECT_EQ(s.vending[0].machine_name, "Vending Machine");
    EXPECT_EQ(s.vending[1].item_id, 0u); // empty machine stays visible with no item
    ASSERT_EQ(s.displays.size(), 1u);
    EXPECT_EQ(s.displays[0].item_name, "Lava");
    EXPECT_EQ(s.displays[0].x, 2); EXPECT_EQ(s.displays[0].y, 1);
}

TEST(Worlds, DroppedItemsAreListedAndTotalledPerItem)
{
    Rig r;
    r.enter("DROPS", 2, 2, {}, {
        { 2, 64.0f, 96.0f, 10, 1 }, { 2, 40.0f, 40.0f, 25, 2 }, { 242, 32.0f, 32.0f, 1, 3 }, { 5, 0, 0, 200, 4 } });

    const auto& s = r.inspector.worlds().at("DROPS");
    ASSERT_EQ(s.drops.size(), 4u);
    EXPECT_EQ(s.drops[0].tile_x, 2); EXPECT_EQ(s.drops[0].tile_y, 3); // 64/32, 96/32
    ASSERT_EQ(s.drop_totals.size(), 3u);
    EXPECT_EQ(s.drop_totals[0].item_name, "Cave Dirt"); EXPECT_EQ(s.drop_totals[0].total, 200u); // largest first
    EXPECT_EQ(s.drop_totals[1].item_name, "Dirt");
    EXPECT_EQ(s.drop_totals[1].total, 35u);   // 10 + 25
    EXPECT_EQ(s.drop_totals[1].stacks, 2u);
}

TEST(Worlds, RemembersEveryVisitedWorldAndSearchesAcrossThem)
{
    Rig r;
    std::vector<TileSpec> a(2); a[0] = { 2978, 242, 3, -1 };
    r.enter("SHOPA", 2, 2, a, { { 2, 0, 0, 5, 1 } });
    std::vector<TileSpec> b(3); b[2] = { 2978, 242, 7, -1 };
    r.enter("SHOPB", 3, 1, b, { { 242, 64, 0, 2, 9 } });
    r.enter("EMPTY", 1, 1, {}, {});

    EXPECT_EQ(r.inspector.worlds().size(), 3u);
    EXPECT_EQ(r.inspector.current(), "EMPTY");
    r.inspector.left_world();
    EXPECT_TRUE(r.inspector.current().empty());

    // by name (case-insensitive substring) and by id; hits from both worlds, vending and drops
    for (const char* q : { "world lock", "242" }) {
        const auto hits = *JsonValue::parse(r.inspector.search_json(q));
        ASSERT_EQ(hits.array.size(), 3u) << q;
        int vending = 0, drops = 0;
        std::set<std::string> worlds;
        for (const auto& h : hits.array) {
            worlds.insert(h.str("world"));
            (h.str("kind") == "vending" ? vending : drops)++;
        }
        EXPECT_EQ(vending, 2);
        EXPECT_EQ(drops, 1);
        EXPECT_EQ(worlds, (std::set<std::string>{ "SHOPA", "SHOPB" }));
    }
    EXPECT_EQ(JsonValue::parse(r.inspector.search_json("dirt"))->array.size(), 1u);
    EXPECT_TRUE(JsonValue::parse(r.inspector.search_json("nothing"))->array.empty());
    EXPECT_TRUE(JsonValue::parse(r.inspector.search_json(""))->array.empty());

    const auto list = *JsonValue::parse(r.inspector.list_json());
    ASSERT_EQ(list.array.size(), 3u);
    EXPECT_EQ(list.array[0].str("name"), "EMPTY"); // newest first
    EXPECT_EQ(r.inspector.get_json("nope"), "");
}

TEST(Worlds, RevisitingAWorldUpdatesItInsteadOfDuplicating)
{
    Rig r;
    r.enter("W", 1, 1, {}, { { 2, 0, 0, 1, 1 } });
    r.enter("W", 1, 1, {}, { { 2, 0, 0, 9, 1 } });
    ASSERT_EQ(r.inspector.worlds().size(), 1u);
    EXPECT_EQ(r.inspector.worlds().at("W").revision, 2u);
    EXPECT_EQ(r.inspector.worlds().at("W").drop_totals[0].total, 9u);
}

TEST(Worlds, DispatcherPathUpdatesInspectorAfterWorldHandler)
{
    event::Dispatcher d; // must outlive the inspector's listener handles
    Rig r;
    core::handlers::WorldHandler handler{ d };
    r.inspector.attach(d);

    std::vector<TileSpec> t(1); t[0] = { 2978, 242, 1, -1 };
    packet::game::SendMapData map;
    map.extra = map_bytes("VIAEVENT", 1, 1, t, {});
    d.dispatch(event::TypedPacketEvent<packet::PacketId::SendMapData>{ event::Direction::ClientBound, std::shared_ptr<packet::IPacket>(&map, [](auto*) {}) });

    ASSERT_TRUE(r.inspector.worlds().contains("VIAEVENT")); // listener ran after WorldHandler filled World
    EXPECT_EQ(r.inspector.worlds().at("VIAEVENT").vending.size(), 1u);

    // server-bound join clears "current" but keeps history
    packet::message::JoinRequest join;
    d.dispatch(event::TypedPacketEvent<packet::PacketId::JoinRequest>{ event::Direction::ServerBound, std::shared_ptr<packet::IPacket>(&join, [](auto*) {}) });
    EXPECT_TRUE(r.inspector.current().empty());
    EXPECT_TRUE(r.inspector.worlds().contains("VIAEVENT"));
}

TEST(Worlds, LiveDropAndPickupEventsReachTheInspector)
{
    event::Dispatcher d; // must outlive the inspector's listener handles
    Rig r;
    core::handlers::WorldHandler handler{ d };
    r.inspector.attach(d);
    r.enter("LIVE", 1, 1, {}, {});

    const auto send = [&](std::int32_t change_type, std::int32_t item_net_id) {
        packet::game::ItemChangeObject pkt;
        pkt.object_change_type = change_type;
        pkt.item_net_id = static_cast<std::uint16_t>(item_net_id);
        pkt.item_id = 2;
        pkt.amount = 4;
        d.dispatch(event::TypedPacketEvent<packet::PacketId::ItemChangeObject>{ event::Direction::ClientBound, std::shared_ptr<packet::IPacket>(&pkt, [](auto*) {}) });
    };
    send(-1, 0); // new drop (signed -1 used to be truncated to 255 and never matched)
    EXPECT_EQ(r.inspector.worlds().at("LIVE").drops.size(), 1u);
    const auto id = r.inspector.worlds().at("LIVE").drops[0].object_id;
    send(0, static_cast<std::int32_t>(id)); // pickup removes it
    EXPECT_EQ(r.inspector.worlds().at("LIVE").drops.size(), 0u);
}

TEST(Worlds, MissingItemDatabaseDoesNotCrashMapParsing)
{
    // upstream dereferenced a null ItemInfo here whenever items.dat was not loaded
    Rig r;
    std::vector<TileSpec> t(1); t[0] = { 9999, -1, 0, -1 };
    EXPECT_NO_THROW(r.enter("NODB", 1, 1, t, {}));
}

TEST(Worlds, ControlApiOperationsAndEvents)
{
    Rig r;
    support::TempDir dir;
    core::Config config{ support::write_config(dir) };
    FakeConnection client, upstream;
    control::PacketFlow flow{ r.hub, client, upstream };
    control::SessionRecorder rec{ r.hub, dir.str("sessions") };
    control::TemplateStore tpl{ dir.str("t.json") };
    control::ControlApi api{ config, r.hub, flow, client, upstream, rec, tpl };
    const auto call = [&](const std::string& op, const std::string& p) { return api.handle(op, *JsonValue::parse(p)); };

    EXPECT_EQ(call("worlds.list", "{}").http_status, 503); // not wired yet
    api.set_world_view(&r.inspector);
    std::vector<TileSpec> t(1); t[0] = { 2978, 242, 2, -1 };
    r.enter("API", 1, 1, t, {});

    EXPECT_NE(std::find_if(r.events.begin(), r.events.end(), [](const std::string& e) { return e.find("\"t\":\"world\"") != std::string::npos; }), r.events.end());
    EXPECT_EQ(JsonValue::parse(call("worlds.list", "{}").body)->find("result")->array.size(), 1u);
    EXPECT_EQ(JsonValue::parse(call("worlds.get", R"({"name":"API"})").body)->find("result")->find("vending")->array.size(), 1u);
    EXPECT_EQ(call("worlds.get", R"({"name":"zzz"})").http_status, 404);
    EXPECT_EQ(call("worlds.search", R"({"query":"world lock"})").ok, true);
    EXPECT_EQ(call("worlds.search", "{}").http_status, 400);
    EXPECT_TRUE(call("worlds.clear", "{}").ok);
    EXPECT_TRUE(JsonValue::parse(call("worlds.list", "{}").body)->find("result")->array.empty());
}
