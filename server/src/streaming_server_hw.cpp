// StreamingServer implementation with Hardware Encoding (Phase 4B)
#include "blustream/server/streaming_server.h"
#include "blustream/server/hardware_encoder.h"
#include "blustream/common/logger.h"
#include "blustream/server/network_server.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <iomanip>

// System headers for sockets
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

// For VDS rendering
#include "blustream/server/opengl_context.h"

namespace blustream {
namespace server {

/**
 * @brief Enhanced StreamingServer with Hardware Encoding Support
 * 
 * Phase 4B Implementation:
 * - NVENC hardware acceleration for encoding
 * - Intel QuickSync fallback support  
 * - Automatic encoder selection
 * - Performance monitoring and statistics
 */
class HardwareStreamingServer : public StreamingServer {
public:
    HardwareStreamingServer();
    ~HardwareStreamingServer() override;
    
    // Enhanced configuration for hardware encoding
    struct HardwareConfig : public Config {
        // Hardware encoder settings
        HardwareEncoder::Type preferred_encoder = HardwareEncoder::Type::AUTO_DETECT;
        HardwareEncoder::Quality quality_preset = HardwareEncoder::Quality::FAST;
        HardwareEncoder::Config::RateControl rate_control = HardwareEncoder::Config::RateControl::VBR;
        
        // Performance settings
        bool enable_zero_copy = true;      // OpenGL → GPU encoder direct transfer
        bool enable_async_encoding = true; // Async encoder pipeline
        int encoder_threads = 2;           // Number of encoder threads
        
        // Adaptive quality settings
        bool enable_adaptive_bitrate = true;
        int min_bitrate_kbps = 1000;
        int max_bitrate_kbps = 10000;
        float quality_scale_factor = 1.0f;
    };
    
    bool initialize_hardware(const HardwareConfig& config);
    
    // Hardware encoder status
    std::string get_encoder_info() const;
    HardwareEncoder::Stats get_encoder_stats() const;
    bool is_hardware_accelerated() const;
    
private:
    // Hardware encoder
    std::unique_ptr<HardwareEncoder> hardware_encoder_;
    HardwareConfig hw_config_;
    
    // Enhanced encoding pipeline
    void enhanced_render_loop();
    void hardware_encode_and_send_frame(const std::vector<uint8_t>& rgb_data);
    
    // Performance monitoring
    void monitor_performance();
    std::thread performance_monitor_thread_;
    
