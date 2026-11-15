#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

#include "encoding.hpp"
#include "json_value.hpp"
#include "json_writer.hpp"

namespace control {
namespace {
bool is_session_event(const std::string& json)
{
    // Cheap prefix test: events are produced by JsonWriter and always start with {"t":"..."
    return json.starts_with("{\"t\":\"packet\"") || json.starts_with("{\"t\":\"verdict\"") || json.starts_with("{\"t\":\"connection\"");
}

class CaptureConnection final : public network::IConnection {
public:
    explicit CaptureConnection(const event::Direction dir) : dir_{ dir } { }
    bool write(std::span<const std::byte> data, int) const override
    {
        if (sink) sink->push_back({ dir_, { data.begin(), data.end() } });
        return true;
    }
    bool write(const std::vector<std::byte>& data, int ch) const override { return write(std::span{ data }, ch); }
    [[nodiscard]] bool is_connected() const override { return true; }
    std::vector<ReplayOutput>* sink{ nullptr };
private:
    event::Direction dir_;
};
}

SessionRecorder::SessionRecorder(EventHub& hub, std::filesystem::path dir)
    : hub_{ hub }
    , dir_{ std::move(dir) }
{
    subscription_ = hub_.subscribe([this](const std::string& json) { on_event(json); });
}

SessionRecorder::~SessionRecorder()
{
    hub_.unsubscribe(subscription_);
    std::ignore = stop();
}

bool SessionRecorder::valid_name(const std::string& name)
{
    return !name.empty() && name.size() <= 64 && name != "." && name != ".." &&
        std::ranges::all_of(name, [](const unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

std::filesystem::path SessionRecorder::path_for(const std::string& name) const
{
    return dir_ / (name + ".jsonl");
}

std::string SessionRecorder::start(const std::string& name, const std::string& profile)
{
    if (!valid_name(name)) {
        return "session name may only contain letters, digits, '_', '-' and '.'";
    }
    std::scoped_lock lock{ mutex_ };
    if (file_.is_open()) {
        return "a session is already being recorded";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return "cannot create sessions directory: " + ec.message();
    }
    file_.open(path_for(name), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!file_) {
        return "cannot open session file for writing";
    }
    name_ = name;
    lines_ = 0;

    using namespace std::chrono;
    JsonWriter w;
    w.begin_object().field("t", "session").field("version", 1)
        .field("started", duration_cast<microseconds>(system_clock::now().time_since_epoch()).count())
        .field("profile", profile).end_object();
    file_ << w.str() << '\n';
    spdlog::info("Recording session \"{}\" to {}", name, path_for(name).string());
    return {};
}

std::string SessionRecorder::stop()
{
    std::scoped_lock lock{ mutex_ };
    if (!file_.is_open()) {
        return "no session is being recorded";
    }
    file_.flush();
    file_.close();
    spdlog::info("Stopped recording session \"{}\" ({} events)", name_, lines_);
    return {};
}

bool SessionRecorder::active() const
{
    std::scoped_lock lock{ mutex_ };
    return file_.is_open();
}

std::string SessionRecorder::current_name() const
{
    std::scoped_lock lock{ mutex_ };
    return file_.is_open() ? name_ : std::string{};
}

std::uint64_t SessionRecorder::lines_written() const
{
    std::scoped_lock lock{ mutex_ };
    return lines_;
}

void SessionRecorder::on_event(const std::string& json)
{
    if (!is_session_event(json)) {
        return;
    }
    std::scoped_lock lock{ mutex_ };
    if (!file_.is_open()) {
        return;
    }
    file_ << json << '\n';
    if (++lines_ % 64 == 0) {
        file_.flush();
    }
}

std::vector<SessionRecorder::Info> SessionRecorder::list() const
{
    std::vector<Info> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{ dir_, ec }) {
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
            out.push_back({ entry.path().stem().string(), static_cast<std::uint64_t>(entry.file_size(ec)) });
        }
    }
    std::ranges::sort(out, {}, &Info::name);
    return out;
}

SessionData load_session(const std::filesystem::path& file)
{
    SessionData data;
    std::ifstream in{ file, std::ios::binary };
    if (!in) {
        data.error = "cannot open " + file.string();
        return data;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto v = JsonValue::parse(line);
        if (!v || !v->is_object()) { ++data.skipped_lines; continue; }
        if (v->str("t") != "packet") continue; // header, verdicts, connections
        auto bytes = encoding::from_hex(v->str("hex"));
        if (!bytes) { ++data.skipped_lines; continue; }
        RecordedPacket p;
        p.id = static_cast<std::uint64_t>(v->integer("id"));
        p.dir = v->str("dir") == "client_bound" ? event::Direction::ClientBound : event::Direction::ServerBound;
        p.bytes = std::move(*bytes);
        p.origin = v->str("origin");
        p.ts = v->integer("ts");
        data.packets.push_back(std::move(p));
    }
    return data;
}

std::vector<ReplayOutcome> replay_session(const SessionData& session, IPacketHooks* hooks)
{
    EventHub hub{ 16 };
    CaptureConnection to_client{ event::Direction::ClientBound };
    CaptureConnection to_server{ event::Direction::ServerBound };
    PacketFlow flow{ hub, to_client, to_server };
    flow.set_hooks(hooks);

    std::vector<ReplayOutcome> outcomes;
    for (const auto& p : session.packets) {
        if (p.origin != "wire") continue;

        ReplayOutcome o;
        o.id = p.id;
        o.dir = p.dir;
        o.input = p.bytes;
        to_client.sink = to_server.sink = &o.outputs;
        flow.process(p.dir, p.bytes);
        to_client.sink = to_server.sink = nullptr;
        o.dropped = o.outputs.empty();
        outcomes.push_back(std::move(o));
    }
    return outcomes;
}
}
