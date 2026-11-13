# Compatibility shim: provide lua::lua from system Lua 5.4 via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)
if (NOT TARGET lua::lua)
    add_library(lua::lua INTERFACE IMPORTED)
    target_link_libraries(lua::lua INTERFACE PkgConfig::LUA)
endif ()
set(lua_FOUND TRUE)
