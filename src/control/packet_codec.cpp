#include "packet_codec.hpp"

#include <cstring>
#include <magic_enum/magic_enum.hpp>

#include "encoding.hpp"
#include "json_writer.hpp"
#include "../packet/packet_types.hpp"

namespace control {
namespace {
static_assert(sizeof(packet::GameUpdatePacket) == kGameHeaderSize, "GameUpdatePacket must be 56 bytes");

constexpr std::uint8_t kVarFloat = 1, kVarString = 2, kVarVec2 = 3, kVarVec3 = 4, kVarUnsigned = 5, kVarSigned = 9;

template <typename T>
bool read_at(std::span<const std::byte> d, std::size_t& off, T& out)
{
    if (d.size() < off + sizeof(T)) return false;
    std::memcpy(&out, d.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}

template <typename T>
void put(std::vector<std::byte>& out, const T& v)
{
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto idx = s.find(sep, start);
        if (idx == std::string_view::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, idx - start));
        start = idx + 1;
    }
}

DecodedPacket raw_fallback(std::span<const std::byte> data, std::string why, bool malformed)
{
    JsonWriter w;
    w.begin_object().field("length", static_cast<std::uint64_t>(data.size()));
    if (!why.empty()) w.field("note", why);
    w.end_object();
    DecodedPacket d;
    d.kind = "raw";
    d.name = malformed ? "malformed" : "unknown";
    d.summary = why.empty() ? fmt::format("{} bytes", data.size()) : why;
    d.fields_json = w.take();
    d.malformed = malformed;
    return d;
}

// Parses the variant list that follows a CALL_FUNCTION header. Returns the
// number of bytes consumed, or nullopt if the list is truncated/invalid.
std::optional<std::string> variants_to_json(std::span<const std::byte> d, std::string& first_string, std::size_t& count_out, bool& partial)
{
    partial = false;
    if (d.empty()) return std::nullopt;
    std::size_t off = 0;
    std::uint8_t count = 0;
    read_at(d, off, count);
    count_out = count;
    JsonWriter w;
    w.begin_array();
    for (std::uint8_t i = 0; i < count; ++i) {
        std::uint8_t index = 0, type = 0;
        if (!read_at(d, off, index) || !read_at(d, off, type)) { partial = true; break; }
        w.begin_object().field("index", static_cast<unsigned>(index));
        bool ok = true;
        switch (type) {
        case kVarFloat: { float f; if ((ok = read_at(d, off, f))) w.field("type", "float").field("value", static_cast<double>(f)); break; }
        case kVarString: {
            std::uint32_t len = 0;
            ok = read_at(d, off, len) && d.size() - off >= len;
            if (ok) {
                std::string s(reinterpret_cast<const char*>(d.data() + off), len);
                off += len;
                if (i == 0) first_string = s;
                w.field("type", "string").field("value", std::string_view{ s });
            }
            break;
        }
        case kVarVec2: {
            float x, y;
            if ((ok = read_at(d, off, x) && read_at(d, off, y))) {
                w.field("type", "vec2").key("value").begin_array().value(static_cast<double>(x)).value(static_cast<double>(y)).end_array();
            }
            break;
        }
        case kVarVec3: {
            float x, y, z;
            if ((ok = read_at(d, off, x) && read_at(d, off, y) && read_at(d, off, z))) {
                w.field("type", "vec3").key("value").begin_array().value(static_cast<double>(x)).value(static_cast<double>(y)).value(static_cast<double>(z)).end_array();
            }
            break;
        }
        case kVarUnsigned: { std::uint32_t u; if ((ok = read_at(d, off, u))) w.field("type", "unsigned").field("value", u); break; }
        case kVarSigned: { std::int32_t s; if ((ok = read_at(d, off, s))) w.field("type", "signed").field("value", s); break; }
        default:
            w.field("type", "unknown").field("code", static_cast<unsigned>(type));
            ok = false;
            break;
        }
        w.end_object();
        if (!ok) { partial = true; break; }
    }
    w.end_array();
    return w.take();
}

void header_to_json(JsonWriter& w, const packet::GameUpdatePacket& h)
{
    w.key("header").begin_object();
    w.field("type", static_cast<unsigned>(h.type));
    w.field("type_name", std::string{ magic_enum::enum_name(h.type) });
    w.field("net_id", static_cast<std::uint32_t>(h.net_id));
    w.field("item_net_id", h.item_net_id);
    w.field("flags", static_cast<std::uint32_t>(h.flags.value));
    w.field("float_var", static_cast<double>(h.float_var));
    w.field("int_data", h.int_data);
    w.field("pos_x", static_cast<double>(h.pos_x));
    w.field("pos_y", static_cast<double>(h.pos_y));
    w.field("pos_x2", static_cast<double>(h.pos_x2));
    w.field("pos_y2", static_cast<double>(h.pos_y2));
    w.field("int_x", h.int_x);
    w.field("int_y", h.int_y);
    w.field("data_size", h.data_size);
    w.end_object();
}

DecodedPacket decode_text(std::uint32_t msg_type, std::span<const std::byte> body)
{
    std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    while (!text.empty() && text.back() == '\0') text.pop_back();

    JsonWriter w;
    w.begin_object();
    w.field("msg_type", msg_type);
    w.field("msg_type_name", message_type_name(msg_type));
    w.field("text", std::string_view{ text });
    w.key("lines").begin_array();
    std::string name;
    for (const auto line : split(text, '\n')) {
        if (line.empty()) continue;
        auto parts = split(line, '|');
        // "|key|value" lines (leading pipe) are keyed by their first field.
        const bool leading_pipe = parts[0].empty() && parts.size() >= 2;
        if (leading_pipe) parts.erase(parts.begin());
        w.begin_object().field("key", parts[0]);
        if (leading_pipe) w.field("leading_pipe", true);
        w.key("values").begin_array();
        for (std::size_t i = 1; i < parts.size(); ++i) w.value(parts[i]);
        w.end_array().end_object();
        if (name.empty()) {
            name = parts.size() > 1 && parts[0] == "action" ? fmt::format("action|{}", parts[1]) : std::string{ parts[0] };
        }
    }
    w.end_array().end_object();

    DecodedPacket d;
    d.kind = "text";
    d.name = name.empty() ? message_type_name(msg_type) : name;
    d.summary = text.substr(0, 120);
    for (auto& c : d.summary) if (c == '\n') c = ' ';
    d.fields_json = w.take();
    return d;
}
} // namespace

