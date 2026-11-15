#pragma once
#include <chrono>
#include <memory>
#include <spdlog/formatter.h>
#include <spdlog/details/log_msg.h>

#include "../control/json_writer.hpp"

namespace core {
// One JSON object per line: {"ts":"2026-11-13T07:30:55.123Z","level":"info","logger":"GTProxy","thread":123,"msg":"..."}
// Attach to a file sink (see main.cpp) for machine-readable logs.
class JsonLogFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override
    {
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(msg.time.time_since_epoch()).count();
        const std::time_t secs = static_cast<std::time_t>(ms / 1000);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &secs);
#else
        gmtime_r(&secs, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);

        control::JsonWriter w;
        w.begin_object()
            .field("ts", fmt::format("{}.{:03}Z", stamp, ms % 1000))
            .field("level", std::string_view{ spdlog::level::to_string_view(msg.level).data(), spdlog::level::to_string_view(msg.level).size() })
            .field("logger", std::string_view{ msg.logger_name.data(), msg.logger_name.size() })
            .field("thread", static_cast<std::uint64_t>(msg.thread_id))
            .field("msg", std::string_view{ msg.payload.data(), msg.payload.size() })
            .end_object();
        const auto& s = w.str();
        dest.append(s.data(), s.data() + s.size());
        dest.push_back('\n');
    }

    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override
    {
        return std::make_unique<JsonLogFormatter>();
    }
};
}
