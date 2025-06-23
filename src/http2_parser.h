#pragma once

#include "http2_types.h"
#include "http2_frame.h"
#include "hpack_decoder.h" // Parser uses HPACK decoder for HEADERS, PUSH_PROMISE, CONTINUATION

#include <vector>
#include <functional>
#include <optional>
#include <span> // C++20

namespace http2 {

// Forward declaration
class Http2Connection; // The parser is typically owned by a connection object

// Parser error conditions
enum class ParserError {
    OK,
    BUFFER_TOO_SMALL,          // Not enough data to parse a complete frame/field
    INVALID_FRAME_TYPE,
    INVALID_FRAME_SIZE,
    INVALID_FRAME_FLAGS,
    INVALID_STREAM_ID,
    INVALID_PADDING,
    INVALID_PRIORITY_DATA,
    INVALID_SETTINGS_VALUE,
    INVALID_WINDOW_UPDATE_INCREMENT,
    HPACK_DECOMPRESSION_FAILED,
    PROTOCOL_ERROR,             // General protocol violation
    INTERNAL_ERROR,             // Internal parser error
    CONTINUATION_EXPECTED,      // Expected CONTINUATION frame, got something else
    CONTINUATION_WRONG_STREAM,  // CONTINUATION frame for unexpected stream ID
    FRAME_SIZE_LIMIT_EXCEEDED,
    // Add more specific errors as needed
};

class Http2Parser {
public:
    // The parser itself doesn't directly invoke callbacks for fully formed semantic frames like "HeadersFrame".
    // Instead, it parses the raw bytes into a structure like AnyHttp2Frame.
    // The Http2Connection (or a similar higher-level component) would then interpret this AnyHttp2Frame
    // and invoke semantic callbacks.
    // However, the parser might have a callback for when a complete frame is parsed.
    using RawFrameParsedCallback = std::function<void(AnyHttp2Frame frame)>;

    // The parser needs access to the HPACK decoder, which is typically managed by the connection
    // due to its statefulness and SETTINGS_HEADER_TABLE_SIZE updates.
    Http2Parser(HpackDecoder& hpack_decoder, Http2Connection& connection_context);


    // Parses a chunk of incoming data.
    // Returns the number of bytes consumed from the input span.
    // If an error occurs, it might return 0 or a negative value, or throw an exception,
    // or in C++23, return std::expected<size_t, ParserError>.
    // For now, let's use a pair: <bytes_consumed, ParserError>
    std::pair<size_t, ParserError> parse(std::span<const std::byte> data);

    void set_raw_frame_parsed_callback(RawFrameParsedCallback cb);

    // Resets parser state, e.g., if the connection is reset.
    // Does not reset HPACK decoder state, as that's managed by Http2Connection.
    void reset();


private:
    // Internal parsing state
    enum class State {
        READING_FRAME_HEADER,
        READING_FRAME_PAYLOAD,
        // Potentially states for CONTINUATION if not handled by connection
    };

    State current_state_ = State::READING_FRAME_HEADER;
    std::vector<std::byte> buffer_; // Internal buffer for accumulating partial frames

    FrameHeader pending_frame_header_; // Header of the frame currently being parsed

    // Reference to the connection's HPACK decoder
    HpackDecoder& hpack_decoder_;
    // Reference to the connection context for settings like max_frame_size
    // and for handling CONTINUATION logic across frames.
    Http2Connection& connection_context_;


    RawFrameParsedCallback raw_frame_parsed_cb_;

    // --- Frame-specific parsing functions ---
    // These take a span of the payload data and the frame header.
    // They return an AnyHttp2Frame or a ParserError.
    // In C++23, std::expected<AnyHttp2Frame, ParserError> would be ideal.

    std::pair<AnyHttp2Frame, ParserError> parse_frame_payload(const FrameHeader& header, std::span<const std::byte> payload);

    std::pair<AnyHttp2Frame, ParserError> parse_data_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_headers_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_priority_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_rst_stream_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_settings_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_push_promise_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_ping_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_goaway_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_window_update_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_continuation_payload(const FrameHeader& header, std::span<const std::byte> payload);
    std::pair<AnyHttp2Frame, ParserError> parse_unknown_payload(const FrameHeader& header, std::span<const std::byte> payload);


    // Helper to read the 9-byte frame header
    std::optional<FrameHeader> read_frame_header(std::span<const std::byte>& data);

    // Max frame size allowed by the peer (obtained from connection settings)
    uint32_t get_remote_max_frame_size() const;
};

} // namespace http2
