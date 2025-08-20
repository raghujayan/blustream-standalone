// StreamingServer implementation with FFmpeg encoding
#include "blustream/server/streaming_server.h"
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

// FFmpeg headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// For VDS rendering
#include "blustream/server/opengl_context.h"

namespace blustream {
namespace server {

// Helper to manage AVCodecContext cleanup
static void cleanup_codec_context(AVCodecContext* ctx) {
    if (ctx) {
        avcodec_free_context(&ctx);
    }
}

// Helper to manage AVFrame cleanup
static void cleanup_frame(AVFrame* frame) {
    if (frame) {
        av_frame_free(&frame);
    }
}

// Helper to manage AVPacket cleanup
static void cleanup_packet(AVPacket* pkt) {
    if (pkt) {
        av_packet_free(&pkt);
    }
}

StreamingServer::StreamingServer()
    : encoder_context_(nullptr, cleanup_codec_context)
    , av_frame_(nullptr, cleanup_frame)
    , av_packet_(nullptr, cleanup_packet)
    
    , vds_manager_(std::make_unique<VDSManager>())
    , running_(false)
    , current_slice_axis_(2)
    , current_slice_index_(32) {
    
    // Initialize stats
    memset(&stats_, 0, sizeof(stats_));
    stats_start_time_ = std::chrono::steady_clock::now();
}

StreamingServer::~StreamingServer() {
    if (vds_manager_) {
        vds_manager_->shutdown();
    }
    stop();
    cleanup_encoder();
}

bool StreamingServer::initialize(const Config& config) {
    config_ = config;
    
    BLUSTREAM_LOG_INFO("Initializing streaming server...");
    BLUSTREAM_LOG_INFO("  Port: " + std::to_string(config_.port));
    BLUSTREAM_LOG_INFO("  Resolution: " + std::to_string(config_.render_width) + "x" + 
                      std::to_string(config_.render_height));
    BLUSTREAM_LOG_INFO("  Target FPS: " + std::to_string(config_.target_fps));
    BLUSTREAM_LOG_INFO("  Encoder: " + config_.encoder);
    BLUSTREAM_LOG_INFO("  Bitrate: " + std::to_string(config_.bitrate_kbps) + " kbps");
    
    // Calculate frame duration
    frame_duration_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / config_.target_fps));
    
    // Initialize OpenGL context for rendering
    gl_context_ = std::make_unique<OpenGLContext>();
    OpenGLContext::ContextConfig gl_config;
    gl_config.width = config_.render_width;
    gl_config.height = config_.render_height;
    
    if (!gl_context_->create_context(gl_config)) {
        BLUSTREAM_LOG_ERROR("Failed to create OpenGL context");
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ OpenGL context created");
    
    // Initialize network server
    network_server_ = std::make_unique<NetworkServer>();
    if (!network_server_->start(config_.port)) {
        BLUSTREAM_LOG_ERROR("Failed to start network server on port " + 
                           std::to_string(config_.port));
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ Network server started on port " + std::to_string(config_.port));
    
    // Initialize encoder
    if (!initialize_encoder()) {
        BLUSTREAM_LOG_ERROR("Failed to initialize encoder");
        return false;
    }
    
    BLUSTREAM_LOG_INFO("✓ Encoder initialized");
    
    // Load VDS if specified
    // Initialize VDS manager
    if (!vds_manager_->initialize()) {
        BLUSTREAM_LOG_ERROR("Failed to initialize VDS manager");
        return false;
    }
    if (!config_.vds_path.empty()) {
        if (!load_vds(config_.vds_path)) {
            BLUSTREAM_LOG_WARN("Failed to load VDS: " + config_.vds_path);
        }
    }
    
    BLUSTREAM_LOG_INFO("✓ Streaming server initialized");
    return true;
}

bool StreamingServer::initialize_encoder() {
    // Note: avcodec_register_all() is deprecated in newer FFmpeg versions
    
    // Find H.264 encoder
    const AVCodec* codec = nullptr;
    if (config_.encoder == "x264") {
        codec = avcodec_find_encoder_by_name("libx264");
    } else {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    
    if (!codec) {
        BLUSTREAM_LOG_ERROR("H.264 encoder not found");
        return false;
    }
    
    // Allocate codec context
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        BLUSTREAM_LOG_ERROR("Failed to allocate codec context");
        return false;
    }
    encoder_context_.reset(ctx);
    
    // Configure encoder
    ctx->bit_rate = config_.bitrate_kbps * 1000;
    ctx->width = config_.render_width;
    ctx->height = config_.render_height;
    ctx->time_base = AVRational{1, static_cast<int>(config_.target_fps)};
    ctx->framerate = AVRational{static_cast<int>(config_.target_fps), 1};
    ctx->gop_size = config_.keyframe_interval;
    ctx->max_b_frames = 0;  // No B-frames for low latency
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    // CRITICAL: Force Annex B format with parameter sets
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    // Set x264 options for proper parameter set generation
    if (config_.encoder == "x264") {
        av_opt_set(ctx->priv_data, "preset", config_.preset.c_str(), 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "x264opts", "no-scenecut", 0);
        // Force parameter sets in stream
        av_opt_set(ctx->priv_data, "annex_b", "1", 0);
        av_opt_set(ctx->priv_data, "repeat-headers", "1", 0);
    }
    
    // Open codec
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to open codec");
        return false;
    }
    
    // Allocate frame
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        BLUSTREAM_LOG_ERROR("Failed to allocate frame");
        return false;
    }
    av_frame_.reset(frame);
    
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    
    if (av_frame_get_buffer(frame, 0) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to allocate frame buffer");
        return false;
    }
    
