#include <fstream>
#include <gtest/gtest.h>

#include "control/fixtures.hpp"
#include "control/support.hpp"
#include "control/control_api.hpp"
#include "plugin/plugin_manager.hpp"

using namespace fixtures;
using event::Direction;

namespace {
struct PluginRig {
    support::TempDir dir;
    control::EventHub hub;
    FakeConnection client, upstream;
    control::PacketFlow flow{ hub, client, upstream };
    plugin::Limits limits;
    std::unique_ptr<plugin::PluginManager> mgr;
    std::vector<std::string> events;

    PluginRig(plugin::Limits l = fastLimits()) : limits{ l }
    {
        hub.subscribe([this](const std::string& e) { events.push_back(e); });
    }
    static plugin::Limits fastLimits()
    {
        plugin::Limits l;
        l.hook_time = std::chrono::milliseconds{ 30 };
        l.load_time = std::chrono::milliseconds{ 100 };
        l.hook_instructions = 500'000;
        l.memory_bytes = 4 * 1024 * 1024;
        return l;
    }

    void add(const std::string& name, const std::string& lua, bool enable = true)
    {
        const auto d = dir.path / "plugins" / name;
        std::filesystem::create_directories(d);
        std::ofstream{ d / "main.lua" } << lua;
        if (!mgr) mgr = std::make_unique<plugin::PluginManager>(dir.path / "plugins", hub, limits);
        mgr->rescan();
        if (enable) EXPECT_EQ(mgr->set_enabled(name, true), "") << name;
        flow.set_hooks(mgr.get());
    }
    control::PluginInfo info(const std::string& name) { for (auto& i : mgr->list()) if (i.name == name) return i; return {}; }
    bool has_log(const std::string& name, const std::string& needle)
    {
        for (auto& l : mgr->logs(name, 500)) if (l.message.find(needle) != std::string::npos) return true;
        return false;
    }
};
}

TEST(Plugins, HooksSeePacketsAndCanReadFields)
{
    PluginRig r;
    r.add("reader", R"(
        function onPacketOut(pkt)
          gt.info("out", pkt.kind, pkt.name, pkt.dir, pkt:get("action"), pkt:get("text"), pkt.length)
        end
        function onPacketIn(pkt) gt.info("in", pkt.kind, pkt.name, pkt.decoded.variants[2].value) end)");
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ClientBound, call_function_packet());
    EXPECT_TRUE(r.has_log("reader", "out text action|input server_bound input hello"));
    EXPECT_TRUE(r.has_log("reader", "in variant OnSpawn 42"));
    EXPECT_EQ(r.upstream.writes.size(), 1u); // unmodified traffic still flows
}

TEST(Plugins, CanModifyDropAndInject)
{
    PluginRig r;
    r.add("mod", R"(
        function onPacketOut(pkt)
          if pkt:get("text") == "drop me" then pkt:drop() return end
          if pkt.kind == "text" then pkt.text = "action|input\n|text|changed" end
        end
        function onPacketIn(pkt)
          gt.inject(gt.SERVER_BOUND, "0300000041")
          return false
        end)");
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ServerBound, text_packet(2, "action|input\n|text|drop me"));
    r.flow.process(Direction::ClientBound, state_packet());
    ASSERT_EQ(r.upstream.writes.size(), 2u);
    EXPECT_EQ(r.upstream.writes[0], text_packet(2, "action|input\n|text|changed"));
    EXPECT_EQ(control::encoding::to_hex(r.upstream.writes[1]), "0300000041"); // injected
    EXPECT_TRUE(r.client.writes.empty());                                   // returning false dropped it
}

