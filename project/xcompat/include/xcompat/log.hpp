#pragma once

#include <format>
#include <string>
#include <utility>

namespace xcompat::log {
    enum class level {
        trace,
        debug,
        log,
        info,
        warn,
        error,
        fatal
    };

    struct request {
        log::level level;
        std::string message;
    };

    void init();
    void shutdown();
    void flush();

    void set_level(level level);
    level get_level();

    void write(level level, std::string message);

    template <typename... Args>
    void write(const level level, std::format_string<Args...> format, Args&&... args) {
        if (level < get_level()) return;
        write(level, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void trace(std::format_string<Args...> format, Args&&... args) {
        write(level::trace, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(std::format_string<Args...> format, Args&&... args) {
        write(level::debug, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::format_string<Args...> format, Args&&... args) {
        write(level::info, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> format, Args&&... args) {
        write(level::warn, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::format_string<Args...> format, Args&&... args) {
        write(level::error, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void fatal(std::format_string<Args...> format, Args&&... args) {
        write(level::fatal, format, std::forward<Args>(args)...);
        flush();
    }
}