std::string message_type_name(const std::uint32_t t)
{
    if (t < packet::NET_MESSAGE_MAX) {
        return std::string{ magic_enum::enum_name(static_cast<packet::NetMessageType>(t)) };
    }
    return fmt::format("NET_MESSAGE_{}", t);
}

DecodedPacket decode_packet(const std::span<const std::byte> data)
{
    using namespace packet;
    if (data.size() < 4) {
        return raw_fallback(data, "shorter than the 4-byte message type", true);
    }

    std::uint32_t msg_type = 0;
    std::memcpy(&msg_type, data.data(), 4);
    const auto body = data.subspan(4);

    switch (msg_type) {
    case NET_MESSAGE_SERVER_HELLO: {
        JsonWriter w;
        w.begin_object().field("msg_type", msg_type).end_object();
        return { "hello", "ServerHello", "server hello", w.take(), false };
    }
    case NET_MESSAGE_GENERIC_TEXT:
    case NET_MESSAGE_GAME_MESSAGE:
    case NET_MESSAGE_ERROR:
    case NET_MESSAGE_TRACK:
    case NET_MESSAGE_CLIENT_LOG_REQUEST:
    case NET_MESSAGE_CLIENT_LOG_RESPONSE:
        return decode_text(msg_type, body);
    case NET_MESSAGE_GAME_PACKET:
        break;
    default:
        return raw_fallback(data, fmt::format("unrecognised message type {}", msg_type), false);
    }

    if (body.size() < kGameHeaderSize) {
        return raw_fallback(data, "game packet shorter than its 56-byte header", true);
    }

    GameUpdatePacket h{};
    std::memcpy(&h, body.data(), kGameHeaderSize);
    const auto extra_all = body.subspan(kGameHeaderSize);

    // data_size describes the extra block. If it lies about its length we
    // still show everything that is present and flag the packet.
    bool size_mismatch = h.flags.extended && h.data_size > extra_all.size();
    const auto extra_len = h.flags.extended ? std::min<std::size_t>(h.data_size, extra_all.size()) : extra_all.size();
    const auto extra = extra_all.subspan(0, extra_len);

    JsonWriter w;
    w.begin_object();
    header_to_json(w, h);

    DecodedPacket d;
    d.malformed = size_mismatch;
    if (h.type == PACKET_CALL_FUNCTION && !extra.empty()) {
        std::string fn;
        std::size_t count = 0;
        bool partial = false;
        auto vars = variants_to_json(extra, fn, count, partial);
        if (vars) {
            w.key("variants").raw(*vars);
            d.kind = "variant";
            d.name = fn.empty() ? "CallFunction" : fn;
            d.summary = fmt::format("{}({} args)", d.name, count);
            d.malformed = d.malformed || partial;
            if (partial) w.field("note", "variant list truncated or contains an unknown type");
        }
    }
    if (d.kind.empty()) {
        d.kind = "game";
        d.name = std::string{ magic_enum::enum_name(h.type) };
        if (d.name.empty()) d.name = fmt::format("PACKET_{}", static_cast<unsigned>(h.type));
        d.summary = fmt::format("{} extra={}B", d.name, extra.size());
    }
    w.field("extra_hex", encoding::to_hex(extra));
    if (size_mismatch) w.field("note", "data_size is larger than the bytes present");
    w.end_object();
    d.fields_json = w.take();
    return d;
}

