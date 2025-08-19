#pragma once

#include <string>
#include <memory>
#include <sstream>

namespace blustream {
namespace common {

enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARN = 3,
    ERROR = 4,
    FATAL = 5
};

class Logger {
public:
    virtual ~Logger() = default;
    
    virtual void log(LogLevel level, const std::string& message) = 0;
    virtual void set_level(LogLevel level) = 0;
    virtual LogLevel get_level() const = 0;
    
    // Convenience methods
    void trace(const std::string& message) { log(LogLevel::TRACE, message); }
    void debug(const std::string& message) { log(LogLevel::DEBUG, message); }
    void info(const std::string& message) { log(LogLevel::INFO, message); }
    void warn(const std::string& message) { log(LogLevel::WARN, message); }
    void error(const std::string& message) { log(LogLevel::ERROR, message); }
    void fatal(const std::string& message) { log(LogLevel::FATAL, message); }
};

// Simple console logger implementation
class ConsoleLogger : public Logger {
public:
    ConsoleLogger(LogLevel level = LogLevel::INFO);
    
    void log(LogLevel level, const std::string& message) override;
    void set_level(LogLevel level) override { level_ = level; }
    LogLevel get_level() const override { return level_; }

private:
    LogLevel level_;
    std::string format_message(LogLevel level, const std::string& message);
};

// Global logger instance
Logger& get_logger();
void set_logger(std::unique_ptr<Logger> logger);

// Convenience macros for logging
#define BLUSTREAM_LOG_TRACE(msg) ::blustream::common::get_logger().trace(msg)
#define BLUSTREAM_LOG_DEBUG(msg) ::blustream::common::get_logger().debug(msg)
#define BLUSTREAM_LOG_INFO(msg) ::blustream::common::get_logger().info(msg)
#define BLUSTREAM_LOG_WARN(msg) ::blustream::common::get_logger().warn(msg)
#define BLUSTREAM_LOG_ERROR(msg) ::blustream::common::get_logger().error(msg)
#define BLUSTREAM_LOG_FATAL(msg) ::blustream::common::get_logger().fatal(msg)

// Stream-style logging
class LogStream {
public:
    LogStream(LogLevel level) : level_(level) {}
    ~LogStream() { get_logger().log(level_, stream_.str()); }
    
    template<typename T>
    LogStream& operator<<(const T& value) {
        stream_ << value;
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream stream_;
};

#define BLUSTREAM_LOG_STREAM(level) ::blustream::common::LogStream(level)

}  // namespace common
}  // namespace blustream