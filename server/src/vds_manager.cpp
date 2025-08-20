#include "blustream/server/vds_manager.h"
#include "blustream/common/logger.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <random>
#include <thread>
#include <limits>
#include <algorithm>

// HueSpace includes - based on the sample code provided
#include <HueSpace3/ProxyInterfaceFactory.h>
#include <HueSpace3/ProxyInterface.h>
#include <HueSpace3/CacheConfigurer.h>
#include <HueSpace3/ConfigMemoryManagement.h>
#include <HueSpace3/VolumeData.h>
#include <HueSpace3/VolumeDataAccess.h>
#include <HueSpace3/BLOB.h>
#include <HueSpace3/Workspace.h>
#include <HueSpace3/SceneManager.h>
#include <HueSpace3/Scene.h>
#include <HueSpace3/ProjectManager.h>
#include <HueSpace3/Project.h>
#include <HueSpace3/VDS.h>
#include <HueSpace3/VDSManager.h>
#include <HueSpace3/VolumeDataChannelDescriptor.h>

namespace blustream {
namespace server {

VDSManager::VDSManager() 
    : proxy_interface_(nullptr)
    , current_vds_(nullptr) {
    vds_data_.width = 0;
    vds_data_.height = 0;
    vds_data_.depth = 0;
    vds_data_.min_value = 0.0f;
    vds_data_.max_value = 1.0f;
}

VDSManager::~VDSManager() {
    shutdown();
}

bool VDSManager::initialize() {
    try {
        BLUSTREAM_LOG_INFO("Initializing HueSpace VDS Manager");
        
        // Create HueSpace proxy interface
        auto* proxy = Hue::ProxyLib::ProxyInterfaceFactory::CreateProxyInterface();
        if (!proxy) {
            BLUSTREAM_LOG_ERROR("Failed to create HueSpace proxy interface");
            return false;
        }
        
        proxy_interface_ = proxy;
        
        // Configure memory management
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetRegisterCUDACallback(false);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->CacheConfigurer()->SetEnabled(false);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetProcessingCPUCacheMax(512);
        
        // Enable CPU processing threads
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread0(true);
        Hue::ProxyLib::ConfigMemoryManagement::Instance()->SetEnableProcessingThread1(true);
        
        BLUSTREAM_LOG_INFO("HueSpace proxy interface created successfully");
        return true;
        
    } catch (const std::exception& e) {
        BLUSTREAM_LOG_ERROR("Exception during HueSpace initialization: " + std::string(e.what()));
        return false;
    }
}

void VDSManager::shutdown() {
    if (current_vds_) {
        // VDS objects are managed by HueSpace, no explicit deletion needed
        current_vds_ = nullptr;
    }
    
    if (proxy_interface_) {
        auto* proxy = static_cast<Hue::ProxyLib::IProxyInterface*>(proxy_interface_);
        proxy->Release();
        proxy_interface_ = nullptr;
    }
    
    vds_data_.data.clear();
}

bool VDSManager::load_from_file(const std::string& file_path) {
    if (!proxy_interface_) {
        BLUSTREAM_LOG_ERROR("HueSpace not initialized");
        return false;
    }
    
    try {
        BLUSTREAM_LOG_INFO("Loading VDS from file: " + file_path);
        
        // Create project using the correct API pattern from sample
        Hue::ProxyLib::Project* project = Hue::ProxyLib::Workspace::Instance()->Scenes().Create()->Projects().Create();
        if (!project) {
            BLUSTREAM_LOG_ERROR("Failed to create HueSpace project");
            return false;
        }
        
        // Load VDS from file using the correct API
        Hue::ProxyLib::VDS* vds = project->VDSs().RestoreVDSFromFileName(file_path);
        if (!vds) {
            BLUSTREAM_LOG_ERROR("Failed to load VDS from file: " + file_path);
            return false;
        }
        
        // Set cache policy for optimal reading
        vds->SetCachePolicy(Hue::ProxyLib::VDSCachePolicy::TimeoutImmediately);
        
        // Store the VDS
        current_vds_ = static_cast<void*>(vds);
        
        // Extract VDS dimensions and data
        if (!extract_vds_data()) {
            BLUSTREAM_LOG_ERROR("Failed to extract VDS data");
            current_vds_ = nullptr;
            return false;
        }
        
        BLUSTREAM_LOG_INFO("VDS loaded successfully: " + 
                          std::to_string(vds_data_.width) + "x" + 
                          std::to_string(vds_data_.height) + "x" + 
                          std::to_string(vds_data_.depth));
        return true;
        
    } catch (const std::exception& e) {
        BLUSTREAM_LOG_ERROR("Exception loading VDS file: " + std::string(e.what()));
        current_vds_ = nullptr;
        return false;
    }
}

bool VDSManager::create_noise_volume(int width, int height, int depth, float noise_scale) {
    BLUSTREAM_LOG_INFO("Creating synthetic noise volume: " + 
                      std::to_string(width) + "x" + 
                      std::to_string(height) + "x" + 
                      std::to_string(depth));
    
    // Clear existing VDS
    current_vds_ = nullptr;
    
    // Create synthetic VDS data
    vds_data_.width = width;
    vds_data_.height = height;
    vds_data_.depth = depth;
    vds_data_.data.resize(width * height * depth);
    
    float min_val = std::numeric_limits<float>::max();
    float max_val = std::numeric_limits<float>::lowest();
    
    // Generate 3D Perlin-like noise
    for (int z = 0; z < depth; z++) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float value = generate_noise_value(x, y, z, noise_scale);
                
                int index = z * (width * height) + y * width + x;
                vds_data_.data[index] = value;
                
                min_val = std::min(min_val, value);
                max_val = std::max(max_val, value);
            }
        }
    }
    
    vds_data_.min_value = min_val;
    vds_data_.max_value = max_val;
    
    BLUSTREAM_LOG_INFO("Synthetic noise volume created. Value range: " + 
                      std::to_string(min_val) + " to " + std::to_string(max_val));
    
    // Mark as having VDS data (synthetic)
    current_vds_ = reinterpret_cast<void*>(0x1);  // Non-null marker
    
    return true;
}

