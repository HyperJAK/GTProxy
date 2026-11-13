#pragma once
#include <string>

#include "json_value.hpp"
#include "json_writer.hpp"

namespace control {
inline void write_json(JsonWriter& w, const JsonValue& v)
{
    switch (v.type) {
    case JsonValue::Type::Null: w.null(); break;
    case JsonValue::Type::Bool: w.value(v.boolean); break;
    case JsonValue::Type::Number: w.value(v.number); break;
    case JsonValue::Type::String: w.value(std::string_view{ v.string }); break;
    case JsonValue::Type::Array:
        w.begin_array();
        for (const auto& e : v.array) write_json(w, e);
        w.end_array();
        break;
    case JsonValue::Type::Object:
        w.begin_object();
        for (const auto& [k, e] : v.object) { w.key(k); write_json(w, e); }
        w.end_object();
        break;
    }
}

[[nodiscard]] inline std::string dump_json(const JsonValue& v)
{
    JsonWriter w;
    write_json(w, v);
    return w.take();
}
}