    // Allocate packet
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        BLUSTREAM_LOG_ERROR("Failed to allocate packet");
        return false;
    }
    av_packet_.reset(pkt);
    
    BLUSTREAM_LOG_INFO("Encoder initialized: " + std::string(codec->name) + 
                      " @ " + std::to_string(config_.bitrate_kbps) + " kbps");
    
    return true;
}

void StreamingServer::cleanup_encoder() {
    // Flush encoder
    if (encoder_context_) {
        avcodec_send_frame(encoder_context_.get(), nullptr);
    }
}

bool StreamingServer::start() {
    if (running_) {
        BLUSTREAM_LOG_WARN("Server already running");
        return true;
    }
    
    BLUSTREAM_LOG_INFO("Starting streaming server...");
    
    running_ = true;
    
    // Start accept thread
    accept_thread_ = std::thread(&StreamingServer::accept_clients_loop, this);
    
    // Start render thread
    render_thread_ = std::thread(&StreamingServer::render_loop, this);
    
    BLUSTREAM_LOG_INFO("✓ Streaming server started");
    return true;
}

void StreamingServer::stop() {
    if (!running_) {
        return;
    }
    
    BLUSTREAM_LOG_INFO("Stopping streaming server...");
    
    running_ = false;
    
    // Stop network server to unblock accept()
    if (network_server_) {
        network_server_->stop();
    }
    
    // Wake up any waiting threads
    frame_queue_cv_.notify_all();
    
    // Join threads
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    
    if (render_thread_.joinable()) {
        render_thread_.join();
    }
    
    // Disconnect all clients
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& client : clients_) {
            client->disconnect();
        }
        clients_.clear();
    }
    
    BLUSTREAM_LOG_INFO("✓ Streaming server stopped");
}