std::vector<float> VDSManager::get_slice_data(int axis, int index) const {
    if (!has_vds()) {
        return {};
    }
    
    std::vector<float> slice_data;
    
    // If we have real VDS data loaded, use it
    if (!vds_data_.data.empty()) {
        // Extract from loaded VDS data
        switch (axis) {
            case 0: { // X-axis slice (YZ plane)
                if (index < 0 || index >= vds_data_.width) return {};
                slice_data.resize(vds_data_.height * vds_data_.depth);
                
                for (int z = 0; z < vds_data_.depth; z++) {
                    for (int y = 0; y < vds_data_.height; y++) {
                        int src_idx = z * (vds_data_.width * vds_data_.height) + y * vds_data_.width + index;
                        int dst_idx = z * vds_data_.height + y;
                        slice_data[dst_idx] = vds_data_.data[src_idx];
                    }
                }
                break;
            }
            
            case 1: { // Y-axis slice (XZ plane)
                if (index < 0 || index >= vds_data_.height) return {};
                slice_data.resize(vds_data_.width * vds_data_.depth);
                
                for (int z = 0; z < vds_data_.depth; z++) {
                    for (int x = 0; x < vds_data_.width; x++) {
                        int src_idx = z * (vds_data_.width * vds_data_.height) + index * vds_data_.width + x;
                        int dst_idx = z * vds_data_.width + x;
                        slice_data[dst_idx] = vds_data_.data[src_idx];
                    }
                }
                break;
            }
            
            case 2: { // Z-axis slice (XY plane)
                if (index < 0 || index >= vds_data_.depth) return {};
                slice_data.resize(vds_data_.width * vds_data_.height);
                
                int base_idx = index * (vds_data_.width * vds_data_.height);
                for (int i = 0; i < vds_data_.width * vds_data_.height; i++) {
                    slice_data[i] = vds_data_.data[base_idx + i];
                }
                break;
            }
            
            default:
                return {};
        }
    } else {
        // Generate synthetic noise data for the slice
        switch (axis) {
            case 0: { // X-axis slice (YZ plane)
                if (index < 0 || index >= vds_data_.width) return {};
                slice_data.resize(vds_data_.height * vds_data_.depth);
                
                for (int z = 0; z < vds_data_.depth; z++) {
                    for (int y = 0; y < vds_data_.height; y++) {
                        int dst_idx = z * vds_data_.height + y;
                        slice_data[dst_idx] = generate_noise_value(index, y, z, 0.05f);
                    }
                }
                break;
            }
            
            case 1: { // Y-axis slice (XZ plane)
                if (index < 0 || index >= vds_data_.height) return {};
                slice_data.resize(vds_data_.width * vds_data_.depth);
                
                for (int z = 0; z < vds_data_.depth; z++) {
                    for (int x = 0; x < vds_data_.width; x++) {
                        int dst_idx = z * vds_data_.width + x;
                        slice_data[dst_idx] = generate_noise_value(x, index, z, 0.05f);
                    }
                }
                break;
            }
            
            case 2: { // Z-axis slice (XY plane)
                if (index < 0 || index >= vds_data_.depth) return {};
                slice_data.resize(vds_data_.width * vds_data_.height);
                
                for (int y = 0; y < vds_data_.height; y++) {
                    for (int x = 0; x < vds_data_.width; x++) {
                        int dst_idx = y * vds_data_.width + x;
                        slice_data[dst_idx] = generate_noise_value(x, y, index, 0.05f);
                    }
                }
                break;
            }
            
            default:
                return {};
        }
    }
    
    return slice_data;
}

