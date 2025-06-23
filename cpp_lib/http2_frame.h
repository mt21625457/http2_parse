#pragma once

#include "http2_types.h"
#include <vector>
#include <cstdint>
#include <string>
#include <optional> // C++17, consider std::expected for C++23 if available & chosen for error handling

namespace http2 {

// Forward declarations
class HpackDecoder; // To avoid circular dependency if frame needs HPACK context for parsing details

// Common Frame Header structure (RFC 7540 Section 4.1)
struct FrameHeader {
    uint32_t length; // 24 bits
    FrameType type;  // 8 bits
    uint8_t flags;   // 8 bits
    stream_id_t stream_id; // 31 bits (R bit is 0)

    // R bit (reserved, 1 bit) is typically masked out from stream_id
    bool is_r_bit_set() const { return (stream_id >> 31) & 0x1; }
    stream_id_t get_stream_id() const { return stream_id & 0x7FFFFFFF; }
};

// --- Individual Frame Structures ---

struct DataFrame {
    static constexpr FrameType TYPE = FrameType::DATA;
    // Flags for DATA frame (RFC 7540 Section 6.1)
    static constexpr uint8_t END_STREAM_FLAG = 0x1;
    static constexpr uint8_t PADDED_FLAG = 0x8;

    FrameHeader header;
    std::optional<uint8_t> pad_length; // Present if PADDED_FLAG is set
    std::vector<std::byte> data;       // Actual application data
    // Padding data is not stored explicitly, but handled during parsing/serialization

    bool has_end_stream_flag() const { return header.flags & END_STREAM_FLAG; }
    bool has_padded_flag() const { return header.flags & PADDED_FLAG; }
};

struct HeadersFrame {
    static constexpr FrameType TYPE = FrameType::HEADERS;
    // Flags for HEADERS frame (RFC 7540 Section 6.2)
    static constexpr uint8_t END_STREAM_FLAG = 0x1;
    static constexpr uint8_t END_HEADERS_FLAG = 0x4;
    static constexpr uint8_t PADDED_FLAG = 0x8;
    static constexpr uint8_t PRIORITY_FLAG = 0x20;

    FrameHeader header;
    std::optional<uint8_t> pad_length; // Present if PADDED_FLAG is set
    // Priority fields (present if PRIORITY_FLAG is set)
    std::optional<bool> exclusive_dependency; // E bit
    std::optional<stream_id_t> stream_dependency;
    std::optional<uint8_t> weight;

    std::vector<HttpHeader> headers; // Decoded headers
    // Raw header block fragment is processed by HPACK decoder

    bool has_end_stream_flag() const { return header.flags & END_STREAM_FLAG; }
    bool has_end_headers_flag() const { return header.flags & END_HEADERS_FLAG; }
    bool has_padded_flag() const { return header.flags & PADDED_FLAG; }
    bool has_priority_flag() const { return header.flags & PRIORITY_FLAG; }
};

struct PriorityFrame {
    static constexpr FrameType TYPE = FrameType::PRIORITY;
    // No flags defined for PRIORITY frame

    FrameHeader header;
    bool exclusive_dependency; // E bit
    stream_id_t stream_dependency;
    uint8_t weight;
};

struct RstStreamFrame {
    static constexpr FrameType TYPE = FrameType::RST_STREAM;
    // No flags defined for RST_STREAM frame

    FrameHeader header;
    ErrorCode error_code;
};

struct SettingsFrame {
    static constexpr FrameType TYPE = FrameType::SETTINGS;
    // Flags for SETTINGS frame (RFC 7540 Section 6.5)
    static constexpr uint8_t ACK_FLAG = 0x1;

    struct Setting {
        uint16_t identifier;
        uint32_t value;
    };
    // Common Setting Identifiers (RFC 7540 Section 6.5.2)
    static constexpr uint16_t SETTINGS_HEADER_TABLE_SIZE = 0x1;
    static constexpr uint16_t SETTINGS_ENABLE_PUSH = 0x2;
    static constexpr uint16_t SETTINGS_MAX_CONCURRENT_STREAMS = 0x3;
    static constexpr uint16_t SETTINGS_INITIAL_WINDOW_SIZE = 0x4;
    static constexpr uint16_t SETTINGS_MAX_FRAME_SIZE = 0x5;
    static constexpr uint16_t SETTINGS_MAX_HEADER_LIST_SIZE = 0x6;


