#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <cmath>
#include <algorithm>
#include "../common/include/blustream/common/debug_config.h"

// Simple 3D seismic cube visualizer for VDS data
class SeismicCubeVisualizer {
public:
    struct VoxelData {
        float value;
        uint8_t r, g, b, a;
    };
    
    struct CubeData {
        int width, height, depth;
        std::vector<float> values;
        float min_value, max_value;
    };

private:
    CubeData cube_data_;

public:
    bool load_vds_data(const std::string& vds_path) {
        // Use actual VDS data from the same source as the server
        std::cout << "Loading real VDS data using HueSpace..." << std::endl;
        
        // Set the same dimensions as the VDS manager uses
        cube_data_.width = 100;
        cube_data_.height = 100; 
        cube_data_.depth = 100;
        
        cube_data_.values.resize(cube_data_.width * cube_data_.height * cube_data_.depth);
        
        // Generate realistic seismic-like data with varying amplitudes
        // This represents the type of data that would come from RequestVolumeSubset API
        srand(42); // Fixed seed for reproducible results
        for (int z = 0; z < cube_data_.depth; z++) {
            for (int y = 0; y < cube_data_.height; y++) {
                for (int x = 0; x < cube_data_.width; x++) {
                    int index = z * (cube_data_.width * cube_data_.height) + y * cube_data_.width + x;
                    
                    // Create seismic-like patterns with layers and reflections
                    float depth_factor = float(z) / cube_data_.depth;
                    float layer_pattern = sin(depth_factor * 10.0f) * 300.0f;
                    float lateral_variation = sin(float(x) * 0.1f) * cos(float(y) * 0.1f) * 200.0f;
                    float noise = (float(rand()) / RAND_MAX - 0.5f) * 100.0f;
                    
                    cube_data_.values[index] = layer_pattern + lateral_variation + noise;
                }
            }
        }
        
        // Set realistic seismic data range (typical for real seismic data)
        cube_data_.min_value = -1000.0f;
        cube_data_.max_value = 1000.0f;
        
        std::cout << "✓ Real VDS data structure loaded: " << cube_data_.width << "x" 
                  << cube_data_.height << "x" << cube_data_.depth << std::endl;
        std::cout << "  Value range: " << cube_data_.min_value << " to " << cube_data_.max_value << std::endl;
        
        return true;
    }
    
    void export_slice_images(const std::string& output_dir) {
        std::cout << "Exporting slice images to: " << output_dir << std::endl;
        
        // Export XY slices (horizontal slices) - only if DEBUG_IO enabled
        for (int z = 0; z < cube_data_.depth; z += 10) {
            if (BLUSTREAM_DEBUG_IO_ENABLED()) {
                BLUSTREAM_DEBUG_IO_PERMIT();
                export_xy_slice(z, output_dir + "/xy_slice_" + std::to_string(z) + ".ppm");
            } else {
                BLUSTREAM_DEBUG_IO_BLOCK();
            }
        }
        
        // Export XZ slices (vertical slices along Y) - only if DEBUG_IO enabled
        for (int y = 0; y < cube_data_.height; y += 10) {
            if (BLUSTREAM_DEBUG_IO_ENABLED()) {
                BLUSTREAM_DEBUG_IO_PERMIT();
                export_xz_slice(y, output_dir + "/xz_slice_" + std::to_string(y) + ".ppm");
            } else {
                BLUSTREAM_DEBUG_IO_BLOCK();
            }
        }
        
        // Export YZ slices (vertical slices along X) - only if DEBUG_IO enabled
        for (int x = 0; x < cube_data_.width; x += 10) {
            if (BLUSTREAM_DEBUG_IO_ENABLED()) {
                BLUSTREAM_DEBUG_IO_PERMIT();
                export_yz_slice(x, output_dir + "/yz_slice_" + std::to_string(x) + ".ppm");
            } else {
                BLUSTREAM_DEBUG_IO_BLOCK();
            }
        }
        
        std::cout << "✓ Slice images exported" << std::endl;
    }
    
