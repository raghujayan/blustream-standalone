#include "blustream/server/webrtc_server.h"
#include "blustream/common/logger.h"

// WebRTC includes
#include "api/peer_connection_interface.h"
#include "api/jsep.h"
#include "api/rtc_error.h"
#include "api/video/video_frame.h"
#include "rtc_base/ref_counted_object.h"

namespace blustream {
namespace server {

// Observer classes for WebRTC callbacks
class WebRTCSession::SessionPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    SessionPeerConnectionObserver(WebRTCSession* session, const std::string& client_id)
        : session_(session), client_id_(client_id) {}

    // PeerConnectionObserver interface
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
        LOG_INFO("Signaling state changed for " << client_id_ << ": " << static_cast<int>(new_state));
    }

    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
        LOG_INFO("Data channel created for " << client_id_);
    }

    void OnRenegotiationNeeded() override {
        LOG_INFO("Renegotiation needed for " << client_id_);
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
        LOG_INFO("ICE connection state changed for " << client_id_ << ": " << static_cast<int>(new_state));
        
        if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed ||
            new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected) {
            // Handle connection failure
            if (session_->on_error) {
                session_->on_error(client_id_, "ICE connection failed");
            }
        }
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
        LOG_INFO("ICE gathering state changed for " << client_id_ << ": " << static_cast<int>(new_state));
    }

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string candidate_str;
        candidate->ToString(&candidate_str);
        
        LOG_INFO("ICE candidate for " << client_id_ << ": " << candidate_str);
        
        if (session_->on_ice_candidate) {
            session_->on_ice_candidate(client_id_, candidate_str, 
                                     candidate->sdp_mid(), candidate->sdp_mline_index());
        }
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
        LOG_INFO("Peer connection state changed for " << client_id_ << ": " << static_cast<int>(new_state));
    }

private:
    WebRTCSession* session_;
    std::string client_id_;
};

class WebRTCSession::SessionCreateSDPObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    SessionCreateSDPObserver(WebRTCSession* session, const std::string& client_id, bool is_offer)
        : session_(session), client_id_(client_id), is_offer_(is_offer) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::string sdp;
        desc->ToString(&sdp);
        
        // Set local description
        auto peer_conn = session_->get_peer_connection(client_id_);
        if (peer_conn) {
            peer_conn->SetLocalDescription(
                rtc::scoped_refptr<WebRTCSession::SessionSetSDPObserver>(
                    new rtc::RefCountedObject<WebRTCSession::SessionSetSDPObserver>(session_, client_id_)),
                desc);
        }
        
        // Send SDP to client
        if (is_offer_ && session_->on_offer_created) {
            session_->on_offer_created(client_id_, sdp);
        } else if (!is_offer_ && session_->on_answer_created) {
            session_->on_answer_created(client_id_, sdp);
        }
    }

    void OnFailure(webrtc::RTCError error) override {
        LOG_ERROR("Failed to create SDP for " << client_id_ << ": " << error.message());
        if (session_->on_error) {
            session_->on_error(client_id_, std::string("SDP creation failed: ") + error.message());
        }
    }

private:
    WebRTCSession* session_;
    std::string client_id_;
    bool is_offer_;
};

class WebRTCSession::SessionSetSDPObserver : public webrtc::SetSessionDescriptionObserver {
public:
    SessionSetSDPObserver(WebRTCSession* session, const std::string& client_id)
        : session_(session), client_id_(client_id) {}

    void OnSuccess() override {
        LOG_INFO("SDP set successfully for " << client_id_);
    }

    void OnFailure(webrtc::RTCError error) override {
        LOG_ERROR("Failed to set SDP for " << client_id_ << ": " << error.message());
        if (session_->on_error) {
            session_->on_error(client_id_, std::string("SDP set failed: ") + error.message());
        }
    }

private:
    WebRTCSession* session_;
    std::string client_id_;
};

// WebRTCSession Implementation
WebRTCSession::WebRTCSession(const std::string& session_id,
                           std::shared_ptr<webrtc::PeerConnectionFactoryInterface> factory,
                           const WebRTCServer::SessionConfig& config)
    : session_id_(session_id)
    , config_(config)
    , active_(false)
    , factory_(factory) {
}

WebRTCSession::~WebRTCSession() {
    close();
}

bool WebRTCSession::initialize(const std::vector<std::string>& ice_servers) {
    if (!factory_) {
        LOG_ERROR("Peer connection factory not available");
        return false;
    }
    
    // Create media stream
    if (!create_media_stream()) {
        LOG_ERROR("Failed to create media stream for session: " << session_id_);
        return false;
    }
    
    active_ = true;
    LOG_INFO("WebRTC session initialized: " << session_id_);
    return true;
}

