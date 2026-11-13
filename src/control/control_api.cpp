#include "control_api.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

#include "encoding.hpp"
#include "json_dump.hpp"
#include "json_writer.hpp"

#ifndef GTPROXY_VERSION_MAJOR
#define GTPROXY_VERSION_MAJOR 0
#define GTPROXY_VERSION_MINOR 0
#define GTPROXY_VERSION_PATCH 0
#endif

namespace control {
ApiReply api_ok(std::string result_json)
{
    JsonWriter w;
    w.begin_object().field("ok", true).key("result").raw(result_json).end_object();
    return { true, 200, w.take() };
}

ApiReply api_error(const int http_status, const std::string_view message)
{
    JsonWriter w;
    w.begin_object().field("ok", false).field("error", message).end_object();
    return { false, http_status, w.take() };
}

namespace {
std::optional<event::Direction> parse_dir(const std::string& s)
{
    if (s == "client_bound") return event::Direction::ClientBound;
    if (s == "server_bound") return event::Direction::ServerBound;
    return std::nullopt;
}

// A packet may be given as {"packet":{spec}} or the shorthand {"hex":"..."}.
EncodeResult packet_from_params(const JsonValue& p)
{
    if (const auto* spec = p.find("packet")) {
        return encode_packet(*spec);
    }
    if (p.has("hex")) {
        JsonValue spec;
        spec.type = JsonValue::Type::Object;
        spec.object.emplace_back("kind", JsonValue{ JsonValue::Type::String, false, 0, "raw", {}, {} });
        spec.object.emplace_back("hex", JsonValue{ JsonValue::Type::String, false, 0, p.str("hex"), {}, {} });
        return encode_packet(spec);
    }
    return { {}, "provide \"packet\" (an edit spec) or \"hex\"" };
}
}

ControlApi::ControlApi(core::Config& config, EventHub& hub, PacketFlow& flow, network::IConnection& game_client,
    network::IConnection& upstream, SessionRecorder& recorder, TemplateStore& templates)
    : config_{ config }, hub_{ hub }, flow_{ flow }, game_client_{ game_client }
    , upstream_{ upstream }, recorder_{ recorder }, templates_{ templates }
{ }

std::vector<std::string> ControlApi::operations()
{
    return {
        "status", "packets.backlog", "packets.clear", "packet.decode", "packet.encode", "inject",
        "intercept.get", "intercept.set", "intercept.forward", "intercept.drop", "intercept.release_all",
        "templates.list", "templates.save", "templates.delete",
        "profiles.list", "profiles.set_active", "profiles.save", "profiles.delete",
        "plugins.list", "plugins.enable", "plugins.disable", "plugins.reload", "plugins.rescan", "plugins.logs",
        "sessions.list", "sessions.start", "sessions.stop", "sessions.replay",
    };
}

std::string ControlApi::profiles_json() const
{
    JsonWriter w;
    w.begin_object().field("active", config_.get_active_profile_name()).key("profiles").begin_array();
    for (const auto& p : config_.get_profiles()) {
        w.begin_object().field("name", p.name).field("address", p.address).field("port", p.port)
            .field("tls", p.tls).field("direct", p.direct).field("enet_address", p.enet_address)
            .field("enet_port", p.enet_port).end_object();
    }
    w.end_array().end_object();
    return w.take();
}

std::string ControlApi::status_json() const
{
    const auto& s = flow_.stats();
    const auto peer = [](JsonWriter& w, std::string_view key, const network::IConnection& c) {
        w.key(key).begin_object().field("connected", c.is_connected())
            .field("address", c.peer_description()).field("rtt_ms", static_cast<std::uint64_t>(c.round_trip_ms())).end_object();
    };
    JsonWriter w;
    w.begin_object().field("t", "status");
    w.field("version", fmt::format("{}.{}.{}", GTPROXY_VERSION_MAJOR, GTPROXY_VERSION_MINOR, GTPROXY_VERSION_PATCH));
    w.field("uptime_s", static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count()));
    w.field("profile", config_.get_active_profile_name());
    peer(w, "game_client", game_client_);
    peer(w, "upstream", upstream_);
    w.key("counters").begin_object()
        .field("client_bound_packets", s.packets[0]).field("client_bound_bytes", s.bytes[0])
        .field("server_bound_packets", s.packets[1]).field("server_bound_bytes", s.bytes[1])
        .field("dropped", s.dropped).field("edited", s.edited).field("injected", s.injected)
        .field("consumed", s.consumed).field("held_total", s.held_total).field("write_failures", s.write_failures)
        .end_object();
    w.key("intercept").begin_object().field("enabled", flow_.intercept().enabled)
        .field("held", static_cast<std::uint64_t>(flow_.held_count())).end_object();
    w.key("recording").begin_object().field("active", recorder_.active()).field("name", recorder_.current_name())
        .field("events", recorder_.lines_written()).end_object();
    std::uint64_t total = 0, enabled = 0, errors = 0;
    if (plugins_) {
        for (const auto& p : plugins_->list()) { ++total; enabled += p.enabled ? 1 : 0; errors += p.errors; }
    }
    w.key("plugins").begin_object().field("total", total).field("enabled", enabled).field("errors", errors).end_object();
    w.field("ws_clients", static_cast<std::uint64_t>(ws_clients_ ? ws_clients_() : 0));
    w.end_object();
    return w.take();
}