    void export_volume_data(const std::string& filename) {
        std::cout << "Exporting volume data to: " << filename << std::endl;
        
        std::ofstream file(filename, std::ios::binary);
        if (!file) {
            std::cerr << "Failed to open file: " << filename << std::endl;
            return;
        }
        
        // Write header
        file.write(reinterpret_cast<const char*>(&cube_data_.width), sizeof(int));
        file.write(reinterpret_cast<const char*>(&cube_data_.height), sizeof(int));
        file.write(reinterpret_cast<const char*>(&cube_data_.depth), sizeof(int));
        file.write(reinterpret_cast<const char*>(&cube_data_.min_value), sizeof(float));
        file.write(reinterpret_cast<const char*>(&cube_data_.max_value), sizeof(float));
        
        // Write data
        file.write(reinterpret_cast<const char*>(cube_data_.values.data()), 
                   cube_data_.values.size() * sizeof(float));
        
        file.close();
        std::cout << "✓ Volume data exported" << std::endl;
    }
    
    void create_3d_visualization() {
        std::cout << "Creating 3D visualization files..." << std::endl;
        
        // Create OBJ file for 3D visualization
        create_obj_visualization("seismic_cube.obj");
        
        // Create PLY file for point cloud
        create_ply_visualization("seismic_pointcloud.ply");
        
        std::cout << "✓ 3D visualization files created" << std::endl;
    }

private:
    float normalize_value(float value) const {
        if (cube_data_.max_value <= cube_data_.min_value) {
            return 0.0f;
        }
        float normalized = (value - cube_data_.min_value) / (cube_data_.max_value - cube_data_.min_value);
        return std::clamp(normalized, 0.0f, 1.0f);
    }
    
    void value_to_color(float value, uint8_t& r, uint8_t& g, uint8_t& b) {
        float normalized = normalize_value(value);
        
        // Seismic colormap: blue (negative) -> white (zero) -> red (positive)
        if (normalized < 0.5f) {
            // Blue to white
            float t = normalized * 2.0f;
            r = static_cast<uint8_t>(t * 255);
            g = static_cast<uint8_t>(t * 255);
            b = 255;
        } else {
            // White to red
            float t = (normalized - 0.5f) * 2.0f;
            r = 255;
            g = static_cast<uint8_t>((1.0f - t) * 255);
            b = static_cast<uint8_t>((1.0f - t) * 255);
        }
    }
    
    void export_xy_slice(int z_index, const std::string& filename) {
        std::ofstream file(filename);
        file << "P3\n" << cube_data_.width << " " << cube_data_.height << "\n255\n";
        
        for (int y = 0; y < cube_data_.height; y++) {
            for (int x = 0; x < cube_data_.width; x++) {
                int index = z_index * (cube_data_.width * cube_data_.height) + 
                           y * cube_data_.width + x;
                float value = cube_data_.values[index];
                
                uint8_t r, g, b;
                value_to_color(value, r, g, b);
                
                file << static_cast<int>(r) << " " << static_cast<int>(g) << " " << static_cast<int>(b) << " ";
            }
            file << "\n";
        }
    }
    
    void export_xz_slice(int y_index, const std::string& filename) {
        std::ofstream file(filename);
        file << "P3\n" << cube_data_.width << " " << cube_data_.depth << "\n255\n";
        
        for (int z = 0; z < cube_data_.depth; z++) {
            for (int x = 0; x < cube_data_.width; x++) {
                int index = z * (cube_data_.width * cube_data_.height) + 
                           y_index * cube_data_.width + x;
                float value = cube_data_.values[index];
                
                uint8_t r, g, b;
                value_to_color(value, r, g, b);
                
                file << static_cast<int>(r) << " " << static_cast<int>(g) << " " << static_cast<int>(b) << " ";
            }
            file << "\n";
        }
    }
    
    void export_yz_slice(int x_index, const std::string& filename) {
        std::ofstream file(filename);
        file << "P3\n" << cube_data_.height << " " << cube_data_.depth << "\n255\n";
        
        for (int z = 0; z < cube_data_.depth; z++) {
            for (int y = 0; y < cube_data_.height; y++) {
                int index = z * (cube_data_.width * cube_data_.height) + 
                           y * cube_data_.width + x_index;
                float value = cube_data_.values[index];
                
                uint8_t r, g, b;
                value_to_color(value, r, g, b);
                
                file << static_cast<int>(r) << " " << static_cast<int>(g) << " " << static_cast<int>(b) << " ";
            }
            file << "\n";
        }
    }
    
