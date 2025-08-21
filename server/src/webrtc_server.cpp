#include "blustream/server/webrtc_server.h"
#include "blustream/server/vds_manager.h"
#include "blustream/common/logger.h"

#include <random>
#include <sstream>
#include <chrono>
#include <algorithm>

// WebRTC includes
#include "api/peer_connection_interface.h"
#include "api/create_peerconnection_factory.h"
#include "api/video/video_frame.h"
#include "api/video/i420_buffer.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "media/engine/webrtc_media_engine.h"

namespace blustream {
namespace server {

// Custom video source for injecting our encoded frames
class CustomVideoSource : public webrtc::VideoTrackSource {
public:
    CustomVideoSource() : VideoTrackSource(false /* remote */) {}
    
    void send_frame(const std::vector<uint8_t>& rgb_data, int width, int height) {
        // Convert RGB to I420 format for WebRTC
        auto i420_buffer = webrtc::I420Buffer::Create(width, height);
        
        // Simple RGB to YUV conversion (can be optimized)
        convert_rgb_to_i420(rgb_data, width, height, i420_buffer);
        
        // Create WebRTC video frame
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(i420_buffer)
            .set_timestamp_us(rtc::TimeMicros())
            .build();
        
        // Send to all sinks
        OnFrame(frame);
    }

private:
    void convert_rgb_to_i420(const std::vector<uint8_t>& rgb_data, int width, int height,
                            rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer) {
        // RGB to YUV420 conversion
        const uint8_t* rgb = rgb_data.data();
        uint8_t* y_plane = i420_buffer->MutableDataY();
        uint8_t* u_plane = i420_buffer->MutableDataU();
        uint8_t* v_plane = i420_buffer->MutableDataV();
        
        int y_stride = i420_buffer->StrideY();
        int u_stride = i420_buffer->StrideU();
        int v_stride = i420_buffer->StrideV();
        
        // Convert RGB to YUV using standard coefficients
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int rgb_idx = (y * width + x) * 3;
                uint8_t r = rgb[rgb_idx + 0];
                uint8_t g = rgb[rgb_idx + 1];
                uint8_t b = rgb[rgb_idx + 2];
                
                // Y component
                uint8_t y_val = (uint8_t)(0.299 * r + 0.587 * g + 0.114 * b);
                y_plane[y * y_stride + x] = y_val;
                
                // U and V components (subsampled for 4:2:0)
                if (y % 2 == 0 && x % 2 == 0) {
                    uint8_t u_val = (uint8_t)(128 - 0.168736 * r - 0.331264 * g + 0.5 * b);
                    uint8_t v_val = (uint8_t)(128 + 0.5 * r - 0.418688 * g - 0.081312 * b);
                    
                    int uv_x = x / 2;
                    int uv_y = y / 2;
                    u_plane[uv_y * u_stride + uv_x] = u_val;
                    v_plane[uv_y * v_stride + uv_x] = v_val;
                }
            }
        }
    }
    
    // VideoTrackSource interface
    rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
        return this;
    }
    bool is_screencast() const override { return false; }
    absl::optional<bool> needs_denoising() const override { return false; }
    webrtc::MediaSourceInterface::SourceState state() const override {
        return webrtc::MediaSourceInterface::kLive;
    }
    bool remote() const override { return false; }
};

// WebRTC Server Implementation
WebRTCServer::WebRTCServer() 
    : running_(false)
    , animation_start_time_(std::chrono::steady_clock::now())
    , stats_start_time_(std::chrono::steady_clock::now()) {
}

WebRTCServer::~WebRTCServer() {
    stop();
}

bool WebRTCServer::initialize(const Config& config) {
    config_ = config;
    
    LOG_INFO("Initializing WebRTC Server for Phase 5");
    LOG_INFO("Configuration:");
    LOG_INFO("  Signaling port: " << config_.signaling_port);
    LOG_INFO("  Max sessions: " << config_.max_sessions);
    LOG_INFO("  Default resolution: " << config_.default_width << "x" << config_.default_height);
    LOG_INFO("  Default FPS: " << config_.default_fps);
    
    // Initialize WebRTC
    if (!initialize_webrtc()) {
        LOG_ERROR("Failed to initialize WebRTC");
        return false;
    }
    
    // Initialize hardware encoder
    if (!initialize_hardware_encoder()) {
        LOG_ERROR("Failed to initialize hardware encoder");
        return false;
    }
    
    // Initialize VDS manager
    vds_manager_ = std::make_unique<VDSManager>();
    
    LOG_INFO("WebRTC Server initialized successfully");
    return true;
}

