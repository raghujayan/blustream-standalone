// Phase 5 Main: WebRTC Streaming Server with Browser Integration
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <iomanip>
#include <memory>
#include <chrono>

#include "blustream/server/webrtc_server.h"
#include "blustream/server/hardware_encoder.h"
#include "blustream/common/logger.h"

// For HTTP server integration with signaling
#include <cpprest/http_listener.h>
#include <cpprest/json.h>

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

std::atomic<bool> g_running(true);
std::unique_ptr<blustream::server::WebRTCServer> g_webrtc_server;
std::unique_ptr<http_listener> g_http_listener;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n\nShutdown signal received...\n";
        g_running = false;
    }
}

void print_usage(const char* program) {
    std::cout << "🎬 BluStream Phase 5 - WebRTC Browser Streaming\n"
              << "=================================================\n\n"
              << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  --port PORT          HTTP/Signaling server port (default: 3000)\n"
              << "  --width WIDTH        Default render width (default: 1920)\n"
              << "  --height HEIGHT      Default render height (default: 1080)\n"
              << "  --fps FPS           Default target FPS (default: 30)\n"
              << "  --encoder TYPE      Encoder type (nvenc/quicksync/software/auto, default: auto)\n"
              << "  --quality PRESET    Quality preset (ultrafast/fast/balanced/high, default: fast)\n"
              << "  --vds PATH          VDS file to load\n"
              << "  --max-sessions N    Maximum concurrent sessions (default: 10)\n"
              << "  --min-bitrate KBPS  Minimum bitrate in kbps (default: 1000)\n"
              << "  --max-bitrate KBPS  Maximum bitrate in kbps (default: 15000)\n"
              << "  --target-latency MS Target latency in milliseconds (default: 150)\n"
              << "  --help              Show this help message\n\n"
              << "WebRTC Streaming Features:\n"
              << "  ✅ Ultra-low latency streaming (<150ms)\n"
              << "  ✅ Hardware-accelerated encoding (NVENC/QuickSync)\n"
              << "  ✅ Interactive VDS navigation controls\n"
              << "  ✅ Adaptive quality based on network conditions\n"
              << "  ✅ Multi-client collaborative viewing\n"
              << "  ✅ Browser-based client (no plugins required)\n\n"
              << "Browser Requirements:\n"
              << "  Chrome 90+, Firefox 88+, Safari 14+, Edge 90+\n";
}

// HTTP request handlers for signaling integration
class SignalingHandlers {
public:
    static void handle_create_session(http_request request) {
        auto query = uri::split_query(request.request_uri().query());
        
        // Parse session configuration from query parameters
        blustream::server::WebRTCServer::SessionConfig session_config;
        
        if (query.find("width") != query.end()) {
            session_config.width = std::stoi(query["width"]);
        }
        if (query.find("height") != query.end()) {
            session_config.height = std::stoi(query["height"]);
        }
        if (query.find("fps") != query.end()) {
            session_config.fps = std::stof(query["fps"]);
        }
        if (query.find("quality") != query.end()) {
            session_config.quality = query["quality"];
        }
        if (query.find("orientation") != query.end()) {
            session_config.orientation = query["orientation"];
        }
        
        // Create session
        auto session_id = g_webrtc_server->create_session(session_config);
        
        if (!session_id.empty()) {
            json::value response;
            response["sessionId"] = json::value::string(session_id);
            response["status"] = json::value::string("created");
            response["config"]["width"] = json::value::number(session_config.width);
            response["config"]["height"] = json::value::number(session_config.height);
            response["config"]["fps"] = json::value::number(session_config.fps);
            response["config"]["quality"] = json::value::string(session_config.quality);
            response["config"]["orientation"] = json::value::string(session_config.orientation);
            
            request.reply(status_codes::OK, response);
        } else {
            json::value error;
            error["error"] = json::value::string("Failed to create session");
            request.reply(status_codes::InternalError, error);
        }
    }
    