TEST(Plugins, SandboxHasNoFileProcessOrCodeLoadingAccess)
{
    PluginRig r;
    r.add("probe", R"(
        local bad = {}
        for _, n in ipairs({"os","io","package","debug","require","dofile","loadfile","load","loadstring","collectgarbage","getmetatable","rawset"}) do
          if _G[n] ~= nil then bad[#bad+1] = n end
        end
        if string.dump ~= nil then bad[#bad+1] = "string.dump" end
        gt.info("leaks:" .. #bad .. " " .. table.concat(bad, ","))
        local ok = pcall(function() return io.open("/etc/passwd") end)
        gt.info("io_open_worked:" .. tostring(ok))
        )");
    EXPECT_TRUE(r.has_log("probe", "leaks:0")) << "sandbox exposes forbidden globals";
    EXPECT_TRUE(r.has_log("probe", "io_open_worked:false"));
}

TEST(Plugins, InfiniteLoopInHookIsKilledAndTrafficContinues)
{
    PluginRig r;
    r.add("spin", "function onPacketOut(pkt) while true do end end");
    const auto start = std::chrono::steady_clock::now();
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    EXPECT_EQ(r.upstream.writes.size(), 1u); // fail-open
    EXPECT_GE(r.info("spin").errors, 1u);
    EXPECT_NE(r.info("spin").error.find("budget"), std::string::npos);
}

TEST(Plugins, InfiniteLoopAtLoadFailsToLoad)
{
    PluginRig r;
    r.add("spinload", "while true do end", false);
    EXPECT_NE(r.mgr->set_enabled("spinload", true), "");
    EXPECT_FALSE(r.info("spinload").loaded);
}

TEST(Plugins, MemoryBombIsRefused)
{
    PluginRig r;
    r.add("bomb", R"(
        function onPacketOut(pkt)
          local t = {}
          for i = 1, 1e9 do t[i] = string.rep("x", 1024) .. i end
        end)");
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_EQ(r.upstream.writes.size(), 1u);
    EXPECT_GE(r.info("bomb").errors, 1u);
    const auto e = r.info("bomb").error;
    EXPECT_TRUE(e.find("memory") != std::string::npos || e.find("budget") != std::string::npos) << e;
}

TEST(Plugins, OneBrokenPluginDoesNotAffectOthers)
{
    PluginRig r;
    r.add("a_broken", "function onPacketOut(pkt) error('boom') end");
    r.add("b_syntax", "this is not lua", false);
    r.add("c_good", "function onPacketOut(pkt) pkt.text = 'action|input\\n|text|from c' end");
    EXPECT_NE(r.mgr->set_enabled("b_syntax", true), "");
    r.flow.process(Direction::ServerBound, input_packet());
    ASSERT_EQ(r.upstream.writes.size(), 1u);
    EXPECT_EQ(r.upstream.writes[0], text_packet(2, "action|input\n|text|from c"));
    EXPECT_TRUE(r.has_log("a_broken", "boom"));
}

TEST(Plugins, RepeatedFailuresSwitchThePluginOffAndPersist)
{
    PluginRig r;
    r.add("flaky", "function onPacketOut(pkt) error('nope') end");
    for (int i = 0; i < 12; ++i) r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_FALSE(r.info("flaky").enabled);
    EXPECT_EQ(r.upstream.writes.size(), 12u);
    // a fresh manager reading state.json must not re-enable it
    plugin::PluginManager again{ r.dir.path / "plugins", r.hub, r.limits };
    again.rescan();
    EXPECT_FALSE(again.list()[0].enabled);
}

TEST(Plugins, NewPluginsStartDisabledAndStateSurvivesRestart)
{
    PluginRig r;
    r.add("p", "function onPacketOut(pkt) pkt:drop() end", false);
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_EQ(r.upstream.writes.size(), 1u); // disabled: no effect
    EXPECT_EQ(r.mgr->set_enabled("p", true), "");
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_EQ(r.upstream.writes.size(), 1u); // now dropping
    plugin::PluginManager again{ r.dir.path / "plugins", r.hub, r.limits };
    again.rescan();
    EXPECT_TRUE(again.list()[0].enabled);
    EXPECT_EQ(r.mgr->set_enabled("p", false), "");
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_EQ(r.upstream.writes.size(), 2u);
    EXPECT_NE(r.mgr->set_enabled("ghost", true), "");
}

TEST(Plugins, ReloadPicksUpEditedSource)
{
    PluginRig r;
    r.add("v", "function onPacketOut(pkt) gt.info('v1') end");
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_TRUE(r.has_log("v", "v1"));
    std::ofstream{ r.dir.path / "plugins" / "v" / "main.lua" } << "function onPacketOut(pkt) gt.info('v2') end";
    EXPECT_EQ(r.mgr->reload("v"), "");
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_TRUE(r.has_log("v", "v2"));
}

TEST(Plugins, ConnectionHooksAndStashedPacketsAreSafe)
{
    PluginRig r;
    r.add("c", R"(
        local stash
        function onConnect(info) gt.info("up:" .. info.peer) end
        function onDisconnect(info) gt.info("down:" .. info.peer) end
        function onPacketOut(pkt) stash = pkt end
        function onPacketIn(pkt) local ok, err = pcall(function() return stash.name end) gt.info("stale_ok:" .. tostring(ok)) end)");
    r.mgr->on_connection(r.flow, "upstream", true);
    r.mgr->on_connection(r.flow, "game_client", false);
    EXPECT_TRUE(r.has_log("c", "up:upstream"));
    EXPECT_TRUE(r.has_log("c", "down:game_client"));
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ClientBound, state_packet());
    EXPECT_TRUE(r.has_log("c", "stale_ok:false"));
}

TEST(Plugins, InjectIsRateLimitedAndOnlyInsideHooks)
{
    PluginRig r;
    r.add("flood", R"(
        local early, err = gt.inject(gt.SERVER_BOUND, "0300000041")
        gt.info("load_inject:" .. tostring(early) .. ":" .. tostring(err))
        function onPacketOut(pkt)
          local n = 0
          for i = 1, 100 do if gt.inject(gt.CLIENT_BOUND, "0300000041") then n = n + 1 end end
          gt.info("sent:" .. n)
        end)");
    EXPECT_TRUE(r.has_log("flood", "load_inject:nil:gt.inject can only be used inside a hook"));
    r.flow.process(Direction::ServerBound, input_packet());
    EXPECT_TRUE(r.has_log("flood", "sent:32"));
    EXPECT_EQ(r.client.writes.size(), 32u);
}

TEST(Plugins, AdminOperationsThroughControlApi)
{
    PluginRig r;
    r.add("x", "function onPacketOut(pkt) end", false);
    core::Config config{ support::write_config(r.dir) };
    control::SessionRecorder rec{ r.hub, r.dir.str("sessions") };
    control::TemplateStore tpl{ r.dir.str("t.json") };
    control::ControlApi api{ config, r.hub, r.flow, r.client, r.upstream, rec, tpl };
    api.set_plugin_admin(r.mgr.get());
    const auto call = [&](const std::string& op, const std::string& p) { return api.handle(op, *control::JsonValue::parse(p)); };
    EXPECT_TRUE(call("plugins.enable", R"({"name":"x"})").ok);
    EXPECT_FALSE(call("plugins.enable", R"({"name":"nope"})").ok);
    EXPECT_TRUE(call("plugins.reload", R"({"name":"x"})").ok);
    const auto list = control::JsonValue::parse(call("plugins.list", "{}").body);
    EXPECT_TRUE(list->find("result")->array[0].flag("enabled"));
    EXPECT_TRUE(call("plugins.logs", R"({"name":"x"})").ok);
    EXPECT_TRUE(call("plugins.disable", R"({"name":"x"})").ok);
}

#ifdef GTPROXY_SOURCE_DIR
namespace {
struct ExampleRig : PluginRig {
    ExampleRig()
    {
        std::filesystem::copy(std::filesystem::path{ GTPROXY_SOURCE_DIR } / "plugins", dir.path / "plugins", std::filesystem::copy_options::recursive);
        mgr = std::make_unique<plugin::PluginManager>(dir.path / "plugins", hub, plugin::Limits{});
        mgr->rescan();
        flow.set_hooks(mgr.get());
    }
};
}

TEST(ExamplePlugins, PacketLoggerLogsEverythingAndChangesNothing)
{
    ExampleRig r;
    ASSERT_EQ(r.mgr->set_enabled("packet_logger", true), "");
    r.flow.process(Direction::ServerBound, input_packet());
    r.flow.process(Direction::ClientBound, call_function_packet());
    r.mgr->on_connection(r.flow, "upstream", false);
    EXPECT_TRUE(r.has_log("packet_logger", "-> text    action|input (29 bytes)"));
    EXPECT_TRUE(r.has_log("packet_logger", "<- variant OnSpawn"));
    EXPECT_TRUE(r.has_log("packet_logger", "upstream disconnected (so far: 1 to client, 1 to server)"));
    EXPECT_EQ(r.upstream.writes[0], input_packet());
    EXPECT_EQ(r.client.writes[0], call_function_packet());
}

TEST(ExamplePlugins, AutoResponderAnswersCommandsAndLeavesChatAlone)
{
    ExampleRig r;
    ASSERT_EQ(r.mgr->set_enabled("auto_responder", true), "");
    r.flow.process(Direction::ServerBound, text_packet(2, "action|input\n|text|!ping"));
    EXPECT_TRUE(r.upstream.writes.empty());          // dropped, server never sees it
    ASSERT_EQ(r.client.writes.size(), 1u);           // reply injected to the client
    const auto d = control::decode_packet(r.client.writes[0]);
    EXPECT_EQ(d.name, "OnConsoleMessage");
    EXPECT_NE(d.fields_json.find("pong"), std::string::npos);

    r.flow.process(Direction::ServerBound, text_packet(2, "action|input\n|text|!echo hello there"));
    EXPECT_NE(control::decode_packet(r.client.writes.back()).fields_json.find("hello there"), std::string::npos);

    r.flow.process(Direction::ServerBound, text_packet(2, "action|input\n|text|just chatting"));
    ASSERT_EQ(r.upstream.writes.size(), 1u);         // ordinary chat is forwarded untouched
    EXPECT_EQ(r.upstream.writes[0], text_packet(2, "action|input\n|text|just chatting"));
}
#endif