bool WebRTCServer::initialize_webrtc() {
    // Create peer connection factory
    peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
        nullptr /* network_thread */,
        nullptr /* worker_thread */,
        nullptr /* signaling_thread */,
        nullptr /* default_adm */,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        nullptr /* audio_mixer */,
        nullptr /* audio_processing */);
    
    if (!peer_connection_factory_) {
        LOG_ERROR("Failed to create peer connection factory");
        return false;
    }
    
    LOG_INFO("WebRTC peer connection factory created");
    return true;
}

bool WebRTCServer::initialize_hardware_encoder() {
    HardwareEncoder::Config encoder_config;
    encoder_config.encoder_type = config_.encoder_type;
    encoder_config.quality_preset = config_.encoder_quality;
    encoder_config.width = config_.default_width;
    encoder_config.height = config_.default_height;
    encoder_config.fps = static_cast<int>(config_.default_fps);
    encoder_config.bitrate_kbps = (config_.min_bitrate_kbps + config_.max_bitrate_kbps) / 2;
    
    // Optimize for low latency
    encoder_config.enable_b_frames = false;
    encoder_config.keyframe_interval = 30;  // More frequent keyframes for WebRTC
    encoder_config.rate_control = HardwareEncoder::Config::VBR;
    
    hardware_encoder_ = std::make_unique<HardwareEncoder>();
    
    if (!hardware_encoder_->initialize(encoder_config)) {
        LOG_ERROR("Failed to initialize hardware encoder");
        return false;
    }
    
    LOG_INFO("Hardware encoder initialized: " << hardware_encoder_->get_encoder_name());
    LOG_INFO("Hardware acceleration: " << (hardware_encoder_->supports_hardware_acceleration() ? "ENABLED" : "DISABLED"));
    
    return true;
}

bool WebRTCServer::start() {
    if (running_) {
        LOG_WARN("WebRTC Server is already running");
        return true;
    }
    
    running_ = true;
    animation_start_time_ = std::chrono::steady_clock::now();
    
    // Start render loop
    render_thread_ = std::thread(&WebRTCServer::render_loop, this);
    
    LOG_INFO("WebRTC Server started successfully");
    LOG_INFO("Ready for Phase 5 WebRTC connections");
    
    return true;
}

void WebRTCServer::stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("Stopping WebRTC Server...");
    
    running_ = false;
    
    // Wait for render thread
    if (render_thread_.joinable()) {
        render_thread_.join();
    }
    
    // Clean up all sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
    
    cleanup();
    
    LOG_INFO("WebRTC Server stopped");
}

bool WebRTCServer::load_vds(const std::string& path) {
    if (!vds_manager_) {
        LOG_ERROR("VDS Manager not initialized");
        return false;
    }
    
    LOG_INFO("Loading VDS file: " << path);
    
    if (!vds_manager_->load_vds(path)) {
        LOG_ERROR("Failed to load VDS file: " << path);
        return false;
    }
    
    auto dimensions = vds_manager_->get_dimensions();
    LOG_INFO("VDS loaded successfully:");
    LOG_INFO("  Dimensions: " << dimensions[0] << "x" << dimensions[1] << "x" << dimensions[2]);
    LOG_INFO("  Total slices: " << dimensions[2]);
    
    return true;
}

std::string WebRTCServer::create_session(const SessionConfig& session_config) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    std::string session_id;
    if (session_config.session_id.empty()) {
        session_id = generate_session_id();
    } else {
        session_id = session_config.session_id;
    }
    
    // Check if session already exists
    if (sessions_.find(session_id) != sessions_.end()) {
        LOG_WARN("Session already exists: " << session_id);
        return session_id;
    }
    
    // Create new session
    SessionConfig config = session_config;
    config.session_id = session_id;
    
    auto session = std::make_unique<WebRTCSession>(session_id, peer_connection_factory_, config);
    
    if (!session->initialize(config_.ice_servers)) {
        LOG_ERROR("Failed to initialize session: " << session_id);
        return "";
    }
    
    // Set up callbacks
    session->on_offer_created = [this, session_id](const std::string& client_id, const std::string& sdp) {
        if (on_offer_created) {
            on_offer_created(session_id, client_id, sdp);
        }
    };
    
    session->on_answer_created = [this, session_id](const std::string& client_id, const std::string& sdp) {
        if (on_answer_created) {
            on_answer_created(session_id, client_id, sdp);
        }
    };
    
    session->on_ice_candidate = [this, session_id](const std::string& client_id, const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index) {
        if (on_ice_candidate) {
            on_ice_candidate(session_id, client_id, candidate, sdp_mid, sdp_mline_index);
        }
    };
    
    session->on_error = [this, session_id](const std::string& client_id, const std::string& error) {
        if (on_error) {
            on_error(session_id, client_id, error);
        }
    };
    
    sessions_[session_id] = std::move(session);
    
    LOG_INFO("Created session: " << session_id);
    return session_id;
}