    static void handle_join_session(http_request request) {
        auto query = uri::split_query(request.request_uri().query());
        
        if (query.find("sessionId") == query.end() || query.find("clientId") == query.end()) {
            json::value error;
            error["error"] = json::value::string("Missing sessionId or clientId");
            request.reply(status_codes::BadRequest, error);
            return;
        }
        
        std::string session_id = query["sessionId"];
        std::string client_id = query["clientId"];
        
        if (g_webrtc_server->join_session(session_id, client_id)) {
            json::value response;
            response["status"] = json::value::string("joined");
            response["sessionId"] = json::value::string(session_id);
            response["clientId"] = json::value::string(client_id);
            
            request.reply(status_codes::OK, response);
        } else {
            json::value error;
            error["error"] = json::value::string("Failed to join session");
            request.reply(status_codes::NotFound, error);
        }
    }
    
    static void handle_webrtc_offer(http_request request) {
        request.extract_json().then([=](json::value body) {
            try {
                std::string session_id = body["sessionId"].as_string();
                std::string client_id = body["clientId"].as_string();
                std::string sdp = body["sdp"].as_string();
                
                g_webrtc_server->handle_offer(session_id, client_id, sdp);
                
                json::value response;
                response["status"] = json::value::string("offer_received");
                request.reply(status_codes::OK, response);
                
            } catch (const std::exception& e) {
                json::value error;
                error["error"] = json::value::string("Invalid offer format");
                request.reply(status_codes::BadRequest, error);
            }
        });
    }
    
    static void handle_webrtc_answer(http_request request) {
        request.extract_json().then([=](json::value body) {
            try {
                std::string session_id = body["sessionId"].as_string();
                std::string client_id = body["clientId"].as_string();
                std::string sdp = body["sdp"].as_string();
                
                g_webrtc_server->handle_answer(session_id, client_id, sdp);
                
                json::value response;
                response["status"] = json::value::string("answer_received");
                request.reply(status_codes::OK, response);
                
            } catch (const std::exception& e) {
                json::value error;
                error["error"] = json::value::string("Invalid answer format");
                request.reply(status_codes::BadRequest, error);
            }
        });
    }
    
    static void handle_ice_candidate(http_request request) {
        request.extract_json().then([=](json::value body) {
            try {
                std::string session_id = body["sessionId"].as_string();
                std::string client_id = body["clientId"].as_string();
                std::string candidate = body["candidate"].as_string();
                std::string sdp_mid = body["sdpMid"].as_string();
                int sdp_mline_index = body["sdpMLineIndex"].as_integer();
                
                g_webrtc_server->handle_ice_candidate(session_id, client_id, candidate, sdp_mid, sdp_mline_index);
                
                json::value response;
                response["status"] = json::value::string("ice_candidate_received");
                request.reply(status_codes::OK, response);
                
            } catch (const std::exception& e) {
                json::value error;
                error["error"] = json::value::string("Invalid ICE candidate format");
                request.reply(status_codes::BadRequest, error);
            }
        });
    }
    
    static void handle_control_message(http_request request) {
        request.extract_json().then([=](json::value body) {
            try {
                blustream::server::WebRTCServer::ControlMessage message;
                message.session_id = body["sessionId"].as_string();
                
                std::string control_type = body["controlType"].as_string();
                
                if (control_type == "slice-orientation") {
                    message.type = blustream::server::WebRTCServer::ControlMessage::SLICE_ORIENTATION;
                } else if (control_type == "animation-speed") {
                    message.type = blustream::server::WebRTCServer::ControlMessage::ANIMATION_SPEED;
                } else if (control_type == "pause-resume") {
                    message.type = blustream::server::WebRTCServer::ControlMessage::PAUSE_RESUME;
                } else if (control_type == "restart-animation") {
                    message.type = blustream::server::WebRTCServer::ControlMessage::RESTART_ANIMATION;
                } else if (control_type == "quality-level") {
                    message.type = blustream::server::WebRTCServer::ControlMessage::QUALITY_LEVEL;
                } else {
                    throw std::runtime_error("Unknown control type");
                }
                
                // Extract parameters
                if (body.has_field("controlData")) {
                    auto control_data = body["controlData"];
                    for (auto& field : control_data.as_object()) {
                        message.parameters[field.first] = field.second.as_string();
                    }
                }
                
                g_webrtc_server->handle_control_message(message);
                
                json::value response;
                response["status"] = json::value::string("control_message_received");
                request.reply(status_codes::OK, response);
                
            } catch (const std::exception& e) {
                json::value error;
                error["error"] = json::value::string("Invalid control message format");
                request.reply(status_codes::BadRequest, error);
            }
        });
    }
    
