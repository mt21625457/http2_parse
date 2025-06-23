#include "http2_parser.h"
#include "http2_connection.h" // For context like max_frame_size and HPACK
#include <arpa/inet.h> // For ntohl, ntohs (or equivalent for other platforms)
#include <algorithm> // For std::copy, std::min
#include <iostream>  // For debugging

// Helper to read a big-endian 24-bit integer (frame length)
uint32_t read_uint24_big_endian(const std::byte* buffer) {
    return (static_cast<uint32_t>(buffer[0]) << 16) |
           (static_cast<uint32_t>(buffer[1]) << 8)  |
           (static_cast<uint32_t>(buffer[2]));
}

// Helper to read a big-endian 32-bit integer
uint32_t read_uint32_big_endian(const std::byte* buffer) {
    return (static_cast<uint32_t>(buffer[0]) << 24) |
           (static_cast<uint32_t>(buffer[1]) << 16) |
           (static_cast<uint32_t>(buffer[2]) << 8)  |
           (static_cast<uint32_t>(buffer[3]));
}
// Helper to read a big-endian 16-bit integer
uint16_t read_uint16_big_endian(const std::byte* buffer) {
    return (static_cast<uint16_t>(buffer[0]) << 8) |
           (static_cast<uint16_t>(buffer[1]));
}


