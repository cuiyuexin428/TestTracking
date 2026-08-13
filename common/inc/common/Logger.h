#pragma once

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

namespace common {

// Thread-safe logging facility. Console output by default; an optional log
// file can be attached with setFile(). Messages below the configured level
// are filtered out before formatting.
class Logger {
public:
    enum class Level {
        Debug = 0,
        Info,
        Warn,
        Error,
        Fatal,
    };

    static Logger& instance();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void setLevel(Level level);
    Level level() const;

    // Appends to the given file. Pass an empty path to close the current file.
    void setFile(const std::string& path);

    bool isEnabled(Level level) const;

    void log(Level level, const std::string& message);

private:
    Logger() = default;
    ~Logger();

    mutable std::mutex mutex_;
    Level level_ = Level::Debug;
    FILE* file_ = nullptr;
};

}  // namespace common

#define COMMON_LOG_IMPL(level_, msg_)                                          \
    do {                                                                       \
        if (::common::Logger::instance().isEnabled(level_)) {                  \
            std::ostringstream log_stream_;                                    \
            log_stream_ << msg_;                                               \
            ::common::Logger::instance().log(level_, log_stream_.str());       \
        }                                                                      \
    } while (0)

#define LOG_DEBUG(msg_) COMMON_LOG_IMPL(::common::Logger::Level::Debug, msg_)
#define LOG_INFO(msg_)  COMMON_LOG_IMPL(::common::Logger::Level::Info, msg_)
#define LOG_WARN(msg_)  COMMON_LOG_IMPL(::common::Logger::Level::Warn, msg_)
#define LOG_ERROR(msg_) COMMON_LOG_IMPL(::common::Logger::Level::Error, msg_)
#define LOG_FATAL(msg_) COMMON_LOG_IMPL(::common::Logger::Level::Fatal, msg_)
