#pragma once
#include <sol/sol.hpp>

#include "../control/json_value.hpp"

namespace plugin {
// JSON <-> Lua conversions used by the plugin API. Arrays are Lua tables with
// keys 1..n; everything else becomes an object. Depth-limited.
[[nodiscard]] sol::object to_lua(sol::state_view lua, const control::JsonValue& v, int depth = 0);
[[nodiscard]] control::JsonValue from_lua(const sol::object& o, int depth = 0);
}