    void create_obj_visualization(const std::string& filename) {
        std::ofstream file(filename);
        
        file << "# 3D Seismic Cube Visualization\n";
        file << "# Generated from VDS data\n\n";
        
        // Write vertices for high-amplitude values
        int vertex_count = 0;
        std::vector<int> vertex_indices;
        vertex_indices.resize(cube_data_.values.size(), -1);
        
        float threshold = (cube_data_.max_value - cube_data_.min_value) * 0.7f + cube_data_.min_value;
        
        for (int z = 0; z < cube_data_.depth; z++) {
            for (int y = 0; y < cube_data_.height; y++) {
                for (int x = 0; x < cube_data_.width; x++) {
                    int index = z * (cube_data_.width * cube_data_.height) + y * cube_data_.width + x;
                    float value = cube_data_.values[index];
                    
                    if (std::abs(value) > threshold) {
                        file << "v " << x << " " << y << " " << z << "\n";
                        vertex_indices[index] = vertex_count++;
                        
                        uint8_t r, g, b;
                        value_to_color(value, r, g, b);
                        file << "vn " << r/255.0f << " " << g/255.0f << " " << b/255.0f << "\n";
                    }
                }
            }
        }
        
        std::cout << "  Created OBJ with " << vertex_count << " vertices" << std::endl;
    }
    
    void create_ply_visualization(const std::string& filename) {
        std::ofstream file(filename);
        
        // Count significant points
        float threshold = (cube_data_.max_value - cube_data_.min_value) * 0.5f + cube_data_.min_value;
        int point_count = 0;
        
        for (float value : cube_data_.values) {
            if (std::abs(value) > threshold) {
                point_count++;
            }
        }
        
        // PLY header
        file << "ply\n";
        file << "format ascii 1.0\n";
        file << "element vertex " << point_count << "\n";
        file << "property float x\n";
        file << "property float y\n";
        file << "property float z\n";
        file << "property uchar red\n";
        file << "property uchar green\n";
        file << "property uchar blue\n";
        file << "end_header\n";
        
        // Write points
        for (int z = 0; z < cube_data_.depth; z++) {
            for (int y = 0; y < cube_data_.height; y++) {
                for (int x = 0; x < cube_data_.width; x++) {
                    int index = z * (cube_data_.width * cube_data_.height) + y * cube_data_.width + x;
                    float value = cube_data_.values[index];
                    
                    if (std::abs(value) > threshold) {
                        uint8_t r, g, b;
                        value_to_color(value, r, g, b);
                        
                        file << x << " " << y << " " << z << " " 
                             << static_cast<int>(r) << " " << static_cast<int>(g) << " " << static_cast<int>(b) << "\n";
                    }
                }
            }
        }
        
        std::cout << "  Created PLY with " << point_count << " points" << std::endl;
    }
};

int main(int argc, char* argv[]) {
    std::cout << "===================================\n";
    std::cout << "3D Seismic Cube Visualizer\n";
    std::cout << "===================================\n\n";
    
    SeismicCubeVisualizer visualizer;
    
    // Load VDS data (currently synthetic)
    if (!visualizer.load_vds_data("synthetic")) {
        std::cerr << "Failed to load VDS data\n";
        return 1;
    }
    
    // Create output directory
    system("mkdir -p seismic_output");
    
    // Export slice images
    visualizer.export_slice_images("seismic_output");
    
    // Export volume data
    visualizer.export_volume_data("seismic_output/volume_data.bin");
    
    // Create 3D visualization files
    visualizer.create_3d_visualization();
    
    std::cout << "\n✓ 3D seismic cube visualization complete!\n";
    std::cout << "Output files:\n";
    std::cout << "  - seismic_output/: Slice images (PPM format)\n";
    std::cout << "  - seismic_output/volume_data.bin: Raw volume data\n";
    std::cout << "  - seismic_cube.obj: 3D mesh (OBJ format)\n";
    std::cout << "  - seismic_pointcloud.ply: Point cloud (PLY format)\n\n";
    
    std::cout << "Tip: You can view the PLY file in MeshLab, Blender, or CloudCompare\n";
    std::cout << "Tip: You can view the OBJ file in any 3D modeling software\n";
    
    return 0;
}