void StreamingServer::render_loop() {
    BLUSTREAM_LOG_INFO("Render loop started");
    
    next_frame_time_ = std::chrono::steady_clock::now();
    animation_start_time_ = std::chrono::steady_clock::now();
    
    // Create a test pattern if no VDS loaded
    std::vector<uint8_t> test_pattern(config_.render_width * config_.render_height * 3);
    
    // Slice cycling for comprehensive capture
    static int frame_count = 0;
    static int slice_change_interval = 5; // Change slice every 5 frames
    
    while (running_) {
        auto render_start = std::chrono::steady_clock::now();
        
        // Make OpenGL context current
        if (!gl_context_->make_current()) {
            BLUSTREAM_LOG_ERROR("Failed to make OpenGL context current");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Render frame (either VDS or test pattern)
        std::vector<uint8_t> rgb_frame;
        
        if (vds_manager_ && vds_manager_->has_vds()) {
            std::vector<uint8_t> slice_rgb;
            int slice_width, slice_height;
            
            if (config_.animate_slice) {
                // Time-based animated slice rendering for seismic wiggles
                auto current_time = std::chrono::steady_clock::now();
                float elapsed_seconds = std::chrono::duration<float>(current_time - animation_start_time_).count();
                
                // Get animated slice data
                slice_rgb = vds_manager_->get_animated_slice_rgb(config_.slice_orientation, elapsed_seconds, config_.animation_duration);
                vds_manager_->get_slice_dimensions(config_.slice_orientation, slice_width, slice_height);
                
                // Log progress occasionally for vertical sections (XZ)
                if (frame_count % 30 == 0 && config_.slice_orientation == "XZ") {
                    float progress = fmod(elapsed_seconds, config_.animation_duration) / config_.animation_duration * 100.0f;
                    BLUSTREAM_LOG_INFO("Vertical section animation: " + std::to_string(static_cast<int>(progress)) + "% through Y-axis");
                }
            } else {
                // Legacy static slice rendering
                slice_rgb = vds_manager_->get_slice_rgb(current_slice_axis_, current_slice_index_);
                
                // Get slice dimensions using legacy method
                switch (current_slice_axis_) {
                    case 0: // YZ plane
                        slice_width = vds_manager_->get_height();
                        slice_height = vds_manager_->get_depth();
                        break;
                    case 1: // XZ plane
                        slice_width = vds_manager_->get_width();
                        slice_height = vds_manager_->get_depth();
                        break;
                    case 2: // XY plane
                    default:
                        slice_width = vds_manager_->get_width();
                        slice_height = vds_manager_->get_height();
                        break;
                }
            }
            frame_count++;
            if (!slice_rgb.empty()) {
                
                // Scale to render size
                rgb_frame.resize(config_.render_width * config_.render_height * 3);
                
                // Simple nearest neighbor scaling
                for (int y = 0; y < config_.render_height; y++) {
                    for (int x = 0; x < config_.render_width; x++) {
                        int src_x = (x * slice_width) / config_.render_width;
                        int src_y = (y * slice_height) / config_.render_height;
                        int src_idx = (src_y * slice_width + src_x) * 3;
                        int dst_idx = (y * config_.render_width + x) * 3;
                        
                        if (src_idx + 2 < slice_rgb.size()) {
                            rgb_frame[dst_idx + 0] = slice_rgb[src_idx + 0];
                            rgb_frame[dst_idx + 1] = slice_rgb[src_idx + 1];
                            rgb_frame[dst_idx + 2] = slice_rgb[src_idx + 2];
                        }
                    }
                }
            } else {
                rgb_frame = test_pattern;  // Fallback
            }
        } else {
            // Generate test pattern
            static int frame_counter = 0;
            for (int y = 0; y < config_.render_height; y++) {
                for (int x = 0; x < config_.render_width; x++) {
                    int idx = (y * config_.render_width + x) * 3;
                    // Animated gradient
                    test_pattern[idx + 0] = (x + frame_counter) % 256;     // R
                    test_pattern[idx + 1] = (y + frame_counter / 2) % 256; // G
                    test_pattern[idx + 2] = (frame_counter) % 256;         // B
                }
            }
            frame_counter++;
            rgb_frame = test_pattern;
        }
        
        auto render_end = std::chrono::steady_clock::now();
        float render_ms = std::chrono::duration<float, std::milli>(render_end - render_start).count();
        
        // Encode and send frame
        auto encode_start = std::chrono::steady_clock::now();
        encode_and_send_frame(rgb_frame);
        auto encode_end = std::chrono::steady_clock::now();
        float encode_ms = std::chrono::duration<float, std::milli>(encode_end - encode_start).count();
        
        // Update statistics
        update_stats(render_ms, encode_ms, 0);
        
        // Frame rate control
        next_frame_time_ += frame_duration_;
        std::this_thread::sleep_until(next_frame_time_);
    }
    
    BLUSTREAM_LOG_INFO("Render loop stopped");
}

void StreamingServer::encode_and_send_frame(const std::vector<uint8_t>& rgb_data) {
    // Convert RGB to YUV420
    auto yuv_data = convert_rgb_to_yuv420(rgb_data);
    
    // Copy YUV data to frame
    AVFrame* frame = av_frame_.get();
    
    // Y plane
    size_t y_size = config_.render_width * config_.render_height;
    memcpy(frame->data[0], yuv_data.data(), y_size);
    
    // U plane
    size_t uv_size = y_size / 4;
    memcpy(frame->data[1], yuv_data.data() + y_size, uv_size);
    
    // V plane
    memcpy(frame->data[2], yuv_data.data() + y_size + uv_size, uv_size);
    
    // Set frame PTS
    static int64_t pts = 0;
    frame->pts = pts++;
    
    // Send frame to encoder
    if (avcodec_send_frame(encoder_context_.get(), frame) < 0) {
        BLUSTREAM_LOG_ERROR("Failed to send frame to encoder");
        return;
    }
    
    // Receive encoded packets
    AVPacket* pkt = av_packet_.get();
    while (avcodec_receive_packet(encoder_context_.get(), pkt) == 0) {
        // Check if keyframe
        bool is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        
        // Create encoded frame data with proper H.264 headers
        std::vector<uint8_t> encoded_data;
        
        // Prepend parameter sets to EVERY frame for maximum compatibility
        // This ensures each frame can be decoded independently
        {
            static bool parameter_sets_logged = false;
            
            if (encoder_context_->extradata_size > 0) {
                if (!parameter_sets_logged) {
                    BLUSTREAM_LOG_INFO("✓ Encoder extradata available (" + std::to_string(encoder_context_->extradata_size) + " bytes)");
                    parameter_sets_logged = true;
                    
                    // Log the extradata in hex for debugging
                    std::string hex_data = "";
                    for (int i = 0; i < std::min(32, encoder_context_->extradata_size); i++) {
                        char buf[4];
                        snprintf(buf, sizeof(buf), "%02x ", encoder_context_->extradata[i]);
                        hex_data += buf;
                    }
                    BLUSTREAM_LOG_INFO("  Extradata: " + hex_data + "...");
                }
                
                // Prepend extradata to ALL frames (keyframe and P-frames)
                if (encoder_context_->extradata_size >= 4 && 
                    encoder_context_->extradata[0] == 0x00 && 
                    encoder_context_->extradata[1] == 0x00 && 
                    encoder_context_->extradata[2] == 0x00 && 
                    encoder_context_->extradata[3] == 0x01) {
                    // Looks like Annex B format, prepend it to every frame
                    encoded_data.insert(encoded_data.end(), 
                                      encoder_context_->extradata, 
                                      encoder_context_->extradata + encoder_context_->extradata_size);
                    
                    if (!parameter_sets_logged) {
                        BLUSTREAM_LOG_INFO("  Will prepend Annex B extradata to ALL frames");
                    }
                }
            }
        }
        
        // Add the actual frame data
        encoded_data.insert(encoded_data.end(), pkt->data, pkt->data + pkt->size);
        
        // Log packet details for debugging
        static int packet_count = 0;
        if (packet_count < 5) { // Only log first 5 packets
            std::string packet_hex = "";
            for (int i = 0; i < std::min(16, pkt->size); i++) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02x ", pkt->data[i]);
                packet_hex += buf;
            }
            BLUSTREAM_LOG_INFO("Packet " + std::to_string(packet_count) + 
                              " (keyframe=" + std::to_string(is_keyframe) + 
                              ", size=" + std::to_string(pkt->size) + 
                              "): " + packet_hex + "...");
            packet_count++;
        }
        
        // Broadcast to all clients
        broadcast_frame(encoded_data, is_keyframe);
        
        // Update stats
        stats_.frames_encoded++;
        stats_.bytes_sent += pkt->size;
        
        av_packet_unref(pkt);
    }
}