ApiReply ControlApi::handle(const std::string& op, const JsonValue& p)
{
    try {
        if (op == "status") return api_ok(status_json());

        if (op == "packets.backlog") {
            const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(0, p.integer("limit", 0)));
            JsonWriter w;
            w.begin_array();
            for (const auto& e : hub_.backlog(limit)) w.raw(e);
            w.end_array();
            return api_ok(w.take());
        }
        if (op == "packets.clear") {
            hub_.clear_backlog();
            return api_ok();
        }

        if (op == "packet.decode") {
            auto bytes = encoding::from_hex(p.str("hex"));
            if (!bytes || bytes->empty()) return api_error(400, "\"hex\" is empty or not valid hex");
            const auto d = decode_packet(*bytes);
            JsonWriter w;
            w.begin_object().field("kind", d.kind).field("name", d.name).field("summary", d.summary)
                .field("malformed", d.malformed).key("decoded").raw(d.fields_json).end_object();
            return api_ok(w.take());
        }
        if (op == "packet.encode") {
            const auto r = packet_from_params(p);
            if (!r.ok()) return api_error(400, r.error);
            const auto d = decode_packet(r.bytes);
            JsonWriter w;
            w.begin_object().field("hex", encoding::to_hex(r.bytes)).field("len", static_cast<std::uint64_t>(r.bytes.size()))
                .field("name", d.name).key("decoded").raw(d.fields_json).end_object();
            return api_ok(w.take());
        }

        if (op == "inject") return op_inject(p);

        if (op == "intercept.get") return api_ok(flow_.intercept_json());
        if (op == "intercept.set") return op_intercept_set(p);
        if (op == "intercept.forward") return op_intercept_release(p, false);
        if (op == "intercept.drop") return op_intercept_release(p, true);
        if (op == "intercept.release_all") {
            const auto n = flow_.release_all(p.flag("drop", false));
            return api_ok(fmt::format("{{\"released\":{}}}", n));
        }

        if (op == "templates.list") return api_ok(templates_.to_json());
        if (op == "templates.save") return op_templates_save(p);
        if (op == "templates.delete") {
            if (auto err = templates_.remove(p.str("name")); !err.empty()) return api_error(404, err);
            return api_ok(templates_.to_json());
        }

        if (op == "profiles.list") return api_ok(profiles_json());
        if (op == "profiles.set_active") {
            if (auto err = config_.set_active_profile(p.str("name")); !err.empty()) return api_error(404, err);
            if (auto err = config_.save(); !err.empty()) spdlog::warn("Could not persist config: {}", err);
            spdlog::info("Active profile is now \"{}\"", p.str("name"));
            if (on_profile_changed_) on_profile_changed_();
            return api_ok(profiles_json());
        }
        if (op == "profiles.save") return op_profile_save(p);
        if (op == "profiles.delete") {
            if (auto err = config_.remove_profile(p.str("name")); !err.empty()) return api_error(400, err);
            std::ignore = config_.save();
            return api_ok(profiles_json());
        }

        if (op.starts_with("plugins.")) return op_plugins(op, p);
        if (op.starts_with("sessions.")) return op_session(op, p);
    } catch (const std::exception& e) {
        spdlog::error("Control API op {} failed: {}", op, e.what());
        return api_error(500, e.what());
    }
    return api_error(404, "unknown operation: " + op);
}

