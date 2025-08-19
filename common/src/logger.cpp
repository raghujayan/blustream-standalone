#include "blustream/common/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <memory>

namespace blustream {
namespace common {

namespace {
    std::unique_ptr<Logger> g_logger;
}

// ConsoleLogger implementation
ConsoleLogger::ConsoleLogger(LogLevel level) : level_(level) {}

void ConsoleLogger::log(LogLevel level, const std::string& message) {
    if (level < level_) {
        return;
    }
    
    std::string formatted = format_message(level, message);
    
    if (level >= LogLevel::ERROR) {
        std::cerr << formatted << std::endl;
    } else {
        std::cout << formatted << std::endl;
    }
}

std::string ConsoleLogger::format_message(LogLevel level, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    const char* level_str = "";
    switch (level) {
        case LogLevel::TRACE: level_str = "TRACE"; break;
        case LogLevel::DEBUG: level_str = "DEBUG"; break;
        case LogLevel::INFO:  level_str = "INFO "; break;
        case LogLevel::WARN:  level_str = "WARN "; break;
        case LogLevel::ERROR: level_str = "ERROR"; break;
        case LogLevel::FATAL: level_str = "FATAL"; break;
    }
    
    oss << " [" << level_str << "] " << message;
    return oss.str();
}

// Global logger functions
Logger& get_logger() {
    if (!g_logger) {
        g_logger = std::make_unique<ConsoleLogger>(LogLevel::INFO);
    }
    return *g_logger;
}

void set_logger(std::unique_ptr<Logger> logger) {
    g_logger = std::move(logger);
}

}  // namespace common
}  // namespace blustream