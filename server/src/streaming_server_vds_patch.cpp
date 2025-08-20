// VDS-related updates for streaming_server.cpp

// In constructor, replace:
//    , huespace_context_(nullptr)
//    , current_vds_(nullptr)
// With:
//    , vds_manager_(std::make_unique<VDSManager>())

// In destructor, add:
//    if (vds_manager_) {
//        vds_manager_->shutdown();
//    }

// In initialize method, after OpenGL context creation:
//    // Initialize VDS manager
//    if (!vds_manager_->initialize()) {
//        BLUSTREAM_LOG_ERROR("Failed to initialize VDS manager");
//        return false;
//    }

// Replace load_vds method:
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

// Replace VDS rendering in render_loop:
//        if (current_vds_) {
//            // TODO: Render VDS slice
//            rgb_frame = test_pattern;  // Placeholder
//        } else {
// With:
//        if (vds_manager_ && vds_manager_->has_vds()) {
//            // Render VDS slice
//            auto slice_rgb = vds_manager_->get_slice_rgb(current_slice_axis_, current_slice_index_);
//            if (!slice_rgb.empty()) {
//                // Get slice dimensions
//                int slice_width, slice_height;
//                switch (current_slice_axis_) {
//                    case 0: // YZ plane
//                        slice_width = vds_manager_->get_height();
//                        slice_height = vds_manager_->get_depth();
//                        break;
//                    case 1: // XZ plane  
//                        slice_width = vds_manager_->get_width();
//                        slice_height = vds_manager_->get_depth();
//                        break;
//                    case 2: // XY plane
//                        slice_width = vds_manager_->get_width();
//                        slice_height = vds_manager_->get_height();
//                        break;
//                }
//                
//                // Scale to render size
//                rgb_frame.resize(config_.render_width * config_.render_height * 3);
//                
//                // Simple nearest neighbor scaling
//                for (int y = 0; y < config_.render_height; y++) {
//                    for (int x = 0; x < config_.render_width; x++) {
//                        int src_x = (x * slice_width) / config_.render_width;
//                        int src_y = (y * slice_height) / config_.render_height;
//                        int src_idx = (src_y * slice_width + src_x) * 3;
//                        int dst_idx = (y * config_.render_width + x) * 3;
//                        
//                        if (src_idx + 2 < slice_rgb.size()) {
//                            rgb_frame[dst_idx + 0] = slice_rgb[src_idx + 0];
//                            rgb_frame[dst_idx + 1] = slice_rgb[src_idx + 1];
//                            rgb_frame[dst_idx + 2] = slice_rgb[src_idx + 2];
//                        }
//                    }
//                }
//            } else {
//                rgb_frame = test_pattern;  // Fallback
//            }
//        } else {
