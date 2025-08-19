#pragma once

#include <string>
#include <exception>

namespace blustream {
namespace common {

enum class ErrorCode : int32_t {
    SUCCESS = 0,
    
    // General errors (1000-1999)
    UNKNOWN_ERROR = 1000,
    INVALID_PARAMETER = 1001,
    OUT_OF_MEMORY = 1002,
    NOT_IMPLEMENTED = 1003,
    TIMEOUT = 1004,
    
    // Authentication errors (2000-2999)
    AUTH_FAILED = 2000,
    INVALID_TOKEN = 2001,
    TOKEN_EXPIRED = 2002,
    UNAUTHORIZED = 2003,
    
    // Session errors (3000-3999)
    SESSION_NOT_FOUND = 3000,
    SESSION_ALREADY_EXISTS = 3001,
    SESSION_LIMIT_EXCEEDED = 3002,
    SESSION_TERMINATED = 3003,
    
    // HueSpace/VDS errors (4000-4999)
    VDS_LOAD_FAILED = 4000,
    VDS_NOT_FOUND = 4001,
    VDS_CORRUPTED = 4002,
    HUESPACE_INIT_FAILED = 4003,
    RENDER_FAILED = 4004,
    
    // Streaming errors (5000-5999)
    WEBRTC_INIT_FAILED = 5000,
    ENCODING_FAILED = 5001,
    DECODING_FAILED = 5002,
    NETWORK_ERROR = 5003,
    PEER_CONNECTION_FAILED = 5004,
    
    // Hardware errors (6000-6999)
    CUDA_ERROR = 6000,
    OPENGL_ERROR = 6001,
    NVENC_ERROR = 6002,
    GPU_NOT_AVAILABLE = 6003,
    
    // Client errors (7000-7999)
    CLIENT_DISCONNECTED = 7000,
    CLIENT_VERSION_MISMATCH = 7001,
    INPUT_EVENT_INVALID = 7002,
    DISPLAY_ERROR = 7003
};

const char* error_code_to_string(ErrorCode code);
std::string format_error(ErrorCode code, const std::string& message = "");

// Exception class for BluStream errors
class BluStreamException : public std::exception {
public:
    BluStreamException(ErrorCode code, const std::string& message = "");
    
    const char* what() const noexcept override;
    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

private:
    ErrorCode code_;
    std::string message_;
    mutable std::string formatted_message_;
};

}  // namespace common
}  // namespace blustream