// ---------------------------------------------------------------------------
// Encoding

namespace {
EncodeResult fail(std::string msg) { return { {}, std::move(msg) }; }

std::optional<std::uint8_t> variant_code(std::string_view t)
{
    if (t == "float") return kVarFloat;
    if (t == "string") return kVarString;
    if (t == "vec2") return kVarVec2;
    if (t == "vec3") return kVarVec3;
    if (t == "unsigned") return kVarUnsigned;
    if (t == "signed") return kVarSigned;
    return std::nullopt;
}

// Numbers may arrive as JSON numbers or numeric strings (UI text boxes).
std::optional<double> as_number(const JsonValue& v)
{
    if (v.is_number()) return v.number;
    if (v.is_string()) {
        try {
            std::size_t used = 0;
            const double d = std::stod(v.string, &used);
            if (used == v.string.size()) return d;
        } catch (...) {}
    }
    return std::nullopt;
}

bool serialise_variants(const JsonValue& values, std::vector<std::byte>& out, std::string& err)
{
    if (!values.is_array() || values.array.empty()) { err = "variant packets need a non-empty \"values\" array"; return false; }
    if (values.array.size() > 255) { err = "too many variant values (max 255)"; return false; }
    put(out, static_cast<std::uint8_t>(values.array.size()));
    std::uint8_t idx = 0;
    for (const auto& item : values.array) {
        const auto code = variant_code(item.str("type"));
        const auto* val = item.find("value");
        if (!code || !val) { err = fmt::format("values[{}] needs a known \"type\" and a \"value\"", idx); return false; }
        put(out, idx);
        put(out, *code);
        const auto number_of = [&](const JsonValue& v, double& n) { auto r = as_number(v); if (!r) return false; n = *r; return true; };
        switch (*code) {
        case kVarString: {
            if (!val->is_string()) { err = fmt::format("values[{}] must be a string", idx); return false; }
            put(out, static_cast<std::uint32_t>(val->string.size()));
            const auto* p = reinterpret_cast<const std::byte*>(val->string.data());
            out.insert(out.end(), p, p + val->string.size());
            break;
        }
        case kVarFloat: {
            double n; if (!number_of(*val, n)) { err = fmt::format("values[{}] must be a number", idx); return false; }
            put(out, static_cast<float>(n));
            break;
        }
        case kVarUnsigned: {
            double n; if (!number_of(*val, n) || n < 0 || n > 4294967295.0) { err = fmt::format("values[{}] must be 0..4294967295", idx); return false; }
            put(out, static_cast<std::uint32_t>(n));
            break;
        }
        case kVarSigned: {
            double n; if (!number_of(*val, n) || n < -2147483648.0 || n > 2147483647.0) { err = fmt::format("values[{}] must be a 32-bit signed integer", idx); return false; }
            put(out, static_cast<std::int32_t>(n));
            break;
        }
        default: { // vec2 / vec3
            const std::size_t dims = *code == kVarVec2 ? 2 : 3;
            if (!val->is_array() || val->array.size() != dims) { err = fmt::format("values[{}] must be an array of {} numbers", idx, dims); return false; }
            for (const auto& comp : val->array) {
                double n; if (!number_of(comp, n)) { err = fmt::format("values[{}] has a non-numeric component", idx); return false; }
                put(out, static_cast<float>(n));
            }
            break;
        }
        }
        ++idx;
    }
    return true;
}

packet::GameUpdatePacket header_from_json(const JsonValue* h)
{
    packet::GameUpdatePacket g{};
    if (!h) return g;
    g.type = static_cast<packet::PacketType>(h->integer("type", 0));
    g.net_id = static_cast<std::uint32_t>(h->integer("net_id", 0));
    g.item_net_id = static_cast<std::int32_t>(h->integer("item_net_id", 0));
    g.flags.value = static_cast<packet::PacketFlag>(static_cast<std::uint32_t>(h->integer("flags", 0)));
    g.float_var = static_cast<float>(h->num("float_var", 0));
    g.int_data = static_cast<std::int32_t>(h->integer("int_data", 0));
    g.pos_x = static_cast<float>(h->num("pos_x", 0));
    g.pos_y = static_cast<float>(h->num("pos_y", 0));
    g.pos_x2 = static_cast<float>(h->num("pos_x2", 0));
    g.pos_y2 = static_cast<float>(h->num("pos_y2", 0));
    g.int_x = static_cast<std::int32_t>(h->integer("int_x", 0));
    g.int_y = static_cast<std::int32_t>(h->integer("int_y", 0));
    return g;
}
} // namespace