bool WebRTCServer::join_session(const std::string& session_id, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        LOG_ERROR("Session not found: " << session_id);
        return false;
    }
    
    if (!it->second->add_client(client_id)) {
        LOG_ERROR("Failed to add client to session: " << client_id << " -> " << session_id);
        return false;
    }
    
    LOG_INFO("Client joined session: " << client_id << " -> " << session_id);
    return true;
}

void WebRTCServer::leave_session(const std::string& session_id, const std::string& client_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->remove_client(client_id);
        LOG_INFO("Client left session: " << client_id << " <- " << session_id);
        
        // Remove session if no clients remain
        if (it->second->get_clients().empty()) {
            sessions_.erase(it);
            LOG_INFO("Removed empty session: " << session_id);
        }
    }
}

void WebRTCServer::handle_offer(const std::string& session_id, const std::string& client_id, const std::string& sdp) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->create_answer(client_id, sdp);
    } else {
        LOG_ERROR("Session not found for offer: " << session_id);
    }
}

void WebRTCServer::handle_answer(const std::string& session_id, const std::string& client_id, const std::string& sdp) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->set_remote_description(client_id, sdp, "answer");
    } else {
        LOG_ERROR("Session not found for answer: " << session_id);
    }
}

void WebRTCServer::handle_ice_candidate(const std::string& session_id, const std::string& client_id,
                                       const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->add_ice_candidate(client_id, candidate, sdp_mid, sdp_mline_index);
    } else {
        LOG_ERROR("Session not found for ICE candidate: " << session_id);
    }
}

void WebRTCServer::handle_control_message(const ControlMessage& message) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(message.session_id);
    if (it == sessions_.end()) {
        LOG_ERROR("Session not found for control message: " << message.session_id);
        return;
    }
    
    auto config = it->second->get_config();
    bool config_changed = false;
    
    switch (message.type) {
        case ControlMessage::SLICE_ORIENTATION:
            if (message.parameters.find("orientation") != message.parameters.end()) {
                config.orientation = message.parameters.at("orientation");
                config_changed = true;
                LOG_INFO("Changed orientation to: " << config.orientation);
            }
            break;
            
        case ControlMessage::ANIMATION_SPEED:
            if (message.parameters.find("speed") != message.parameters.end()) {
                config.animation_speed = std::stof(message.parameters.at("speed"));
                config_changed = true;
                LOG_INFO("Changed animation speed to: " << config.animation_speed);
            }
            break;
            
        case ControlMessage::PAUSE_RESUME:
            if (message.parameters.find("paused") != message.parameters.end()) {
                config.paused = (message.parameters.at("paused") == "true");
                config_changed = true;
                LOG_INFO("Animation " << (config.paused ? "PAUSED" : "RESUMED"));
            }
            break;
            
        case ControlMessage::RESTART_ANIMATION:
            animation_start_time_ = std::chrono::steady_clock::now();
            config.current_slice = -1;  // Reset to animated mode
            config_changed = true;
            LOG_INFO("Animation restarted");
            break;
            
        case ControlMessage::QUALITY_LEVEL:
            if (message.parameters.find("quality") != message.parameters.end()) {
                config.quality = message.parameters.at("quality");
                config_changed = true;
                LOG_INFO("Changed quality to: " << config.quality);
            }
            break;
            
        default:
            LOG_WARN("Unknown control message type: " << static_cast<int>(message.type));
            break;
    }
    
    if (config_changed) {
        it->second->update_config(config);
    }
}

