#include "blustream/server/hardware_encoder.h"
#include "blustream/common/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <numeric>

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace blustream {
namespace server {

// Helper function for FFmpeg resource cleanup
static void free_codec_context(AVCodecContext* ctx) {
    if (ctx) {
        avcodec_free_context(&ctx);
    }
}

static void free_frame(AVFrame* frame) {
    if (frame) {
        av_frame_free(&frame);
    }
}

static void free_packet(AVPacket* packet) {
    if (packet) {
        av_packet_free(&packet);
    }
}

HardwareEncoder::HardwareEncoder()
    : active_encoder_type_(Type::SOFTWARE_X264)
    , initialized_(false)
    , encoder_context_(nullptr, free_codec_context)
    , input_frame_(nullptr, free_frame)
    , output_packet_(nullptr, free_packet)
    , hw_device_ctx_(nullptr)
    , hw_frames_ctx_(nullptr)
    , hw_frame_(nullptr, free_frame) {
    
    // Initialize stats
    stats_ = {};
}

HardwareEncoder::~HardwareEncoder() {
    shutdown();
}

bool HardwareEncoder::initialize(const Config& config) {
    if (initialized_) {
        BLUSTREAM_LOG_WARN("Hardware encoder already initialized");
        return true;
    }
    
    config_ = config;
    
    BLUSTREAM_LOG_INFO("Initializing hardware encoder...");
    BLUSTREAM_LOG_INFO("Target resolution: " + std::to_string(config_.width) + "x" + std::to_string(config_.height));
    BLUSTREAM_LOG_INFO("Target FPS: " + std::to_string(config_.fps));
    BLUSTREAM_LOG_INFO("Target bitrate: " + std::to_string(config_.bitrate_kbps) + " kbps");
    
    // Detect best encoder if auto-detect is requested
    if (config_.encoder_type == Type::AUTO_DETECT) {
        active_encoder_type_ = detect_best_encoder();
        BLUSTREAM_LOG_INFO("Auto-detected encoder: " + encoder_type_to_string(active_encoder_type_));
    } else {
        active_encoder_type_ = config_.encoder_type;
        BLUSTREAM_LOG_INFO("Using specified encoder: " + encoder_type_to_string(active_encoder_type_));
    }
    
    // Initialize the selected encoder
    bool success = false;
    switch (active_encoder_type_) {
        case Type::NVENC_H264:
        case Type::NVENC_HEVC:
            success = initialize_nvenc_encoder();
            break;
            
        case Type::QUICKSYNC_H264:
            success = initialize_quicksync_encoder();
            break;
            
        case Type::SOFTWARE_X264:
        default:
            success = initialize_software_encoder();
            active_encoder_type_ = Type::SOFTWARE_X264;
            break;
    }
    
    if (!success) {
        BLUSTREAM_LOG_ERROR("Failed to initialize " + encoder_type_to_string(active_encoder_type_) + " encoder");
        
        // Fallback to software encoder
        if (active_encoder_type_ != Type::SOFTWARE_X264) {
            BLUSTREAM_LOG_INFO("Falling back to software x264 encoder...");
            active_encoder_type_ = Type::SOFTWARE_X264;
            success = initialize_software_encoder();
        }
        
        if (!success) {
            BLUSTREAM_LOG_ERROR("Failed to initialize fallback software encoder");
            return false;
        }
    }
    
    // Allocate frames and packets
    input_frame_.reset(av_frame_alloc());
    output_packet_.reset(av_packet_alloc());
    
    if (!input_frame_ || !output_packet_) {
        BLUSTREAM_LOG_ERROR("Failed to allocate AVFrame/AVPacket");
        return false;
    }
    
    // Set up input frame properties
    input_frame_->format = AV_PIX_FMT_YUV420P;
    input_frame_->width = config_.width;
    input_frame_->height = config_.height;
    
    if (av_frame_get_buffer(input_frame_.get(), 32) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to allocate frame buffer");
        return false;
    }
    
    // Allocate YUV conversion buffer
    yuv_buffer_.resize(config_.width * config_.height * 3 / 2);  // YUV420 size
    
    initialized_ = true;
    stats_start_time_ = std::chrono::steady_clock::now();
    
    BLUSTREAM_LOG_INFO("✓ Hardware encoder initialized successfully!");
    BLUSTREAM_LOG_INFO("Active encoder: " + get_encoder_name());
    
    return true;
}

void HardwareEncoder::shutdown() {
    if (!initialized_) {
        return;
    }
    
    BLUSTREAM_LOG_INFO("Shutting down hardware encoder...");
    
    // Clean up hardware contexts
    if (hw_frames_ctx_) {
        av_buffer_unref(&hw_frames_ctx_);
    }
    
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
    }
    
    // Reset smart pointers (automatic cleanup)
    hw_frame_.reset();
    output_packet_.reset();
    input_frame_.reset();
    encoder_context_.reset();
    
    initialized_ = false;
    BLUSTREAM_LOG_INFO("Hardware encoder shut down");
}

std::vector<uint8_t> HardwareEncoder::encode_frame(const std::vector<uint8_t>& rgb_data) {
    if (!initialized_) {
        BLUSTREAM_LOG_ERROR("Encoder not initialized");
        return {};
    }
    
    auto encode_start = std::chrono::steady_clock::now();
    
    // Convert RGB to YUV420
    if (!convert_rgb_to_yuv420(rgb_data, input_frame_.get())) {
        BLUSTREAM_LOG_ERROR("Failed to convert RGB to YUV420");
        return {};
    }
    
    // Send frame to encoder
    int ret = avcodec_send_frame(encoder_context_.get(), input_frame_.get());
    if (ret < 0) {
        BLUSTREAM_LOG_ERROR("Failed to send frame to encoder: " + std::to_string(ret));
        return {};
    }
    
    // Receive encoded packet
    ret = avcodec_receive_packet(encoder_context_.get(), output_packet_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        // Need more frames or end of stream
        return {};
    } else if (ret < 0) {
        BLUSTREAM_LOG_ERROR("Failed to receive packet from encoder: " + std::to_string(ret));
        return {};
    }
    
    // Extract encoded data
    std::vector<uint8_t> encoded_data = extract_encoded_data(output_packet_.get());
    av_packet_unref(output_packet_.get());
    
    // Update performance statistics
    auto encode_end = std::chrono::steady_clock::now();
    float encode_time_ms = std::chrono::duration<float, std::milli>(encode_end - encode_start).count();
    update_performance_stats(encode_time_ms);
    
    stats_.frames_encoded++;
    
    return encoded_data;
}

bool HardwareEncoder::initialize_nvenc_encoder() {
    BLUSTREAM_LOG_INFO("Initializing NVENC encoder...");
    
    // Find NVENC codec
    const char* codec_name = get_nvenc_codec_name();
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        BLUSTREAM_LOG_ERROR("NVENC codec not found: " + std::string(codec_name));
        return false;
    }
    
    // Allocate codec context
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        BLUSTREAM_LOG_ERROR("Failed to allocate NVENC codec context");
        return false;
    }
    encoder_context_.reset(ctx);
    
    // Configure encoder
    ctx->width = config_.width;
    ctx->height = config_.height;
    ctx->time_base = {1, config_.fps};
    ctx->framerate = {config_.fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->bit_rate = config_.bitrate_kbps * 1000;
    ctx->gop_size = config_.keyframe_interval;
    ctx->max_b_frames = config_.enable_b_frames ? 2 : 0;
    
    // NVENC-specific options
    av_opt_set(ctx->priv_data, "preset", "p4", 0);  // Fast preset for low latency
    av_opt_set(ctx->priv_data, "tune", "ll", 0);    // Low latency tuning
    av_opt_set_int(ctx->priv_data, "delay", 0, 0);  // Zero delay
    av_opt_set_int(ctx->priv_data, "zerolatency", 1, 0);
    
    // Rate control
    switch (config_.rate_control) {
        case Config::CBR:
            av_opt_set(ctx->priv_data, "rc", "cbr", 0);
            break;
        case Config::VBR:
            av_opt_set(ctx->priv_data, "rc", "vbr", 0);
            ctx->rc_max_rate = config_.max_bitrate_kbps * 1000;
            break;
        case Config::CQP:
            av_opt_set(ctx->priv_data, "rc", "constqp", 0);
            av_opt_set_int(ctx->priv_data, "qp", config_.crf_quality, 0);
            break;
    }
    
    // Quality preset
    switch (config_.quality_preset) {
        case Quality::ULTRA_FAST:
            av_opt_set(ctx->priv_data, "preset", "p1", 0);
            break;
        case Quality::FAST:
            av_opt_set(ctx->priv_data, "preset", "p4", 0);
            break;
        case Quality::BALANCED:
            av_opt_set(ctx->priv_data, "preset", "p5", 0);
            break;
        case Quality::HIGH_QUALITY:
            av_opt_set(ctx->priv_data, "preset", "p7", 0);
            break;
    }
    
    // Open codec
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        BLUSTREAM_LOG_ERROR("Failed to open NVENC codec: " + std::to_string(ret));
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ NVENC encoder initialized successfully");
    return true;
}

