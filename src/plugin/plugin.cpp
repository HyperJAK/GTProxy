#include "plugin.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <spdlog/spdlog.h>

#include "lua_json.hpp"
#include "../control/encoding.hpp"
#include "../control/json_dump.hpp"
#include "../control/json_writer.hpp"

namespace plugin {
using control::JsonValue;

// What a hook receives. Only valid while the hook runs.
struct Plugin::LuaPacket {
    event::Direction dir;
    std::uint64_t id;
    std::vector<std::byte> bytes;
    control::DecodedPacket decoded;
    std::optional<JsonValue> decoded_json;
    bool dropped{ false };
    bool valid{ true };

    void refresh()
    {
        decoded = control::decode_packet(bytes);
        decoded_json = JsonValue::parse(decoded.fields_json);
    }
    void check() const
    {
        if (!valid) throw std::runtime_error{ "packet objects are only valid inside the hook that received them" };
    }
};

namespace {
void budget_hook(lua_State* L, lua_Debug*)
{
    auto* self = *static_cast<Plugin**>(lua_getextraspace(L));
    if (self && self->budget_exceeded()) {
        luaL_error(L, "plugin exceeded its execution budget (infinite loop?)");
    }
}

std::int64_t now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}
}

void* Plugin::limited_alloc(void* ud, void* ptr, const std::size_t osize, const std::size_t nsize)
{
    auto* m = static_cast<Mem*>(ud);
    if (nsize == 0) {
        if (ptr) m->used -= osize;
        std::free(ptr);
        return nullptr;
    }
    const std::size_t old = ptr ? osize : 0; // osize is a type tag when ptr is null
    if (nsize > old && m->used - old + nsize > m->limit) {
        return nullptr; // Lua turns this into a "not enough memory" error
    }
    void* p = std::realloc(ptr, nsize);
    if (p) m->used = m->used - old + nsize;
    return p;
}

Plugin::Plugin(std::string id, std::filesystem::path dir, control::EventHub& hub, Limits limits)
    : id_{ std::move(id) }, dir_{ std::move(dir) }, hub_{ hub }, limits_{ limits }
    , name_{ id_ }
{
    read_manifest();
}

Plugin::~Plugin() { unload(); }

void Plugin::read_manifest()
{
    std::ifstream in{ dir_ / "plugin.json", std::ios::binary };
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    if (const auto m = JsonValue::parse(ss.str()); m && m->is_object()) {
        name_ = m->str("name", id_);
        version_ = m->str("version");
        description_ = m->str("description");
    }
}

control::PluginInfo Plugin::info() const
{
    control::PluginInfo i;
    i.name = id_;
    i.version = version_;
    i.description = description_;
    i.path = dir_.string();
    i.loaded = loaded();
    i.error = last_error_;
    i.errors = errors_;
    i.packets = packets_;
    return i;
}

std::vector<control::PluginLogLine> Plugin::logs(const std::size_t limit) const
{
    const auto n = std::min(limit, log_.size());
    return { log_.end() - static_cast<std::ptrdiff_t>(n), log_.end() };
}

void Plugin::log(const std::string_view level, const std::string_view message)
{
    control::PluginLogLine line{ now_us(), std::string{ level }, std::string{ message.substr(0, 2000) } };
    if (log_.size() >= kLogCap) log_.pop_front();
    log_.push_back(line);

    const auto lvl = level == "error" ? spdlog::level::err : level == "warn" ? spdlog::level::warn : level == "debug" ? spdlog::level::debug : spdlog::level::info;
    spdlog::log(lvl, "[plugin:{}] {}", id_, line.message);

    control::JsonWriter w;
    w.begin_object().field("t", "plugin_log").field("plugin", id_).field("ts", line.ts).field("level", line.level).field("message", line.message).end_object();
    hub_.publish(w.take(), false);
}

void Plugin::record_error(const std::string_view where, const std::string_view what)
{
    ++errors_;
    ++consecutive_errors_;
    last_error_ = std::string{ where } + ": " + std::string{ what };
    log("error", last_error_);
    if (consecutive_errors_ >= limits_.max_consecutive_errors && !tripped_) {
        tripped_ = true;
        log("error", fmt::format("{} consecutive errors; switching this plugin off", consecutive_errors_));
    }
}

