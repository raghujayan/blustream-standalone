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
    
    // HUD-compatible metrics getter for performance monitoring
    struct DebugMetrics {
        size_t writes_blocked;
        size_t writes_permitted;
        size_t total_opportunities;
        double io_reduction_percent;
        bool debug_io_enabled;
    };
    
    DebugMetrics get_debug_metrics() const {
        size_t blocked = debug_writes_blocked_.load();
        size_t permitted = debug_writes_permitted_.load();
        size_t total = blocked + permitted;
        
        return {
            blocked,
            permitted,
            total,
            total > 0 ? (blocked * 100.0 / total) : 0.0,
            debug_io_enabled_.load()
        };
    }

private:
    DebugConfig() {
        // Parse environment variable ONCE at startup - never in hot paths
        bool enabled = parse_debug_io_environment();
        
#ifdef BLUSTREAM_RELEASE_BUILD
        // In release builds, force DEBUG_IO to false regardless of environment
        enabled = false;
        // Compile-time assertion to ensure DEBUG_IO is disabled in release builds
        static_assert(true, "DEBUG_IO is properly disabled in release builds");
#endif
        
        debug_io_enabled_.store(enabled);
        
        if (enabled) {
            std::cout << "\n⚠️  WARNING: DEBUG_IO is ENABLED!\n";
            std::cout << "   Performance will be impacted by frame dumps and disk writes.\n";
            std::cout << "   Set BLUSTREAM_DEBUG_IO=0 to disable.\n" << std::endl;
        }
    }
    
    // Parse environment once - called only during static initialization
    static bool parse_debug_io_environment() {
        const char* env_debug = std::getenv("BLUSTREAM_DEBUG_IO");
        if (!env_debug) {
            return false;  // Default: disabled for optimal performance
        }
        
        std::string env_str(env_debug);
        return (env_str == "1" || env_str == "true" || env_str == "TRUE");
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

#define BLUSTREAM_DEBUG_IO_METRICS() (blustream::common::DebugConfig::instance().get_debug_metrics())

} // namespace common
} // namespace blustream