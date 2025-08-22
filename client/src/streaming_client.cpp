// Simple streaming client for testing Phase 4
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <cstdlib>  // for getenv

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "blustream/common/types.h"
#include "blustream/common/logger.h"
#include "blustream/common/debug_config.h"

// FFmpeg for decoding
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/cpu.h>
#include <libavutil/hwcontext.h>
}

namespace blustream {
namespace client {

class StreamingClient {
public:
    enum class HardwareDecodeMode {
        AUTO,    // Attempt HW, fallback to SW if unsupported
        OFF,     // Always use software (for debugging)
        FORCE    // Fail if HW init fails
    };

    struct Config {
        std::string server_ip = "127.0.0.1";
        int server_port = 8080;
        bool save_frames = false;
        std::string output_dir = "./frames";
        bool decode_frames = true;
        bool display_stats = true;
        HardwareDecodeMode hw_decode = HardwareDecodeMode::AUTO;
    };
    
    StreamingClient() 
        : socket_fd_(-1)
        , connected_(false)
        , decoder_context_(nullptr)
        , av_frame_(nullptr)
        , av_packet_(nullptr) {
        
        // Initialize stats
        stats_.frames_received = 0;
        stats_.frames_decoded = 0;
        stats_.bytes_received = 0;
        stats_.decode_errors = 0;
        stats_.avg_decode_time_ms = 0;
        stats_.hw_decode_frames = 0;
        stats_.sw_decode_frames = 0;
        stats_.hw_decode_active = false;
        stats_start_time_ = std::chrono::steady_clock::now();
    }
    
    ~StreamingClient() {
        disconnect();
        cleanup_decoder();
    }
    
    bool connect_to_server(const Config& config) {
        config_ = config;
        
        BLUSTREAM_LOG_INFO("Connecting to server: " + config.server_ip + ":" + 
                          std::to_string(config.server_port));
        
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            BLUSTREAM_LOG_ERROR("WSAStartup failed");
            return false;
        }
#endif
        
        // Create socket
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            BLUSTREAM_LOG_ERROR("Failed to create socket");
            return false;
        }
        
        // Set up server address
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(config.server_port);
        
        if (inet_pton(AF_INET, config.server_ip.c_str(), &server_addr.sin_addr) <= 0) {
            BLUSTREAM_LOG_ERROR("Invalid server IP address");
            close(socket_fd_);
            return false;
        }
        
        // Connect to server
        if (connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            BLUSTREAM_LOG_ERROR("Failed to connect to server");
            close(socket_fd_);
            return false;
        }
        
        // Set socket timeout
        struct timeval timeout;
        timeout.tv_sec = 5;  // 5 second timeout
        timeout.tv_usec = 0;
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            BLUSTREAM_LOG_WARN("Could not set socket timeout");
        }
        
        connected_ = true;
        BLUSTREAM_LOG_INFO("✓ Connected to server, connected=" + std::to_string(connected_.load()));
        
        // Receive initial configuration
        BLUSTREAM_LOG_INFO("Waiting for config header from server...");
        common::MessageHeader header;
        ssize_t bytes_received = recv(socket_fd_, &header, sizeof(header), MSG_WAITALL);
        if (bytes_received != sizeof(header)) {
            BLUSTREAM_LOG_ERROR("Failed to receive config header. Got " + std::to_string(bytes_received) + " bytes, expected " + std::to_string(sizeof(header)));
            if (bytes_received > 0) {
                BLUSTREAM_LOG_ERROR("Partial header data received - protocol mismatch or server disconnect");
            } else if (bytes_received == 0) {
                BLUSTREAM_LOG_ERROR("Server closed connection before sending config");
            } else {
                BLUSTREAM_LOG_ERROR("Socket error: " + std::string(strerror(errno)));
            }
            disconnect();
            return false;
        }
        
