#pragma once

#include <string>
#include <string_view>
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace crossscale {

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

    void log(LogLevel level, std::string_view tag, std::string_view message) {
        if (level < min_level_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // Format: [2024-01-15 10:30:45.123] [Tag] [LEVEL] Message
        struct tm tm_buf{};
        localtime_r(&time, &tm_buf);
        std::ostringstream ts_stream;
        ts_stream << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        std::string timestamp = ts_stream.str() + "." +
            (ms.count() < 100 ? "0" : "") + (ms.count() < 10 ? "0" : "") + std::to_string(ms.count());

        std::string level_str = level_to_string(level);
        const char* color = level_to_color(level);

        std::cout << "[" << timestamp << "] "
                  << "[" << console_colors::CYAN << tag << console_colors::RESET << "] "
                  << "[" << color << level_str << console_colors::RESET << "] "
                  << message << std::endl;

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

    void debug(std::string_view tag, std::string_view message) {
        log(LogLevel::DEBUG, tag, message);
    }

    void info(std::string_view tag, std::string_view message) {
        log(LogLevel::INFO, tag, message);
    }

    void warning(std::string_view tag, std::string_view message) {
        log(LogLevel::WARNING, tag, message);
    }

    void error(std::string_view tag, std::string_view message) {
        log(LogLevel::ERROR, tag, message);
    }

private:
    Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::string level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }

    static const char* level_to_color(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return console_colors::BLUE;
            case LogLevel::INFO: return console_colors::GREEN;
            case LogLevel::WARNING: return console_colors::YELLOW;
            case LogLevel::ERROR: return console_colors::RED;
            default: return console_colors::WHITE;
        }
    }

    LogLevel min_level_ = [] {
        // CROSSSCALE_LOG_LEVEL=debug enables DEBUG output (default INFO).
        if (const char* lvl = std::getenv("CROSSSCALE_LOG_LEVEL")) {
            if (std::string_view(lvl) == "debug") return LogLevel::DEBUG;
        }
        return LogLevel::INFO;
    }();
    std::mutex mutex_;
};

// Convenience macros
#define LOG_DEBUG(tag, msg) crossscale::Logger::instance().debug(tag, msg)
#define LOG_INFO(tag, msg) crossscale::Logger::instance().info(tag, msg)
#define LOG_WARNING(tag, msg) crossscale::Logger::instance().warning(tag, msg)
#define LOG_ERROR(tag, msg) crossscale::Logger::instance().error(tag, msg)

} // namespace crossscale