std::vector<uint8_t> VDSManager::get_slice_rgb(int axis, int index) const {
    auto slice_data = get_slice_data(axis, index);
    if (slice_data.empty()) {
        return {};
    }
    
    return float_to_rgb(slice_data);
}

std::vector<float> VDSManager::get_animated_slice_data(const std::string& orientation, float time, float duration) const {
    if (!has_vds()) {
        return {};
    }
    
    // Calculate animated position based on time
    int axis;
    int max_slices;
    
    if (orientation == "XY") {
        axis = 2; // Z-axis (time slices)
        max_slices = vds_data_.depth;
    } else if (orientation == "XZ") {
        axis = 1; // Y-axis (inline sections) - DEFAULT for wiggles
        max_slices = vds_data_.height;
    } else if (orientation == "YZ") {
        axis = 0; // X-axis (crossline sections)
        max_slices = vds_data_.width;
    } else {
        // Default to XZ (vertical sections)
        axis = 1;
        max_slices = vds_data_.height;
    }
    
    // Calculate current slice index based on animation time
    float progress = fmod(time, duration) / duration; // 0.0 to 1.0
    int slice_index = static_cast<int>(progress * (max_slices - 1));
    slice_index = std::max(0, std::min(slice_index, max_slices - 1));
    
    return get_slice_data(axis, slice_index);
}

std::vector<uint8_t> VDSManager::get_animated_slice_rgb(const std::string& orientation, float time, float duration) const {
    auto slice_data = get_animated_slice_data(orientation, time, duration);
    if (slice_data.empty()) {
        return {};
    }
    return float_to_rgb(slice_data);
}

void VDSManager::get_slice_dimensions(const std::string& orientation, int& width, int& height) const {
    if (orientation == "XY") {
        width = vds_data_.width;   // X dimension
        height = vds_data_.height; // Y dimension
    } else if (orientation == "XZ") {
        width = vds_data_.width;   // X dimension  
        height = vds_data_.depth;  // Z dimension (vertical)
    } else if (orientation == "YZ") {
        width = vds_data_.height;  // Y dimension
        height = vds_data_.depth;  // Z dimension (vertical)
    } else {
        // Default to XZ
        width = vds_data_.width;
        height = vds_data_.depth;
    }
}