        BLUSTREAM_LOG_INFO("Received header: magic=0x" + std::to_string(header.magic) + ", type=" + std::to_string(header.type) + ", payload_size=" + std::to_string(header.payload_size));
        
        if (header.magic != 0x42535452 || 
            header.type != static_cast<uint32_t>(common::MessageType::CONFIG)) {
            BLUSTREAM_LOG_ERROR("Invalid config message. Got magic=0x" + std::to_string(header.magic) + 
                              ", expected=0x42535452, got type=" + std::to_string(header.type) + 
                              ", expected=" + std::to_string(static_cast<uint32_t>(common::MessageType::CONFIG)));
            disconnect();
            return false;
        }
        
        common::StreamConfig stream_config;
        if (recv(socket_fd_, &stream_config, sizeof(stream_config), MSG_WAITALL) != sizeof(stream_config)) {
            BLUSTREAM_LOG_ERROR("Failed to receive stream config");
            disconnect();
            return false;
        }
        
        BLUSTREAM_LOG_INFO("Stream configuration:");
        BLUSTREAM_LOG_INFO("  Resolution: " + std::to_string(stream_config.width) + "x" + 
                          std::to_string(stream_config.height));
        BLUSTREAM_LOG_INFO("  FPS: " + std::to_string(stream_config.fps));
        BLUSTREAM_LOG_INFO("  Bitrate: " + std::to_string(stream_config.bitrate_kbps) + " kbps");
        BLUSTREAM_LOG_INFO("  Codec: " + std::to_string(static_cast<uint32_t>(stream_config.codec)));
        
        stream_config_ = stream_config;
        
        // Initialize H.264 parameter sets (SPS + PPS from server)
        // These are extracted from the I-frame and needed for all subsequent frames
        initialize_parameter_sets();
        
        // Initialize decoder if requested
        if (config.decode_frames) {
            if (!initialize_decoder()) {
                BLUSTREAM_LOG_WARN("Failed to initialize decoder - will save raw H.264");
            }
        }
        
        return true;
    }
    
    void start_receiving() {
        BLUSTREAM_LOG_INFO("start_receiving() called, connected=" + std::to_string(connected_.load()));
        if (!connected_) {
            BLUSTREAM_LOG_ERROR("Not connected to server");
            return;
        }
        
        BLUSTREAM_LOG_INFO("Starting to receive frames...");
        
        receive_thread_ = std::thread(&StreamingClient::receive_loop, this);
        
        // Start stats display thread if requested
        if (config_.display_stats) {
            stats_thread_ = std::thread(&StreamingClient::stats_loop, this);
        }
    }
    
    void stop() {
        connected_ = false;
        
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        
        if (stats_thread_.joinable()) {
            stats_thread_.join();
        }
    }
    
    void disconnect() {
        connected_ = false;
        
        if (socket_fd_ >= 0) {
#ifdef _WIN32
            closesocket(socket_fd_);
            WSACleanup();
#else
            close(socket_fd_);
#endif
            socket_fd_ = -1;
        }
    }
    