namespace http2 {

Http2Parser::Http2Parser(HpackDecoder& hpack_decoder, Http2Connection& connection_context)
    : hpack_decoder_(hpack_decoder), connection_context_(connection_context) {
}

void Http2Parser::set_raw_frame_parsed_callback(RawFrameParsedCallback cb) {
    raw_frame_parsed_cb_ = std::move(cb);
}

void Http2Parser::reset() {
    current_state_ = State::READING_FRAME_HEADER;
    buffer_.clear();
    // Does not reset HPACK decoder; that's connection's responsibility.
}

uint32_t Http2Parser::get_remote_max_frame_size() const {
    // Get this from connection context, which knows remote settings
    return connection_context_.get_remote_settings().max_frame_size;
}


std::optional<FrameHeader> Http2Parser::read_frame_header(std::span<const std::byte>& data) {
    if (data.size() < 9) { // Frame header is 9 bytes
        return std::nullopt;
    }

    FrameHeader header;
    header.length = read_uint24_big_endian(data.data());
    header.type = static_cast<FrameType>(data[3]);
    header.flags = static_cast<uint8_t>(data[4]);
    // Stream ID is 31 bits, highest bit is reserved (R)
    header.stream_id = read_uint32_big_endian(data.data() + 5) & 0x7FFFFFFF; // Mask out R bit

    // Basic validation
    // Max frame size check against remote peer's SETTINGS_MAX_FRAME_SIZE
    if (header.length > get_remote_max_frame_size()) {
        // This is a FRAME_SIZE_ERROR, connection should handle.
        // For now, the parser notes it by returning an error later.
        // Or, parser could have an error state.
    }

    data = data.subspan(9); // Consume header bytes
    return header;
}


std::pair<size_t, ParserError> Http2Parser::parse(std::span<const std::byte> data) {
    size_t total_consumed_bytes = 0;
    ParserError last_error = ParserError::OK;

    // Append new data to internal buffer
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    while (true) {
        if (current_state_ == State::READING_FRAME_HEADER) {
            if (buffer_.size() < 9) { // Not enough data for a header
                break;
            }
            std::span<const std::byte> header_span(buffer_.data(), 9);
            auto header_opt = read_frame_header(header_span); // header_span is not advanced by read_frame_header here
                                                              // because we are operating on buffer_

            if (!header_opt) { // Should not happen if size >= 9, unless read_frame_header has internal errors
                last_error = ParserError::INTERNAL_ERROR; // Should be more specific
                break;
            }
            pending_frame_header_ = header_opt.value();

            if (pending_frame_header_.length > connection_context_.get_remote_settings().max_frame_size) {
                last_error = ParserError::FRAME_SIZE_LIMIT_EXCEEDED;
                // Connection should send GOAWAY with FRAME_SIZE_ERROR
                // For now, parser stops and reports.
                break;
            }

            current_state_ = State::READING_FRAME_PAYLOAD;
            // Do not consume header from buffer_ yet, wait until payload is also read.
        }

        if (current_state_ == State::READING_FRAME_PAYLOAD) {
            if (buffer_.size() < (9 + pending_frame_header_.length)) { // Not enough data for header + payload
                break;
            }

            // We have enough data for the full frame (header + payload)
            std::span<const std::byte> payload_span(buffer_.data() + 9, pending_frame_header_.length);
            AnyHttp2Frame parsed_frame_variant{DataFrame{}}; // Dummy init
            ParserError parse_payload_error = ParserError::OK;

            // --- Frame Type Dispatch ---
            switch (pending_frame_header_.type) {
                case FrameType::DATA: {
                    auto [frame, err] = parse_data_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::HEADERS: {
                    auto [frame, err] = parse_headers_payload(pending_frame_header_, payload_span);
                     if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::PRIORITY: {
                    auto [frame, err] = parse_priority_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::RST_STREAM: {
                    auto [frame, err] = parse_rst_stream_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::SETTINGS: {
                    auto [frame, err] = parse_settings_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::PUSH_PROMISE: {
                     auto [frame, err] = parse_push_promise_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::PING: {
                    auto [frame, err] = parse_ping_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::GOAWAY: {
                    auto [frame, err] = parse_goaway_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::WINDOW_UPDATE: {
                    auto [frame, err] = parse_window_update_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                case FrameType::CONTINUATION: {
                    auto [frame, err] = parse_continuation_payload(pending_frame_header_, payload_span);
                    if (err == ParserError::OK) parsed_frame_variant = AnyHttp2Frame(frame);
                    parse_payload_error = err;
                    break;
                }
                default:
                    // Unknown frame type. Per RFC 7540 Sec 4.1:
                    // "Implementations MUST ignore and discard any frame that has a type that is unknown."
                    // So, we consume it but don't callback or error, unless strict.
                    // For now, let's treat as an error to be aware.
                    parse_payload_error = ParserError::INVALID_FRAME_TYPE;
            }

            size_t frame_total_size = 9 + pending_frame_header_.length;
            if (parse_payload_error == ParserError::OK) {
                if (raw_frame_parsed_cb_) {
                    raw_frame_parsed_cb_(parsed_frame_variant);
                }
                total_consumed_bytes += frame_total_size;
                buffer_.erase(buffer_.begin(), buffer_.begin() + frame_total_size);
                current_state_ = State::READING_FRAME_HEADER; // Ready for next frame
                last_error = ParserError::OK;
            } else {
                last_error = parse_payload_error;
                // On error, stop parsing. The connection might need to send GOAWAY.
                // How much is consumed depends on error recovery strategy.
                // For now, assume fatal error, nothing more consumed from this point.
                // A more robust parser might try to skip the erroneous frame if possible.
                break;
            }
        } // end if READING_FRAME_PAYLOAD

        if (last_error != ParserError::OK) break; // Exit loop if error occurred
        if (buffer_.empty() && current_state_ == State::READING_FRAME_HEADER) break; // No more data and ready for new frame.
        if (buffer_.size() < 9 && current_state_ == State::READING_FRAME_HEADER) break; // Not enough for next header
    } // end while(true)

    return {total_consumed_bytes, last_error};
}


// --- Frame-specific payload parsing functions ---
// These are simplified stubs. Real implementation needs careful handling of flags, padding, etc.

std::pair<DataFrame, ParserError> Http2Parser::parse_data_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    DataFrame frame;
    frame.header = header;
    size_t current_offset = 0;

    if (header.stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID}; // DATA frames MUST be on non-zero stream

    if (frame.has_padded_flag()) {
        if (payload.empty()) return {{}, ParserError::INVALID_PADDING}; // Need at least 1 byte for Pad Length
        frame.pad_length = static_cast<uint8_t>(payload[0]);
        current_offset += 1;
        if (frame.pad_length.value() > (payload.size() - current_offset) ) {
             return {{}, ParserError::INVALID_PADDING}; // Pad Length > remaining payload
        }
    } else {
        frame.pad_length = 0; // For convenience, even if not optional
    }

    size_t data_length = payload.size() - current_offset - frame.pad_length.value_or(0);
    if (static_cast<int>(data_length) < 0) return {{}, ParserError::INVALID_PADDING}; // Should be caught by above

    frame.data.assign(payload.begin() + current_offset, payload.begin() + current_offset + data_length);
    // Padding data is implicitly at the end, not stored in frame.data.

    return {frame, ParserError::OK};
}

std::pair<HeadersFrame, ParserError> Http2Parser::parse_headers_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    HeadersFrame frame;
    frame.header = header;
    size_t current_offset = 0;

    if (header.stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID};

    if (frame.has_padded_flag()) {
        if (payload.empty()) return {{}, ParserError::INVALID_PADDING};
        frame.pad_length = static_cast<uint8_t>(payload[0]);
        current_offset += 1;
        if (frame.pad_length.value() > (payload.size() - current_offset)) {
            return {{}, ParserError::INVALID_PADDING};
        }
    }

    if (frame.has_priority_flag()) {
        if ((payload.size() - current_offset) < 5) return {{}, ParserError::INVALID_PRIORITY_DATA};
        uint32_t stream_dep_raw = read_uint32_big_endian(payload.data() + current_offset);
        frame.exclusive_dependency = (stream_dep_raw >> 31) & 0x1;
        frame.stream_dependency = stream_dep_raw & 0x7FFFFFFF;
        frame.weight = static_cast<uint8_t>(payload[current_offset + 4]); // Weight is 1+value
        current_offset += 5;
    }

    size_t header_block_fragment_len = payload.size() - current_offset - frame.pad_length.value_or(0);
    if (static_cast<int>(header_block_fragment_len) < 0) return {{}, ParserError::INVALID_PADDING};

    std::span<const std::byte> hpack_payload = payload.subspan(current_offset, header_block_fragment_len);

    // The Http2Parser itself doesn't maintain the "current header block".
    // It passes the fragment to the connection, which manages HPACK decoding across CONTINUATIONs.
    // For now, let's assume Http2Connection handles this logic.
    // This parser's job is to extract the fragment.
    // The actual HPACK decoding and header list population happens in Http2Connection.
    // So, this HeadersFrame will contain the raw fragment for now.
    // This is a simplification; the parser might be more tightly coupled with HPACK state
    // or the connection would pass the raw fragment to the hpack_decoder directly.

    // Let's simulate what the connection would do for now for basic parsing
    // This is NOT how it would work with CONTINUATION.
    // This part needs to be handled by HttpConnection using connection_context_.get_hpack_decoder()
    // and managing header_block_buffer_ and expected_continuation_stream_id_
    // For a standalone HEADERS frame (no CONTINUATION):
    // if (!connection_context_.is_expecting_continuation()) { // This check is tricky if multiple streams interleave non-CONTINUATION frames
    //      connection_context_.clear_header_block_buffer(); // Start fresh only if not already in a sequence for this stream
    // }
    // The connection context should manage buffering per stream if necessary, or globally if only one sequence is allowed at a time.
    // For now, global buffer is used. Clear it if starting a new sequence.
    if (!connection_context_.is_expecting_continuation() ||
        (connection_context_.is_expecting_continuation() && header.get_stream_id() != connection_context_.get_expected_continuation_stream_id())) {
        connection_context_.clear_header_block_buffer(); // New header sequence starts
    }

    connection_context_.append_to_header_block_buffer(hpack_payload);

    if (frame.has_end_headers_flag()) {
        auto [decoded_headers, hpack_err] = hpack_decoder_.decode(connection_context_.get_header_block_buffer_span());
        if (hpack_err != HpackError::OK) {
            connection_context_.clear_header_block_buffer(); // Clear buffer on error
            connection_context_.finish_continuation();       // Reset continuation state
            return {{}, ParserError::HPACK_DECOMPRESSION_FAILED};
        }
        frame.headers = std::move(decoded_headers);
        connection_context_.clear_header_block_buffer();
        connection_context_.finish_continuation();
    } else {
        // Expect CONTINUATION
        connection_context_.expect_continuation_for_stream(header.get_stream_id(), FrameType::HEADERS, AnyHttp2Frame(frame));
    }

    return {frame, ParserError::OK};
}

std::pair<PriorityFrame, ParserError> Http2Parser::parse_priority_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    PriorityFrame frame;
    frame.header = header;
    if (header.length != 5) return {{}, ParserError::INVALID_FRAME_SIZE}; // PRIORITY payload is exactly 5 bytes
    if (header.stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID};

    uint32_t stream_dep_raw = read_uint32_big_endian(payload.data());
    frame.exclusive_dependency = (stream_dep_raw >> 31) & 0x1;
    frame.stream_dependency = stream_dep_raw & 0x7FFFFFFF;
    frame.weight = static_cast<uint8_t>(payload[4]); // Weight is 1+value

    return {frame, ParserError::OK};
}

std::pair<RstStreamFrame, ParserError> Http2Parser::parse_rst_stream_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    RstStreamFrame frame;
    frame.header = header;
    if (header.length != 4) return {{}, ParserError::INVALID_FRAME_SIZE};
    if (header.stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID};

    frame.error_code = static_cast<ErrorCode>(read_uint32_big_endian(payload.data()));
    return {frame, ParserError::OK};
}

std::pair<SettingsFrame, ParserError> Http2Parser::parse_settings_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    SettingsFrame frame;
    frame.header = header;

    if (header.stream_id != 0) return {{}, ParserError::INVALID_STREAM_ID}; // SETTINGS MUST be on stream 0

    if (frame.has_ack_flag()) {
        if (header.length != 0) return {{}, ParserError::INVALID_FRAME_SIZE}; // ACK SETTINGS must have 0 length
        return {frame, ParserError::OK}; // No payload to parse
    }

    if (header.length % 6 != 0) return {{}, ParserError::INVALID_FRAME_SIZE}; // Each setting is 6 bytes

    for (size_t offset = 0; offset < header.length; offset += 6) {
        SettingsFrame::Setting setting;
        setting.identifier = read_uint16_big_endian(payload.data() + offset);
        setting.value = read_uint32_big_endian(payload.data() + offset + 2);
        // TODO: Validate setting identifiers and values per RFC 7540 Section 6.5.2
        frame.settings.push_back(setting);
    }
    return {frame, ParserError::OK};
}

std::pair<PushPromiseFrame, ParserError> Http2Parser::parse_push_promise_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    PushPromiseFrame frame;
    frame.header = header;
     size_t current_offset = 0;

    if (header.stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID}; // Must be on existing client stream
    if (connection_context_.is_server()) return {{}, ParserError::PROTOCOL_ERROR}; // Client cannot send PUSH_PROMISE

    if (frame.has_padded_flag()) {
        if (payload.empty()) return {{}, ParserError::INVALID_PADDING};
        frame.pad_length = static_cast<uint8_t>(payload[0]);
        current_offset += 1;
         if (frame.pad_length.value() > (payload.size() - current_offset)) {
            return {{}, ParserError::INVALID_PADDING};
        }
    }
    if ((payload.size() - current_offset) < 4) return {{}, ParserError::INVALID_FRAME_SIZE}; // Need at least 4 bytes for Promised Stream ID

    frame.promised_stream_id = read_uint32_big_endian(payload.data() + current_offset) & 0x7FFFFFFF; // Mask R bit
    current_offset += 4;
    if (frame.promised_stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID}; // Promised stream ID cannot be 0.

    size_t header_block_fragment_len = payload.size() - current_offset - frame.pad_length.value_or(0);
    if (static_cast<int>(header_block_fragment_len) < 0) return {{}, ParserError::INVALID_PADDING};

    std::span<const std::byte> hpack_payload = payload.subspan(current_offset, header_block_fragment_len);

    if (!connection_context_.is_expecting_continuation() ||
        (connection_context_.is_expecting_continuation() && header.get_stream_id() != connection_context_.get_expected_continuation_stream_id())) {
        connection_context_.clear_header_block_buffer(); // New header sequence for this PUSH_PROMISE
    }
    connection_context_.append_to_header_block_buffer(hpack_payload);

    if (frame.has_end_headers_flag()) {
        auto [decoded_headers, hpack_err] = hpack_decoder_.decode(connection_context_.get_header_block_buffer_span());
        if (hpack_err != HpackError::OK) {
            connection_context_.clear_header_block_buffer();
            connection_context_.finish_continuation();
            return {{}, ParserError::HPACK_DECOMPRESSION_FAILED};
        }
        frame.headers = std::move(decoded_headers);
        connection_context_.clear_header_block_buffer();
        connection_context_.finish_continuation();
    } else {
        // Continuation expected for the parent stream of PUSH_PROMISE, carrying these promised headers.
        connection_context_.expect_continuation_for_stream(header.get_stream_id(), FrameType::PUSH_PROMISE, AnyHttp2Frame(frame));
    }

    return {frame, ParserError::OK};
}

std::pair<PingFrame, ParserError> Http2Parser::parse_ping_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    PingFrame frame;
    frame.header = header;
    if (header.length != 8) return {{}, ParserError::INVALID_FRAME_SIZE};
    if (header.stream_id != 0) return {{}, ParserError::INVALID_STREAM_ID};

    std::copy(payload.begin(), payload.end(), frame.opaque_data.begin());
    return {frame, ParserError::OK};
}

std::pair<GoAwayFrame, ParserError> Http2Parser::parse_goaway_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    GoAwayFrame frame;
    frame.header = header;
    if (header.length < 8) return {{}, ParserError::INVALID_FRAME_SIZE}; // Must be at least 8 bytes
    if (header.stream_id != 0) return {{}, ParserError::INVALID_STREAM_ID};

    frame.last_stream_id = read_uint32_big_endian(payload.data()) & 0x7FFFFFFF; // Mask R bit
    frame.error_code = static_cast<ErrorCode>(read_uint32_big_endian(payload.data() + 4));

    if (header.length > 8) {
        frame.additional_debug_data.assign(payload.begin() + 8, payload.end());
    }
    return {frame, ParserError::OK};
}

std::pair<WindowUpdateFrame, ParserError> Http2Parser::parse_window_update_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    WindowUpdateFrame frame;
    frame.header = header;
    if (header.length != 4) return {{}, ParserError::INVALID_FRAME_SIZE};

    frame.window_size_increment = read_uint32_big_endian(payload.data()) & 0x7FFFFFFF; // Mask R bit
    if (frame.window_size_increment == 0) return {{}, ParserError::INVALID_WINDOW_UPDATE_INCREMENT};

    return {frame, ParserError::OK};
}

std::pair<ContinuationFrame, ParserError> Http2Parser::parse_continuation_payload(const FrameHeader& header, std::span<const std::byte> payload) {
    ContinuationFrame frame;
    frame.header = header;

    if (!connection_context_.is_expecting_continuation()) {
        return {{}, ParserError::PROTOCOL_ERROR}; // CONTINUATION without preceding HEADERS/PUSH_PROMISE
    }
    if (header.stream_id != connection_context_.get_expected_continuation_stream_id()) {
        return {{}, ParserError::CONTINUATION_WRONG_STREAM};
    }
    if (header.stream_id == 0) return {{}, ParserError::INVALID_STREAM_ID};


    // The header_block_fragment is the entire payload of CONTINUATION
    // It's appended to the connection's buffer.
    connection_context_.append_to_header_block_buffer(payload);

    if (frame.has_end_headers_flag()) {
        auto [decoded_headers, hpack_err] = hpack_decoder_.decode(connection_context_.get_header_block_buffer_span());
         if (hpack_err != HpackError::OK) {
            return {{}, ParserError::HPACK_DECOMPRESSION_FAILED};
        }
        // The decoded headers are associated with the original HEADERS/PUSH_PROMISE frame,
        // not directly with this ContinuationFrame object in terms of storing them.
        // The Http2Connection will handle associating these decoded headers with the correct stream/event.
        // For now, we can store them in the ContinuationFrame if desired, or rely on connection.
        // Let's assume the connection handles it. For this frame object, the raw fragment is enough.
        frame.header_block_fragment.assign(payload.begin(), payload.end()); // Store raw for this specific frame object

        // The HttpConnection needs to be notified to populate its original HeadersFrame/PushPromiseFrame
    // The `populate_pending_headers` method will update the stored initiator frame.
    connection_context_.populate_pending_headers(std::move(decoded_headers));
    connection_context_.clear_header_block_buffer();
    // finish_continuation also triggers the callback for the now-complete initiator frame.
    connection_context_.finish_continuation();
    } else {
        // Still expecting more CONTINUATION frames
        frame.header_block_fragment.assign(payload.begin(), payload.end());
    }

    return {frame, ParserError::OK};
}


} // namespace http2