void WebRTCSession::close() {
    if (!active_) {
        return;
    }
    
    active_ = false;
    
    // Close all peer connections
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [client_id, peer_connection] : peer_connections_) {
        if (peer_connection) {
            peer_connection->Close();
        }
    }
    peer_connections_.clear();
    clients_.clear();
    
    // Clean up media stream
    media_stream_ = nullptr;
    video_track_ = nullptr;
    video_source_ = nullptr;
    
    LOG_INFO("WebRTC session closed: " << session_id_);
}

bool WebRTCSession::add_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // Check if client already exists
    if (std::find(clients_.begin(), clients_.end(), client_id) != clients_.end()) {
        LOG_WARN("Client already in session: " << client_id);
        return true;
    }
    
    // Create peer connection configuration
    webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
    
    // Add ICE servers (STUN servers)
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:stun.l.google.com:19302";
    rtc_config.servers.push_back(ice_server);
    
    ice_server.uri = "stun:stun1.l.google.com:19302";
    rtc_config.servers.push_back(ice_server);
    
    // Create peer connection observer
    auto observer = std::make_unique<SessionPeerConnectionObserver>(this, client_id);
    
    // Create peer connection
    auto peer_connection = factory_->CreatePeerConnection(rtc_config, nullptr, nullptr, observer.get());
    
    if (!peer_connection) {
        LOG_ERROR("Failed to create peer connection for client: " << client_id);
        return false;
    }
    
    // Add media stream to peer connection
    if (media_stream_) {
        auto result = peer_connection->AddStream(media_stream_);
        if (!result) {
            LOG_ERROR("Failed to add media stream to peer connection");
            return false;
        }
    }
    
    // Store peer connection
    peer_connections_[client_id] = peer_connection;
    clients_.push_back(client_id);
    
    LOG_INFO("Client added to session: " << client_id << " -> " << session_id_);
    return true;
}

void WebRTCSession::remove_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // Remove from clients list
    auto it = std::find(clients_.begin(), clients_.end(), client_id);
    if (it != clients_.end()) {
        clients_.erase(it);
    }
    
    // Clean up peer connection
    cleanup_peer_connection(client_id);
    
    LOG_INFO("Client removed from session: " << client_id << " <- " << session_id_);
}

std::vector<std::string> WebRTCSession::get_clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return clients_;
}

void WebRTCSession::create_offer(const std::string& client_id) {
    auto peer_connection = get_peer_connection(client_id);
    if (!peer_connection) {
        LOG_ERROR("Peer connection not found for client: " << client_id);
        return;
    }
    
    // Create offer with video only
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    options.offer_to_receive_video = false;  // We're sending video, not receiving
    options.offer_to_receive_audio = false;
    
    auto observer = rtc::scoped_refptr<SessionCreateSDPObserver>(
        new rtc::RefCountedObject<SessionCreateSDPObserver>(this, client_id, true));
    
    peer_connection->CreateOffer(observer, options);
    LOG_INFO("Creating offer for client: " << client_id);
}

void WebRTCSession::create_answer(const std::string& client_id, const std::string& offer_sdp) {
    auto peer_connection = get_peer_connection(client_id);
    if (!peer_connection) {
        LOG_ERROR("Peer connection not found for client: " << client_id);
        return;
    }
    
    // Set remote description (offer)
    webrtc::SdpParseError error;
    auto offer = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer_sdp, &error);
    
    if (!offer) {
        LOG_ERROR("Failed to parse offer SDP: " << error.description);
        if (on_error) {
            on_error(client_id, "Invalid offer SDP");
        }
        return;
    }
    
    auto set_observer = rtc::scoped_refptr<SessionSetSDPObserver>(
        new rtc::RefCountedObject<SessionSetSDPObserver>(this, client_id));
    
    peer_connection->SetRemoteDescription(set_observer, offer.release());
    
    // Create answer
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    
    auto create_observer = rtc::scoped_refptr<SessionCreateSDPObserver>(
        new rtc::RefCountedObject<SessionCreateSDPObserver>(this, client_id, false));
    
    peer_connection->CreateAnswer(create_observer, options);
    LOG_INFO("Creating answer for client: " << client_id);
}