bool HardwareEncoder::initialize_quicksync_encoder() {
    BLUSTREAM_LOG_INFO("Initializing Intel QuickSync encoder...");
    
    // Find QuickSync codec
    const char* codec_name = get_quicksync_codec_name();
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        BLUSTREAM_LOG_ERROR("QuickSync codec not found: " + std::string(codec_name));
        return false;
    }
    
    // Similar setup to NVENC but with QuickSync-specific options
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        BLUSTREAM_LOG_ERROR("Failed to allocate QuickSync codec context");
        return false;
    }
    encoder_context_.reset(ctx);
    
    // Configure encoder (similar to NVENC)
    ctx->width = config_.width;
    ctx->height = config_.height;
    ctx->time_base = {1, config_.fps};
    ctx->framerate = {config_.fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->bit_rate = config_.bitrate_kbps * 1000;
    ctx->gop_size = config_.keyframe_interval;
    
    // QuickSync-specific options
    av_opt_set(ctx->priv_data, "preset", "fast", 0);
    
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        BLUSTREAM_LOG_ERROR("Failed to open QuickSync codec: " + std::to_string(ret));
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ QuickSync encoder initialized successfully");
    return true;
}

bool HardwareEncoder::initialize_software_encoder() {
    BLUSTREAM_LOG_INFO("Initializing software x264 encoder...");
    
    // Find x264 codec
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        BLUSTREAM_LOG_ERROR("x264 codec not found");
        return false;
    }
    
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        BLUSTREAM_LOG_ERROR("Failed to allocate x264 codec context");
        return false;
    }
    encoder_context_.reset(ctx);
    
    // Configure encoder
    ctx->width = config_.width;
    ctx->height = config_.height;
    ctx->time_base = {1, config_.fps};
    ctx->framerate = {config_.fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->bit_rate = config_.bitrate_kbps * 1000;
    ctx->gop_size = config_.keyframe_interval;
    
    // x264-specific options
    av_opt_set(ctx->priv_data, "preset", "fast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        BLUSTREAM_LOG_ERROR("Failed to open x264 codec: " + std::to_string(ret));
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ Software x264 encoder initialized successfully");
    return true;
}

HardwareEncoder::Type HardwareEncoder::detect_best_encoder() {
    BLUSTREAM_LOG_INFO("Auto-detecting best available encoder...");
    
    // Priority order: NVENC > QuickSync > Software
    if (test_encoder_availability(Type::NVENC_H264)) {
        BLUSTREAM_LOG_INFO("NVENC H.264 encoder available");
        return Type::NVENC_H264;
    }
    
    if (test_encoder_availability(Type::QUICKSYNC_H264)) {
        BLUSTREAM_LOG_INFO("Intel QuickSync H.264 encoder available");
        return Type::QUICKSYNC_H264;
    }
    
    BLUSTREAM_LOG_INFO("Falling back to software x264 encoder");
    return Type::SOFTWARE_X264;
}

bool HardwareEncoder::test_encoder_availability(Type type) {
    const char* codec_name = nullptr;
    
    switch (type) {
        case Type::NVENC_H264:
            codec_name = "h264_nvenc";
            break;
        case Type::NVENC_HEVC:
            codec_name = "hevc_nvenc";
            break;
        case Type::QUICKSYNC_H264:
            codec_name = "h264_qsv";
            break;
        default:
            return false;
    }
    
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name);
    return codec != nullptr;
}