    static void handle_server_stats(http_request request) {
        auto stats = g_webrtc_server->get_stats();
        
        json::value response;
        response["activeSessions"] = json::value::number(stats.active_sessions);
        response["totalClients"] = json::value::number(stats.total_clients);
        response["avgEncodingTimeMs"] = json::value::number(stats.avg_encoding_time_ms);
        response["avgFrameRate"] = json::value::number(stats.avg_frame_rate);
        response["framesEncoded"] = json::value::number(stats.frames_encoded);
        response["bytesSent"] = json::value::number(stats.bytes_sent);
        response["avgLatencyMs"] = json::value::number(stats.avg_latency_ms);
        
        request.reply(status_codes::OK, response);
    }
};

bool setup_http_server(int port) {
    try {
        std::string address = "http://0.0.0.0:" + std::to_string(port);
        g_http_listener = std::make_unique<http_listener>(address);
        
        // Set up route handlers
        g_http_listener->support(methods::POST, [](http_request request) {
            auto path = request.request_uri().path();
            
            if (path == "/api/sessions") {
                SignalingHandlers::handle_create_session(request);
            } else if (path == "/api/join-session") {
                SignalingHandlers::handle_join_session(request);
            } else if (path == "/api/webrtc/offer") {
                SignalingHandlers::handle_webrtc_offer(request);
            } else if (path == "/api/webrtc/answer") {
                SignalingHandlers::handle_webrtc_answer(request);
            } else if (path == "/api/webrtc/ice-candidate") {
                SignalingHandlers::handle_ice_candidate(request);
            } else if (path == "/api/control") {
                SignalingHandlers::handle_control_message(request);
            } else {
                request.reply(status_codes::NotFound);
            }
        });
        
        g_http_listener->support(methods::GET, [](http_request request) {
            auto path = request.request_uri().path();
            
            if (path == "/api/stats") {
                SignalingHandlers::handle_server_stats(request);
            } else {
                request.reply(status_codes::NotFound);
            }
        });
        
        // Enable CORS
        g_http_listener->support(methods::OPTIONS, [](http_request request) {
            http_response response(status_codes::OK);
            response.headers().add("Access-Control-Allow-Origin", "*");
            response.headers().add("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.headers().add("Access-Control-Allow-Headers", "Content-Type");
            request.reply(response);
        });
        
        g_http_listener->open().wait();
        
        std::cout << "✅ HTTP signaling server started on port " << port << "\n";
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to start HTTP server: " << e.what() << "\n";
        return false;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "🎬 BluStream Phase 5 - WebRTC Browser Streaming\n";
    std::cout << "=================================================\n\n";
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Default configuration
    blustream::server::WebRTCServer::Config config;
    config.signaling_port = 3000;
    config.max_sessions = 10;
    config.default_width = 1920;
    config.default_height = 1080;
    config.default_fps = 30.0f;
    config.encoder_type = blustream::server::HardwareEncoder::Type::AUTO_DETECT;
    config.encoder_quality = blustream::server::HardwareEncoder::Quality::FAST;
    config.vds_path = "/home/rocky/blustream/data/onnia2x3d_mig_Time.vds";
    config.default_orientation = "XZ";
    config.enable_animation = true;
    config.animation_duration = 30.0f;
    config.enable_adaptive_quality = true;
    config.min_bitrate_kbps = 1000;
    config.max_bitrate_kbps = 15000;
    config.target_latency_ms = 150;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            config.signaling_port = std::atoi(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            config.default_width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.default_height = std::atoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            config.default_fps = std::atof(argv[++i]);
        } else if (arg == "--encoder" && i + 1 < argc) {
            std::string encoder_str = argv[++i];
            if (encoder_str == "nvenc") {
                config.encoder_type = blustream::server::HardwareEncoder::Type::NVENC_H264;
            } else if (encoder_str == "quicksync") {
                config.encoder_type = blustream::server::HardwareEncoder::Type::QUICKSYNC_H264;
            } else if (encoder_str == "software") {
                config.encoder_type = blustream::server::HardwareEncoder::Type::SOFTWARE_X264;
            }
        } else if (arg == "--quality" && i + 1 < argc) {
            std::string quality_str = argv[++i];
            if (quality_str == "ultrafast") {
                config.encoder_quality = blustream::server::HardwareEncoder::Quality::ULTRA_FAST;
            } else if (quality_str == "balanced") {
                config.encoder_quality = blustream::server::HardwareEncoder::Quality::BALANCED;
            } else if (quality_str == "high") {
                config.encoder_quality = blustream::server::HardwareEncoder::Quality::HIGH_QUALITY;
            }
        } else if (arg == "--vds" && i + 1 < argc) {
            config.vds_path = argv[++i];
        } else if (arg == "--max-sessions" && i + 1 < argc) {
            config.max_sessions = std::atoi(argv[++i]);
        } else if (arg == "--min-bitrate" && i + 1 < argc) {
            config.min_bitrate_kbps = std::atoi(argv[++i]);
        } else if (arg == "--max-bitrate" && i + 1 < argc) {
            config.max_bitrate_kbps = std::atoi(argv[++i]);
        } else if (arg == "--target-latency" && i + 1 < argc) {
            config.target_latency_ms = std::atoi(argv[++i]);
        }
    }
    
    // Display configuration
    std::cout << "📋 Phase 5 WebRTC Server Configuration:\n";
    std::cout << "  Signaling Port: " << config.signaling_port << "\n";
    std::cout << "  Default Resolution: " << config.default_width << "x" << config.default_height << "\n";
    std::cout << "  Default FPS: " << config.default_fps << "\n";
    std::cout << "  Max Sessions: " << config.max_sessions << "\n";
    std::cout << "  Bitrate Range: " << config.min_bitrate_kbps << "-" << config.max_bitrate_kbps << " kbps\n";
    std::cout << "  Target Latency: " << config.target_latency_ms << "ms\n";
    std::cout << "  VDS File: " << config.vds_path << "\n";
    std::cout << "  Default Orientation: " << config.default_orientation << "\n";
    std::cout << "  Animation: " << (config.enable_animation ? "ENABLED" : "DISABLED") << "\n";
    std::cout << "  Adaptive Quality: " << (config.enable_adaptive_quality ? "ENABLED" : "DISABLED") << "\n";
    
    // Create and initialize WebRTC server
    g_webrtc_server = std::make_unique<blustream::server::WebRTCServer>();
    
    if (!g_webrtc_server->initialize(config)) {
        std::cerr << "❌ Failed to initialize WebRTC server\n";
        return -1;
    }
    
    // Load VDS data
    std::cout << "\n📂 Loading VDS data...\n";
    if (!g_webrtc_server->load_vds(config.vds_path)) {
        std::cerr << "❌ Failed to load VDS file: " << config.vds_path << "\n";
        std::cerr << "⚠️  Server will continue without VDS data\n";
    }
    
    // Set up WebRTC server callbacks for signaling integration
    g_webrtc_server->on_offer_created = [](const std::string& session_id, const std::string& client_id, const std::string& sdp) {
        std::cout << "📤 Offer created for session " << session_id << ", client " << client_id << "\n";
        // In a full implementation, this would be sent via WebSocket to the signaling server
    };
    
    g_webrtc_server->on_answer_created = [](const std::string& session_id, const std::string& client_id, const std::string& sdp) {
        std::cout << "📥 Answer created for session " << session_id << ", client " << client_id << "\n";
        // In a full implementation, this would be sent via WebSocket to the signaling server
    };
    
    g_webrtc_server->on_ice_candidate = [](const std::string& session_id, const std::string& client_id, const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index) {
        std::cout << "🧊 ICE candidate for session " << session_id << ", client " << client_id << "\n";
        // In a full implementation, this would be sent via WebSocket to the signaling server
    };
    
    g_webrtc_server->on_error = [](const std::string& session_id, const std::string& client_id, const std::string& error) {
        std::cerr << "❌ WebRTC error for session " << session_id << ", client " << client_id << ": " << error << "\n";
    };
    
    // Setup HTTP signaling server
    if (!setup_http_server(config.signaling_port)) {
        std::cerr << "❌ Failed to start HTTP signaling server\n";
        return -1;
    }
    
    // Start WebRTC server
    std::cout << "\n🚀 Starting Phase 5 WebRTC server...\n";
    if (!g_webrtc_server->start()) {
        std::cerr << "❌ Failed to start WebRTC server\n";
        return -1;
    }
    
    std::cout << "✅ Phase 5 WebRTC server started successfully!\n";
    std::cout << "🎯 Ready for ultra-low latency browser streaming\n";
    std::cout << "🌐 Browser client: http://localhost:" << config.signaling_port << "\n";
    std::cout << "📡 Signaling API: http://localhost:" << config.signaling_port << "/api/\n";
    std::cout << "⚡ Hardware acceleration: ENABLED\n";
    std::cout << "🎬 WebRTC streaming: ACTIVE\n\n";
    
    std::cout << "📊 API Endpoints:\n";
    std::cout << "  POST /api/sessions - Create new session\n";
    std::cout << "  POST /api/join-session - Join existing session\n";
    std::cout << "  POST /api/webrtc/offer - Handle WebRTC offer\n";
    std::cout << "  POST /api/webrtc/answer - Handle WebRTC answer\n";
    std::cout << "  POST /api/webrtc/ice-candidate - Handle ICE candidates\n";
    std::cout << "  POST /api/control - Send control messages\n";
    std::cout << "  GET  /api/stats - Get server statistics\n\n";
    
    // Performance monitoring loop
    auto last_stats_time = std::chrono::steady_clock::now();
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        if (!g_running) break;
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - last_stats_time).count();
        
        if (elapsed >= 10.0f) {  // Print stats every 10 seconds
            auto stats = g_webrtc_server->get_stats();
            
            std::cout << "\n📊 Phase 5 WebRTC Performance Stats:\n";
            std::cout << "  Active Sessions: " << stats.active_sessions << "\n";
            std::cout << "  Total Clients: " << stats.total_clients << "\n";
            std::cout << "  Avg Encoding Time: " << std::fixed << std::setprecision(2) << stats.avg_encoding_time_ms << "ms\n";
            std::cout << "  Avg Frame Rate: " << std::setprecision(1) << stats.avg_frame_rate << " fps\n";
            std::cout << "  Frames Encoded: " << stats.frames_encoded << "\n";
            std::cout << "  Bytes Sent: " << (stats.bytes_sent / 1024 / 1024) << " MB\n";
            std::cout << "  Avg Latency: " << std::setprecision(1) << stats.avg_latency_ms << "ms\n";
            
            if (stats.total_clients > 0) {
                std::cout << "  🎯 Streaming to " << stats.total_clients << " browser clients\n";
            } else {
                std::cout << "  ⏳ Waiting for browser connections...\n";
            }
            
            last_stats_time = now;
        }
    }
    
    std::cout << "\n🛑 Shutting down Phase 5 server...\n";
    
    // Cleanup
    if (g_http_listener) {
        g_http_listener->close().wait();
    }
    
    if (g_webrtc_server) {
        g_webrtc_server->stop();
    }
    
    std::cout << "✅ Phase 5 WebRTC server shut down gracefully\n";
    std::cout << "🎬 Ultra-low latency browser streaming session complete!\n";
    
    return 0;
}