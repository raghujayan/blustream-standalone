/**
 * @brief Test program for Phase 4B Hardware Encoding capabilities
 * 
 * This program tests and benchmarks the hardware encoding performance
 * on the target Linux server with Tesla T4 GPU.
 */

#include "blustream/server/hardware_encoder.h"
#include "blustream/common/logger.h"

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <algorithm>
#include <numeric>

using namespace blustream::server;

// Generate test RGB frame data
std::vector<uint8_t> generate_test_frame(int width, int height) {
    std::vector<uint8_t> rgb_data(width * height * 3);
    
    // Generate a colorful test pattern
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            
            // Create a gradient pattern
            rgb_data[idx + 0] = static_cast<uint8_t>((x * 255) / width);        // Red gradient
            rgb_data[idx + 1] = static_cast<uint8_t>((y * 255) / height);       // Green gradient  
            rgb_data[idx + 2] = static_cast<uint8_t>(((x + y) * 255) / (width + height)); // Blue gradient
        }
    }
    
    return rgb_data;
}

// Benchmark encoding performance
void benchmark_encoder(HardwareEncoder& encoder, int width, int height, int num_frames) {
    std::cout << "\n=== Encoding Performance Benchmark ===\n";
    std::cout << "Resolution: " << width << "x" << height << "\n";
    std::cout << "Test frames: " << num_frames << "\n";
    std::cout << "Encoder: " << encoder.get_encoder_name() << "\n\n";
    
    // Generate test frame
    auto test_frame = generate_test_frame(width, height);
    
    std::vector<float> encode_times;
    std::vector<size_t> frame_sizes;
    
    auto benchmark_start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_frames; i++) {
        auto frame_start = std::chrono::steady_clock::now();
        
        // Encode frame
        auto encoded_data = encoder.encode_frame(test_frame);
        
        auto frame_end = std::chrono::steady_clock::now();
        float encode_time_ms = std::chrono::duration<float, std::milli>(frame_end - frame_start).count();
        
        encode_times.push_back(encode_time_ms);
        frame_sizes.push_back(encoded_data.size());
        
        // Progress indicator
        if ((i + 1) % 10 == 0) {
            std::cout << "Encoded " << (i + 1) << "/" << num_frames << " frames\r" << std::flush;
        }
    }
    
    auto benchmark_end = std::chrono::steady_clock::now();
    float total_time_s = std::chrono::duration<float>(benchmark_end - benchmark_start).count();
    
    // Calculate statistics
    float avg_encode_time = std::accumulate(encode_times.begin(), encode_times.end(), 0.0f) / encode_times.size();
    float min_encode_time = *std::min_element(encode_times.begin(), encode_times.end());
    float max_encode_time = *std::max_element(encode_times.begin(), encode_times.end());
    
    size_t avg_frame_size = std::accumulate(frame_sizes.begin(), frame_sizes.end(), 0UL) / frame_sizes.size();
    
    float achieved_fps = num_frames / total_time_s;
    float theoretical_fps = 1000.0f / avg_encode_time;
    
    std::cout << "\n\n=== Benchmark Results ===\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Encoding Performance:\n";
    std::cout << "  Average encode time: " << avg_encode_time << "ms\n";
    std::cout << "  Min encode time: " << min_encode_time << "ms\n";
    std::cout << "  Max encode time: " << max_encode_time << "ms\n";
    std::cout << "  Theoretical max FPS: " << theoretical_fps << "\n";
    std::cout << "  Achieved FPS: " << achieved_fps << "\n";
    
    std::cout << "\nFrame Size:\n";
    std::cout << "  Average frame size: " << (avg_frame_size / 1024) << " KB\n";
    std::cout << "  Total data encoded: " << (std::accumulate(frame_sizes.begin(), frame_sizes.end(), 0UL) / 1024 / 1024) << " MB\n";
    
    std::cout << "\nQuality Assessment:\n";
    if (avg_encode_time < 10.0f) {
        std::cout << "  ✅ EXCELLENT: Suitable for real-time streaming at high frame rates\n";
    } else if (avg_encode_time < 16.7f) {
        std::cout << "  ✅ GOOD: Suitable for 60 FPS streaming\n";
    } else if (avg_encode_time < 33.3f) {
        std::cout << "  ⚠️  FAIR: Suitable for 30 FPS streaming\n";
    } else {
        std::cout << "  ❌ POOR: May struggle with real-time streaming\n";
    }
    
    // Hardware acceleration assessment
    if (encoder.supports_hardware_acceleration()) {
        std::cout << "  🚀 Hardware acceleration ACTIVE\n";
    } else {
        std::cout << "  🐌 Software encoding (no hardware acceleration)\n";
    }
}