ApiReply ControlApi::op_inject(const JsonValue& p)
{
    auto dir = parse_dir(p.str("dir"));
    JsonValue tmpl_params;
    EncodeResult enc;
    if (const auto name = p.str("template"); !name.empty()) {
        const auto t = templates_.find(name);
        if (!t) return api_error(404, "no template named " + name);
        enc = encode_packet(t->packet);
        if (!dir) dir = parse_dir(t->dir);
    } else {
        enc = packet_from_params(p);
    }
    if (!dir) return api_error(400, "\"dir\" must be client_bound or server_bound");
    if (!enc.ok()) return api_error(400, enc.error);

    const auto id = flow_.inject(*dir, enc.bytes, "ui");
    if (id == 0) return api_error(409, "not sent: the target peer is not connected");
    return api_ok(fmt::format("{{\"id\":{},\"len\":{}}}", id, enc.bytes.size()));
}

ApiReply ControlApi::op_intercept_set(const JsonValue& p)
{
    InterceptConfig cfg;
    cfg.enabled = p.flag("enabled", flow_.intercept().enabled);
    if (const auto* rules = p.find("rules")) {
        if (!rules->is_array()) return api_error(400, "\"rules\" must be an array");
        for (const auto& r : rules->array) {
            InterceptRule rule;
            rule.direction = r.str("direction", "any");
            if (rule.direction != "any" && !parse_dir(rule.direction)) return api_error(400, "rule direction must be any, client_bound or server_bound");
            rule.match = r.str("match");
            rule.kind = r.str("kind");
            cfg.rules.push_back(std::move(rule));
        }
    } else {
        cfg.rules = flow_.intercept().rules;
    }
    flow_.set_intercept(cfg, p.str("flush", "forward") != "drop");
    return api_ok(flow_.intercept_json());
}

ApiReply ControlApi::op_intercept_release(const JsonValue& p, const bool drop)
{
    const auto id = static_cast<std::uint64_t>(p.integer("id"));
    std::optional<std::vector<std::byte>> replacement;
    if (!drop && (p.has("packet") || p.has("hex"))) {
        auto enc = packet_from_params(p);
        if (!enc.ok()) return api_error(400, enc.error);
        replacement = std::move(enc.bytes);
    }
    const auto r = flow_.release(id, drop, std::move(replacement));
    if (!r.ok) return api_error(404, r.error);
    return api_ok(flow_.intercept_json());
}

ApiReply ControlApi::op_templates_save(const JsonValue& p)
{
    PacketTemplate t;
    t.name = p.str("name");
    t.description = p.str("description");
    t.dir = p.str("dir", "server_bound");
    if (const auto* spec = p.find("packet")) {
        t.packet = *spec;
        if (auto enc = encode_packet(t.packet); !enc.ok()) return api_error(400, "invalid packet spec: " + enc.error);
    }
    if (auto err = templates_.save(std::move(t)); !err.empty()) return api_error(400, err);
    return api_ok(templates_.to_json());
}

ApiReply ControlApi::op_profile_save(const JsonValue& p)
{
    core::Config::ProfileConfig prof;
    prof.name = p.str("name");
    prof.address = p.str("address", "127.0.0.1");
    prof.port = static_cast<int>(p.integer("port", 443));
    prof.tls = p.flag("tls", true);
    prof.direct = p.flag("direct", false);
    prof.enet_address = p.str("enet_address");
    prof.enet_port = static_cast<int>(p.integer("enet_port", 17091));
    if (auto err = config_.upsert_profile(prof); !err.empty()) return api_error(400, err);
    if (auto err = config_.save(); !err.empty()) spdlog::warn("Could not persist config: {}", err);
    return api_ok(profiles_json());
}

