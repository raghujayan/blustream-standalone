// Phase 4 Main: Streaming Server with Software Encoding
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <iomanip>

#include "blustream/server/streaming_server.h"
#include "blustream/common/logger.h"

std::atomic<bool> g_running(true);

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n\nShutdown signal received...\n";
        g_running = false;
    }
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  --port PORT          Server port (default: 8080)\n"
              << "  --width WIDTH        Render width (default: 1920)\n"
              << "  --height HEIGHT      Render height (default: 1080)\n"
              << "  --fps FPS           Target FPS (default: 30)\n"
              << "  --bitrate KBPS      Bitrate in kbps (default: 5000)\n"
              << "  --preset PRESET     x264 preset (ultrafast/fast/medium/slow, default: fast)\n"
              << "  --vds PATH          VDS file to load\n"
              << "  --slice-orientation ORIENT  Slice orientation: XY, XZ, YZ (default: XZ for vertical sections)\n"
              << "  --animate-slice     Enable slice position animation (default: enabled)\n"
              << "  --no-animate-slice  Disable slice position animation\n"
              << "  --animation-duration SEC    Duration to traverse all slices in seconds (default: 30)\n"
              << "  --max-clients N     Maximum clients (default: 10)\n"
              << "  --help              Show this help message\n";
}

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Parse command line arguments
    blustream::server::StreamingServer::Config config;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--port" && i + 1 < argc) {
            config.port = std::atoi(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            config.render_width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.render_height = std::atoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            config.target_fps = std::atof(argv[++i]);
        } else if (arg == "--bitrate" && i + 1 < argc) {
            config.bitrate_kbps = std::atoi(argv[++i]);
        } else if (arg == "--preset" && i + 1 < argc) {
            config.preset = argv[++i];
        } else if (arg == "--vds" && i + 1 < argc) {
            config.vds_path = argv[++i];
        } else if (arg == "--slice-orientation" && i + 1 < argc) {
            config.slice_orientation = argv[++i];
        } else if (arg == "--animate-slice") {
            config.animate_slice = true;
        } else if (arg == "--no-animate-slice") {
            config.animate_slice = false;
        } else if (arg == "--animation-duration" && i + 1 < argc) {
            config.animation_duration = std::atof(argv[++i]);
        } else if (arg == "--max-clients" && i + 1 < argc) {
            config.max_clients = std::atoi(argv[++i]);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    std::cout << "\n====================================\n";
    std::cout << "BluStream Phase 4: Streaming Server\n";
    std::cout << "====================================\n\n";
    
    // Check if Xvfb is running (for headless OpenGL)
    const char* display = std::getenv("DISPLAY");
    if (!display) {
        std::cout << "WARNING: DISPLAY not set. Setting to :99\n";
        setenv("DISPLAY", ":99", 1);
        
        std::cout << "Starting Xvfb...\n";
        system("Xvfb :99 -screen 0 1920x1080x24 +extension GLX +render -noreset &");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    
    // Create and initialize server
    blustream::server::StreamingServer server;
    
    if (!server.initialize(config)) {
        std::cerr << "Failed to initialize streaming server\n";
        return 1;
    }
    
    // Start server
    if (!server.start()) {
        std::cerr << "Failed to start streaming server\n";
        return 1;
    }
    
    std::cout << "\n✓ Streaming server running on port " << config.port << "\n";
    std::cout << "  Resolution: " << config.render_width << "x" << config.render_height << "\n";
    std::cout << "  Target FPS: " << config.target_fps << "\n";
    std::cout << "  Bitrate: " << config.bitrate_kbps << " kbps\n";
    std::cout << "  Encoder: x264 (" << config.preset << " preset)\n";
    std::cout << "\nPress Ctrl+C to stop...\n\n";
    
    // Main loop - display statistics
    auto last_stats_time = std::chrono::steady_clock::now();
    
    while (g_running && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - last_stats_time).count() >= 1.0f) {
            auto stats = server.get_stats();
            
            std::cout << "\r[SERVER] "
                     << "FPS: " << std::fixed << std::setprecision(1) << stats.current_fps
                     << " | Clients: " << server.get_client_count()
                     << " | Render: " << std::setprecision(1) << stats.render_time_ms << "ms"
                     << " | Encode: " << stats.encoding_time_ms << "ms"
                     << " | Bitrate: " << stats.bitrate_mbps << " Mbps"
                     << " | Frames: " << stats.frames_encoded
                     << "    " << std::flush;
            
            last_stats_time = now;
        }
    }
    
    std::cout << "\n\nStopping server...\n";
    server.stop();
    
    std::cout << "✓ Server stopped\n";
    
    return 0;
}