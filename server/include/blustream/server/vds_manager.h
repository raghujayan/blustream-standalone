#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Forward declarations to avoid including heavy HueSpace headers
namespace Hue {
namespace ProxyLib {
    class IProxyInterface;
    class VDS;
}
}

namespace blustream {
namespace server {

class VDSManager {
public:
    struct VDSData {
        int width;
        int height; 
        int depth;
        std::vector<float> data;
        float min_value;
        float max_value;
    };

    VDSManager();
    ~VDSManager();

    // Initialize HueSpace context
    bool initialize();
    void shutdown();

    // VDS operations
    bool load_from_file(const std::string& file_path);
    bool create_noise_volume(int width, int height, int depth, float noise_scale = 1.0f);
    
    // Get slice data for rendering
    std::vector<float> get_slice_data(int axis, int index) const;
    std::vector<uint8_t> get_slice_rgb(int axis, int index) const;
    
    // Animated slice extraction with time-based positioning
    std::vector<float> get_animated_slice_data(const std::string& orientation, float time, float duration) const;
    std::vector<uint8_t> get_animated_slice_rgb(const std::string& orientation, float time, float duration) const;
    
    // Get slice dimensions for given orientation
    void get_slice_dimensions(const std::string& orientation, int& width, int& height) const;
    
    // Properties
    bool has_vds() const { return current_vds_ != nullptr; }
    int get_width() const { return vds_data_.width; }
    int get_height() const { return vds_data_.height; }
    int get_depth() const { return vds_data_.depth; }
    
    // Value range for visualization
    float get_min_value() const { return vds_data_.min_value; }
    float get_max_value() const { return vds_data_.max_value; }

private:
    // HueSpace objects (using void* to avoid header dependencies)
    void* proxy_interface_;  // Actually Hue::ProxyLib::IProxyInterface*
    void* current_vds_;      // Actually Hue::ProxyLib::VDS*
    
    // Cached VDS data for efficient slice extraction
    VDSData vds_data_;
    
    // Helper methods
    bool extract_vds_data();
    std::vector<uint8_t> float_to_rgb(const std::vector<float>& data) const;
    float normalize_value(float value) const;
    
    // Noise generation
    float generate_noise_value(int x, int y, int z, float scale) const;
};

} // namespace server
} // namespace blustream