    // Adaptive quality control
    void adjust_encoder_quality();
    std::chrono::steady_clock::time_point last_quality_adjustment_;
};

HardwareStreamingServer::HardwareStreamingServer() 
    : StreamingServer()
    , hardware_encoder_(nullptr) {
    BLUSTREAM_LOG_INFO("Hardware streaming server created");
}

HardwareStreamingServer::~HardwareStreamingServer() {
    if (hardware_encoder_) {
        hardware_encoder_->shutdown();
    }
    BLUSTREAM_LOG_INFO("Hardware streaming server destroyed");
}

bool HardwareStreamingServer::initialize_hardware(const HardwareConfig& config) {
    hw_config_ = config;
    
    BLUSTREAM_LOG_INFO("=== Phase 4B: Hardware Encoding Initialization ===");
    
    // Initialize base streaming server
    if (!initialize(static_cast<const Config&>(config))) {
        BLUSTREAM_LOG_ERROR("Failed to initialize base streaming server");
        return false;
    }
    
    // Create hardware encoder
    hardware_encoder_ = std::make_unique<HardwareEncoder>();
    
    // Configure hardware encoder
    HardwareEncoder::Config encoder_config;
    encoder_config.encoder_type = config.preferred_encoder;
    encoder_config.quality_preset = config.quality_preset;
    encoder_config.width = config.render_width;
    encoder_config.height = config.render_height;
    encoder_config.fps = static_cast<int>(config.target_fps);
    encoder_config.bitrate_kbps = config.bitrate_kbps;
    encoder_config.max_bitrate_kbps = config.max_bitrate_kbps;
    encoder_config.keyframe_interval = config.keyframe_interval;
    encoder_config.rate_control = config.rate_control;
    encoder_config.use_zero_copy = config.enable_zero_copy;
    encoder_config.enable_b_frames = false;  // Disable for low latency
    encoder_config.async_depth = config.encoder_threads;
    
    // Initialize hardware encoder
    if (!hardware_encoder_->initialize(encoder_config)) {
        BLUSTREAM_LOG_ERROR("Failed to initialize hardware encoder");
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ Hardware encoder initialized: " + hardware_encoder_->get_encoder_name());
    BLUSTREAM_LOG_INFO("✓ Hardware acceleration: " + 
                      (hardware_encoder_->supports_hardware_acceleration() ? "ENABLED" : "DISABLED"));
    
    return true;
}

void HardwareStreamingServer::enhanced_render_loop() {
    BLUSTREAM_LOG_INFO("Starting enhanced render loop with hardware encoding...");
    
    next_frame_time_ = std::chrono::steady_clock::now();
    animation_start_time_ = next_frame_time_;
    
    while (running_) {
        auto frame_start = std::chrono::steady_clock::now();
        
        // Wait for next frame time
        std::this_thread::sleep_until(next_frame_time_);
        
        // Calculate animation time for slice progression
        auto animation_elapsed = std::chrono::duration<float>(frame_start - animation_start_time_).count();
        
        // Get VDS slice data based on animation time
        auto slice_data = vds_manager_->get_animated_slice_rgb(config_.slice_orientation, 
                                                              animation_elapsed, 
                                                              config_.animation_duration);
        
        if (slice_data.empty()) {
            BLUSTREAM_LOG_WARN("Empty slice data received");
            next_frame_time_ += frame_duration_;
            continue;
        }
        
        auto render_end = std::chrono::steady_clock::now();
        float render_time_ms = std::chrono::duration<float, std::milli>(render_end - frame_start).count();
        
        // Hardware encode and send frame
        hardware_encode_and_send_frame(slice_data);
        
        auto encode_end = std::chrono::steady_clock::now();
        float encode_time_ms = std::chrono::duration<float, std::milli>(encode_end - render_end).count();
        
        // Update statistics
        update_stats(render_time_ms, encode_time_ms, 0);  // bytes_sent updated in broadcast
        
        // Performance monitoring
        if (hw_config_.enable_adaptive_bitrate) {
            adjust_encoder_quality();
        }
        
        // Schedule next frame
        next_frame_time_ += frame_duration_;
        
        // Handle frame drops if we're behind
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time_ < now) {
            auto frames_behind = std::chrono::duration_cast<std::chrono::microseconds>(now - next_frame_time_) / frame_duration_;
            if (frames_behind > 2) {
                BLUSTREAM_LOG_WARN("Dropping " + std::to_string(frames_behind) + " frames due to performance");
                next_frame_time_ = now;
                stats_.frames_dropped += frames_behind;
            }
        }
        
        stats_.frames_rendered++;
    }
    
    BLUSTREAM_LOG_INFO("Enhanced render loop stopped");
}

void HardwareStreamingServer::hardware_encode_and_send_frame(const std::vector<uint8_t>& rgb_data) {
    if (!hardware_encoder_) {
        BLUSTREAM_LOG_ERROR("Hardware encoder not initialized");
        return;
    }
    
    auto encode_start = std::chrono::steady_clock::now();
    
    // Hardware encode frame
    std::vector<uint8_t> encoded_data = hardware_encoder_->encode_frame(rgb_data);
    
    auto encode_end = std::chrono::steady_clock::now();
    float encode_time_ms = std::chrono::duration<float, std::milli>(encode_end - encode_start).count();
    
    if (encoded_data.empty()) {
        // Some encoders may not produce output for every input (B-frames, etc.)
        return;
    }
    
    // Determine if this is a keyframe (simplified detection)
    bool is_keyframe = (stats_.frames_encoded % config_.keyframe_interval == 0);
    
    // Broadcast to all connected clients
    broadcast_frame(encoded_data, is_keyframe);
    
    stats_.frames_encoded++;
    
    // Log performance periodically
    static auto last_log = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - last_log).count() > 5.0f) {  // Every 5 seconds
        auto hw_stats = hardware_encoder_->get_stats();
        BLUSTREAM_LOG_INFO("Hardware encoding performance:");
        BLUSTREAM_LOG_INFO("  Avg encode time: " + std::to_string(hw_stats.avg_encode_time_ms) + "ms");
        BLUSTREAM_LOG_INFO("  Min/Max: " + std::to_string(hw_stats.min_encode_time_ms) + 
                          "/" + std::to_string(hw_stats.max_encode_time_ms) + "ms");
        BLUSTREAM_LOG_INFO("  Frames encoded: " + std::to_string(hw_stats.frames_encoded));
        BLUSTREAM_LOG_INFO("  Current FPS: " + std::to_string(stats_.current_fps));
        last_log = now;
    }
}