void WebRTCSession::set_remote_description(const std::string& client_id, const std::string& sdp, const std::string& type) {
    auto peer_connection = get_peer_connection(client_id);
    if (!peer_connection) {
        LOG_ERROR("Peer connection not found for client: " << client_id);
        return;
    }
    
    webrtc::SdpParseError error;
    webrtc::SdpType sdp_type = (type == "offer") ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer;
    auto session_desc = webrtc::CreateSessionDescription(sdp_type, sdp, &error);
    
    if (!session_desc) {
        LOG_ERROR("Failed to parse SDP: " << error.description);
        if (on_error) {
            on_error(client_id, "Invalid SDP");
        }
        return;
    }
    
    auto observer = rtc::scoped_refptr<SessionSetSDPObserver>(
        new rtc::RefCountedObject<SessionSetSDPObserver>(this, client_id));
    
    peer_connection->SetRemoteDescription(observer, session_desc.release());
    LOG_INFO("Set remote description for client: " << client_id);
}

void WebRTCSession::add_ice_candidate(const std::string& client_id, const std::string& candidate,
                                    const std::string& sdp_mid, int sdp_mline_index) {
    auto peer_connection = get_peer_connection(client_id);
    if (!peer_connection) {
        LOG_ERROR("Peer connection not found for client: " << client_id);
        return;
    }
    
    webrtc::SdpParseError error;
    auto ice_candidate = webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate, &error);
    
    if (!ice_candidate) {
        LOG_ERROR("Failed to parse ICE candidate: " << error.description);
        return;
    }
    
    if (!peer_connection->AddIceCandidate(ice_candidate.get())) {
        LOG_ERROR("Failed to add ICE candidate for client: " << client_id);
    } else {
        LOG_INFO("Added ICE candidate for client: " << client_id);
    }
}

void WebRTCSession::send_frame(const std::vector<uint8_t>& encoded_frame) {
    // For WebRTC, we need to send raw video frames, not encoded frames
    // The WebRTC library handles encoding internally
    // This would need to be adapted to send raw RGB data to the video source
    
    if (video_source_) {
        // Extract dimensions from config
        int width = config_.width;
        int height = config_.height;
        
        // Assuming the encoded_frame is actually RGB data for this implementation
        // In a real implementation, you'd need to decode the frame first
        // or modify the pipeline to provide raw frames
        
        // For now, we'll log the frame send
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_sent++;
        stats_.bytes_sent += encoded_frame.size();
    }
}

void WebRTCSession::send_frame_to_client(const std::string& client_id, const std::vector<uint8_t>& encoded_frame) {
    // Similar to send_frame but for a specific client
    send_frame(encoded_frame);
}

void WebRTCSession::update_config(const WebRTCServer::SessionConfig& new_config) {
    config_ = new_config;
    LOG_INFO("Updated configuration for session: " << session_id_);
}

bool WebRTCSession::create_media_stream() {
    // Create custom video source
    video_source_ = rtc::scoped_refptr<CustomVideoSource>(new rtc::RefCountedObject<CustomVideoSource>());
    
    if (!video_source_) {
        LOG_ERROR("Failed to create video source");
        return false;
    }
    
    // Create video track
    video_track_ = factory_->CreateVideoTrack("video_track", video_source_);
    
    if (!video_track_) {
        LOG_ERROR("Failed to create video track");
        return false;
    }
    
    // Create media stream
    media_stream_ = factory_->CreateLocalMediaStream("media_stream");
    
    if (!media_stream_) {
        LOG_ERROR("Failed to create media stream");
        return false;
    }
    
    // Add video track to stream
    if (!media_stream_->AddTrack(video_track_)) {
        LOG_ERROR("Failed to add video track to media stream");
        return false;
    }
    
    LOG_INFO("Media stream created successfully for session: " << session_id_);
    return true;
}

void WebRTCSession::cleanup_peer_connection(const std::string& client_id) {
    auto it = peer_connections_.find(client_id);
    if (it != peer_connections_.end()) {
        if (it->second) {
            it->second->Close();
        }
        peer_connections_.erase(it);
    }
}

std::shared_ptr<webrtc::PeerConnectionInterface> WebRTCSession::get_peer_connection(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = peer_connections_.find(client_id);
    return (it != peer_connections_.end()) ? it->second : nullptr;
}

void WebRTCSession::update_session_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    // Calculate frame rate
    static auto last_update = std::chrono::steady_clock::now();
    static size_t last_frame_count = stats_.frames_sent;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - last_update).count();
    
    if (elapsed >= 1.0f) {
        size_t frames_delta = stats_.frames_sent - last_frame_count;
        stats_.frame_rate = frames_delta / elapsed;
        
        last_update = now;
        last_frame_count = stats_.frames_sent;
    }
}

WebRTCSession::SessionStats WebRTCSession::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

} // namespace server
} // namespace blustream