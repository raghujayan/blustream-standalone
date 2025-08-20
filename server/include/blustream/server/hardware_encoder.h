#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>

// Forward declarations for FFmpeg
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace blustream {
namespace server {

/**
 * @brief Hardware-accelerated video encoder supporting NVENC and QuickSync
 * 
 * Phase 4B: Hardware Encoding Implementation
 * - NVENC (NVIDIA GPU acceleration)
 * - Intel QuickSync (Intel GPU acceleration) 
 * - Automatic fallback to software encoding
 * - Zero-copy optimization with OpenGL
 */
class HardwareEncoder {
public:
    enum class Type {
        AUTO_DETECT,    // Automatically select best available
        NVENC_H264,     // NVIDIA NVENC H.264
        NVENC_HEVC,     // NVIDIA NVENC H.265/HEVC
        QUICKSYNC_H264, // Intel QuickSync H.264
        SOFTWARE_X264   // Software fallback
    };
    
    enum class Quality {
        ULTRA_FAST,     // Lowest latency, higher bitrate
        FAST,          // Balanced performance
        BALANCED,      // Good quality/performance ratio
        HIGH_QUALITY   // Best quality, higher latency
    };
    
    struct Config {
        // Encoder selection
        Type encoder_type = Type::AUTO_DETECT;
        Quality quality_preset = Quality::FAST;
        
        // Video parameters
        int width = 1920;
        int height = 1080;
        int fps = 30;
        int bitrate_kbps = 5000;
        int max_bitrate_kbps = 7500;  // For VBR
        int keyframe_interval = 60;    // GOP size
        
        // Hardware-specific settings
        bool use_zero_copy = true;     // OpenGL → GPU encoder direct
        bool enable_b_frames = false;  // Disable for lower latency
        int async_depth = 4;           // Encoder pipeline depth
        
        // Rate control
        enum RateControl {
            CBR,    // Constant bitrate
            VBR,    // Variable bitrate  
            CQP     // Constant quality
        } rate_control = VBR;
        
        int crf_quality = 23;  // For CQP mode (18-28 range)
    };
    
    HardwareEncoder();
    ~HardwareEncoder();
    
    // Encoder lifecycle
    bool initialize(const Config& config);
    void shutdown();
    bool is_initialized() const { return initialized_; }
    
    // Encoding operations
    std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& rgb_data);
    std::vector<uint8_t> encode_frame_yuv(const uint8_t* y_data, const uint8_t* u_data, const uint8_t* v_data);
    
    // Zero-copy OpenGL integration (future enhancement)
    std::vector<uint8_t> encode_from_texture(unsigned int gl_texture_id);
    
    // Encoder information
    Type get_active_encoder_type() const { return active_encoder_type_; }
    std::string get_encoder_name() const;
    bool supports_hardware_acceleration() const;
    
    // Performance metrics
    struct Stats {
        float avg_encode_time_ms;
        float min_encode_time_ms;
        float max_encode_time_ms;
        size_t frames_encoded;
        size_t frames_dropped;
        float hardware_utilization_percent;
        size_t gpu_memory_usage_mb;
    };
    Stats get_stats() const;
    
    // Static utility functions
    static std::vector<Type> get_available_encoders();
    static std::string encoder_type_to_string(Type type);
    static bool is_nvidia_gpu_available();
    static bool is_intel_gpu_available();

private:
    // Configuration
    Config config_;
    Type active_encoder_type_;
    bool initialized_;
    
    // FFmpeg encoder context
    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> encoder_context_;
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> input_frame_;
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> output_packet_;
    
    // Hardware-specific contexts
    AVBufferRef* hw_device_ctx_;    // Hardware device context
    AVBufferRef* hw_frames_ctx_;    // Hardware frames context
    
    // Frame conversion
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> hw_frame_;
    std::vector<uint8_t> yuv_buffer_;  // For RGB→YUV conversion
    
    // Performance tracking
    mutable std::mutex stats_mutex_;
    Stats stats_;
    std::chrono::steady_clock::time_point encode_start_time_;
    std::chrono::steady_clock::time_point stats_start_time_;
    std::vector<float> encode_times_;  // Rolling window for averages
    
    // Initialization helpers
    bool initialize_nvenc_encoder();
    bool initialize_quicksync_encoder();
    bool initialize_software_encoder();
    
    // Hardware detection
    Type detect_best_encoder();
    bool test_encoder_availability(Type type);
    
    // Frame processing
    bool convert_rgb_to_yuv420(const std::vector<uint8_t>& rgb_data, AVFrame* frame);
    bool upload_frame_to_hardware(AVFrame* sw_frame, AVFrame* hw_frame);
    std::vector<uint8_t> extract_encoded_data(AVPacket* packet);
    
    // Utility
    void update_performance_stats(float encode_time_ms);
    const char* get_nvenc_codec_name();
    const char* get_quicksync_codec_name();
};

/**
 * @brief Hardware encoder factory for automatic selection
 */
class HardwareEncoderFactory {
public:
    static std::unique_ptr<HardwareEncoder> create_optimal_encoder(
        int width, int height, int fps, int bitrate_kbps);
    
    static std::unique_ptr<HardwareEncoder> create_encoder(
        HardwareEncoder::Type type, const HardwareEncoder::Config& config);
    
    // System capability detection
    static bool has_nvidia_encoding_support();
    static bool has_intel_encoding_support();
    static std::string get_system_encoding_capabilities();
};

} // namespace server
} // namespace blustream