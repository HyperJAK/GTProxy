#pragma once
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace control {
// Lets other threads (HTTP, WebSocket) run a function on the proxy main
// thread, where ENet and all packet handling live, and wait for its result.
class MainThreadQueue {
public:
    // Call from the thread that will drain(). Calls made from that same
    // thread run inline (avoids self-deadlock and simplifies tests).
    MainThreadQueue() : owner_{ std::this_thread::get_id() } { }

    void bind_to_current_thread() { owner_ = std::this_thread::get_id(); }

    template <typename F>
    auto call(F&& fn, const std::chrono::milliseconds timeout = std::chrono::milliseconds{ 5000 })
        -> std::optional<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        if (std::this_thread::get_id() == owner_) {
            return std::invoke(std::forward<F>(fn));
        }

        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        {
            std::scoped_lock lock{ mutex_ };
            if (closed_) {
                return std::nullopt;
            }
            tasks_.emplace_back([promise, f = std::forward<F>(fn)]() mutable {
                try {
                    promise->set_value(std::invoke(f));
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        }
        if (future.wait_for(timeout) != std::future_status::ready) {
            return std::nullopt; // the task still runs later; its result is discarded
        }
        try {
            return future.get();
        } catch (const std::future_error&) {
            return std::nullopt; // queue closed while waiting
        }
    }

    // Run queued tasks. Call once per main-loop iteration.
    std::size_t drain()
    {
        std::deque<std::function<void()>> batch;
        {
            std::scoped_lock lock{ mutex_ };
            batch.swap(tasks_);
        }
        for (auto& t : batch) {
            t();
        }
        return batch.size();
    }

    void close()
    {
        std::scoped_lock lock{ mutex_ };
        closed_ = true;
        tasks_.clear();
    }

private:
    std::thread::id owner_;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    bool closed_{ false };
};
}
