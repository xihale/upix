#pragma once

#include <format>
#include <string>
#include <string_view>
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <iostream>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace upix {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3
};

// ANSI color codes for terminal output
namespace console_colors {
    constexpr const char* RESET = "\033[0m";
    constexpr const char* RED = "\033[31m";       // ERROR
    constexpr const char* GREEN = "\033[32m";     // INFO
    constexpr const char* YELLOW = "\033[33m";    // WARNING
    constexpr const char* BLUE = "\033[34m";      // DEBUG
    constexpr const char* CYAN = "\033[36m";      // Tag
    constexpr const char* WHITE = "\033[37m";     // Message
}

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // Pre-formatted sink (also used by the templated overloads below).
    void log(LogLevel level, std::string_view tag, std::string_view message) {
        if (level < min_level_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Format: [2024-01-15 10:30:45.123] [Tag] [LEVEL] Message
        const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now());
        const auto time = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf {};
        localtime_r(&time, &tm_buf);
        const int ms = static_cast<int>(now.time_since_epoch().count() % 1000);

        const auto timestamp = std::format(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);

        std::cout << std::format("[{}] [{}{}] [{}{}] {}\n",
                                 timestamp,
                                 console_colors::CYAN, tag,
                                 level_to_color(level), level_to_string(level),
                                 message) << std::flush;

#ifdef __ANDROID__
        // Mirror to Android logcat so native engine logs are visible alongside
        // the Kotlin layer. stdout is not captured by logcat on release devices.
        int android_prio = ANDROID_LOG_UNKNOWN;
        switch (level) {
            case LogLevel::DEBUG:   android_prio = ANDROID_LOG_DEBUG; break;
            case LogLevel::INFO:    android_prio = ANDROID_LOG_INFO;  break;
            case LogLevel::WARNING: android_prio = ANDROID_LOG_WARN;  break;
            case LogLevel::ERROR:   android_prio = ANDROID_LOG_ERROR; break;
        }
        std::string tag_str(tag);
        std::string msg_str(message);
        __android_log_print(android_prio, tag_str.c_str(), "%s", msg_str.c_str());
#endif
    }

    // Pre-formatted messages: passed through verbatim, never re-formatted
    // (braces/% in dynamic strings stay literal).
    void debug(std::string_view tag, std::string_view message) { log(LogLevel::DEBUG, tag, message); }
    void info(std::string_view tag, std::string_view message) { log(LogLevel::INFO, tag, message); }
    void warning(std::string_view tag, std::string_view message) { log(LogLevel::WARNING, tag, message); }
    void error(std::string_view tag, std::string_view message) { log(LogLevel::ERROR, tag, message); }

    // Formatted overloads: info(tag, "loaded {} tiles", n), compile-time
    // checked via std::format_string.
    template <typename... Args>
    void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        debug(tag, std::vformat(fmt.get(), std::make_format_args(args...)));
    }
    template <typename... Args>
    void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        info(tag, std::vformat(fmt.get(), std::make_format_args(args...)));
    }
    template <typename... Args>
    void warning(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        warning(tag, std::vformat(fmt.get(), std::make_format_args(args...)));
    }
    template <typename... Args>
    void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
        error(tag, std::vformat(fmt.get(), std::make_format_args(args...)));
    }

private:
    Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static constexpr std::string_view level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }

    static const char* level_to_color(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return console_colors::BLUE;
            case LogLevel::INFO: return console_colors::GREEN;
            case LogLevel::WARNING: return console_colors::YELLOW;
            case LogLevel::ERROR: return console_colors::RED;
        }
        return console_colors::WHITE;
    }

    LogLevel min_level_ = [] {
        // UPIX_LOG_LEVEL=debug enables DEBUG output (default INFO).
        if (const char* lvl = std::getenv("UPIX_LOG_LEVEL")) {
            if (std::string_view(lvl) == "debug") return LogLevel::DEBUG;
        }
        return LogLevel::INFO;
    }();
    std::mutex mutex_;
};

} // namespace upix

// Convenience macros. Single-string calls work unchanged; extra args are
// treated as std::format arguments:
//   LOG_INFO(tag, msg)                    -> literal/dynamic message
//   LOG_INFO(tag, "loaded {} tiles", n)   -> formatted at the call site
#define LOG_DEBUG(tag, ...) upix::Logger::instance().debug(tag __VA_OPT__(,) __VA_ARGS__)
#define LOG_INFO(tag, ...) upix::Logger::instance().info(tag __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARNING(tag, ...) upix::Logger::instance().warning(tag __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(tag, ...) upix::Logger::instance().error(tag __VA_OPT__(,) __VA_ARGS__)