bool Plugin::budget_exceeded()
{
    if (instructions_left_ <= 1000) return true;
    instructions_left_ -= 1000;
    return std::chrono::steady_clock::now() > deadline_;
}

void Plugin::begin_budget(const std::chrono::milliseconds time)
{
    instructions_left_ = limits_.hook_instructions;
    deadline_ = std::chrono::steady_clock::now() + time;
}

std::string Plugin::load()
{
    unload();
    last_error_.clear();
    errors_ = 0;
    consecutive_errors_ = 0;
    tripped_ = false;
    read_manifest();

    const auto main = dir_ / "main.lua";
    if (!std::filesystem::exists(main)) {
        last_error_ = "main.lua not found";
        return last_error_;
    }

    mem_ = { 0, limits_.memory_bytes };
    L_ = lua_newstate(&Plugin::limited_alloc, &mem_);
    if (!L_) {
        last_error_ = "could not create Lua state";
        return last_error_;
    }
    *static_cast<Plugin**>(lua_getextraspace(L_)) = this;
    lua_ = std::make_unique<sol::state_view>(L_);
    lua_sethook(L_, &budget_hook, LUA_MASKCOUNT, 1000);

    setup_sandbox();
    setup_api();

    begin_budget(limits_.load_time);
    std::string load_error;
    {
        // The result object references the Lua stack, so it must be gone
        // before the state is closed below.
        const auto result = lua_->safe_script_file(main.string(), sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error err = result;
            load_error = err.what();
        }
    }
    if (!load_error.empty()) {
        last_error_ = load_error;
        log("error", "load failed: " + last_error_);
        unload();
        return last_error_;
    }

    if (auto on_load = lua_->get<sol::protected_function>("onLoad"); on_load.valid()) {
        begin_budget(limits_.load_time);
        if (const auto r = on_load(); !r.valid()) {
            const sol::error err = r;
            record_error("onLoad", err.what());
        }
    }
    log("info", fmt::format("loaded ({} {})", name_, version_));
    return {};
}

void Plugin::unload()
{
    if (!L_) return;
    if (!tripped_) {
        if (auto f = lua_->get<sol::protected_function>("onUnload"); f.valid()) {
            begin_budget(limits_.load_time);
            std::ignore = f();
        }
    }
    lua_.reset();
    lua_close(L_);
    L_ = nullptr;
}

void Plugin::setup_sandbox()
{
    // No io, os, package, debug: plugins cannot touch files, processes or the network.
    lua_->open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::utf8, sol::lib::coroutine);
    for (const char* name : { "dofile", "loadfile", "load", "loadstring", "require", "collectgarbage", "rawset", "rawget", "getmetatable" }) {
        (*lua_)[name] = sol::lua_nil;
    }
    // Keep setmetatable (common idiom) but not getmetatable/rawset; string.dump can leak bytecode.
    (*lua_)["string"]["dump"] = sol::lua_nil;
}

