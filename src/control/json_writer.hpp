#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>

namespace control {
// Minimal streaming JSON builder. Handles commas, string escaping and
// invalid UTF-8 (replaced by U+FFFD) so packet text can never produce
// a malformed document.
class JsonWriter {
public:
    JsonWriter& begin_object() { value_prefix(); out_ += '{'; first_.push_back(true); return *this; }
    JsonWriter& end_object() { out_ += '}'; first_.pop_back(); return *this; }
    JsonWriter& begin_array() { value_prefix(); out_ += '['; first_.push_back(true); return *this; }
    JsonWriter& end_array() { out_ += ']'; first_.pop_back(); return *this; }

    JsonWriter& key(std::string_view k)
    {
        value_prefix();
        append_string(k);
        out_ += ':';
        after_key_ = true;
        return *this;
    }

    JsonWriter& value(std::string_view s) { value_prefix(); append_string(s); return *this; }
    JsonWriter& value(const char* s) { return value(std::string_view{ s }); }
    JsonWriter& value(bool b) { value_prefix(); out_ += b ? "true" : "false"; return *this; }
    JsonWriter& value(std::int64_t n) { value_prefix(); out_ += std::to_string(n); return *this; }
    JsonWriter& value(std::uint64_t n) { value_prefix(); out_ += std::to_string(n); return *this; }
    JsonWriter& value(int n) { return value(static_cast<std::int64_t>(n)); }
    JsonWriter& value(unsigned n) { return value(static_cast<std::uint64_t>(n)); }
    JsonWriter& value(double d)
    {
        value_prefix();
        if (!std::isfinite(d)) {
            out_ += "null";
        } else {
            out_ += fmt::format("{}", d);
        }
        return *this;
    }
    JsonWriter& null() { value_prefix(); out_ += "null"; return *this; }
    // Insert an already-serialised JSON fragment.
    JsonWriter& raw(std::string_view json) { value_prefix(); out_ += json; return *this; }

    template <typename T>
    JsonWriter& field(std::string_view k, const T& v) { key(k); return value(v); }

    [[nodiscard]] const std::string& str() const { return out_; }
    [[nodiscard]] std::string take() { return std::move(out_); }

private:
    void value_prefix()
    {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (first_.empty()) {
            return;
        }
        if (first_.back()) {
            first_.back() = false;
        } else {
            out_ += ',';
        }
    }

    void append_string(std::string_view s)
    {
        out_ += '"';
        for (std::size_t i = 0; i < s.size();) {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c == '"') { out_ += "\\\""; ++i; }
            else if (c == '\\') { out_ += "\\\\"; ++i; }
            else if (c == '\n') { out_ += "\\n"; ++i; }
            else if (c == '\r') { out_ += "\\r"; ++i; }
            else if (c == '\t') { out_ += "\\t"; ++i; }
            else if (c < 0x20 || c == 0x7f) { out_ += fmt::format("\\u{:04x}", c); ++i; }
            else if (c < 0x80) { out_ += static_cast<char>(c); ++i; }
            else {
                const std::size_t len = utf8_len(s, i);
                if (len == 0) {
                    out_ += "\xEF\xBF\xBD";
                    ++i;
                } else {
                    out_.append(s.substr(i, len));
                    i += len;
                }
            }
        }
        out_ += '"';
    }

    // Returns the length of the valid UTF-8 sequence at i, or 0 if invalid.
    static std::size_t utf8_len(std::string_view s, std::size_t i)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        else return 0;
        if (i + n > s.size()) return 0;
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
        }
        return n;
    }

    std::string out_;
    std::vector<bool> first_;
    bool after_key_{ false };
};
}