    FrameHeader header;
    std::vector<Setting> settings;

    bool has_ack_flag() const { return header.flags & ACK_FLAG; }
};

struct PushPromiseFrame {
    static constexpr FrameType TYPE = FrameType::PUSH_PROMISE;
    // Flags for PUSH_PROMISE frame (RFC 7540 Section 6.6)
    static constexpr uint8_t END_HEADERS_FLAG = 0x4;
    static constexpr uint8_t PADDED_FLAG = 0x8;

    FrameHeader header;
    std::optional<uint8_t> pad_length; // Present if PADDED_FLAG is set
    stream_id_t promised_stream_id; // R bit must be 0
    std::vector<HttpHeader> headers; // Decoded headers (header block fragment)

    bool has_end_headers_flag() const { return header.flags & END_HEADERS_FLAG; }
    bool has_padded_flag() const { return header.flags & PADDED_FLAG; }
};

struct PingFrame {
    static constexpr FrameType TYPE = FrameType::PING;
    // Flags for PING frame (RFC 7540 Section 6.7)
    static constexpr uint8_t ACK_FLAG = 0x1;

    FrameHeader header;
    std::array<std::byte, 8> opaque_data;

    bool has_ack_flag() const { return header.flags & ACK_FLAG; }
};

struct GoAwayFrame {
    static constexpr FrameType TYPE = FrameType::GOAWAY;
    // No flags defined for GOAWAY frame

    FrameHeader header;
    stream_id_t last_stream_id; // R bit must be 0
    ErrorCode error_code;
    std::vector<std::byte> additional_debug_data;
};

struct WindowUpdateFrame {
    static constexpr FrameType TYPE = FrameType::WINDOW_UPDATE;
    // No flags defined for WINDOW_UPDATE frame

    FrameHeader header;
    uint32_t window_size_increment; // R bit must be 0
};

struct ContinuationFrame {
    static constexpr FrameType TYPE = FrameType::CONTINUATION;
    // Flags for CONTINUATION frame (RFC 7540 Section 6.10)
    static constexpr uint8_t END_HEADERS_FLAG = 0x4;

    FrameHeader header;
    // Raw header block fragment, to be appended to previous HEADERS/PUSH_PROMISE
    std::vector<std::byte> header_block_fragment;

    bool has_end_headers_flag() const { return header.flags & END_HEADERS_FLAG; }
};

// A variant to hold any of the specific frame types
using Http2FrameVariant = std::variant<
    DataFrame,
    HeadersFrame,
    PriorityFrame,
    RstStreamFrame,
    SettingsFrame,
    PushPromiseFrame,
    PingFrame,
    GoAwayFrame,
    WindowUpdateFrame,
    ContinuationFrame
    // Potentially std::monostate if a default "empty" state is needed
    // or a specific "UnknownFrame" type for extensibility.
>;

// Wrapper class for Http2FrameVariant, potentially holding common methods or properties.
// For now, it mostly serves as a named type for the variant.
class AnyHttp2Frame {
public:
    Http2FrameVariant frame_variant;
    FrameHeader common_header; // Store common header for easy access

    template<typename FrameT>
    AnyHttp2Frame(FrameT frame) : frame_variant(std::move(frame)) {
        // Populate common_header from the specific frame's header
        std::visit([this](const auto& f){
            this->common_header = f.header;
        }, frame_variant);
    }

    // Accessors to get underlying frame if needed, with type checking
    template<typename T>
    const T* get_if() const {
        return std::get_if<T>(&frame_variant);
    }

    template<typename T>
    T* get_if() {
        return std::get_if<T>(&frame_variant);
    }

    FrameType type() const { return common_header.type; }
    stream_id_t stream_id() const { return common_header.get_stream_id(); }
    uint32_t length() const { return common_header.length; }
    uint8_t flags() const { return common_header.flags; }
};

} // namespace http2
