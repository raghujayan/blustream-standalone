#include "blustream/common/error_codes.h"
#include <sstream>

namespace blustream {
namespace common {

const char* error_code_to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::SUCCESS: return "Success";
        
        // General errors
        case ErrorCode::UNKNOWN_ERROR: return "Unknown error";
        case ErrorCode::INVALID_PARAMETER: return "Invalid parameter";
        case ErrorCode::OUT_OF_MEMORY: return "Out of memory";
        case ErrorCode::NOT_IMPLEMENTED: return "Not implemented";
        case ErrorCode::TIMEOUT: return "Timeout";
        
        // Authentication errors
        case ErrorCode::AUTH_FAILED: return "Authentication failed";
        case ErrorCode::INVALID_TOKEN: return "Invalid token";
        case ErrorCode::TOKEN_EXPIRED: return "Token expired";
        case ErrorCode::UNAUTHORIZED: return "Unauthorized";
        
        // Session errors
        case ErrorCode::SESSION_NOT_FOUND: return "Session not found";
        case ErrorCode::SESSION_ALREADY_EXISTS: return "Session already exists";
        case ErrorCode::SESSION_LIMIT_EXCEEDED: return "Session limit exceeded";
        case ErrorCode::SESSION_TERMINATED: return "Session terminated";
        
        // HueSpace/VDS errors
        case ErrorCode::VDS_LOAD_FAILED: return "VDS load failed";
        case ErrorCode::VDS_NOT_FOUND: return "VDS not found";
        case ErrorCode::VDS_CORRUPTED: return "VDS corrupted";
        case ErrorCode::HUESPACE_INIT_FAILED: return "HueSpace initialization failed";
        case ErrorCode::RENDER_FAILED: return "Render failed";
        
        // Streaming errors
        case ErrorCode::WEBRTC_INIT_FAILED: return "WebRTC initialization failed";
        case ErrorCode::ENCODING_FAILED: return "Encoding failed";
        case ErrorCode::DECODING_FAILED: return "Decoding failed";
        case ErrorCode::NETWORK_ERROR: return "Network error";
        case ErrorCode::PEER_CONNECTION_FAILED: return "Peer connection failed";
        
        // Hardware errors
        case ErrorCode::CUDA_ERROR: return "CUDA error";
        case ErrorCode::OPENGL_ERROR: return "OpenGL error";
        case ErrorCode::NVENC_ERROR: return "NVENC error";
        case ErrorCode::GPU_NOT_AVAILABLE: return "GPU not available";
        
        // Client errors
        case ErrorCode::CLIENT_DISCONNECTED: return "Client disconnected";
        case ErrorCode::CLIENT_VERSION_MISMATCH: return "Client version mismatch";
        case ErrorCode::INPUT_EVENT_INVALID: return "Input event invalid";
        case ErrorCode::DISPLAY_ERROR: return "Display error";
        
        default: return "Unknown error code";
    }
}

std::string format_error(ErrorCode code, const std::string& message) {
    std::ostringstream oss;
    oss << "[" << static_cast<int32_t>(code) << "] " << error_code_to_string(code);
    if (!message.empty()) {
        oss << ": " << message;
    }
    return oss.str();
}

// BluStreamException implementation
BluStreamException::BluStreamException(ErrorCode code, const std::string& message)
    : code_(code), message_(message) {}

const char* BluStreamException::what() const noexcept {
    if (formatted_message_.empty()) {
        formatted_message_ = format_error(code_, message_);
    }
    return formatted_message_.c_str();
}

}  // namespace common
}  // namespace blustream