std::vector<uint8_t> StreamingServer::convert_rgb_to_yuv420(const std::vector<uint8_t>& rgb_data) {
    int width = config_.render_width;
    int height = config_.render_height;
    size_t y_size = width * height;
    size_t uv_size = y_size / 4;
    
    std::vector<uint8_t> yuv_data(y_size + uv_size * 2);
    
    // Simple RGB to YUV420 conversion (not optimized)
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int rgb_idx = (y * width + x) * 3;
            uint8_t r = rgb_data[rgb_idx + 0];
            uint8_t g = rgb_data[rgb_idx + 1];
            uint8_t b = rgb_data[rgb_idx + 2];
            
            // Y component
            int y_idx = y * width + x;
            yuv_data[y_idx] = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            
            // U and V components (subsample 2x2)
            if (x % 2 == 0 && y % 2 == 0) {
                int uv_idx = (y / 2) * (width / 2) + (x / 2);
                yuv_data[y_size + uv_idx] = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                yuv_data[y_size + uv_size + uv_idx] = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            }
        }
    }
    
    return yuv_data;
}

void StreamingServer::accept_clients_loop() {
    BLUSTREAM_LOG_INFO("Accept clients loop started");
    
    while (running_) {
        // Accept new client
        std::string client_addr;
        int client_fd = network_server_->accept_client(client_addr);
        
        if (client_fd < 0) {
            if (running_) {
                BLUSTREAM_LOG_ERROR("Failed to accept client");
            }
            continue;
        }
        
        BLUSTREAM_LOG_INFO("New client connected from: " + client_addr);
        
        // Create client connection
        auto client = std::make_shared<ClientConnection>(client_fd, client_addr);
        
        // Add to client list
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(client);
        }
        
        // Start client thread
        std::thread client_thread(&StreamingServer::handle_client, this, client_fd);
        client_thread.detach();
    }
    
    BLUSTREAM_LOG_INFO("Accept clients loop stopped");
}

