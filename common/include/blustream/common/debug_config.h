#pragma once

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace blustream {
namespace common {

class DebugConfig {
public:
    static DebugConfig& instance() {
        static DebugConfig instance;
        return instance;
    }
    
    bool is_debug_io_enabled() const {
        return debug_io_enabled_.load();
    }
    
    void set_debug_io_enabled(bool enabled) {
        debug_io_enabled_.store(enabled);
        if (enabled) {
            std::cout << "\n⚠️  WARNING: DEBUG_IO is ENABLED - Performance will be impacted by disk writes!\n" << std::endl;
        }
    }
    
    // Instrumentation counters
    void increment_debug_writes_blocked() {
        debug_writes_blocked_.fetch_add(1);
    }
    
    void increment_debug_writes_permitted() {
        debug_writes_permitted_.fetch_add(1);
    }
    
    void print_debug_stats() const {
        size_t blocked = debug_writes_blocked_.load();
        size_t permitted = debug_writes_permitted_.load();
        size_t total = blocked + permitted;
        
        if (total > 0) {
            std::cout << "\n📊 DEBUG I/O STATISTICS:\n";
            std::cout << "  Debug writes blocked: " << blocked << "\n";
            std::cout << "  Debug writes permitted: " << permitted << "\n";
            std::cout << "  Total debug opportunities: " << total << "\n";
            std::cout << "  I/O reduction: " << (blocked * 100.0 / total) << "%\n" << std::endl;
        }
    }

private:
    DebugConfig() {
        // Check environment variable BLUSTREAM_DEBUG_IO
        const char* env_debug = std::getenv("BLUSTREAM_DEBUG_IO");
        bool enabled = false;
        
        if (env_debug) {
            enabled = (std::string(env_debug) == "1" || 
                      std::string(env_debug) == "true" || 
                      std::string(env_debug) == "TRUE");
        }
        
#ifdef DEBUG
        // Default to enabled in debug builds, but still allow override
        if (!env_debug) {
            enabled = false;  // Changed: even debug builds default to false for performance
        }
#endif
        
        debug_io_enabled_.store(enabled);
        
        if (enabled) {
            std::cout << "\n⚠️  WARNING: DEBUG_IO is ENABLED via environment variable!\n";
            std::cout << "   Performance will be impacted by frame dumps and disk writes.\n";
            std::cout << "   Set BLUSTREAM_DEBUG_IO=0 to disable.\n" << std::endl;
        }
    }
    
    std::atomic<bool> debug_io_enabled_{false};
    std::atomic<size_t> debug_writes_blocked_{0};
    std::atomic<size_t> debug_writes_permitted_{0};
};

// Convenience macros for debug I/O
#define BLUSTREAM_DEBUG_IO_ENABLED() (blustream::common::DebugConfig::instance().is_debug_io_enabled())

#define BLUSTREAM_DEBUG_IO_BLOCK() do { \
    blustream::common::DebugConfig::instance().increment_debug_writes_blocked(); \
} while(0)

#define BLUSTREAM_DEBUG_IO_PERMIT() do { \
    blustream::common::DebugConfig::instance().increment_debug_writes_permitted(); \
} while(0)

#define BLUSTREAM_DEBUG_IO_STATS() (blustream::common::DebugConfig::instance().print_debug_stats())

} // namespace common
} // namespace blustream