bool VDSManager::extract_vds_data() {
    if (!current_vds_ || current_vds_ == reinterpret_cast<void*>(0x1)) {
        return true;  // Synthetic data, already handled
    }
    
    try {
        auto* vds = static_cast<Hue::ProxyLib::VDS*>(current_vds_);
        
        // Get VDS layout using the correct API pattern
        const Hue::HueSpaceLib::VolumeDataLayout* layout = 
            Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->GetVolumeDataLayout(*vds->GetHueObj());
        
        if (!layout) {
            BLUSTREAM_LOG_ERROR("Failed to get VDS layout");
            return false;
        }
        
        // Extract dimensions
        vds_data_.width = layout->GetDimensionNumSamples(0);
        vds_data_.height = layout->GetDimensionNumSamples(1);
        vds_data_.depth = layout->GetDimensionNumSamples(2);
        
        BLUSTREAM_LOG_INFO("VDS dimensions: " + 
                          std::to_string(vds_data_.width) + "x" + 
                          std::to_string(vds_data_.height) + "x" + 
                          std::to_string(vds_data_.depth));
        
        // Extract real VDS data using RequestVolumeSubset API  
        // Use safe sampling to avoid memory allocation errors
        int sample_width = std::min(512, vds_data_.width);     // Safe width 
        int sample_height = std::min(1024, vds_data_.height);  // Safe height
        int sample_depth = std::min(512, vds_data_.depth);     // Safe depth
        
        // Allocate buffer for real VDS data extraction
        vds_data_.data.resize(sample_width * sample_height * sample_depth);
        
        // Set up coordinates for VDS extraction (centered sampling for stability)
        int start_x = (vds_data_.width - sample_width) / 2;   // Center
        int start_y = (vds_data_.height - sample_height) / 2; // Center  
        int start_z = (vds_data_.depth - sample_depth) / 2;   // Center
        
        int startRead[6];
        int endRead[6];
        
        // Set up coordinate ranges for sampling
        startRead[0] = start_x;
        startRead[1] = start_y;
        startRead[2] = start_z;
        startRead[3] = 0;
        startRead[4] = 0;
        startRead[5] = 0;
        
        endRead[0] = start_x + sample_width;
        endRead[1] = start_y + sample_height;
        endRead[2] = start_z + sample_depth;
        endRead[3] = 1;
        endRead[4] = 1;
        endRead[5] = 1;
        
        BLUSTREAM_LOG_INFO("Extracting real VDS data from coordinates: " +
                          std::to_string(start_x) + "," + std::to_string(start_y) + "," + std::to_string(start_z) +
                          " to " + std::to_string(start_x + sample_width) + "," + 
                          std::to_string(start_y + sample_height) + "," + std::to_string(start_z + sample_depth));
        
        // Create buffer for raw VDS data (as uint8)
        std::vector<uint8_t> raw_vds_buffer(sample_width * sample_height * sample_depth);
        
        // Request real VDS data using the same pattern as working standalone code
        Hue::ProxyLib::int64 requestID = Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->RequestVolumeSubset(
            raw_vds_buffer.data(),           // output buffer
            layout,                          // VDS layout
            Hue::HueSpaceLib::DimensionGroup012,  // dimension group (xyz)
            0,                               // LOD
            0,                               // channel
            startRead,                       // start coordinates
            endRead,                         // end coordinates
            Hue::HueSpaceLib::VolumeDataChannelDescriptor::Format_U8   // 8-bit unsigned format
        );
        
        // Wait for the request to complete
        Hue::ProxyLib::ProxyInterface::GetVolumeDataAccessInterface()->WaitForCompletion(requestID);
        
        // Convert the extracted real VDS data from uint8 to float
        vds_data_.data.resize(raw_vds_buffer.size());
        for (size_t i = 0; i < raw_vds_buffer.size(); i++) {
            vds_data_.data[i] = static_cast<float>(raw_vds_buffer[i]);
        }
        
        // Calculate min/max values from the REAL VDS data
        float min_val = std::numeric_limits<float>::max();
        float max_val = std::numeric_limits<float>::lowest();
        
        for (float value : vds_data_.data) {
            if (std::isfinite(value)) {  // Skip NaN/inf values
                min_val = std::min(min_val, value);
                max_val = std::max(max_val, value);
            }
        }
        
        vds_data_.min_value = min_val;
        vds_data_.max_value = max_val;
        
        // Update dimensions to match extracted data
        vds_data_.width = sample_width;
        vds_data_.height = sample_height;
        vds_data_.depth = sample_depth;
        
        BLUSTREAM_LOG_INFO("✓ Extracted REAL VDS seismic data! Range: " + 
                          std::to_string(vds_data_.min_value) + " to " + std::to_string(vds_data_.max_value) + 
                          " Dimensions: " + std::to_string(sample_width) + "x" + 
                          std::to_string(sample_height) + "x" + std::to_string(sample_depth));
        
        return true;
        
    } catch (const std::exception& e) {
        BLUSTREAM_LOG_ERROR("Exception extracting VDS data: " + std::string(e.what()));
        return false;
    }
}

