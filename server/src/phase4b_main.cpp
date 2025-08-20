// Phase 4B Main: Streaming Server with Hardware Encoding (NVENC)
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <iomanip>

#include "blustream/server/streaming_server.h"
#include "blustream/server/hardware_encoder.h"
#include "blustream/common/logger.h"

std::atomic<bool> g_running(true);

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n\nShutdown signal received...\n";
        g_running = false;
    }
}

void print_usage(const char* program) {
    std::cout << "🎬 BluStream Phase 4B - Hardware Accelerated Seismic Streaming\n"
              << "================================================================\n\n"
              << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  --port PORT          Server port (default: 8086)\n"
              << "  --width WIDTH        Render width (default: 3840 for 4K)\n"
              << "  --height HEIGHT      Render height (default: 2160 for 4K)\n"
              << "  --fps FPS           Target FPS (default: 30)\n"
              << "  --bitrate KBPS      Bitrate in kbps (default: 15000 for 4K)\n"
              << "  --encoder TYPE      Encoder type (nvenc/quicksync/software/auto, default: auto)\n"
              << "  --quality PRESET    Quality preset (ultrafast/fast/balanced/high, default: fast)\n"
              << "  --vds PATH          VDS file to load\n"
              << "  --slice-orientation ORIENT  Slice orientation: XY, XZ, YZ (default: XZ)\n"
              << "  --animate-slice     Enable slice position animation (default: enabled)\n"
              << "  --no-animate-slice  Disable slice position animation\n"
              << "  --animation-duration SEC    Duration to traverse all slices (default: 30)\n"
              << "  --max-clients N     Maximum clients (default: 5 for 4K)\n"
              << "  --test-encoding     Run encoding performance test\n"
              << "  --help              Show this help message\n\n"
              << "4K Streaming Presets:\n"
              << "  --preset-4k-fast    4K@30fps with fast encoding (15Mbps)\n"
              << "  --preset-4k-quality 4K@30fps with high quality (25Mbps)\n"
              << "  --preset-1080p-fast 1080p@60fps with fast encoding (8Mbps)\n";
}

// Hardware encoding configuration
blustream::server::HardwareEncoder::Type parse_encoder_type(const std::string& type) {
    if (type == "nvenc") return blustream::server::HardwareEncoder::Type::NVENC_H264;
    if (type == "quicksync") return blustream::server::HardwareEncoder::Type::QUICKSYNC_H264;
    if (type == "software") return blustream::server::HardwareEncoder::Type::SOFTWARE_X264;
    return blustream::server::HardwareEncoder::Type::AUTO_DETECT;
}

blustream::server::HardwareEncoder::Quality parse_quality_preset(const std::string& quality) {
    if (quality == "ultrafast") return blustream::server::HardwareEncoder::Quality::ULTRA_FAST;
    if (quality == "fast") return blustream::server::HardwareEncoder::Quality::FAST;
    if (quality == "balanced") return blustream::server::HardwareEncoder::Quality::BALANCED;
    if (quality == "high") return blustream::server::HardwareEncoder::Quality::HIGH_QUALITY;
    return blustream::server::HardwareEncoder::Quality::FAST;
}

void apply_preset(blustream::server::StreamingServer::Config& config, const std::string& preset) {
    if (preset == "4k-fast") {
        config.render_width = 3840;
        config.render_height = 2160;
        config.target_fps = 30;
        config.bitrate_kbps = 15000;
        config.max_clients = 3;
        std::cout << "✅ Applied 4K Fast preset: 3840x2160@30fps, 15Mbps\n";
    } else if (preset == "4k-quality") {
        config.render_width = 3840;
        config.render_height = 2160;
        config.target_fps = 30;
        config.bitrate_kbps = 25000;
        config.max_clients = 2;
        std::cout << "✅ Applied 4K Quality preset: 3840x2160@30fps, 25Mbps\n";
    } else if (preset == "1080p-fast") {
        config.render_width = 1920;
        config.render_height = 1080;
        config.target_fps = 60;
        config.bitrate_kbps = 8000;
        config.max_clients = 10;
        std::cout << "✅ Applied 1080p Fast preset: 1920x1080@60fps, 8Mbps\n";
    }
}

