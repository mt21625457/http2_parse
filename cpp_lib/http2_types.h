#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <span> // C++20, but good practice

namespace http2 {

// Common type aliases
using stream_id_t = uint32_t;
using window_size_t = uint32_t;

// HTTP/2 Frame Types (RFC 7540 Section 11.2)
enum class FrameType : uint8_t {
    DATA = 0x0,
    HEADERS = 0x1,
    PRIORITY = 0x2,
    RST_STREAM = 0x3,
    SETTINGS = 0x4,
    PUSH_PROMISE = 0x5,
    PING = 0x6,
    GOAWAY = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION = 0x9,
    // Add others if needed, e.g. ALTSVC, ORIGIN
};

// Error Codes (RFC 7540 Section 7)
enum class ErrorCode : uint32_t {
    NO_ERROR = 0x0,
    PROTOCOL_ERROR = 0x1,
    INTERNAL_ERROR = 0x2,
    FLOW_CONTROL_ERROR = 0x3,
    SETTINGS_TIMEOUT = 0x4,
    STREAM_CLOSED = 0x5,
    FRAME_SIZE_ERROR = 0x6,
    REFUSED_STREAM = 0x7,
    CANCEL = 0x8,
    COMPRESSION_ERROR = 0x9,
    CONNECT_ERROR = 0xa,
    ENHANCE_YOUR_CALM = 0xb,
    INADEQUATE_SECURITY = 0xc,
    HTTP_1_1_REQUIRED = 0xd
};

struct HttpHeader {
    std::string name;
    std::string value;
    bool sensitive = false; // For HPACK
};

// Max frame size default and limits
constexpr uint32_t DEFAULT_MAX_FRAME_SIZE = 16384; // 2^14
constexpr uint32_t MAX_ALLOWED_FRAME_SIZE = 16777215; // 2^24 - 1
constexpr uint32_t MAX_ALLOWED_WINDOW_SIZE = (1U << 31) -1;


// For specifying priority when sending HEADERS or PRIORITY frames
struct PriorityData {
    bool exclusive_dependency;
    stream_id_t stream_dependency;
    uint8_t weight; // Represents weight value 1-256, so store as 0-255 (actual value sent on wire)
};


} // namespace http2
