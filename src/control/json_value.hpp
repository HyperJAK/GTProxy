#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace control {
// Small DOM + parser for control-plane messages (RFC 8259, depth-limited).
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{ Type::Null };
    bool boolean{ false };
    double number{ 0 };
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] bool is_null() const { return type == Type::Null; }
    [[nodiscard]] bool is_object() const { return type == Type::Object; }
    [[nodiscard]] bool is_array() const { return type == Type::Array; }
    [[nodiscard]] bool is_string() const { return type == Type::String; }
    [[nodiscard]] bool is_number() const { return type == Type::Number; }
    [[nodiscard]] bool is_bool() const { return type == Type::Bool; }

    [[nodiscard]] const JsonValue* find(std::string_view key) const
    {
        if (type != Type::Object) return nullptr;
        for (const auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    [[nodiscard]] std::string str(std::string_view key, std::string def = {}) const
    {
        const auto* v = find(key);
        return v && v->is_string() ? v->string : def;
    }

    [[nodiscard]] std::int64_t integer(std::string_view key, std::int64_t def = 0) const
    {
        const auto* v = find(key);
        return v && v->is_number() ? static_cast<std::int64_t>(v->number) : def;
    }

    [[nodiscard]] double num(std::string_view key, double def = 0) const
    {
        const auto* v = find(key);
        return v && v->is_number() ? v->number : def;
    }

    [[nodiscard]] bool flag(std::string_view key, bool def = false) const
    {
        const auto* v = find(key);
        return v && v->is_bool() ? v->boolean : def;
    }

    [[nodiscard]] bool has(std::string_view key) const { return find(key) != nullptr; }

    [[nodiscard]] static std::optional<JsonValue> parse(std::string_view text)
    {
        Parser p{ text };
        p.skip_ws();
        auto v = p.parse_value(0);
        if (!v) return std::nullopt;
        p.skip_ws();
        if (p.pos != text.size()) return std::nullopt;
        return v;
    }

private:
    struct Parser {
        std::string_view s;
        std::size_t pos{ 0 };

        explicit Parser(std::string_view text) : s{ text } { }

        void skip_ws()
        {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos;
        }

        bool consume(std::string_view lit)
        {
            if (s.substr(pos, lit.size()) == lit) { pos += lit.size(); return true; }
            return false;
        }

        std::optional<JsonValue> parse_value(int depth)
        {
            if (depth > 64 || pos >= s.size()) return std::nullopt;
            JsonValue v;
            const char c = s[pos];
            if (c == '{') {
                ++pos; v.type = Type::Object; skip_ws();
                if (pos < s.size() && s[pos] == '}') { ++pos; return v; }
                while (true) {
                    skip_ws();
                    auto k = parse_string();
                    if (!k) return std::nullopt;
                    skip_ws();
                    if (pos >= s.size() || s[pos] != ':') return std::nullopt;
                    ++pos; skip_ws();
                    auto val = parse_value(depth + 1);
                    if (!val) return std::nullopt;
                    v.object.emplace_back(std::move(*k), std::move(*val));
                    skip_ws();
                    if (pos >= s.size()) return std::nullopt;
                    if (s[pos] == ',') { ++pos; continue; }
                    if (s[pos] == '}') { ++pos; return v; }
                    return std::nullopt;
                }
            }
            if (c == '[') {
                ++pos; v.type = Type::Array; skip_ws();
                if (pos < s.size() && s[pos] == ']') { ++pos; return v; }
                while (true) {
                    skip_ws();
                    auto val = parse_value(depth + 1);
                    if (!val) return std::nullopt;
                    v.array.push_back(std::move(*val));
                    skip_ws();
                    if (pos >= s.size()) return std::nullopt;
                    if (s[pos] == ',') { ++pos; continue; }
                    if (s[pos] == ']') { ++pos; return v; }
                    return std::nullopt;
                }
            }
            if (c == '"') {
                auto str = parse_string();
                if (!str) return std::nullopt;
                v.type = Type::String; v.string = std::move(*str);
                return v;
            }
            if (consume("true")) { v.type = Type::Bool; v.boolean = true; return v; }
            if (consume("false")) { v.type = Type::Bool; v.boolean = false; return v; }
            if (consume("null")) { return v; }
            return parse_number();
        }

        std::optional<JsonValue> parse_number()
        {
            const std::size_t start = pos;
            if (pos < s.size() && s[pos] == '-') ++pos;
            std::size_t digits = 0;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { ++pos; ++digits; }
            if (digits == 0) return std::nullopt;
            if (pos < s.size() && s[pos] == '.') {
                ++pos;
                std::size_t frac = 0;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { ++pos; ++frac; }
                if (frac == 0) return std::nullopt;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                std::size_t ex = 0;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') { ++pos; ++ex; }
                if (ex == 0) return std::nullopt;
            }
            JsonValue v;
            v.type = Type::Number;
            try {
                v.number = std::stod(std::string{ s.substr(start, pos - start) });
            } catch (...) {
                return std::nullopt;
            }
            return v;
        }

        static void append_utf8(std::string& out, std::uint32_t cp)
        {
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        std::optional<std::uint32_t> hex4()
        {
            if (pos + 4 > s.size()) return std::nullopt;
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                const char h = s[pos++];
                v <<= 4;
                if (h >= '0' && h <= '9') v |= static_cast<std::uint32_t>(h - '0');
                else if (h >= 'a' && h <= 'f') v |= static_cast<std::uint32_t>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v |= static_cast<std::uint32_t>(h - 'A' + 10);
                else return std::nullopt;
            }
            return v;
        }

        std::optional<std::string> parse_string()
        {
            if (pos >= s.size() || s[pos] != '"') return std::nullopt;
            ++pos;
            std::string out;
            while (pos < s.size()) {
                const char c = s[pos++];
                if (c == '"') return out;
                if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
                if (c != '\\') { out += c; continue; }
                if (pos >= s.size()) return std::nullopt;
                const char e = s[pos++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto cp = hex4();
                    if (!cp) return std::nullopt;
                    if (*cp >= 0xD800 && *cp <= 0xDBFF) {
                        if (!consume("\\u")) return std::nullopt;
                        auto lo = hex4();
                        if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return std::nullopt;
                        *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
                    }
                    append_utf8(out, *cp);
                    break;
                }
                default: return std::nullopt;
                }
            }
            return std::nullopt;
        }
    };
};
}
