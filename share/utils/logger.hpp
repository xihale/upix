#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <fstream>
#include <chrono>
#include <iostream>
#include <source_location>
#include <iomanip>
#include <sstream>

namespace crossscale {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

// ANSI color codes for terminal output
namespace console_colors {
    constexpr const char* RESET = "\033[0m";
    constexpr const char* RED = "\033[31m";       // ERROR
    constexpr const char* GREEN = "\033[32m";     // INFO
    constexpr const char* YELLOW = "\033[33m";    // WARNING
    constexpr const char* BLUE = "\033[34m";      // DEBUG
    constexpr const char* MAGENTA = "\033[35m";   // CRITICAL
    constexpr const char* CYAN = "\033[36m";      // Tag
    constexpr const char* WHITE = "\033[37m";     // Message
}

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        min_level_ = level;
    }

    void set_output_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.close();
        }
        file_.open(path, std::ios::app);
    }

    void log(LogLevel level, std::string_view tag, std::string_view message,
             const std::source_location& location = std::source_location::current()) {
        if (level < min_level_) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Get current time
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        // Format: [2024-01-15 10:30:45.123] [Tag] [LEVEL] Message
        std::ostringstream ts_stream;
        ts_stream << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        std::string timestamp = ts_stream.str() + "." + 
            (ms.count() < 100 ? "0" : "") + (ms.count() < 10 ? "0" : "") + std::to_string(ms.count());

        std::string level_str = level_to_string(level);
        const char* color = level_to_color(level);

        // Console output with colors
        std::cout << "[" << timestamp << "] "
                  << "[" << console_colors::CYAN << tag << console_colors::RESET << "] "
                  << "[" << color << level_str << console_colors::RESET << "] "
                  << message << std::endl;

        // File output (no colors)
        if (file_.is_open()) {
            file_ << "[" << timestamp << "] [" << tag << "] [" << level_str << "] " 
                  << message << " (" << location.file_name() << ":" << location.line() << ")\n";
            file_.flush();
        }

        // Android callback if set
        if (android_callback_) {
            android_callback_(level, tag.data(), message.data());
        }
    }

    void debug(std::string_view tag, std::string_view message,
               const std::source_location& location = std::source_location::current()) {
        log(LogLevel::DEBUG, tag, message, location);
    }

    void info(std::string_view tag, std::string_view message,
              const std::source_location& location = std::source_location::current()) {
        log(LogLevel::INFO, tag, message, location);
    }

    void warning(std::string_view tag, std::string_view message,
                 const std::source_location& location = std::source_location::current()) {
        log(LogLevel::WARNING, tag, message, location);
    }

    void error(std::string_view tag, std::string_view message,
               const std::source_location& location = std::source_location::current()) {
        log(LogLevel::ERROR, tag, message, location);
    }

    void critical(std::string_view tag, std::string_view message,
                  const std::source_location& location = std::source_location::current()) {
        log(LogLevel::CRITICAL, tag, message, location);
    }

    using AndroidLogCallback = void(*)(LogLevel, const char*, const char*);
    void set_android_callback(AndroidLogCallback callback) {
        android_callback_ = callback;
    }

private:
    Logger() = default;
    ~Logger() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::string level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::CRITICAL: return "CRIT";
            default: return "UNKNOWN";
        }
    }

    static const char* level_to_color(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return console_colors::BLUE;
            case LogLevel::INFO: return console_colors::GREEN;
            case LogLevel::WARNING: return console_colors::YELLOW;
            case LogLevel::ERROR: return console_colors::RED;
            case LogLevel::CRITICAL: return console_colors::MAGENTA;
            default: return console_colors::WHITE;
        }
    }

    LogLevel min_level_ = LogLevel::INFO;
    std::mutex mutex_;
    std::ofstream file_;
    AndroidLogCallback android_callback_ = nullptr;
};

// Convenience macros
#define LOG_DEBUG(tag, msg) crossscale::Logger::instance().debug(tag, msg)
#define LOG_INFO(tag, msg) crossscale::Logger::instance().info(tag, msg)
#define LOG_WARNING(tag, msg) crossscale::Logger::instance().warning(tag, msg)
#define LOG_ERROR(tag, msg) crossscale::Logger::instance().error(tag, msg)
#define LOG_CRITICAL(tag, msg) crossscale::Logger::instance().critical(tag, msg)

} // namespace crossscale