private:
    Config config_;
    common::StreamConfig stream_config_;
    int socket_fd_;
    std::atomic<bool> connected_;
    
    // Threads
    std::thread receive_thread_;
    std::thread stats_thread_;
    
    // Decoder
    AVCodecContext* decoder_context_;
    AVFrame* av_frame_;
    AVPacket* av_packet_;
    
    // H.264 parameter sets for decoding
    std::vector<uint8_t> sps_pps_headers_;
    
    void initialize_parameter_sets() {
        // SPS (Sequence Parameter Set) + PPS (Picture Parameter Set) 
        // Extracted from the I-frame for Phase 4 server
        const uint8_t sps_pps[] = {
            // SPS NAL unit (28 bytes)
            0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28,
            0xac, 0xb6, 0x03, 0xc0, 0x11, 0x3f, 0x2c, 0x20,
            0x00, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x07,
            0x91, 0xe3, 0x06, 0x5c,
            // PPS NAL unit (12 bytes)  
            0x00, 0x00, 0x00, 0x01, 0x68, 0xea, 0xcc, 0xb2,
            0x2c, 0x00, 0x00, 0x01
        };
        
        sps_pps_headers_.assign(sps_pps, sps_pps + sizeof(sps_pps));
        BLUSTREAM_LOG_INFO("✓ H.264 parameter sets initialized (" + std::to_string(sps_pps_headers_.size()) + " bytes)");
    }
    
    // Statistics
    struct Stats {
        std::atomic<size_t> frames_received;
        std::atomic<size_t> frames_decoded;
        std::atomic<size_t> bytes_received;
        std::atomic<size_t> decode_errors;
        std::atomic<float> avg_decode_time_ms;
        std::atomic<size_t> hw_decode_frames;  // Frames decoded using hardware
        std::atomic<size_t> sw_decode_frames;  // Frames decoded using software
        std::atomic<bool> hw_decode_active;    // Whether HW decode is currently active
    } stats_;
    std::chrono::steady_clock::time_point stats_start_time_;
    
    bool initialize_decoder() {
        // avcodec_register_all(); // Deprecated in newer FFmpeg versions
        
        // Find H.264 decoder
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            BLUSTREAM_LOG_ERROR("H.264 decoder not found");
            return false;
        }
        
        // Allocate decoder context
        decoder_context_ = avcodec_alloc_context3(codec);
        if (!decoder_context_) {
            BLUSTREAM_LOG_ERROR("Failed to allocate decoder context");
            return false;
        }
        
        // Setup decoder options with enhanced hardware decode support
        AVDictionary* opts = nullptr;
        bool hw_attempted = false;
        bool hw_success = false;
        AVBufferRef* hw_device_ctx = nullptr;
        
        // 1. Enable frame threading (always beneficial)
        av_dict_set(&opts, "threads", "auto", 0);
        av_dict_set(&opts, "thread_type", "frame", 0);
        BLUSTREAM_LOG_INFO("[decode] Frame threading enabled: auto threads");
        
        // 2. Check environment variable for hardware decode override
        HardwareDecodeMode effective_hw_mode = config_.hw_decode;
        const char* env_hw_decode = getenv("HW_DECODE");
        if (env_hw_decode) {
            if (strcasecmp(env_hw_decode, "off") == 0) {
                effective_hw_mode = HardwareDecodeMode::OFF;
                BLUSTREAM_LOG_INFO("[decode] Environment variable HW_DECODE=off overrides config");
            } else if (strcasecmp(env_hw_decode, "force") == 0) {
                effective_hw_mode = HardwareDecodeMode::FORCE;
                BLUSTREAM_LOG_INFO("[decode] Environment variable HW_DECODE=force overrides config");
            } else if (strcasecmp(env_hw_decode, "auto") == 0) {
                effective_hw_mode = HardwareDecodeMode::AUTO;
                BLUSTREAM_LOG_INFO("[decode] Environment variable HW_DECODE=auto overrides config");
            } else {
                BLUSTREAM_LOG_WARN("[decode] Invalid HW_DECODE value: " + std::string(env_hw_decode) + " (using config default)");
            }
        }
        
        // 3. Hardware acceleration based on platform and effective config
        if (effective_hw_mode != HardwareDecodeMode::OFF) {
            hw_attempted = true;
            
#ifdef __APPLE__
            // macOS: Try VideoToolbox
            av_dict_set(&opts, "hwaccel", "videotoolbox", 0);
            av_dict_set(&opts, "hwaccel_output_format", "videotoolbox_vld", 0);
            BLUSTREAM_LOG_INFO("[decode] Attempting hardware acceleration: VideoToolbox");
#elif defined(_WIN32)
            // Windows: Try D3D11VA
            av_dict_set(&opts, "hwaccel", "d3d11va", 0);
            av_dict_set(&opts, "hwaccel_output_format", "d3d11", 0);
            BLUSTREAM_LOG_INFO("[decode] Attempting hardware acceleration: D3D11VA");
#elif defined(__linux__)
            // Linux: Enhanced VAAPI setup with device context
            const char* vaapi_device = getenv("VAAPI_DEVICE");
            if (!vaapi_device) {
                vaapi_device = "/dev/dri/renderD128";
            }
            
            int vaapi_ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, 
                                                   vaapi_device, nullptr, 0);
            if (vaapi_ret >= 0) {
                decoder_context_->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                av_dict_set(&opts, "hwaccel", "vaapi", 0);
                av_dict_set(&opts, "hwaccel_output_format", "vaapi", 0);
                BLUSTREAM_LOG_INFO("[decode] VAAPI device context created: " + std::string(vaapi_device));
            } else {
                BLUSTREAM_LOG_WARN("[decode] Failed to create VAAPI device context: " + std::string(vaapi_device));
                // Fallback to basic VAAPI without device context
                av_dict_set(&opts, "hwaccel", "vaapi", 0);
                av_dict_set(&opts, "hwaccel_output_format", "vaapi", 0);
            }
            BLUSTREAM_LOG_INFO("[decode] Attempting hardware acceleration: VAAPI");
