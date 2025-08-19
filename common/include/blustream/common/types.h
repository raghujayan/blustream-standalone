#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace blustream {
namespace common {

// Core type definitions
using TimePoint = std::chrono::high_resolution_clock::time_point;
using Duration = std::chrono::nanoseconds;

// Session management
using SessionId = std::string;
using UserId = std::string;

// Video streaming types
struct VideoFrame {
    uint32_t width;
    uint32_t height;
    uint32_t format;  // Pixel format (e.g., YUV, RGB)
    TimePoint timestamp;
    std::vector<uint8_t> data;
};

struct StreamingConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate_kbps = 5000;  // kbps
    uint32_t target_width = 1920;
    uint32_t target_height = 1080;
    uint32_t target_fps = 60;
    uint32_t target_bitrate = 5000000;  // bps
    bool hardware_encoding = true;
};

// Performance metrics
struct PerformanceMetrics {
    Duration render_time{0};
    Duration capture_time{0};
    Duration encode_time{0};
    Duration network_rtt{0};
    Duration decode_time{0};
    Duration total_latency{0};
    uint32_t frame_count = 0;
    uint32_t dropped_frames = 0;
};

// Video codec types
enum class VideoCodec : uint32_t {
    H264 = 0,
    H265 = 1,
    VP8 = 2,
    VP9 = 3,
    AV1 = 4
};

// Network protocol types
enum class MessageType : uint8_t {
    HANDSHAKE = 0x01,
    AUTH_REQUEST = 0x02,
    AUTH_RESPONSE = 0x03,
    SESSION_START = 0x04,
    SESSION_END = 0x05,
    INPUT_EVENT = 0x06,
    CAMERA_CONTROL = 0x07,
    METRICS_UPDATE = 0x08,
    CONFIG = 0x09,
    FRAME = 0x0A,
    ERROR = 0xFF
};

// Message header for streaming protocol (Phase 4 server format)
struct MessageHeader {
    uint32_t magic;       // Magic number 'BSTR' (0x42535452)
    uint32_t version;     // Protocol version
    uint32_t type;        // MessageType enum value
    uint32_t payload_size;// Size of payload following header
    uint32_t sequence;    // Sequence number
    uint32_t timestamp;   // Timestamp in milliseconds
    uint32_t checksum;    // CRC32 checksum of payload
    uint32_t reserved;    // Reserved for future use
};

// Stream configuration message (Phase 4 server format)
struct StreamConfig {
    uint32_t width;
    uint32_t height;
    float fps;
    VideoCodec codec;
    uint32_t bitrate_kbps;
};

struct Message {
    MessageType type;
    SessionId session_id;
    std::vector<uint8_t> payload;
    TimePoint timestamp;
};

// Input event types
enum class InputType : uint8_t {
    MOUSE_MOVE = 0x01,
    MOUSE_BUTTON = 0x02,
    KEYBOARD = 0x03,
    TOUCH = 0x04
};

struct InputEvent {
    InputType type;
    TimePoint timestamp;
    std::vector<uint8_t> data;
};

// Camera control
struct CameraState {
    float position[3];
    float target[3];
    float up[3];
    float fov;
    float near_plane;
    float far_plane;
};

}  // namespace common
}  // namespace blustream