ApiReply ControlApi::op_plugins(const std::string& op, const JsonValue& p)
{
    if (!plugins_) return api_error(503, "plugin system is not available");
    std::string err;
    if (op == "plugins.list") {
        // fallthrough to listing below
    } else if (op == "plugins.enable") err = plugins_->set_enabled(p.str("name"), true);
    else if (op == "plugins.disable") err = plugins_->set_enabled(p.str("name"), false);
    else if (op == "plugins.reload") err = plugins_->reload(p.str("name"));
    else if (op == "plugins.rescan") err = plugins_->rescan();
    else if (op == "plugins.logs") {
        const auto lines = plugins_->logs(p.str("name"), static_cast<std::size_t>(std::clamp<std::int64_t>(p.integer("limit", 200), 1, 2000)));
        JsonWriter w;
        w.begin_array();
        for (const auto& l : lines) w.begin_object().field("ts", l.ts).field("level", l.level).field("message", l.message).end_object();
        w.end_array();
        return api_ok(w.take());
    } else return api_error(404, "unknown operation: " + op);

    if (!err.empty()) return api_error(400, err);

    JsonWriter w;
    w.begin_array();
    for (const auto& i : plugins_->list()) {
        w.begin_object().field("name", i.name).field("version", i.version).field("description", i.description)
            .field("path", i.path).field("enabled", i.enabled).field("loaded", i.loaded).field("error", i.error)
            .field("errors", i.errors).field("packets", i.packets).end_object();
    }
    w.end_array();
    return api_ok(w.take());
}

ApiReply ControlApi::op_session(const std::string& op, const JsonValue& p)
{
    if (op == "sessions.list") {
        JsonWriter w;
        w.begin_array();
        for (const auto& i : recorder_.list()) w.begin_object().field("name", i.name).field("bytes", i.bytes).end_object();
        w.end_array();
        return api_ok(w.take());
    }
    if (op == "sessions.start") {
        auto name = p.str("name");
        if (name.empty()) name = fmt::format("session-{}", flow_.now_us() / 1000000);
        if (auto err = recorder_.start(name, config_.get_active_profile_name()); !err.empty()) return api_error(400, err);
        return api_ok(fmt::format("{{\"name\":\"{}\"}}", name));
    }
    if (op == "sessions.stop") {
        if (auto err = recorder_.stop(); !err.empty()) return api_error(400, err);
        return api_ok();
    }
    if (op == "sessions.replay") {
        const auto name = p.str("name");
        if (!SessionRecorder::valid_name(name)) return api_error(400, "invalid session name");
        const auto data = load_session(recorder_.path_for(name));
        if (!data.error.empty()) return api_error(404, data.error);
        const auto outcomes = replay_session(data, replay_hooks_);
        std::uint64_t dropped = 0, changed = 0;
        JsonWriter w;
        w.begin_object().field("name", name).field("skipped_lines", static_cast<std::uint64_t>(data.skipped_lines));
        w.key("packets").begin_array();
        for (const auto& o : outcomes) {
            const bool same = o.outputs.size() == 1 && o.outputs[0].bytes == o.input;
            dropped += o.dropped ? 1 : 0;
            changed += (!o.dropped && !same) ? 1 : 0;
            w.begin_object().field("id", o.id).field("dir", direction_name(o.dir)).field("dropped", o.dropped).field("changed", !o.dropped && !same)
                .field("in_hex", encoding::to_hex(o.input));
            w.key("outputs").begin_array();
            for (const auto& out : o.outputs) w.begin_object().field("dir", direction_name(out.dir)).field("hex", encoding::to_hex(out.bytes)).end_object();
            w.end_array().end_object();
        }
        w.end_array().field("total", static_cast<std::uint64_t>(outcomes.size())).field("dropped", dropped).field("changed", changed).end_object();
        return api_ok(w.take());
    }
    return api_error(404, "unknown operation: " + op);
}
}
