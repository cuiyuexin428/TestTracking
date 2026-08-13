#include "common/Logger.h"

#include <chrono>
#include <ctime>
#include <thread>

namespace common {

namespace {

const char* LevelName(Logger::Level level) {
    switch (level) {
        case Logger::Level::Debug:
            return "DEBUG";
        case Logger::Level::Info:
            return "INFO";
        case Logger::Level::Warn:
            return "WARN";
        case Logger::Level::Error:
            return "ERROR";
        case Logger::Level::Fatal:
            return "FATAL";
    }
    return "UNKNOWN";
}

}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Logger::setLevel(Level level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

Logger::Level Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!path.empty()) {
        file_ = std::fopen(path.c_str(), "a");
    }
}

bool Logger::isEnabled(Level level) const {
    return static_cast<int>(level) >= static_cast<int>(this->level());
}

void Logger::log(Level level, const std::string& message) {
    if (!isEnabled(level)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    char timebuf[32] = {};
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream line;
    line << "[" << timebuf << "." << ms.count() << "]"
         << "[" << LevelName(level) << "]"
         << "[" << std::this_thread::get_id() << "] "
         << message << "\n";

    std::lock_guard<std::mutex> lock(mutex_);
    std::fputs(line.str().c_str(), stdout);
    std::fflush(stdout);
    if (file_ != nullptr) {
        std::fputs(line.str().c_str(), file_);
        std::fflush(file_);
    }
}

}  // namespace common