void WebRTCServer::render_loop() {
    LOG_INFO("Starting WebRTC render loop");
    
    auto next_frame_time = std::chrono::steady_clock::now();
    const auto frame_duration = std::chrono::microseconds(static_cast<int>(1000000.0f / config_.default_fps));
    
    while (running_) {
        auto frame_start = std::chrono::steady_clock::now();
        
        // Render frames for all active sessions
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& [session_id, session] : sessions_) {
                if (session->is_active()) {
                    render_session(session.get());
                }
            }
        }
        
        // Update statistics
        update_stats();
        
        // Frame timing
        next_frame_time += frame_duration;
        auto now = std::chrono::steady_clock::now();
        
        if (next_frame_time > now) {
            std::this_thread::sleep_until(next_frame_time);
        } else {
            // We're behind schedule, skip frame timing adjustment
            next_frame_time = now;
        }
        
        // Cleanup inactive sessions periodically
        static int cleanup_counter = 0;
        if (++cleanup_counter % 300 == 0) {  // Every 10 seconds at 30fps
            cleanup_inactive_sessions();
        }
    }
    
    LOG_INFO("WebRTC render loop ended");
}

void WebRTCServer::render_session(WebRTCSession* session) {
    if (!session || !session->is_active()) {
        return;
    }
    
    auto config = session->get_config();
    
    // Calculate animation time
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - animation_start_time_).count();
    float animation_time = elapsed * config.animation_speed;
    
    // Render VDS frame
    auto rgb_data = render_vds_frame(config, animation_time);
    
    if (!rgb_data.empty()) {
        // Encode frame using hardware encoder
        std::lock_guard<std::mutex> encoder_lock(encoder_mutex_);
        auto encoded_frame = hardware_encoder_->encode_frame(rgb_data);
        
        if (!encoded_frame.empty()) {
            session->send_frame(encoded_frame);
            
            // Update statistics
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.frames_encoded++;
            stats_.bytes_sent += encoded_frame.size();
        }
    }
}

std::vector<uint8_t> WebRTCServer::render_vds_frame(const SessionConfig& config, float animation_time) {
    if (!vds_manager_) {
        return {};
    }
    
    // Calculate slice position based on animation
    int slice_axis = 2;  // Default to Z axis for XZ orientation
    if (config.orientation == "XY") slice_axis = 2;
    else if (config.orientation == "XZ") slice_axis = 1;
    else if (config.orientation == "YZ") slice_axis = 0;
    
    int slice_index = config.current_slice;
    
    if (config.animate && !config.paused && slice_index < 0) {
        auto dimensions = vds_manager_->get_dimensions();
        float progress = fmod(animation_time / config.animation_duration, 1.0f);
        slice_index = static_cast<int>(progress * dimensions[slice_axis]);
        slice_index = std::clamp(slice_index, 0, dimensions[slice_axis] - 1);
    }
    
    // Set slice parameters
    vds_manager_->set_slice_orientation(config.orientation);
    vds_manager_->set_slice_params(slice_axis, slice_index);
    
    // Render frame
    return vds_manager_->render_frame(config.width, config.height);
}

std::string WebRTCServer::generate_session_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    for (int i = 0; i < 8; i++) {
        ss << std::hex << dis(gen);
    }
    return ss.str();
}

void WebRTCServer::cleanup_inactive_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if (!it->second->is_active()) {
            LOG_INFO("Removing inactive session: " << it->first);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void WebRTCServer::update_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - stats_start_time_).count();
    
    if (elapsed >= 1.0f) {  // Update every second
        stats_.active_sessions = sessions_.size();
        
        size_t total_clients = 0;
        for (const auto& [session_id, session] : sessions_) {
            total_clients += session->get_clients().size();
        }
        stats_.total_clients = total_clients;
        
        // Get hardware encoder stats
        if (hardware_encoder_) {
            auto encoder_stats = hardware_encoder_->get_stats();
            stats_.avg_encoding_time_ms = encoder_stats.avg_encode_time_ms;
            stats_.avg_frame_rate = 1000.0f / encoder_stats.avg_encode_time_ms;
        }
        
        stats_start_time_ = now;
    }
}

WebRTCServer::Stats WebRTCServer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void WebRTCServer::cleanup() {
    hardware_encoder_.reset();
    vds_manager_.reset();
    peer_connection_factory_ = nullptr;
}

} // namespace server
} // namespace blustream