void StreamingServer::handle_client(int client_fd) {
    // Send initial configuration
    common::MessageHeader header;
    header.magic = 0x42535452;  // 'BSTR'
    header.version = 1;
    header.type = static_cast<uint32_t>(common::MessageType::CONFIG);
    header.payload_size = sizeof(common::StreamConfig);
    
    common::StreamConfig stream_config;
    stream_config.width = config_.render_width;
    stream_config.height = config_.render_height;
    stream_config.fps = config_.target_fps;
    stream_config.codec = common::VideoCodec::H264;
    stream_config.bitrate_kbps = config_.bitrate_kbps;
    
    send(client_fd, &header, sizeof(header), 0);
    send(client_fd, &stream_config, sizeof(stream_config), 0);
    
    // Client will be handled by broadcast_frame
}

void StreamingServer::broadcast_frame(const std::vector<uint8_t>& encoded_data, bool is_keyframe) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    // Remove disconnected clients
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [](const std::shared_ptr<ClientConnection>& client) {
                return !client->is_connected();
            }),
        clients_.end()
    );
    
    // Send to all connected clients
    for (auto& client : clients_) {
        client->send_frame(encoded_data);
    }
}

void StreamingServer::update_stats(float render_ms, float encode_ms, size_t bytes_sent) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.render_time_ms = render_ms;
    stats_.encoding_time_ms = encode_ms;
    stats_.frames_rendered++;
    
    // Calculate FPS
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration<float>(now - stats_start_time_).count();
    if (duration > 0) {
        stats_.current_fps = stats_.frames_rendered / duration;
        stats_.bitrate_mbps = (stats_.bytes_sent * 8.0f) / (duration * 1000000.0f);
    }
}

StreamingServer::Stats StreamingServer::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool StreamingServer::load_vds(const std::string& path) {
    if (!vds_manager_) {
        BLUSTREAM_LOG_ERROR("VDS manager not initialized");
        return false;
    }
    
    BLUSTREAM_LOG_INFO("Loading VDS: " + path);
    
    // Try to load from file first
    if (vds_manager_->load_from_file(path)) {
        BLUSTREAM_LOG_INFO("Successfully loaded VDS from file: " + path);
        return true;
    }
    
    // If file loading fails, create noise volume
    BLUSTREAM_LOG_WARN("Failed to load VDS from file, creating noise volume");
    if (vds_manager_->create_noise_volume(128, 128, 128, 0.05f)) {
        BLUSTREAM_LOG_INFO("Created noise volume as fallback");
        return true;
    }
    
    BLUSTREAM_LOG_ERROR("Failed to load VDS or create noise volume");
    return false;
}

void StreamingServer::set_slice_params(int axis, int index) {
    current_slice_axis_ = axis;
    current_slice_index_ = index;
    BLUSTREAM_LOG_INFO("Slice params set: axis=" + std::to_string(axis) + 
                      ", index=" + std::to_string(index));
}

size_t StreamingServer::get_client_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return clients_.size();
}

// ClientConnection implementation
ClientConnection::ClientConnection(int socket_fd, const std::string& address)
    : socket_fd_(socket_fd)
    , address_(address)
    , connected_(true)
    , bytes_sent_(0) {
    
    // Start send thread
    send_thread_ = std::thread(&ClientConnection::send_loop, this);
}

ClientConnection::~ClientConnection() {
    disconnect();
    
    if (send_thread_.joinable()) {
        send_thread_.join();
    }
}

bool ClientConnection::send_frame(const std::vector<uint8_t>& data) {
    if (!connected_) {
        return false;
    }
    
    // Add to send queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        send_queue_.push(data);
    }
    queue_cv_.notify_one();
    
    return true;
}

void ClientConnection::send_loop() {
    while (connected_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !send_queue_.empty() || !connected_; });
        
        if (!connected_) {
            break;
        }
        
        if (!send_queue_.empty()) {
            auto data = send_queue_.front();
            send_queue_.pop();
            lock.unlock();
            
            // Send frame header
            common::MessageHeader header;
            header.magic = 0x42535452;
            header.version = 1;
            header.type = static_cast<uint32_t>(common::MessageType::FRAME);
            header.payload_size = data.size();
            header.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            
            if (send(socket_fd_, &header, sizeof(header), MSG_NOSIGNAL) < 0) {
                connected_ = false;
                break;
            }
            
            // Send frame data
            if (send(socket_fd_, data.data(), data.size(), MSG_NOSIGNAL) < 0) {
                connected_ = false;
                break;
            }
            
            bytes_sent_ += sizeof(header) + data.size();
        }
    }
}

void ClientConnection::disconnect() {
    connected_ = false;
    queue_cv_.notify_one();
    
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

std::string ClientConnection::get_info() const {
    return address_ + " (sent: " + std::to_string(bytes_sent_ / 1024) + " KB)";
}

} // namespace server
} // namespace blustream