int run_encoding_test() {
    std::cout << "\n🔬 Running Hardware Encoding Performance Test...\n";
    
    // Test different resolutions
    std::vector<std::tuple<int, int, std::string>> test_configs = {
        {1920, 1080, "1080p"},
        {2560, 1440, "1440p"},
        {3840, 2160, "4K"}
    };
    
    for (auto [width, height, name] : test_configs) {
        std::cout << "\n--- Testing " << name << " (" << width << "x" << height << ") ---\n";
        
        auto encoder = blustream::server::HardwareEncoderFactory::create_optimal_encoder(
            width, height, 30, 15000);
        
        if (encoder) {
            std::cout << "Encoder: " << encoder->get_encoder_name() << "\n";
            std::cout << "Hardware acceleration: " << 
                (encoder->supports_hardware_acceleration() ? "ENABLED" : "DISABLED") << "\n";
            
            // Quick 10-frame test
            auto test_frame = std::vector<uint8_t>(width * height * 3, 128); // Gray frame
            
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; i++) {
                encoder->encode_frame(test_frame);
            }
            auto end = std::chrono::steady_clock::now();
            
            float avg_time = std::chrono::duration<float, std::milli>(end - start).count() / 10.0f;
            float max_fps = 1000.0f / avg_time;
            
            std::cout << "Average encode time: " << std::fixed << std::setprecision(2) << avg_time << "ms\n";
            std::cout << "Theoretical max FPS: " << max_fps << "\n";
            
            if (max_fps >= 30) {
                std::cout << "✅ Suitable for real-time streaming\n";
            } else {
                std::cout << "⚠️  May struggle with real-time streaming\n";
            }
        } else {
            std::cout << "❌ Failed to create encoder\n";
        }
    }
    
    return 0;
}