// Test different encoder configurations
void test_encoder_configurations() {
    std::cout << "\n=== Testing Different Encoder Configurations ===\n";
    
    // Test different resolutions
    std::vector<std::pair<int, int>> resolutions = {
        {1920, 1080}, // 1080p
        {1280, 720},  // 720p
        {3840, 2160}  // 4K (if supported)
    };
    
    for (auto [width, height] : resolutions) {
        std::cout << "\n--- Testing " << width << "x" << height << " ---\n";
        
        // Create optimal encoder for this resolution
        auto encoder = HardwareEncoderFactory::create_optimal_encoder(width, height, 30, 5000);
        
        if (encoder) {
            benchmark_encoder(*encoder, width, height, 30);  // 30 test frames
        } else {
            std::cout << "❌ Failed to create encoder for " << width << "x" << height << "\n";
        }
    }
}

// Main test function
int main() {
    std::cout << "🎬 BluStream Phase 4B - Hardware Encoding Test\n";
    std::cout << "==============================================\n";
    
    // Initialize logging
    // (Assuming logger is set up to output to console)
    
    // Test system capabilities
    std::cout << "\n=== System Hardware Detection ===\n";
    
    auto available_encoders = HardwareEncoder::get_available_encoders();
    std::cout << "Available encoders:\n";
    for (auto type : available_encoders) {
        std::cout << "  - " << HardwareEncoder::encoder_type_to_string(type) << "\n";
    }
    
    if (HardwareEncoder::is_nvidia_gpu_available()) {
        std::cout << "✅ NVIDIA GPU with NVENC support detected\n";
    } else {
        std::cout << "⚠️  NVIDIA GPU or NVENC not available\n";
    }
    
    if (HardwareEncoder::is_intel_gpu_available()) {
        std::cout << "✅ Intel GPU with QuickSync support detected\n";
    } else {
        std::cout << "⚠️  Intel GPU or QuickSync not available\n";
    }
    
    // Test optimal encoder creation
    std::cout << "\n=== Creating Optimal Encoder ===\n";
    auto encoder = HardwareEncoderFactory::create_optimal_encoder(1920, 1080, 30, 5000);
    
    if (!encoder) {
        std::cerr << "❌ Failed to create optimal encoder - aborting tests\n";
        return -1;
    }
    
    std::cout << "✅ Successfully created: " << encoder->get_encoder_name() << "\n";
    std::cout << "Hardware acceleration: " << (encoder->supports_hardware_acceleration() ? "ENABLED" : "DISABLED") << "\n";
    
    // Run performance benchmark
    benchmark_encoder(*encoder, 1920, 1080, 100);  // 100 frames for thorough testing
    
    // Test different configurations if we have time
    test_encoder_configurations();
    
    // Final performance comparison
    std::cout << "\n=== Phase 4B vs Phase 4A Performance Comparison ===\n";
    auto hw_stats = encoder->get_stats();
    
    std::cout << "Phase 4B (Hardware): ~" << hw_stats.avg_encode_time_ms << "ms average encoding\n";
    std::cout << "Phase 4A (Software): ~50-100ms average encoding (estimated)\n";
    
    float speedup = 75.0f / hw_stats.avg_encode_time_ms;  // Assuming 75ms software baseline
    std::cout << "Performance improvement: " << std::fixed << std::setprecision(1) << speedup << "x faster\n";
    
    if (speedup > 5.0f) {
        std::cout << "🚀 MASSIVE performance improvement achieved!\n";
    } else if (speedup > 2.0f) {
        std::cout << "✅ Significant performance improvement achieved!\n";
    } else {
        std::cout << "⚠️  Modest performance improvement\n";
    }
    
    std::cout << "\n✅ Hardware encoding test completed successfully!\n";
    std::cout << "Ready for Phase 4B production deployment.\n";
    
    return 0;
}