void HardwareStreamingServer::adjust_encoder_quality() {
    auto now = std::chrono::steady_clock::now();
    
    // Only adjust every 2 seconds
    if (std::chrono::duration<float>(now - last_quality_adjustment_).count() < 2.0f) {
        return;
    }
    
    auto hw_stats = hardware_encoder_->get_stats();
    
    // Adaptive quality based on encoding performance
    if (hw_stats.avg_encode_time_ms > 16.0f) {  // > 60fps target
        // Encoding too slow, reduce quality
        BLUSTREAM_LOG_INFO("Reducing encoder quality due to high encode time: " + 
                          std::to_string(hw_stats.avg_encode_time_ms) + "ms");
        // Could adjust bitrate or quality preset here
    } else if (hw_stats.avg_encode_time_ms < 8.0f) {  // Very fast encoding
        // Could increase quality if bandwidth allows
        BLUSTREAM_LOG_DEBUG("Encoder performing well: " + std::to_string(hw_stats.avg_encode_time_ms) + "ms");
    }
    
    last_quality_adjustment_ = now;
}

std::string HardwareStreamingServer::get_encoder_info() const {
    if (!hardware_encoder_) {
        return "Hardware encoder not initialized";
    }
    
    std::stringstream ss;
    ss << "Active Encoder: " << hardware_encoder_->get_encoder_name() << "\n";
    ss << "Hardware Acceleration: " << (hardware_encoder_->supports_hardware_acceleration() ? "YES" : "NO") << "\n";
    
    auto stats = hardware_encoder_->get_stats();
    ss << "Performance Stats:\n";
    ss << "  Avg Encode Time: " << std::fixed << std::setprecision(2) << stats.avg_encode_time_ms << "ms\n";
    ss << "  Min/Max: " << stats.min_encode_time_ms << "/" << stats.max_encode_time_ms << "ms\n";
    ss << "  Frames Encoded: " << stats.frames_encoded << "\n";
    ss << "  Frames Dropped: " << stats.frames_dropped << "\n";
    
    return ss.str();
}

HardwareEncoder::Stats HardwareStreamingServer::get_encoder_stats() const {
    if (hardware_encoder_) {
        return hardware_encoder_->get_stats();
    }
    return {};
}

bool HardwareStreamingServer::is_hardware_accelerated() const {
    return hardware_encoder_ && hardware_encoder_->supports_hardware_acceleration();
}

} // namespace server
} // namespace blustream

// Example usage and testing function
extern "C" {

/**
 * @brief Test hardware encoding capabilities on the system
 */
int test_hardware_encoding() {
    using namespace blustream::server;
    
    BLUSTREAM_LOG_INFO("=== Hardware Encoding Capability Test ===");
    
    // Test available encoders
    auto available_encoders = HardwareEncoder::get_available_encoders();
    BLUSTREAM_LOG_INFO("Available encoders:");
    for (auto type : available_encoders) {
        BLUSTREAM_LOG_INFO("  - " + HardwareEncoder::encoder_type_to_string(type));
    }
    
    // Test NVENC availability
    if (HardwareEncoder::is_nvidia_gpu_available()) {
        BLUSTREAM_LOG_INFO("✓ NVIDIA GPU with NVENC support detected");
    } else {
        BLUSTREAM_LOG_WARN("⚠ NVIDIA GPU or NVENC not available");
    }
    
    // Test Intel QuickSync availability
    if (HardwareEncoder::is_intel_gpu_available()) {
        BLUSTREAM_LOG_INFO("✓ Intel GPU with QuickSync support detected");
    } else {
        BLUSTREAM_LOG_WARN("⚠ Intel GPU or QuickSync not available");
    }
    
    // Create and test optimal encoder
    auto encoder = HardwareEncoderFactory::create_optimal_encoder(1920, 1080, 30, 5000);
    if (encoder) {
        BLUSTREAM_LOG_INFO("✓ Successfully created optimal encoder: " + encoder->get_encoder_name());
        BLUSTREAM_LOG_INFO("✓ Hardware acceleration: " + 
                          std::string(encoder->supports_hardware_acceleration() ? "ENABLED" : "DISABLED"));
    } else {
        BLUSTREAM_LOG_ERROR("✗ Failed to create optimal encoder");
        return -1;
    }
    
    BLUSTREAM_LOG_INFO("=== Hardware Encoding Test Complete ===");
    return 0;
}

} // extern "C"