bool HardwareEncoder::convert_rgb_to_yuv420(const std::vector<uint8_t>& rgb_data, AVFrame* frame) {
    // Simple RGB to YUV420 conversion
    // This is a basic implementation - could be optimized with SIMD or hardware
    
    if (rgb_data.size() != config_.width * config_.height * 3) {
        BLUSTREAM_LOG_ERROR("Invalid RGB data size");
        return false;
    }
    
    const uint8_t* rgb = rgb_data.data();
    uint8_t* y_plane = frame->data[0];
    uint8_t* u_plane = frame->data[1];
    uint8_t* v_plane = frame->data[2];
    
    // Convert RGB to YUV using standard coefficients
    for (int y = 0; y < config_.height; y++) {
        for (int x = 0; x < config_.width; x++) {
            int rgb_idx = (y * config_.width + x) * 3;
            int y_idx = y * frame->linesize[0] + x;
            
            uint8_t r = rgb[rgb_idx + 0];
            uint8_t g = rgb[rgb_idx + 1];
            uint8_t b = rgb[rgb_idx + 2];
            
            // Y component
            y_plane[y_idx] = (77 * r + 150 * g + 29 * b + 128) >> 8;
            
            // U and V components (subsampled)
            if ((y % 2 == 0) && (x % 2 == 0)) {
                int uv_idx = (y / 2) * frame->linesize[1] + (x / 2);
                u_plane[uv_idx] = ((-43 * r - 84 * g + 127 * b + 128) >> 8) + 128;
                v_plane[uv_idx] = ((127 * r - 106 * g - 21 * b + 128) >> 8) + 128;
            }
        }
    }
    
    return true;
}

