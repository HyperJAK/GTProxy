#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "control/packet_flow.hpp"
#include "control/json_value.hpp"

using namespace control;
using namespace fixtures;
using event::Direction;

namespace {
struct Rig {
    EventHub hub;
    FakeConnection client, upstream;
    std::int64_t t{ 1000 };
    PacketFlow flow{ hub, client, upstream, [this] { return t += 10; } };
    std::vector<std::string> events;
    Rig() { hub.subscribe([this](const std::string& e) { events.push_back(e); }); }
    int count(std::string_view needle) const
    {
        int n = 0;
        for (const auto& e : events) if (e.find(needle) != std::string::npos) ++n;
        return n;
    }
};

struct Hooks final : IPacketHooks {
    std::function<HookAction(PacketFlow&, Direction, std::vector<std::byte>&)> fn;
    HookAction on_packet(PacketFlow& f, Direction d, std::uint64_t, const DecodedPacket&, std::vector<std::byte>& b) override { return fn(f, d, b); }
};
}

TEST(Flow, ForwardsServerBoundToUpstreamAndPublishesEvent)
{
    Rig r;
    r.flow.process(Direction::ServerBound, input_packet());
    ASSERT_EQ(r.upstream.writes.size(), 1u);
    EXPECT_EQ(r.upstream.writes[0], input_packet());
    EXPECT_TRUE(r.client.writes.empty());
    EXPECT_EQ(r.count(R"("t":"packet")"), 1);
    EXPECT_EQ(r.count(R"("state":"forwarded")"), 1);
    const auto ev = JsonValue::parse(r.events[0]);
    EXPECT_EQ(ev->str("dir"), "server_bound");
    EXPECT_EQ(ev->str("name"), "action|input");
    EXPECT_EQ(ev->str("hex"), control::encoding::to_hex(input_packet()));
    EXPECT_EQ(r.flow.stats().packets[1], 1u);
}

TEST(Flow, ConsumedWhenNoForwardHappens)
{
    Rig r;
    r.flow.begin_receive(Direction::ClientBound, call_function_packet());
    r.flow.end_receive(); // typed handler cancelled it: forward() never called
    EXPECT_EQ(r.count(R"("state":"consumed")"), 1);
    EXPECT_TRUE(r.client.writes.empty());
}

TEST(Flow, HookCanDropAndEdit)
{
    Rig r;
    Hooks h;
    h.fn = [](PacketFlow&, Direction d, std::vector<std::byte>& b) {
        if (d == Direction::ServerBound) return HookAction::Drop;
        b = text_packet(3, "edited");
        return HookAction::Pass;
    };
    r.flow.set_hooks(&h);
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ClientBound, input_packet());
    EXPECT_TRUE(r.upstream.writes.empty());
    ASSERT_EQ(r.client.writes.size(), 1u);
    EXPECT_EQ(r.client.writes[0], text_packet(3, "edited"));
    EXPECT_EQ(r.flow.stats().dropped, 1u);
    EXPECT_EQ(r.flow.stats().edited, 1u);
}

TEST(Flow, ThrowingHookDoesNotStopForwarding)
{
    Rig r;
    Hooks h;
    h.fn = [](PacketFlow&, Direction, std::vector<std::byte>&) -> HookAction { throw std::runtime_error{ "boom" }; };
    r.flow.set_hooks(&h);
    r.flow.process(Direction::ClientBound, input_packet());
    EXPECT_EQ(r.client.writes.size(), 1u);
}

TEST(Flow, InterceptHoldsThenForwardsEditedOrDrops)
{
    Rig r;
    r.flow.set_intercept({ true, { { "server_bound", "input", "" } } });
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ServerBound, state_packet()); // behind a held packet: parked for ordering
    r.flow.process(Direction::ClientBound, state_packet()); // other direction, no rule: passes
    EXPECT_TRUE(r.upstream.writes.empty());
    EXPECT_EQ(r.client.writes.size(), 1u);
    EXPECT_EQ(r.flow.held_count(), 2u);

    const auto q = JsonValue::parse(r.flow.intercept_json());
    const auto first = static_cast<std::uint64_t>(q->find("held")->array[0].integer("id"));

    EXPECT_TRUE(r.flow.release(first, false, text_packet(2, "action|input\n|text|changed")).ok);
    // released packet then the ordering-parked one follow, in order
    ASSERT_EQ(r.upstream.writes.size(), 2u);
    EXPECT_EQ(r.upstream.writes[0], text_packet(2, "action|input\n|text|changed"));
    EXPECT_EQ(r.upstream.writes[1], state_packet());
    EXPECT_EQ(r.flow.held_count(), 0u);
    EXPECT_FALSE(r.flow.release(first, false).ok);

    r.flow.process(Direction::ServerBound, input_packet());
    const auto id2 = static_cast<std::uint64_t>(JsonValue::parse(r.flow.intercept_json())->find("held")->array[0].integer("id"));
    EXPECT_TRUE(r.flow.release(id2, true).ok);
    EXPECT_EQ(r.upstream.writes.size(), 2u);
    EXPECT_EQ(r.flow.stats().dropped, 1u);
}

TEST(Flow, DisablingInterceptFlushes)
{
    Rig r;
    r.flow.set_intercept({ true, {} });
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_EQ(r.flow.held_count(), 1u);
    r.flow.set_intercept({ false, {} });
    EXPECT_EQ(r.flow.held_count(), 0u);
    EXPECT_EQ(r.upstream.writes.size(), 1u);
}

TEST(Flow, InjectWritesAndReportsFailure)
{
    Rig r;
    EXPECT_NE(r.flow.inject(Direction::ClientBound, input_packet(), "ui"), 0u);
    EXPECT_EQ(r.client.writes.size(), 1u);
    EXPECT_EQ(r.count(R"("origin":"ui")"), 1);
    r.upstream.connected = false;
    EXPECT_EQ(r.flow.inject(Direction::ServerBound, input_packet()), 0u);
    EXPECT_EQ(r.count(R"("state":"failed")"), 1);
}

TEST(Flow, WireTapObservesProxyOriginatedWritesButNotForwards)
{
    Rig r;
    auto* tap = r.flow.server_tap(); // receives ServerBound; its writes go ClientBound
    tap->on_receive_begin(input_packet());
    tap->on_write(state_packet(), 0); // e.g. a /command reply written by a handler
    tap->on_receive_end();
    EXPECT_EQ(r.count(R"("origin":"proxy")"), 1);
    EXPECT_EQ(r.count(R"("state":"consumed")"), 1);
}
