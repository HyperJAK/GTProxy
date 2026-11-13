#include <gtest/gtest.h>

#include "fixtures.hpp"
#include "control/encoding.hpp"
#include "control/json_value.hpp"
#include "control/json_writer.hpp"
#include "control/packet_codec.hpp"
#include "control/ws_server.hpp"

using namespace control;
using namespace fixtures;

TEST(Encoding, HexRoundTripAndTolerance)
{
    const Bytes b = hex("de AD:be,0xef");
    ASSERT_EQ(b.size(), 4u);
    EXPECT_EQ(encoding::to_hex(b), "deadbeef");
    EXPECT_FALSE(encoding::from_hex("abc"));
    EXPECT_FALSE(encoding::from_hex("zz"));
    EXPECT_TRUE(encoding::from_hex("")->empty());
}

TEST(Encoding, Base64AndSha1Vectors)
{
    EXPECT_EQ(encoding::base64_encode(encoding::as_bytes("foobar")), "Zm9vYmFy");
    EXPECT_EQ(encoding::base64_encode(encoding::as_bytes("fo")), "Zm8=");
    const auto d = encoding::base64_decode("Zm9vYg==");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->size(), 4u);
    // RFC 6455 section 1.3 example
    EXPECT_EQ(WsServer::accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(Json, ParseAndEscape)
{
    const auto v = JsonValue::parse(R"({"a":[1,2.5,"x\né"],"b":{"c":true},"n":null})");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->find("a")->array.size(), 3u);
    EXPECT_EQ(v->find("a")->array[2].string, "x\n\xC3\xA9");
    EXPECT_TRUE(v->find("b")->flag("c"));
    EXPECT_FALSE(JsonValue::parse("{\"a\":}"));
    EXPECT_FALSE(JsonValue::parse("[1,2"));
    EXPECT_FALSE(JsonValue::parse("{} trailing"));

    JsonWriter w;
    w.begin_object().field("s", std::string_view{ "a\"b\x01\xff" }).end_object();
    const auto round = JsonValue::parse(w.str());
    ASSERT_TRUE(round) << w.str(); // invalid UTF-8 must still yield valid JSON
}

TEST(Codec, DecodesTextPacket)
{
    const auto d = decode_packet(input_packet());
    EXPECT_EQ(d.kind, "text");
    EXPECT_EQ(d.name, "action|input");
    const auto f = JsonValue::parse(d.fields_json);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->str("text"), "action|input\n|text|hello");
    EXPECT_EQ(f->find("lines")->array.size(), 2u);
}

TEST(Codec, DecodesVariantCall)
{
    const auto d = decode_packet(call_function_packet());
    EXPECT_EQ(d.kind, "variant");
    EXPECT_EQ(d.name, "OnSpawn");
    EXPECT_FALSE(d.malformed);
    const auto f = JsonValue::parse(d.fields_json);
    ASSERT_TRUE(f);
    const auto& vars = f->find("variants")->array;
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[1].str("type"), "signed");
    EXPECT_EQ(vars[1].integer("value"), 42);
}

TEST(Codec, DecodesStatePacketHeader)
{
    const auto d = decode_packet(state_packet());
    EXPECT_EQ(d.kind, "game");
    EXPECT_EQ(d.name, "PACKET_STATE");
    const auto f = JsonValue::parse(d.fields_json);
    EXPECT_EQ(f->find("header")->num("pos_x"), 12.5);
    EXPECT_EQ(f->find("header")->integer("net_id"), 7);
}

TEST(Codec, FallsBackToHexForGarbage)
{
    EXPECT_TRUE(decode_packet(hex("0102")).malformed);
    const auto unknown = decode_packet(hex("63000000aabb"));
    EXPECT_EQ(unknown.kind, "raw");
    EXPECT_FALSE(unknown.malformed);
    auto truncated = state_packet();
    truncated.resize(30);
    EXPECT_TRUE(decode_packet(truncated).malformed);
    // data_size claims more than is present
    auto lying = game_packet(1, 0, 8, variant_extra_onspawn());
    lying.resize(lying.size() - 10);
    EXPECT_TRUE(decode_packet(lying).malformed);
    EXPECT_TRUE(JsonValue::parse(decode_packet(lying).fields_json).has_value());
}

TEST(Codec, FuzzNeverCrashesAndAlwaysEmitsValidJson)
{
    std::uint32_t seed = 12345;
    const auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 16; };
    const Bytes bases[] = { input_packet(), call_function_packet(), state_packet() };
    for (int i = 0; i < 2000; ++i) {
        Bytes b = bases[i % 3];
        for (int k = 0; k < 4; ++k) b[rnd() % b.size()] = static_cast<std::byte>(rnd());
        b.resize(rnd() % (b.size() + 1));
        const auto d = decode_packet(b);
        ASSERT_TRUE(JsonValue::parse(d.fields_json)) << encoding::to_hex(b);
    }
}

TEST(Codec, EncodeMatchesHandAssembledFixtures)
{
    auto spec = JsonValue::parse(R"({"kind":"text","msg_type":2,"text":"action|input\n|text|hello"})");
    EXPECT_EQ(encode_packet(*spec).bytes, input_packet());

    spec = JsonValue::parse(R"({"kind":"variant","header":{"net_id":4294967295},
        "values":[{"type":"string","value":"OnSpawn"},{"type":"signed","value":"42"}]})");
    const auto r = encode_packet(*spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.bytes, call_function_packet());

    spec = JsonValue::parse(R"({"kind":"game","header":{"type":0,"net_id":7,"pos_x":12.5,"pos_y":30},"extra_hex":""})");
    EXPECT_EQ(encode_packet(*spec).bytes, state_packet());
}

TEST(Codec, DecodeThenEncodeRoundTripsVariant)
{
    const auto original = call_function_packet();
    const auto d = decode_packet(original);
    const auto f = JsonValue::parse(d.fields_json);
    JsonValue spec;
    spec.type = JsonValue::Type::Object;
    spec.object.emplace_back("kind", JsonValue{ JsonValue::Type::String, false, 0, "variant", {}, {} });
    spec.object.emplace_back("header", *f->find("header"));
    spec.object.emplace_back("values", *f->find("variants"));
    EXPECT_EQ(encode_packet(spec).bytes, original);
}

TEST(Codec, EncodeRejectsBadSpecs)
{
    EXPECT_FALSE(encode_packet(*JsonValue::parse(R"({"kind":"raw","hex":"abc"})")).ok());
    EXPECT_FALSE(encode_packet(*JsonValue::parse(R"({"kind":"variant","values":[]})")).ok());
    EXPECT_FALSE(encode_packet(*JsonValue::parse(R"({"kind":"variant","values":[{"type":"unsigned","value":-1}]})")).ok());
    EXPECT_FALSE(encode_packet(*JsonValue::parse(R"({"kind":"nope"})")).ok());
    EXPECT_FALSE(encode_packet(*JsonValue::parse("[]")).ok());
}
