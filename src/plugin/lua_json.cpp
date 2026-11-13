#include "lua_json.hpp"

namespace plugin {
using control::JsonValue;

sol::object to_lua(sol::state_view lua, const JsonValue& v, const int depth)
{
    if (depth > 32) {
        return sol::make_object(lua, sol::lua_nil);
    }
    switch (v.type) {
    case JsonValue::Type::Null: return sol::make_object(lua, sol::lua_nil);
    case JsonValue::Type::Bool: return sol::make_object(lua, v.boolean);
    case JsonValue::Type::Number: return sol::make_object(lua, v.number);
    case JsonValue::Type::String: return sol::make_object(lua, v.string);
    case JsonValue::Type::Array: {
        sol::table t = lua.create_table(static_cast<int>(v.array.size()), 0);
        int i = 1;
        for (const auto& e : v.array) t[i++] = to_lua(lua, e, depth + 1);
        return t;
    }
    case JsonValue::Type::Object: {
        sol::table t = lua.create_table(0, static_cast<int>(v.object.size()));
        for (const auto& [k, e] : v.object) t[k] = to_lua(lua, e, depth + 1);
        return t;
    }
    }
    return sol::make_object(lua, sol::lua_nil);
}

JsonValue from_lua(const sol::object& o, const int depth)
{
    JsonValue v;
    if (depth > 32) return v;
    switch (o.get_type()) {
    case sol::type::boolean: v.type = JsonValue::Type::Bool; v.boolean = o.as<bool>(); break;
    case sol::type::number: v.type = JsonValue::Type::Number; v.number = o.as<double>(); break;
    case sol::type::string: v.type = JsonValue::Type::String; v.string = o.as<std::string>(); break;
    case sol::type::table: {
        const sol::table t = o.as<sol::table>();
        std::size_t count = 0, max_index = 0;
        bool all_int = true;
        for (const auto& kv : t) {
            ++count;
            if (kv.first.get_type() == sol::type::number) {
                const double d = kv.first.as<double>();
                if (d >= 1 && d == static_cast<double>(static_cast<std::size_t>(d))) {
                    max_index = std::max(max_index, static_cast<std::size_t>(d));
                    continue;
                }
            }
            all_int = false;
        }
        if (count > 0 && all_int && max_index == count) {
            v.type = JsonValue::Type::Array;
            for (std::size_t i = 1; i <= count; ++i) v.array.push_back(from_lua(t[i], depth + 1));
        } else {
            v.type = JsonValue::Type::Object;
            for (const auto& kv : t) {
                if (kv.first.get_type() == sol::type::string) {
                    v.object.emplace_back(kv.first.as<std::string>(), from_lua(kv.second, depth + 1));
                }
            }
        }
        break;
    }
    default: break; // nil, functions, userdata -> null
    }
    return v;
}
}
