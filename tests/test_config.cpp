#include <gtest/gtest.h>

#include "control/support.hpp"
#include "core/config.hpp"
#include "utils/network.hpp"

TEST(Config, WritesDefaultsWithPrivateServerProfiles)
{
    support::TempDir d;
    core::Config c{ d.str("config.json") };
    EXPECT_TRUE(std::filesystem::exists(d.str("config.json")));
    const auto profiles = c.get_profiles();
    ASSERT_GE(profiles.size(), 2u);
    EXPECT_EQ(c.get_active_profile_name(), "localhost");
    EXPECT_EQ(c.get_server_config().address, "127.0.0.1");
    EXPECT_EQ(c.get_client_config().dns_server, "system"); // no outside lookups by default
    EXPECT_FALSE(c.get_control_config().allow_remote);
}

TEST(Config, LegacyFileWithoutProfilesGetsOne)
{
    support::TempDir d;
    {
        std::ofstream f{ d.str("config.json") };
        f << R"({"server":{"port":1234,"address":"10.0.0.5"}})";
    }
    core::Config c{ d.str("config.json") };
    ASSERT_EQ(c.get_profiles().size(), 1u);
    EXPECT_EQ(c.get_active_profile()->address, "10.0.0.5");
    EXPECT_EQ(c.get_server_config().port, 1234);
}

TEST(Config, ProfileValidationAndSwitch)
{
    support::TempDir d;
    core::Config c{ d.str("config.json") };
    core::Config::ProfileConfig p;
    p.name = "x";
    p.address = "";
    EXPECT_FALSE(c.upsert_profile(p).empty());
    p.address = "192.168.1.9";
    EXPECT_TRUE(c.upsert_profile(p).empty());
    EXPECT_TRUE(c.set_active_profile("x").empty());
    EXPECT_FALSE(c.set_active_profile("missing").empty());
    EXPECT_FALSE(c.remove_profile("x").empty());
}

TEST(Network, IpClassificationAcceptsZeroOctets)
{
    EXPECT_TRUE(utils::network::is_valid_ip_address("10.0.0.5"));
    EXPECT_TRUE(utils::network::is_valid_ip_address("192.168.1.100"));
    EXPECT_FALSE(utils::network::is_valid_ip_address("example.com"));
    EXPECT_FALSE(utils::network::is_valid_ip_address("1.2.3.256"));
}

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include "control/json_value.hpp"
#include "core/json_log_formatter.hpp"

TEST(StructuredLogging, EmitsOneValidJsonObjectPerRecord)
{
    std::ostringstream out;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
    sink->set_formatter(std::make_unique<core::JsonLogFormatter>());
    spdlog::logger logger{ "test", sink };
    logger.info("hello \"quoted\"\nnewline {}", 42);
    logger.error("second");

    std::istringstream in{ out.str() };
    std::string line;
    std::vector<control::JsonValue> records;
    while (std::getline(in, line)) {
        const auto v = control::JsonValue::parse(line);
        ASSERT_TRUE(v) << line;
        records.push_back(*v);
    }
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].str("level"), "info");
    EXPECT_EQ(records[0].str("msg"), "hello \"quoted\"\nnewline 42");
    EXPECT_EQ(records[0].str("logger"), "test");
    EXPECT_EQ(records[1].str("level"), "error");
    EXPECT_EQ(records[0].str("ts").size(), 24u); // 2026-11-13T07:30:55.123Z
}
