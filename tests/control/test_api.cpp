#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "support.hpp"
#include "control/control_api.hpp"

using namespace control;
using namespace fixtures;
using event::Direction;

namespace {
struct ApiRig {
    support::TempDir dir;
    core::Config config{ support::write_config(dir) };
    EventHub hub;
    FakeConnection client, upstream;
    PacketFlow flow{ hub, client, upstream };
    SessionRecorder recorder{ hub, dir.str("sessions") };
    TemplateStore templates{ dir.str("templates.json") };
    ControlApi api{ config, hub, flow, client, upstream, recorder, templates };

    JsonValue call(const std::string& op, const std::string& params = "{}", bool expect_ok = true)
    {
        const auto reply = api.handle(op, *JsonValue::parse(params));
        EXPECT_EQ(reply.ok, expect_ok) << op << ": " << reply.body;
        return *JsonValue::parse(reply.body);
    }
};
}

TEST(Api, StatusReportsPeersAndCounters)
{
    ApiRig r;
    r.flow.process(Direction::ServerBound, input_packet());
    const auto s = r.call("status");
    const auto* res = s.find("result");
    EXPECT_TRUE(res->find("game_client")->flag("connected"));
    EXPECT_EQ(res->find("game_client")->integer("rtt_ms"), 17);
    EXPECT_EQ(res->find("counters")->integer("server_bound_packets"), 1);
}

TEST(Api, InjectByHexSpecAndTemplate)
{
    ApiRig r;
    r.call("inject", R"({"dir":"server_bound","hex":"02000000616374696f6e7c717569740a00"})");
    ASSERT_EQ(r.upstream.writes.size(), 1u);
    r.call("inject", R"({"dir":"client_bound","packet":{"kind":"text","msg_type":3,"text":"hi"}})");
    EXPECT_EQ(r.client.writes[0], text_packet(3, "hi"));

    r.call("templates.save", R"({"name":"say-hi","dir":"client_bound","packet":{"kind":"text","msg_type":3,"text":"hi"}})");
    r.call("inject", R"({"template":"say-hi"})");
    EXPECT_EQ(r.client.writes.size(), 2u);

    r.call("inject", R"({"dir":"sideways","hex":"00"})", false);
    r.call("inject", R"({"dir":"server_bound","hex":"zz"})", false);
    r.call("inject", R"({"template":"missing"})", false);
    r.upstream.connected = false;
    EXPECT_EQ(r.api.handle("inject", *JsonValue::parse(R"({"dir":"server_bound","hex":"00"})")).http_status, 409);
}

TEST(Api, TemplatesPersistAcrossReload)
{
    ApiRig r;
    r.call("templates.save", R"({"name":"t1","packet":{"kind":"raw","hex":"0100"}})");
    TemplateStore reloaded{ r.dir.str("templates.json") };
    ASSERT_TRUE(reloaded.find("t1"));
    r.call("templates.save", R"({"name":"bad","packet":{"kind":"raw","hex":"x"}})", false);
    r.call("templates.delete", R"({"name":"t1"})");
    r.call("templates.delete", R"({"name":"t1"})", false);
}

TEST(Api, InterceptEditForwardAndDropViaApi)
{
    ApiRig r;
    r.call("intercept.set", R"({"enabled":true,"rules":[{"direction":"server_bound","match":"input"}]})");
    r.flow.process(Direction::ServerBound, input_packet());
    auto q = r.call("intercept.get");
    const auto id = q.find("result")->find("held")->array[0].integer("id");
    r.call("intercept.forward", R"({"id":)" + std::to_string(id) + R"(,"packet":{"kind":"text","msg_type":2,"text":"action|input\n|text|edited"}})");
    ASSERT_EQ(r.upstream.writes.size(), 1u);
    EXPECT_EQ(r.upstream.writes[0], text_packet(2, "action|input\n|text|edited"));
    r.call("intercept.forward", R"({"id":)" + std::to_string(id) + "}", false);
    r.call("intercept.set", R"({"rules":[{"direction":"bogus"}]})", false);
}