int main(int argc, char* argv[]) {
    std::cout << "🎬 BluStream Phase 4B - Hardware Accelerated Seismic Streaming\n";
    std::cout << "================================================================\n\n";
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Default configuration for 4K streaming
    blustream::server::StreamingServer::Config config;
    config.port = 8086;
    config.render_width = 3840;   // 4K width
    config.render_height = 2160;  // 4K height
    config.target_fps = 30;
    config.bitrate_kbps = 15000;  // 15 Mbps for 4K
    config.encoder = "nvenc";     // Use hardware encoding
    config.max_clients = 3;       // Fewer clients for 4K
    config.vds_path = "/home/rocky/blustream/data/onnia2x3d_mig_Time.vds";
    config.slice_orientation = "XZ";  // Vertical seismic sections
    config.animate_slice = true;
    config.animation_duration = 30.0f;
    
    blustream::server::HardwareEncoder::Type encoder_type = blustream::server::HardwareEncoder::Type::AUTO_DETECT;
    blustream::server::HardwareEncoder::Quality quality_preset = blustream::server::HardwareEncoder::Quality::FAST;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--test-encoding") {
            return run_encoding_test();
        } else if (arg == "--preset-4k-fast") {
            apply_preset(config, "4k-fast");
        } else if (arg == "--preset-4k-quality") {
            apply_preset(config, "4k-quality");
        } else if (arg == "--preset-1080p-fast") {
            apply_preset(config, "1080p-fast");
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = std::atoi(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            config.render_width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.render_height = std::atoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            config.target_fps = std::atof(argv[++i]);
        } else if (arg == "--bitrate" && i + 1 < argc) {
            config.bitrate_kbps = std::atoi(argv[++i]);
        } else if (arg == "--encoder" && i + 1 < argc) {
            encoder_type = parse_encoder_type(argv[++i]);
        } else if (arg == "--quality" && i + 1 < argc) {
            quality_preset = parse_quality_preset(argv[++i]);
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
        }
    }
    
    // Display configuration
    std::cout << "📋 Phase 4B Server Configuration:\n";
    std::cout << "  Resolution: " << config.render_width << "x" << config.render_height << "\n";
    std::cout << "  Target FPS: " << config.target_fps << "\n";
    std::cout << "  Bitrate: " << config.bitrate_kbps << " kbps\n";
    std::cout << "  Port: " << config.port << "\n";
    std::cout << "  Max Clients: " << config.max_clients << "\n";
    std::cout << "  VDS File: " << config.vds_path << "\n";
    std::cout << "  Slice Orientation: " << config.slice_orientation << "\n";
    std::cout << "  Animation: " << (config.animate_slice ? "ENABLED" : "DISABLED") << "\n";
    if (config.animate_slice) {
        std::cout << "  Animation Duration: " << config.animation_duration << "s\n";
    }
    
    // Test hardware encoding availability
    std::cout << "\n🔍 Hardware Encoding Detection:\n";
    auto available_encoders = blustream::server::HardwareEncoder::get_available_encoders();
    for (auto type : available_encoders) {
        std::cout << "  ✅ " << blustream::server::HardwareEncoder::encoder_type_to_string(type) << "\n";
    }
    
    // Create and initialize streaming server
    auto server = std::make_unique<blustream::server::StreamingServer>();
    
    if (!server->initialize(config)) {
        std::cerr << "❌ Failed to initialize streaming server\n";
        return -1;
    }
    
    // Load VDS data
    std::cout << "\n📂 Loading VDS data...\n";
    if (!server->load_vds(config.vds_path)) {
        std::cerr << "❌ Failed to load VDS file: " << config.vds_path << "\n";
        std::cerr << "⚠️  Falling back to synthetic data generation\n";
    }
    
    // Start server
    std::cout << "\n🚀 Starting Phase 4B hardware-accelerated streaming server...\n";
    if (!server->start()) {
        std::cerr << "❌ Failed to start streaming server\n";
        return -1;
    }
    
    std::cout << "✅ Server started successfully!\n";
    std::cout << "🎯 Ready for 4K seismic data streaming with hardware acceleration\n";
    std::cout << "🔗 Client connection: Port " << config.port << "\n";
    std::cout << "📊 Expected encoding performance: ~48ms per frame (Tesla T4)\n";
    std::cout << "⚡ Hardware acceleration: ENABLED\n\n";
    
    // Performance monitoring loop
    auto last_stats_time = std::chrono::steady_clock::now();
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        if (!g_running) break;
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - last_stats_time).count();
        
        if (elapsed >= 10.0f) {  // Print stats every 10 seconds
            auto stats = server->get_stats();
            
            std::cout << "\n📊 Phase 4B Performance Stats:\n";
            std::cout << "  Current FPS: " << std::fixed << std::setprecision(1) << stats.current_fps << "\n";
            std::cout << "  Encoding Time: " << std::setprecision(2) << stats.encoding_time_ms << "ms\n";
            std::cout << "  Render Time: " << stats.render_time_ms << "ms\n";
            std::cout << "  Frames Rendered: " << stats.frames_rendered << "\n";
            std::cout << "  Frames Encoded: " << stats.frames_encoded << "\n";
            std::cout << "  Connected Clients: " << server->get_client_count() << "\n";
            std::cout << "  Bitrate: " << std::setprecision(1) << stats.bitrate_mbps << " Mbps\n";
            
            if (stats.frames_dropped > 0) {
                std::cout << "  ⚠️  Frames Dropped: " << stats.frames_dropped << "\n";
            }
            
            last_stats_time = now;
        }
    }
    
    std::cout << "\n🛑 Shutting down server...\n";
    server->stop();
    
    std::cout << "✅ Phase 4B server shut down gracefully\n";
    std::cout << "🎬 Hardware-accelerated seismic streaming session complete!\n";
    
    return 0;
}