EncodeResult encode_packet(const JsonValue& spec)
{
    using namespace packet;
    if (!spec.is_object()) return fail("packet spec must be a JSON object");
    const auto kind = spec.str("kind");
    std::vector<std::byte> out;

    if (kind == "raw") {
        auto bytes = encoding::from_hex(spec.str("hex"));
        if (!bytes) return fail("\"hex\" is not valid hex (odd length or non-hex character)");
        if (bytes->empty()) return fail("\"hex\" is empty");
        if (bytes->size() > kMaxPacketSize) return fail("packet too large");
        return { std::move(*bytes), {} };
    }

    if (kind == "text") {
        const auto mt = static_cast<std::uint32_t>(spec.integer("msg_type", NET_MESSAGE_GENERIC_TEXT));
        if (!spec.has("text")) return fail("text packets need a \"text\" string");
        auto text = spec.str("text");
        put(out, mt);
        const auto* p = reinterpret_cast<const std::byte*>(text.data());
        out.insert(out.end(), p, p + text.size());
        out.push_back(std::byte{ 0 });
        return { std::move(out), {} };
    }

    if (kind == "variant" || kind == "game") {
        auto h = header_from_json(spec.find("header"));
        std::vector<std::byte> extra;
        if (kind == "variant") {
            h.type = PACKET_CALL_FUNCTION;
            std::string err;
            if (!serialise_variants(spec.find("values") ? *spec.find("values") : JsonValue{}, extra, err)) return fail(err);
        } else if (spec.has("extra_hex")) {
            auto e = encoding::from_hex(spec.str("extra_hex"));
            if (!e) return fail("\"extra_hex\" is not valid hex");
            extra = std::move(*e);
        }
        if (extra.size() > kMaxPacketSize) return fail("packet too large");
        if (!extra.empty()) {
            h.flags.value = static_cast<PacketFlag>(static_cast<std::uint32_t>(h.flags.value) | PACKET_FLAG_EXTENDED);
        }
        h.data_size = static_cast<std::uint32_t>(extra.size());
        put(out, static_cast<std::uint32_t>(NET_MESSAGE_GAME_PACKET));
        put(out, h);
        out.insert(out.end(), extra.begin(), extra.end());
        return { std::move(out), {} };
    }

    return fail("\"kind\" must be one of raw, text, variant, game");
}
}