TEST(Api, DecodeEncodeEndpointsAndUnknownOp)
{
    ApiRig r;
    const auto d = r.call("packet.decode", R"({"hex":")" + control::encoding::to_hex(call_function_packet()) + R"("})");
    EXPECT_EQ(d.find("result")->str("name"), "OnSpawn");
    r.call("packet.decode", R"({"hex":"1"})", false);
    r.call("no.such.op", "{}", false);
}

TEST(Api, ProfilesSwitchAndPersist)
{
    ApiRig r;
    r.call("profiles.save", R"({"name":"lan","address":"192.168.0.7","port":8443})");
    r.call("profiles.set_active", R"({"name":"lan"})");
    EXPECT_EQ(r.config.get_server_config().address, "192.168.0.7");
    r.call("profiles.set_active", R"({"name":"nope"})", false);
    r.call("profiles.delete", R"({"name":"lan"})", false); // active
    core::Config reloaded{ r.dir.str("config.json") };
    EXPECT_EQ(reloaded.get_active_profile_name(), "lan");
    r.call("profiles.save", R"({"name":"","address":"x"})", false);
}

TEST(Api, SessionRecordAndReplay)
{
    ApiRig r;
    r.call("sessions.start", R"({"name":"s1"})");
    r.call("sessions.start", R"({"name":"s2"})", false);
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ClientBound, call_function_packet());
    r.flow.inject(Direction::ClientBound, state_packet(), "ui"); // not "wire": must not be replayed
    r.call("sessions.stop");
    r.call("sessions.start", R"({"name":"../evil"})", false);

    const auto list = r.call("sessions.list");
    EXPECT_EQ(list.find("result")->array.size(), 1u);

    const auto data = load_session(r.recorder.path_for("s1"));
    ASSERT_EQ(data.packets.size(), 3u);
    const auto rep = r.call("sessions.replay", R"({"name":"s1"})");
    EXPECT_EQ(rep.find("result")->integer("total"), 2);
    EXPECT_EQ(rep.find("result")->integer("changed"), 0);
    r.call("sessions.replay", R"({"name":"missing"})", false);
}

TEST(Session, ReplayRunsHooksDeterministicallyWithoutTouchingLivePeers)
{
    ApiRig r;
    SessionData s;
    s.packets.push_back({ 1, Direction::ServerBound, input_packet(), "wire", 0 });
    s.packets.push_back({ 2, Direction::ClientBound, state_packet(), "wire", 0 });
    struct H final : IPacketHooks {
        HookAction on_packet(PacketFlow&, Direction d, std::uint64_t, const DecodedPacket&, std::vector<std::byte>& b) override
        {
            if (d == Direction::ServerBound) return HookAction::Drop;
            b.push_back(std::byte{ 0xAA });
            return HookAction::Pass;
        }
    } hooks;
    const auto out = replay_session(s, &hooks);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_TRUE(out[0].dropped);
    EXPECT_FALSE(out[1].dropped);
    EXPECT_EQ(out[1].outputs[0].bytes.back(), std::byte{ 0xAA });
    EXPECT_TRUE(r.upstream.writes.empty());
    EXPECT_TRUE(r.client.writes.empty());
}

TEST(Session, LoadToleratesCorruptLines)
{
    support::TempDir d;
    {
        std::ofstream f{ d.str("x.jsonl") };
        f << "{\"t\":\"session\"}\nnot json\n{\"t\":\"packet\",\"id\":1,\"dir\":\"server_bound\",\"origin\":\"wire\",\"hex\":\"zz\"}\n"
             "{\"t\":\"packet\",\"id\":2,\"dir\":\"server_bound\",\"origin\":\"wire\",\"hex\":\"0200000041\"}\n";
    }
    const auto s = load_session(d.str("x.jsonl"));
    EXPECT_EQ(s.packets.size(), 1u);
    EXPECT_EQ(s.skipped_lines, 2u);
}
