#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace control {
// Thread-safe fan-out of already-serialised JSON events. The proxy's main
// thread publishes; WebSocket clients, the session recorder and tests
// subscribe. Listeners MUST NOT block (they run on the publishing thread).
// A bounded backlog of packet events lets a freshly opened UI catch up.
class EventHub {
public:
    using Listener = std::function<void(const std::string&)>;

    explicit EventHub(std::size_t backlog_limit = 5000)
        : backlog_limit_{ backlog_limit }
    { }

    std::uint64_t subscribe(Listener listener)
    {
        std::scoped_lock lock{ mutex_ };
        const auto id = ++next_id_;
        listeners_.emplace(id, std::move(listener));
        return id;
    }

    void unsubscribe(const std::uint64_t id)
    {
        std::scoped_lock lock{ mutex_ };
        listeners_.erase(id);
    }

    // `backlog`: keep for late joiners (packet + verdict + connection events).
    void publish(const std::string& json, const bool backlog = false)
    {
        std::vector<Listener> copy;
        {
            std::scoped_lock lock{ mutex_ };
            if (backlog) {
                backlog_.push_back(json);
                if (backlog_.size() > backlog_limit_) {
                    backlog_.pop_front();
                }
            }
            copy.reserve(listeners_.size());
            for (const auto& [id, l] : listeners_) {
                copy.push_back(l);
            }
        }
        for (const auto& l : copy) {
            l(json);
        }
    }

    [[nodiscard]] std::vector<std::string> backlog(const std::size_t limit = 0) const
    {
        std::scoped_lock lock{ mutex_ };
        const auto n = limit == 0 || limit > backlog_.size() ? backlog_.size() : limit;
        return { backlog_.end() - static_cast<std::ptrdiff_t>(n), backlog_.end() };
    }

    void clear_backlog()
    {
        std::scoped_lock lock{ mutex_ };
        backlog_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Listener> listeners_;
    std::deque<std::string> backlog_;
    std::size_t backlog_limit_;
    std::uint64_t next_id_{ 0 };
};
}