#endif
        } else {
            BLUSTREAM_LOG_INFO("[decode] Hardware acceleration disabled by config");
        }
        
        // Try to open decoder with hardware acceleration
        int ret = avcodec_open2(decoder_context_, codec, &opts);
        
        if (ret < 0 && hw_attempted && effective_hw_mode == HardwareDecodeMode::AUTO) {
            // Hardware failed, try software fallback
            BLUSTREAM_LOG_WARN("[decode] Hardware acceleration failed, falling back to software");
            
            // Clean up and try again without hardware acceleration
            avcodec_free_context(&decoder_context_);
            decoder_context_ = avcodec_alloc_context3(codec);
            if (!decoder_context_) {
                BLUSTREAM_LOG_ERROR("Failed to allocate decoder context for fallback");
                av_dict_free(&opts);
                return false;
            }
            
            // Clear hardware options, keep threading
            av_dict_free(&opts);
            av_dict_set(&opts, "threads", "auto", 0);
            av_dict_set(&opts, "thread_type", "frame", 0);
            
            ret = avcodec_open2(decoder_context_, codec, &opts);
            hw_success = false;
        } else if (ret >= 0 && hw_attempted) {
            hw_success = true;
        }
        
        av_dict_free(&opts);
        
        if (ret < 0) {
            if (effective_hw_mode == HardwareDecodeMode::FORCE) {
                BLUSTREAM_LOG_ERROR("[decode] Hardware decode forced but failed to initialize");
            } else {
                BLUSTREAM_LOG_ERROR("[decode] Failed to open decoder (both HW and SW failed)");
            }
            avcodec_free_context(&decoder_context_);
            decoder_context_ = nullptr;
            return false;
        }
        
        // Log the chosen decode path
        stats_.hw_decode_active = hw_success;
        if (hw_success) {
#ifdef __APPLE__
            BLUSTREAM_LOG_INFO("[decode] ✓ Using hardware acceleration: VideoToolbox");
#elif defined(_WIN32)
            BLUSTREAM_LOG_INFO("[decode] ✓ Using hardware acceleration: D3D11VA");
#elif defined(__linux__)
            BLUSTREAM_LOG_INFO("[decode] ✓ Using hardware acceleration: VAAPI");
#endif
        } else {
            BLUSTREAM_LOG_INFO("[decode] ✓ Using software decode");
        }
        
        // Get thread count information
        int thread_count = decoder_context_->thread_count;
        if (thread_count <= 0) {
            thread_count = av_cpu_count();
        }
        BLUSTREAM_LOG_INFO("[decode] Threading: " + std::to_string(thread_count) + " threads");
        
        // Allocate frame
        av_frame_ = av_frame_alloc();
        if (!av_frame_) {
            BLUSTREAM_LOG_ERROR("Failed to allocate frame");
            cleanup_decoder();
            return false;
        }
        
        // Allocate packet
        av_packet_ = av_packet_alloc();
        if (!av_packet_) {
            BLUSTREAM_LOG_ERROR("Failed to allocate packet");
            cleanup_decoder();
            return false;
        }
        
        // Set initial hardware decode state based on success
        stats_.hw_decode_active = hw_success;
        
        // Cleanup VAAPI device context if we created one
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        
        std::string hw_status = hw_success ? "YES" : "NO";
        std::string mode_status = effective_hw_mode == HardwareDecodeMode::AUTO ? "AUTO" :
                                 effective_hw_mode == HardwareDecodeMode::FORCE ? "FORCE" : "OFF";
        BLUSTREAM_LOG_INFO("[decode] Decoder initialized successfully (HW: " + hw_status + ", Mode: " + mode_status + ")");
        return true;
    }
    
    void cleanup_decoder() {
        if (av_packet_) {
            av_packet_free(&av_packet_);
        }
        
        if (av_frame_) {
            av_frame_free(&av_frame_);
        }
        
        if (decoder_context_) {
            avcodec_free_context(&decoder_context_);
        }
    }
    
    void receive_loop() {
        std::vector<uint8_t> buffer(1024 * 1024);  // 1MB buffer
        
        BLUSTREAM_LOG_INFO("Receive loop starting, connected=" + std::to_string(connected_.load()));
        
        while (connected_) {
            BLUSTREAM_LOG_INFO("Waiting for next message header...");
            // Receive message header with timeout
            common::MessageHeader header;
            int bytes = recv(socket_fd_, &header, sizeof(header), MSG_WAITALL);
            
            if (bytes != sizeof(header)) {
                if (connected_) {
                    if (bytes == 0) {
                        BLUSTREAM_LOG_INFO("Server closed connection gracefully");
                    } else if (bytes < 0) {
                        BLUSTREAM_LOG_ERROR("Socket error: " + std::string(strerror(errno)));
                    } else {
                        BLUSTREAM_LOG_ERROR("Partial header received: " + std::to_string(bytes) + "/" + std::to_string(sizeof(header)) + " bytes");
                    }
                }
                break;
            }
            
            // Validate header
            if (header.magic != 0x42535452) {
                BLUSTREAM_LOG_ERROR("Invalid magic number");
                break;
            }
            
            if (header.type != static_cast<uint32_t>(common::MessageType::FRAME)) {
                BLUSTREAM_LOG_INFO("Received non-frame message, type=" + std::to_string(header.type) + ", waiting for frames...");
                continue;
            }
            
            BLUSTREAM_LOG_INFO("Received frame header: size=" + std::to_string(header.payload_size));
            
            // Receive frame data
            if (header.payload_size > buffer.size()) {
                buffer.resize(header.payload_size);
            }
            
            bytes = recv(socket_fd_, buffer.data(), header.payload_size, MSG_WAITALL);
            if (static_cast<uint32_t>(bytes) != header.payload_size) {
                BLUSTREAM_LOG_ERROR("Failed to receive frame data");
                break;
            }
            
            stats_.frames_received++;
            stats_.bytes_received += header.payload_size;
            
            // Process frame
            process_frame(buffer.data(), header.payload_size);
        }
        
        BLUSTREAM_LOG_INFO("Receive loop ended, connected=" + std::to_string(connected_.load()));
    }
    
    void process_frame(const uint8_t* data, size_t size) {
        // Save raw H.264 if requested AND debug I/O is enabled
        if (config_.save_frames) {
            if (BLUSTREAM_DEBUG_IO_ENABLED()) {
                BLUSTREAM_DEBUG_IO_PERMIT();
                static int frame_num = 0;
                std::string filename = config_.output_dir + "/frame_" + 
                                     std::to_string(frame_num++) + ".h264";
                std::ofstream file(filename, std::ios::binary);
                file.write(reinterpret_cast<const char*>(data), size);
            } else {
                BLUSTREAM_DEBUG_IO_BLOCK();
            }
        }
        
        // Decode if decoder is available
        if (decoder_context_ && av_packet_ && av_frame_) {
            auto decode_start = std::chrono::steady_clock::now();
            
            // Create frame data with SPS/PPS prepended for proper decoding
            std::vector<uint8_t> frame_with_headers;
            frame_with_headers.reserve(sps_pps_headers_.size() + size);
            frame_with_headers.insert(frame_with_headers.end(), sps_pps_headers_.begin(), sps_pps_headers_.end());
            frame_with_headers.insert(frame_with_headers.end(), data, data + size);
            
            av_packet_->data = frame_with_headers.data();
            av_packet_->size = frame_with_headers.size();
            
            // Send packet to decoder
            if (avcodec_send_packet(decoder_context_, av_packet_) < 0) {
                stats_.decode_errors++;
                return;
            }
            
            // Receive decoded frame
            while (avcodec_receive_frame(decoder_context_, av_frame_) == 0) {
                stats_.frames_decoded++;
                
                // Track hardware vs software decode statistics
                if (stats_.hw_decode_active) {
                    stats_.hw_decode_frames++;
                } else {
                    stats_.sw_decode_frames++;
                }
                
                // Process decoded frame (convert to RGB, display, etc.)
                process_decoded_frame(av_frame_);
            }
            
            auto decode_end = std::chrono::steady_clock::now();
            float decode_ms = std::chrono::duration<float, std::milli>(decode_end - decode_start).count();
            
            // Update average decode time
            float alpha = 0.1f;  // Exponential moving average factor
            stats_.avg_decode_time_ms = stats_.avg_decode_time_ms * (1 - alpha) + decode_ms * alpha;
        }
    }
    
    void process_decoded_frame(AVFrame* frame) {
        // Save decoded frame as PPM if requested AND debug I/O is enabled
        if (config_.save_frames) {
            if (BLUSTREAM_DEBUG_IO_ENABLED()) {
                BLUSTREAM_DEBUG_IO_PERMIT();
                static int decoded_frame_num = 0;
                
                // Convert YUV to RGB
                std::vector<uint8_t> rgb_data(frame->width * frame->height * 3);
                
                for (int y = 0; y < frame->height; y++) {
                    for (int x = 0; x < frame->width; x++) {
                        int y_idx = y * frame->linesize[0] + x;
                        int uv_idx = (y / 2) * frame->linesize[1] + (x / 2);
                        
                        uint8_t Y = frame->data[0][y_idx];
                        uint8_t U = frame->data[1][uv_idx];
                        uint8_t V = frame->data[2][uv_idx];
                        
                        // YUV to RGB conversion
                        int C = Y - 16;
                        int D = U - 128;
                        int E = V - 128;
                        
                        int R = (298 * C + 409 * E + 128) >> 8;
                        int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
                        int B = (298 * C + 516 * D + 128) >> 8;
                        
                        // Clamp values
                        R = std::max(0, std::min(255, R));
                        G = std::max(0, std::min(255, G));
                        B = std::max(0, std::min(255, B));
                        
                        int rgb_idx = (y * frame->width + x) * 3;
                        rgb_data[rgb_idx + 0] = R;
                        rgb_data[rgb_idx + 1] = G;
                        rgb_data[rgb_idx + 2] = B;
                    }
                }
                
                // Save as PPM
                std::string filename = config_.output_dir + "/decoded_" + 
                                     std::to_string(decoded_frame_num++) + ".ppm";
                std::ofstream file(filename, std::ios::binary);
                file << "P6\n" << frame->width << " " << frame->height << "\n255\n";
                file.write(reinterpret_cast<const char*>(rgb_data.data()), rgb_data.size());
            } else {
                BLUSTREAM_DEBUG_IO_BLOCK();
            }
        }
    }
    
    void stats_loop() {
        while (connected_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            auto now = std::chrono::steady_clock::now();
            float duration = std::chrono::duration<float>(now - stats_start_time_).count();
            
            if (duration > 0) {
                float fps = stats_.frames_received / duration;
                float bitrate_mbps = (stats_.bytes_received * 8.0f) / (duration * 1000000.0f);
                
                std::string decode_mode = stats_.hw_decode_active ? "HW" : "SW";
                
                std::cout << "\r[STATS] FPS: " << std::fixed << std::setprecision(1) << fps
                         << " | Bitrate: " << bitrate_mbps << " Mbps"
                         << " | Frames: " << stats_.frames_received
                         << " | Decoded: " << stats_.frames_decoded << " (" << decode_mode << ")"
                         << " | Decode: " << stats_.avg_decode_time_ms << " ms"
                         << " | HW/SW: " << stats_.hw_decode_frames << "/" << stats_.sw_decode_frames
                         << " | Errors: " << stats_.decode_errors
                         << "    " << std::flush;
            }
        }
    }
};

} // namespace client
} // namespace blustream