std::vector<uint8_t> HardwareEncoder::extract_encoded_data(AVPacket* packet) {
    std::vector<uint8_t> data(packet->data, packet->data + packet->size);
    return data;
}

void HardwareEncoder::update_performance_stats(float encode_time_ms) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    encode_times_.push_back(encode_time_ms);
    if (encode_times_.size() > 60) {  // Keep last 60 samples (2 seconds at 30fps)
        encode_times_.erase(encode_times_.begin());
    }
    
    stats_.avg_encode_time_ms = std::accumulate(encode_times_.begin(), encode_times_.end(), 0.0f) / encode_times_.size();
    stats_.min_encode_time_ms = *std::min_element(encode_times_.begin(), encode_times_.end());
    stats_.max_encode_time_ms = *std::max_element(encode_times_.begin(), encode_times_.end());
}

const char* HardwareEncoder::get_nvenc_codec_name() {
    return (active_encoder_type_ == Type::NVENC_HEVC) ? "hevc_nvenc" : "h264_nvenc";
}

const char* HardwareEncoder::get_quicksync_codec_name() {
    return "h264_qsv";
}

std::string HardwareEncoder::get_encoder_name() const {
    switch (active_encoder_type_) {
        case Type::NVENC_H264:
            return "NVIDIA NVENC H.264";
        case Type::NVENC_HEVC:
            return "NVIDIA NVENC H.265/HEVC";
        case Type::QUICKSYNC_H264:
            return "Intel QuickSync H.264";
        case Type::SOFTWARE_X264:
            return "Software x264";
        default:
            return "Unknown";
    }
}

bool HardwareEncoder::supports_hardware_acceleration() const {
    return active_encoder_type_ != Type::SOFTWARE_X264;
}

HardwareEncoder::Stats HardwareEncoder::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::string HardwareEncoder::encoder_type_to_string(Type type) {
    switch (type) {
        case Type::AUTO_DETECT: return "Auto Detect";
        case Type::NVENC_H264: return "NVENC H.264";
        case Type::NVENC_HEVC: return "NVENC HEVC";
        case Type::QUICKSYNC_H264: return "QuickSync H.264";
        case Type::SOFTWARE_X264: return "Software x264";
        default: return "Unknown";
    }
}

std::vector<HardwareEncoder::Type> HardwareEncoder::get_available_encoders() {
    std::vector<Type> available;
    
    if (avcodec_find_encoder_by_name("h264_nvenc")) {
        available.push_back(Type::NVENC_H264);
    }
    
    if (avcodec_find_encoder_by_name("hevc_nvenc")) {
        available.push_back(Type::NVENC_HEVC);
    }
    
    if (avcodec_find_encoder_by_name("h264_qsv")) {
        available.push_back(Type::QUICKSYNC_H264);
    }
    
    // Software encoder is always available
    available.push_back(Type::SOFTWARE_X264);
    
    return available;
}

bool HardwareEncoder::is_nvidia_gpu_available() {
    return avcodec_find_encoder_by_name("h264_nvenc") != nullptr;
}

bool HardwareEncoder::is_intel_gpu_available() {
    return avcodec_find_encoder_by_name("h264_qsv") != nullptr;
}

// Factory implementation
std::unique_ptr<HardwareEncoder> HardwareEncoderFactory::create_optimal_encoder(
    int width, int height, int fps, int bitrate_kbps) {
    
    HardwareEncoder::Config config;
    config.encoder_type = HardwareEncoder::Type::AUTO_DETECT;
    config.quality_preset = HardwareEncoder::Quality::FAST;
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.bitrate_kbps = bitrate_kbps;
    
    auto encoder = std::make_unique<HardwareEncoder>();
    if (encoder->initialize(config)) {
        return encoder;
    }
    
    return nullptr;
}

std::unique_ptr<HardwareEncoder> HardwareEncoderFactory::create_encoder(
    HardwareEncoder::Type type, const HardwareEncoder::Config& config) {
    
    auto encoder = std::make_unique<HardwareEncoder>();
    
    auto modified_config = config;
    modified_config.encoder_type = type;
    
    if (encoder->initialize(modified_config)) {
        return encoder;
    }
    
    return nullptr;
}

bool HardwareEncoderFactory::has_nvidia_encoding_support() {
    return HardwareEncoder::is_nvidia_gpu_available();
}

bool HardwareEncoderFactory::has_intel_encoding_support() {
    return HardwareEncoder::is_intel_gpu_available();
}

std::string HardwareEncoderFactory::get_system_encoding_capabilities() {
    std::stringstream ss;
    ss << "Available Hardware Encoders:\n";
    
    auto available = HardwareEncoder::get_available_encoders();
    for (auto type : available) {
        ss << "  - " << HardwareEncoder::encoder_type_to_string(type) << "\n";
    }
    
    return ss.str();
}

} // namespace server
} // namespace blustream