#pragma once
#include <utility>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "json_log_formatter.hpp"

namespace core {
class Logger {
public:
    Logger() = default;
    ~Logger() { logger_->flush(); }

    [[nodiscard]] static std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> create_console_sink()
    {
        auto console_sink{ std::make_shared<spdlog::sinks::stdout_color_sink_mt>() };
#ifdef GTPROXY_DEBUG
        console_sink->set_level(spdlog::level::trace);
#else
        console_sink->set_level(spdlog::level::info);
#endif
        return console_sink;
    }

    [[nodiscard]] static std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> create_file_sink()
    {
        return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "proxy.log",
            1024 * 1024 * 2,
            4
        );
    }

    // Structured (JSON Lines) log, one object per record. Always at debug level
    // and above so it is useful for post-mortems even when the console is quiet.
    [[nodiscard]] static std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> create_json_sink()
    {
        auto sink{ std::make_shared<spdlog::sinks::rotating_file_sink_mt>("proxy.jsonl", 1024 * 1024 * 5, 3) };
        sink->set_formatter(std::make_unique<JsonLogFormatter>());
        sink->set_level(spdlog::level::debug);
        return sink;
    }

    [[nodiscard]] std::shared_ptr<spdlog::logger> get_logger() const { return logger_; }
    void set_logger(std::shared_ptr<spdlog::logger> logger) { logger_ = std::move(logger); }

private:
    std::shared_ptr<spdlog::logger> logger_;
};
}