void Plugin::setup_api()
{
    sol::state_view& lua = *lua_;
    sol::table gt = lua.create_named_table("gt");
    gt["plugin"] = id_;
    gt["version"] = 1;
    gt["CLIENT_BOUND"] = "client_bound";
    gt["SERVER_BOUND"] = "server_bound";

    const auto logger = [this](const char* level) {
        return [this, level](sol::variadic_args args, sol::this_state ts) {
            std::string msg;
            for (auto a : args) {
                if (!msg.empty()) msg += ' ';
                msg += luaL_tolstring(ts.L, a.stack_index(), nullptr);
                lua_pop(ts.L, 1);
            }
            log(level, msg);
        };
    };
    gt["debug"] = logger("debug");
    gt["info"] = logger("info");
    gt["warn"] = logger("warn");
    gt["error"] = logger("error");
    lua["print"] = logger("info");

    gt["now_ms"] = [] { return now_us() / 1000; };

    gt["json_encode"] = [](sol::object o) { return control::dump_json(from_lua(o)); };
    gt["json_decode"] = [](sol::this_state ts, const std::string& s) -> sol::variadic_results {
        sol::variadic_results r;
        sol::state_view view{ ts };
        if (const auto v = JsonValue::parse(s)) r.push_back(to_lua(view, *v));
        else { r.push_back(sol::make_object(view, sol::lua_nil)); r.push_back(sol::make_object(view, "invalid JSON")); }
        return r;
    };

    // gt.inject(dir, spec | hex_string) -> id | nil, error
    gt["inject"] = [this](sol::this_state ts, const std::string& dir, sol::object what) -> sol::variadic_results {
        sol::state_view view{ ts };
        sol::variadic_results r;
        const auto fail = [&](const std::string& why) {
            r.push_back(sol::make_object(view, sol::lua_nil));
            r.push_back(sol::make_object(view, why));
            return r;
        };
        if (!current_flow_) return fail("gt.inject can only be used inside a hook");
        if (dir != "client_bound" && dir != "server_bound") return fail("dir must be gt.CLIENT_BOUND or gt.SERVER_BOUND");
        if (++injects_this_call_ > limits_.injects_per_call) return fail("too many injects in one hook call");

        JsonValue spec;
        if (what.get_type() == sol::type::string) {
            spec.type = JsonValue::Type::Object;
            spec.object.emplace_back("kind", JsonValue{ JsonValue::Type::String, false, 0, "raw", {}, {} });
            spec.object.emplace_back("hex", JsonValue{ JsonValue::Type::String, false, 0, what.as<std::string>(), {}, {} });
        } else {
            spec = from_lua(what);
        }
        const auto enc = control::encode_packet(spec);
        if (!enc.ok()) return fail(enc.error);
        const auto id = current_flow_->inject(dir == "client_bound" ? event::Direction::ClientBound : event::Direction::ServerBound, enc.bytes, "plugin:" + id_);
        if (id == 0) return fail("target peer is not connected");
        r.push_back(sol::make_object(view, id));
        return r;
    };

    lua.new_usertype<LuaPacket>("Packet", sol::no_constructor,
        "id", sol::property([](LuaPacket& p) { p.check(); return p.id; }),
        "dir", sol::property([](LuaPacket& p) { p.check(); return p.dir == event::Direction::ClientBound ? "client_bound" : "server_bound"; }),
        "kind", sol::property([](LuaPacket& p) { p.check(); return p.decoded.kind; }),
        "name", sol::property([](LuaPacket& p) { p.check(); return p.decoded.name; }),
        "length", sol::property([](LuaPacket& p) { p.check(); return p.bytes.size(); }),
        "hex", sol::property(
            [](LuaPacket& p) { p.check(); return control::encoding::to_hex(p.bytes); },
            [](LuaPacket& p, const std::string& hex) {
                p.check();
                auto b = control::encoding::from_hex(hex);
                if (!b || b->empty()) throw std::runtime_error{ "invalid hex" };
                p.bytes = std::move(*b);
                p.refresh();
            }),
        "text", sol::property(
            [](LuaPacket& p, sol::this_state ts) -> sol::object {
                p.check();
                if (p.decoded.kind != "text" || !p.decoded_json) return sol::make_object(ts, sol::lua_nil);
                return sol::make_object(ts, p.decoded_json->str("text"));
            },
            [](LuaPacket& p, const std::string& text) {
                p.check();
                if (p.decoded.kind != "text" || !p.decoded_json) throw std::runtime_error{ "not a text packet" };
                JsonValue spec;
                spec.type = JsonValue::Type::Object;
                spec.object.emplace_back("kind", JsonValue{ JsonValue::Type::String, false, 0, "text", {}, {} });
                spec.object.emplace_back("msg_type", JsonValue{ JsonValue::Type::Number, false, static_cast<double>(p.decoded_json->integer("msg_type", 3)), {}, {}, {} });
                spec.object.emplace_back("text", JsonValue{ JsonValue::Type::String, false, 0, text, {}, {} });
                p.bytes = control::encode_packet(spec).bytes;
                p.refresh();
            }),
        "decoded", sol::property([](LuaPacket& p, sol::this_state ts) -> sol::object {
            p.check();
            sol::state_view v{ ts };
            return p.decoded_json ? to_lua(v, *p.decoded_json) : sol::make_object(ts, sol::lua_nil);
        }),
        // pkt:get("key") -> first value of the first "key|value" line of a text packet, or nil
        "get", [](LuaPacket& p, const std::string& key, sol::this_state ts) -> sol::object {
            p.check();
            if (p.decoded_json) {
                if (const auto* lines = p.decoded_json->find("lines")) {
                    for (const auto& l : lines->array) {
                        if (l.str("key") == key) {
                            const auto* vals = l.find("values");
                            return vals && !vals->array.empty() ? sol::make_object(ts, vals->array[0].string) : sol::make_object(ts, "");
                        }
                    }
                }
            }
            return sol::make_object(ts, sol::lua_nil);
        },
        // pkt:edit(spec) -> true | false, error   (same spec shape the UI uses)
        "edit", [](LuaPacket& p, sol::object spec, sol::this_state ts) -> sol::variadic_results {
            p.check();
            sol::variadic_results r;
            const auto enc = control::encode_packet(from_lua(spec));
            if (!enc.ok()) {
                r.push_back(sol::make_object(ts, false));
                r.push_back(sol::make_object(ts, enc.error));
                return r;
            }
            p.bytes = enc.bytes;
            p.refresh();
            r.push_back(sol::make_object(ts, true));
            return r;
        },
        "drop", [](LuaPacket& p) { p.check(); p.dropped = true; });
}

