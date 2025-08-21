#pragma once

#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <string>
#include <functional>
#include <queue>

#include "blustream/common/types.h"
#include "blustream/server/hardware_encoder.h"

// Forward declarations for libwebrtc
namespace webrtc {
    class PeerConnectionFactoryInterface;
    class PeerConnectionInterface;
    class DataChannelInterface;
    class VideoTrackInterface;
    class VideoTrackSourceInterface;
    class MediaStreamInterface;
    class SessionDescriptionInterface;
    class IceCandidateInterface;
    class CreateSessionDescriptionObserver;
    class SetSessionDescriptionObserver;
    class PeerConnectionObserver;
    class VideoFrameBuffer;
    struct VideoFrame;
}

namespace blustream {
namespace server {

// Forward declarations
class VDSManager;
class WebRTCSession;
class WebRTCSignalingServer;

/**
 * @brief WebRTC-based streaming server for Phase 5
 * 
 * Provides ultra-low latency streaming of seismic data to browsers
 * using WebRTC with hardware-accelerated encoding integration.
 * 
 * Features:
 * - Real-time WebRTC streaming (<150ms latency)
 * - Hardware encoding integration (NVENC/QuickSync)
 * - Interactive VDS navigation controls
 * - Multi-client session management
 * - Adaptive quality based on network conditions
 */
class WebRTCServer {
public:
    struct Config {
        // Network
        int signaling_port = 3000;     // For signaling server integration
        int max_sessions = 10;         // Maximum concurrent sessions
        
        // Rendering
        int default_width = 1920;
        int default_height = 1080;
        float default_fps = 30.0f;
        
        // WebRTC
        std::vector<std::string> ice_servers = {
            "stun:stun.l.google.com:19302",
            "stun:stun1.l.google.com:19302"
        };
        
        // Hardware encoding
        HardwareEncoder::Type encoder_type = HardwareEncoder::Type::AUTO_DETECT;
        HardwareEncoder::Quality encoder_quality = HardwareEncoder::Quality::FAST;
        
        // VDS
        std::string vds_path;
        std::string default_orientation = "XZ";
        bool enable_animation = true;
        float animation_duration = 30.0f;
        
        // Quality adaptation
        bool enable_adaptive_quality = true;
        int min_bitrate_kbps = 1000;
        int max_bitrate_kbps = 15000;
        int target_latency_ms = 150;
    };
    
    /**
     * @brief Session configuration for individual clients
     */
    struct SessionConfig {
        std::string session_id;
        int width = 1920;
        int height = 1080;
        float fps = 30.0f;
        int bitrate_kbps = 5000;
        std::string quality = "auto";
        
        // VDS configuration
        std::string orientation = "XZ";
        bool animate = true;
        float animation_speed = 1.0f;
        float animation_duration = 30.0f;
        
        // Real-time controls
        bool paused = false;
        int current_slice = -1;  // -1 for animated mode
    };
    
    /**
     * @brief Control message from browser client
     */
    struct ControlMessage {
        enum Type {
            SLICE_ORIENTATION,
            ANIMATION_SPEED,
            ANIMATION_DURATION,
            PAUSE_RESUME,
            RESTART_ANIMATION,
            QUALITY_LEVEL,
            FRAME_RATE
        };
        
        Type type;
        std::string session_id;
        std::unordered_map<std::string, std::string> parameters;
    };
    
    WebRTCServer();
    ~WebRTCServer();
    
    // Server lifecycle
    bool initialize(const Config& config);
    bool start();
    void stop();
    bool is_running() const { return running_; }
    
    // VDS management
    bool load_vds(const std::string& path);
    
    // Session management
    std::string create_session(const SessionConfig& session_config = {});
    bool join_session(const std::string& session_id, const std::string& client_id);
    void leave_session(const std::string& session_id, const std::string& client_id);
    bool remove_session(const std::string& session_id);
    
    // WebRTC signaling
    void handle_offer(const std::string& session_id, const std::string& client_id, 
                     const std::string& sdp);
    void handle_answer(const std::string& session_id, const std::string& client_id,
                      const std::string& sdp);
    void handle_ice_candidate(const std::string& session_id, const std::string& client_id,
                             const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index);
    
    // Control message handling
    void handle_control_message(const ControlMessage& message);
    
    // Statistics
    struct Stats {
        size_t active_sessions;
        size_t total_clients;
        float avg_encoding_time_ms;
        float avg_frame_rate;
        size_t frames_encoded;
        size_t bytes_sent;
        float avg_latency_ms;
        std::unordered_map<std::string, float> session_stats;
    };
    Stats get_stats() const;
    