// Main function for testing
int main(int argc, char* argv[]) {
    using namespace blustream::client;
    
    StreamingClient::Config config;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--server" && i + 1 < argc) {
            config.server_ip = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.server_port = std::atoi(argv[++i]);
        } else if (arg == "--save-frames") {
            config.save_frames = true;
        } else if (arg == "--output-dir" && i + 1 < argc) {
            config.output_dir = argv[++i];
        } else if (arg == "--no-decode") {
            config.decode_frames = false;
        } else if (arg == "--no-stats") {
            config.display_stats = false;
        } else if (arg == "--hw-decode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "auto") {
                config.hw_decode = StreamingClient::HardwareDecodeMode::AUTO;
            } else if (mode == "off") {
                config.hw_decode = StreamingClient::HardwareDecodeMode::OFF;
            } else if (mode == "force") {
                config.hw_decode = StreamingClient::HardwareDecodeMode::FORCE;
            } else {
                std::cerr << "Invalid hw-decode mode: " << mode << ". Use auto|off|force\n";
                return 1;
            }
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                     << "Options:\n"
                     << "  --server IP       Server IP address (default: 127.0.0.1)\n"
                     << "  --port PORT       Server port (default: 8080)\n"
                     << "  --save-frames     Save received frames to disk\n"
                     << "  --output-dir DIR  Output directory for frames (default: ./frames)\n"
                     << "  --no-decode       Don't decode frames\n"
                     << "  --no-stats        Don't display statistics\n"
                     << "  --hw-decode MODE  Hardware decode mode: auto|off|force (default: auto)\n"
                     << "                    auto: attempt HW, fallback to SW if unsupported\n"
                     << "                    off: always use software decode\n"
                     << "                    force: fail if HW decode init fails\n"
                     << "  --help            Show this help message\n";
            return 0;
        }
    }
    
    std::cout << "===================================\n";
    std::cout << "BluStream Test Client\n";
    std::cout << "===================================\n\n";
    
    StreamingClient client;
    
    // Connect to server
    if (!client.connect_to_server(config)) {
        std::cerr << "Failed to connect to server\n";
        return 1;
    }
    
    // Start receiving
    client.start_receiving();
    
    std::cout << "Running for 10 seconds to capture frames...\n";
    std::this_thread::sleep_for(std::chrono::seconds(10));
    
    // Stop client
    client.stop();
    client.disconnect();
    
    std::cout << "\n✓ Client stopped\n";
    
    // Print debug I/O statistics
    BLUSTREAM_DEBUG_IO_STATS();
    
    return 0;
}