std::vector<uint8_t> VDSManager::float_to_rgb(const std::vector<float>& data) const {
    std::vector<uint8_t> rgb_data(data.size() * 3);
    
    for (size_t i = 0; i < data.size(); i++) {
        float normalized = normalize_value(data[i]);
        
        // Enhanced seismic visualization with smoother rendering to reduce graininess
        uint8_t intensity;
        
        // Apply gentler gamma correction and smoothing for less grainy appearance
        float smoothed = normalized;
        
        // Apply Gaussian-like smoothing curve to reduce graininess
        float smooth_factor = 1.0f - std::exp(-normalized * 3.0f);
        smoothed = normalized * 0.7f + smooth_factor * 0.3f;
        
        // Apply gentler gamma correction
        float gamma_corrected = std::pow(smoothed, 0.6f);
        
        // Create smooth seismic-style color mapping with reduced contrast jumps
        if (normalized < 0.05f) {
            // Very low amplitude - smooth dark
            intensity = static_cast<uint8_t>(smoothed * 20.0f * 255.0f);
        } else if (normalized > 0.95f) {
            // Very high amplitude - smooth bright
            intensity = static_cast<uint8_t>(255.0f * (0.8f + 0.2f * smoothed));
        } else {
            // Mid range - smooth gradient with enhanced contrast
            intensity = static_cast<uint8_t>(gamma_corrected * 255.0f);
        }
        
        rgb_data[i * 3 + 0] = intensity;  // R
        rgb_data[i * 3 + 1] = intensity;  // G
        rgb_data[i * 3 + 2] = intensity;  // B
    }
    
    return rgb_data;
}

float VDSManager::normalize_value(float value) const {
    if (vds_data_.max_value <= vds_data_.min_value) {
        return 0.0f;
    }
    
    float normalized = (value - vds_data_.min_value) / (vds_data_.max_value - vds_data_.min_value);
    return std::clamp(normalized, 0.0f, 1.0f);
}

float VDSManager::generate_noise_value(int x, int y, int z, float scale) const {
    // Simple 3D noise function using sine waves
    float fx = static_cast<float>(x) * scale;
    float fy = static_cast<float>(y) * scale;
    float fz = static_cast<float>(z) * scale;
    
    float noise = 0.0f;
    
    // Multiple octaves for more interesting noise
    noise += 0.5f * std::sin(fx * 0.1f) * std::cos(fy * 0.1f) * std::sin(fz * 0.1f);
    noise += 0.25f * std::sin(fx * 0.2f) * std::cos(fy * 0.2f) * std::sin(fz * 0.2f);
    noise += 0.125f * std::sin(fx * 0.4f) * std::cos(fy * 0.4f) * std::sin(fz * 0.4f);
    
    // Add some randomness based on position
    std::hash<int> hasher;
    float seed = static_cast<float>(hasher(x * 73856093 ^ y * 19349663 ^ z * 83492791)) / 
                 static_cast<float>(std::numeric_limits<int>::max());
    noise += 0.1f * (seed - 0.5f);
    
    return noise;
}

} // namespace server
} // namespace blustream
