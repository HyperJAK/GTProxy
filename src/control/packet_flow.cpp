#include "packet_flow.hpp"

#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

#include "encoding.hpp"
#include "json_writer.hpp"

namespace control {
const char* direction_name(const event::Direction d)
{
    return d == event::Direction::ClientBound ? "client_bound" : "server_bound";
}

namespace {
std::string lower(std::string_view s)
{
    std::string out{ s };
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::int64_t system_now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

std::size_t idx(const event::Direction d) { return d == event::Direction::ClientBound ? 0 : 1; }
}

PacketFlow::PacketFlow(EventHub& hub, network::IConnection& to_game_client, network::IConnection& to_upstream, Clock clock)
    : hub_{ hub }
    , to_game_client_{ to_game_client }
    , to_upstream_{ to_upstream }
    , clock_{ clock ? std::move(clock) : Clock{ system_now_us } }
{ }

std::string PacketFlow::packet_json(const std::uint64_t id, const event::Direction dir, const std::int64_t ts,
    const std::span<const std::byte> bytes, const DecodedPacket& d, const std::string_view origin, const std::string_view state) const
{
    JsonWriter w;
    w.begin_object()
        .field("t", "packet")
        .field("id", id)
        .field("ts", ts)
        .field("dir", direction_name(dir))
        .field("origin", origin)
        .field("state", state)
        .field("kind", d.kind)
        .field("name", d.name)
        .field("summary", d.summary)
        .field("malformed", d.malformed)
        .field("len", static_cast<std::uint64_t>(bytes.size()))
        .field("hex", encoding::to_hex(bytes))
        .key("decoded").raw(d.fields_json)
        .end_object();
    return w.take();
}

std::uint64_t PacketFlow::publish_new(const event::Direction dir, const std::span<const std::byte> data,
    const DecodedPacket& decoded, const std::string_view origin, const std::string_view state)
{
    const auto id = ++next_id_;
    hub_.publish(packet_json(id, dir, clock_(), data, decoded, origin, state), true);
    return id;
}

void PacketFlow::publish_verdict(const std::uint64_t id, const std::string_view state, const std::string_view by, const std::vector<std::byte>* bytes)
{
    JsonWriter w;
    w.begin_object().field("t", "verdict").field("id", id).field("state", state).field("ts", clock_());
    if (!by.empty()) w.field("by", by);
    if (bytes) {
        const auto d = decode_packet(*bytes);
        w.field("len", static_cast<std::uint64_t>(bytes->size()))
            .field("hex", encoding::to_hex(*bytes))
            .field("name", d.name)
            .field("summary", d.summary)
            .key("decoded").raw(d.fields_json);
    }
    w.end_object();
    hub_.publish(w.take(), true);
}

void PacketFlow::begin_receive(const event::Direction dir, const std::span<const std::byte> data)
{
    if (current_.active) {
        // Re-entrancy would mean a handler fed a packet back into the proxy.
        // Close the previous scope rather than corrupting the bookkeeping.
        end_receive();
    }
    stats_.packets[idx(dir)]++;
    stats_.bytes[idx(dir)] += data.size();

    current_.decoded = decode_packet(data);
    current_.dir = dir;
    current_.resolved = false;
    current_.active = true;
    current_.id = publish_new(dir, data, current_.decoded, "wire", "received");
}

void PacketFlow::end_receive()
{
    if (!current_.active) {
        return;
    }
    if (!current_.resolved) {
        stats_.consumed++;
        publish_verdict(current_.id, "consumed", "proxy", nullptr);
    }
    current_.active = false;
}

void PacketFlow::process(const event::Direction dir, const std::span<const std::byte> data)
{
    begin_receive(dir, data);
    forward(dir, data);
    end_receive();
}

bool PacketFlow::write_toward(const event::Direction dir, const std::span<const std::byte> data)
{
    suppress_observer_ = true;
    const bool ok = (dir == event::Direction::ClientBound ? to_game_client_ : to_upstream_).write(data, 0);
    suppress_observer_ = false;
    if (!ok) {
        stats_.write_failures++;
        spdlog::warn("Failed to forward {} byte packet ({})", data.size(), direction_name(dir));
    }
    return ok;
}

bool PacketFlow::rule_matches(const event::Direction dir, const DecodedPacket& d) const
{
    if (intercept_.rules.empty()) {
        return true;
    }
    const auto hay_name = lower(d.name);
    const auto hay_sum = lower(d.summary);
    for (const auto& r : intercept_.rules) {
        if (r.direction != "any" && r.direction != direction_name(dir)) continue;
        if (!r.kind.empty() && r.kind != d.kind) continue;
        if (r.match.empty()) return true;
        const auto needle = lower(r.match);
        if (hay_name.find(needle) != std::string::npos || hay_sum.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool PacketFlow::queue_has(const event::Direction dir) const
{
    return std::ranges::any_of(held_, [dir](const Held& h) { return h.dir == dir; });
}

void PacketFlow::forward(const event::Direction dir, const std::span<const std::byte> data)
{
    // Calls from outside a receive scope (tests, replay of recorded files)
    // get their own record so nothing is ever forwarded invisibly.
    const bool own_scope = !current_.active || current_.dir != dir;
    if (own_scope) {
        begin_receive(dir, data);
    }

    std::vector<std::byte> bytes{ data.begin(), data.end() };
    const auto id = current_.id;
    const auto decoded = current_.decoded;

    if (hooks_) {
        const auto before = bytes;
        HookAction action = HookAction::Pass;
        try {
            action = hooks_->on_packet(*this, dir, id, decoded, bytes);
        } catch (const std::exception& e) {
            spdlog::error("Packet hook threw: {}", e.what());
        } catch (...) {
            spdlog::error("Packet hook threw an unknown exception");
        }
        if (action == HookAction::Drop) {
            stats_.dropped++;
            current_.resolved = true;
            publish_verdict(id, "dropped", "plugin", nullptr);
            if (own_scope) end_receive();
            return;
        }
        if (bytes != before) {
            stats_.edited++;
            publish_verdict(id, "edited", "plugin", &bytes);
        }
    }

    const auto final_decoded = decode_packet(bytes);
    const bool matches = intercept_.enabled && rule_matches(dir, final_decoded);
    if (intercept_.enabled && (matches || queue_has(dir))) {
        if (held_.size() >= kMaxHeld) {
            spdlog::warn("Intercept queue full ({}); forwarding packet {} immediately", kMaxHeld, id);
        } else {
            current_.resolved = true;
            stats_.held_total++;
            held_.push_back({ id, dir, std::move(bytes), final_decoded.name, final_decoded.summary, clock_(), matches });
            publish_verdict(id, "held", matches ? "intercept" : "intercept-order", nullptr);
            publish_intercept();
            if (own_scope) end_receive();
            return;
        }
    }

    current_.resolved = true;
    write_toward(dir, bytes);
    publish_verdict(id, "forwarded", "", nullptr);
    if (own_scope) end_receive();
}

std::uint64_t PacketFlow::inject(const event::Direction dir, const std::span<const std::byte> data, const std::string& origin)
{
    if (data.empty()) {
        return 0;
    }
    const auto decoded = decode_packet(data);
    const auto id = publish_new(dir, data, decoded, origin, "injected");
    if (!write_toward(dir, data)) {
        publish_verdict(id, "failed", "no connected peer", nullptr);
        return 0;
    }
    stats_.injected++;
    return id;
}

void PacketFlow::note_written(const event::Direction toward, const std::span<const std::byte> data)
{
    if (suppress_observer_) {
        return;
    }
    // Something other than the forwarding path wrote to a peer: a command,
    // a rewrite (e.g. OnSendToServer) or an upstream script. Show it.
    publish_new(toward, data, decode_packet(data), "proxy", "sent");
}

void PacketFlow::set_intercept(const InterceptConfig& cfg, const bool flush_forward)
{
    const bool was = intercept_.enabled;
    intercept_ = cfg;
    if (was && !cfg.enabled) {
        release_all(!flush_forward);
    }
    publish_intercept();
}

ReleaseResult PacketFlow::release(const std::uint64_t id, const bool drop, std::optional<std::vector<std::byte>> replacement)
{
    const auto it = std::ranges::find_if(held_, [id](const Held& h) { return h.id == id; });
    if (it == held_.end()) {
        return { false, "no held packet with that id (already released?)" };
    }
    Held h = std::move(*it);
    held_.erase(it);

    if (drop) {
        stats_.dropped++;
        publish_verdict(h.id, "dropped", "user", nullptr);
    } else {
        if (replacement) {
            if (replacement->empty()) {
                held_.push_front(std::move(h)); // put it back; refuse empty edits
                return { false, "replacement packet is empty" };
            }
            if (*replacement != h.bytes) {
                stats_.edited++;
                h.bytes = std::move(*replacement);
                publish_verdict(h.id, "edited", "user", &h.bytes);
            }
        }
        write_toward(h.dir, h.bytes);
        publish_verdict(h.id, "forwarded", "user", nullptr);
    }
    advance(h.dir);
    publish_intercept();
    return { true, {} };
}

void PacketFlow::advance(const event::Direction dir)
{
    // Packets parked only to preserve ordering follow as soon as nothing
    // that matched a rule stands in front of them.
    while (true) {
        const auto it = std::ranges::find_if(held_, [dir](const Held& h) { return h.dir == dir; });
        if (it == held_.end() || it->by_rule) {
            return;
        }
        Held h = std::move(*it);
        held_.erase(it);
        write_toward(h.dir, h.bytes);
        publish_verdict(h.id, "forwarded", "intercept-order", nullptr);
    }
}

std::size_t PacketFlow::release_all(const bool drop)
{
    std::size_t n = 0;
    while (!held_.empty()) {
        Held h = std::move(held_.front());
        held_.pop_front();
        ++n;
        if (drop) {
            stats_.dropped++;
            publish_verdict(h.id, "dropped", "user", nullptr);
        } else {
            write_toward(h.dir, h.bytes);
            publish_verdict(h.id, "forwarded", "user", nullptr);
        }
    }
    publish_intercept();
    return n;
}

std::string PacketFlow::intercept_json() const
{
    JsonWriter w;
    w.begin_object().field("t", "intercept").field("enabled", intercept_.enabled);
    w.key("rules").begin_array();
    for (const auto& r : intercept_.rules) {
        w.begin_object().field("direction", r.direction).field("match", r.match).field("kind", r.kind).end_object();
    }
    w.end_array();
    w.key("held").begin_array();
    for (const auto& h : held_) {
        w.begin_object()
            .field("id", h.id)
            .field("dir", direction_name(h.dir))
            .field("name", h.name)
            .field("summary", h.summary)
            .field("len", static_cast<std::uint64_t>(h.bytes.size()))
            .field("since", h.since_us)
            .field("by_rule", h.by_rule)
            .field("hex", encoding::to_hex(h.bytes))
            .end_object();
    }
    w.end_array().end_object();
    return w.take();
}

void PacketFlow::publish_intercept()
{
    hub_.publish(intercept_json(), false);
}
}