bool Plugin::protected_call(const char* hook, const std::function<sol::protected_function_result(sol::protected_function&)>& invoke, bool* returned_false)
{
    sol::protected_function fn = lua_->get<sol::protected_function>(hook);
    if (!fn.valid()) return true;
    begin_budget(limits_.hook_time);
    injects_this_call_ = 0;
    auto result = invoke(fn);
    if (!result.valid()) {
        const sol::error err = result;
        record_error(hook, err.what());
        return false;
    }
    consecutive_errors_ = 0;
    if (returned_false && result.get_type() == sol::type::boolean && !result.get<bool>()) *returned_false = true;
    return true;
}

control::HookAction Plugin::on_packet(control::PacketFlow& flow, const event::Direction dir, const std::uint64_t id,
    const control::DecodedPacket& decoded, std::vector<std::byte>& bytes)
{
    if (!loaded() || tripped_) return control::HookAction::Pass;
    const char* hook = dir == event::Direction::ClientBound ? "onPacketIn" : "onPacketOut";
    if (!lua_->get<sol::protected_function>(hook).valid()) return control::HookAction::Pass;

    auto pkt = std::make_shared<LuaPacket>();
    pkt->dir = dir;
    pkt->id = id;
    pkt->bytes = bytes;
    pkt->decoded = decoded;
    pkt->decoded_json = JsonValue::parse(decoded.fields_json);

    current_flow_ = &flow;
    bool returned_false = false;
    const bool ok = protected_call(hook, [&](sol::protected_function& fn) { return fn(pkt); }, &returned_false);
    current_flow_ = nullptr;
    pkt->valid = false;
    ++packets_;

    if (!ok) return control::HookAction::Pass; // fail open: a broken plugin never blocks traffic
    if (pkt->dropped || returned_false) return control::HookAction::Drop;
    if (pkt->bytes != bytes) bytes = std::move(pkt->bytes);
    return control::HookAction::Pass;
}

void Plugin::on_connection(control::PacketFlow& flow, const std::string_view peer, const bool connected)
{
    if (!loaded() || tripped_) return;
    current_flow_ = &flow;
    protected_call(connected ? "onConnect" : "onDisconnect", [&](sol::protected_function& fn) {
        sol::table info = lua_->create_table();
        info["peer"] = std::string{ peer };
        return fn(info);
    });
    current_flow_ = nullptr;
}
}