    // Callbacks for signaling server integration
    std::function<void(const std::string& session_id, const std::string& client_id, const std::string& sdp)> on_offer_created;
    std::function<void(const std::string& session_id, const std::string& client_id, const std::string& sdp)> on_answer_created;
    std::function<void(const std::string& session_id, const std::string& client_id, const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index)> on_ice_candidate;
    std::function<void(const std::string& session_id, const std::string& client_id, const std::string& error)> on_error;

private:
    // Configuration
    Config config_;
    
    // WebRTC factory and components
    std::shared_ptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    
    // VDS Manager
    std::unique_ptr<VDSManager> vds_manager_;
    
    // Session management
    std::unordered_map<std::string, std::unique_ptr<WebRTCSession>> sessions_;
    mutable std::mutex sessions_mutex_;
    
    // Hardware encoder
    std::unique_ptr<HardwareEncoder> hardware_encoder_;
    std::mutex encoder_mutex_;
    
    // Rendering loop
    std::atomic<bool> running_;
    std::thread render_thread_;
    std::chrono::steady_clock::time_point animation_start_time_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    Stats stats_;
    std::chrono::steady_clock::time_point stats_start_time_;
    
    // Initialization helpers
    bool initialize_webrtc();
    bool initialize_hardware_encoder();
    void cleanup();
    
    // Rendering pipeline
    void render_loop();
    void render_session(WebRTCSession* session);
    std::vector<uint8_t> render_vds_frame(const SessionConfig& config, float animation_time);
    
    // Session helpers
    std::string generate_session_id();
    void cleanup_inactive_sessions();
    
    // Statistics helpers
    void update_stats();
};

/**
 * @brief Individual WebRTC session for a browser client
 */
class WebRTCSession {
public:
    WebRTCSession(const std::string& session_id, 
                  std::shared_ptr<webrtc::PeerConnectionFactoryInterface> factory,
                  const WebRTCServer::SessionConfig& config);
    ~WebRTCSession();
    
    // Session management
    bool initialize(const std::vector<std::string>& ice_servers);
    void close();
    bool is_active() const { return active_; }
    
    // Client management
    bool add_client(const std::string& client_id);
    void remove_client(const std::string& client_id);
    std::vector<std::string> get_clients() const;
    
    // WebRTC signaling
    void create_offer(const std::string& client_id);
    void create_answer(const std::string& client_id, const std::string& offer_sdp);
    void set_remote_description(const std::string& client_id, const std::string& sdp, const std::string& type);
    void add_ice_candidate(const std::string& client_id, const std::string& candidate, 
                          const std::string& sdp_mid, int sdp_mline_index);
    
    // Frame streaming
    void send_frame(const std::vector<uint8_t>& encoded_frame);
    void send_frame_to_client(const std::string& client_id, const std::vector<uint8_t>& encoded_frame);
    
    // Configuration
    void update_config(const WebRTCServer::SessionConfig& new_config);
    WebRTCServer::SessionConfig get_config() const { return config_; }
    
    // Statistics
    struct SessionStats {
        float frame_rate;
        float encoding_time_ms;
        size_t frames_sent;
        size_t bytes_sent;
        float avg_latency_ms;
        std::unordered_map<std::string, float> client_latencies;
    };
    SessionStats get_stats() const;
    
    // Callbacks
    std::function<void(const std::string& client_id, const std::string& sdp)> on_offer_created;
    std::function<void(const std::string& client_id, const std::string& sdp)> on_answer_created;
    std::function<void(const std::string& client_id, const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index)> on_ice_candidate;
    std::function<void(const std::string& client_id, const std::string& error)> on_error;

private:
    std::string session_id_;
    WebRTCServer::SessionConfig config_;
    std::atomic<bool> active_;
    
    // WebRTC components
    std::shared_ptr<webrtc::PeerConnectionFactoryInterface> factory_;
    std::unordered_map<std::string, std::shared_ptr<webrtc::PeerConnectionInterface>> peer_connections_;
    std::shared_ptr<webrtc::VideoTrackSourceInterface> video_source_;
    std::shared_ptr<webrtc::VideoTrackInterface> video_track_;
    std::shared_ptr<webrtc::MediaStreamInterface> media_stream_;
    
    // Client management
    std::vector<std::string> clients_;
    mutable std::mutex clients_mutex_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    SessionStats stats_;
    
    // Helper classes
    class SessionPeerConnectionObserver;
    class SessionCreateSDPObserver;
    class SessionSetSDPObserver;
    class CustomVideoSource;
    
    // Internal helpers
    bool create_media_stream();
    void cleanup_peer_connection(const std::string& client_id);
    void update_session_stats();
};

} // namespace server
} // namespace blustream