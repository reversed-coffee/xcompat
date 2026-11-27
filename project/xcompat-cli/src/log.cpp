#include "xcompat/log.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include <phnt_windows.h>

namespace {
    using namespace std::chrono_literals;

    bool colors_enabled = false;

    void init_colors() {
        const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;

        if (GetConsoleMode(handle, &mode))
            colors_enabled = SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
    }

    constexpr std::string_view level_color(xcompat::log::level level) {
        using enum xcompat::log::level;

        switch (level) {
        case trace: return "\x1b[90m";
        case debug: return "\x1b[36m";
        case log:   return "\x1b[37m";
        case info:  return "\x1b[32m";
        case warn:  return "\x1b[33m";
        case error: return "\x1b[31m";
        case fatal: return "\x1b[1;31m";
        }

        return "\x1b[0m";
    }

    struct digest_context {
        DWORD tid;

        std::atomic<bool> in_use{ false };
        std::atomic<bool> retired{ false };

        std::queue<xcompat::log::request> requests;

        digest_context(DWORD tid) : tid(tid) {}

        void acquire() {
            while (in_use.exchange(true, std::memory_order_acquire))
                in_use.wait(true, std::memory_order_relaxed);
        }

        void release() {
            in_use.store(false, std::memory_order_release);
            in_use.notify_one();
        }
    };

    std::atomic<bool> digest_awaiting{ false };

    std::mutex lifecycle_mutex;
    std::mutex registry_mutex;
    std::mutex flush_mutex;

    std::vector<std::shared_ptr<digest_context>> digest_registry;

    struct local_context {
        std::shared_ptr<digest_context> digest;

        local_context(DWORD tid) : digest(std::make_shared<digest_context>(tid)) {
            std::scoped_lock lock(registry_mutex);
            digest_registry.push_back(digest);
        }

        ~local_context() {
            digest->retired.store(true, std::memory_order_release);
        }
    };

    thread_local local_context local_digest{GetCurrentThreadId()};

    constexpr std::string_view level_name(xcompat::log::level level) {
        using enum xcompat::log::level;

        switch (level) {
        case trace: return "TRC";
        case debug: return "DBG";
        case log:   return "LOG";
        case info:  return "INF";
        case warn:  return "WRN";
        case error: return "ERR";
        case fatal: return "FTL";
        }

        return "UNKNOWN";
    }

    void digest_all() {
        std::scoped_lock flush_lock(flush_mutex);

        std::vector<std::shared_ptr<digest_context>> registry;

        {
            std::scoped_lock lock(registry_mutex);
            registry = digest_registry;
        }

        std::queue<xcompat::log::request> requests;

        for (const auto& digest : registry) {
            const bool retired = digest->retired.load(std::memory_order_acquire);

            digest->acquire();
            requests.swap(digest->requests);
            digest->release();

            while (!requests.empty()) {
                const auto& request = requests.front();

                std::cout << std::format("[{:X}] ", digest->tid);

                if (colors_enabled)
                    std::cout << level_color(request.level);

                std::cout << std::format("[{}]", level_name(request.level));

                if (colors_enabled)
                    std::cout << "\x1b[0m";

                std::cout << ' ' << request.message << '\n';
                requests.pop();
            }
        }

        std::cout.flush();
    }

    void digest_entry(const std::stop_token& stop_token) {
        while (digest_awaiting.load(std::memory_order_acquire) && !stop_token.stop_requested()) {
            digest_all();
            std::this_thread::sleep_for(5ms);
        }

        digest_all();
    }

    std::jthread digest_thread;
}

namespace xcompat::log {
    void init() {
        std::scoped_lock lock(lifecycle_mutex);

        if (bool expected = false; !digest_awaiting.compare_exchange_strong(expected, true))
            return;

        init_colors();

        try {
            digest_thread = std::jthread(digest_entry);
        }
        catch (...) {
            digest_awaiting = false;
            throw;
        }
    }

    void shutdown() {
        std::scoped_lock lock(lifecycle_mutex);

        if (!digest_awaiting.exchange(false))
            return;

        digest_thread.request_stop();

        if (digest_thread.joinable())
            digest_thread.join();
    }

    void flush() {
        digest_all();
    }

    void write(level level, std::string message) {
        auto& digest = *local_digest.digest;

        digest.acquire();

        try {
            digest.requests.push({ level, std::move(message) });
        }
        catch (...) {
            digest.release();
            throw;
        }

        digest.release();
    }
}
