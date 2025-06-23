#include "http2_frame_serializer.h"
#include <algorithm> // for std::copy, std::min
#include <vector>

namespace http2 {
namespace FrameSerializer {

// --- Network byte order helpers (Big Endian) ---
static void write_uint16_big_endian(std::vector<std::byte>& buffer, uint16_t value) {
    buffer.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<std::byte>(value & 0xFF));
}

static void write_uint24_big_endian(std::vector<std::byte>& buffer, uint32_t value) {
    buffer.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<std::byte>(value & 0xFF));
}

static void write_uint32_big_endian(std::vector<std::byte>& buffer, uint32_t value) {
    buffer.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<std::byte>(value & 0xFF));
}


void write_frame_header(std::vector<std::byte>& buffer, const FrameHeader& header) {
    // Ensure buffer has space or use buffer.insert if it's pre-sized.
    // For simplicity, using push_back.
    write_uint24_big_endian(buffer, header.length);
    buffer.push_back(static_cast<std::byte>(header.type));
    buffer.push_back(static_cast<std::byte>(header.flags));
    write_uint32_big_endian(buffer, header.stream_id & 0x7FFFFFFF); // Mask R bit (must be 0 when sending)
}

std::vector<std::byte> serialize_data_frame(const DataFrame& frame) {
    std::vector<std::byte> buffer;
    // FrameHeader needs its length field calculated based on payload.
    FrameHeader header_to_write = frame.header; // Copy to modify length

    size_t payload_size = 0;
    if (frame.has_padded_flag()) {
        payload_size += 1; // Pad Length field
    }
    payload_size += frame.data.size();
    if (frame.has_padded_flag()) {
        payload_size += frame.pad_length.value_or(0);
    }
    header_to_write.length = static_cast<uint32_t>(payload_size);

    write_frame_header(buffer, header_to_write);

    if (frame.has_padded_flag()) {
        buffer.push_back(static_cast<std::byte>(frame.pad_length.value_or(0)));
    }
    buffer.insert(buffer.end(), frame.data.begin(), frame.data.end());
    if (frame.has_padded_flag() && frame.pad_length.value_or(0) > 0) {
        std::vector<std::byte> padding(frame.pad_length.value_or(0), static_cast<std::byte>(0));
        buffer.insert(buffer.end(), padding.begin(), padding.end());
    }
    return buffer;
}

std::vector<std::byte> serialize_headers_frame(const HeadersFrame& frame, HpackEncoder& hpack_encoder) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header; // Copy to modify length

    std::vector<std::byte> payload_buffer;
    if (frame.has_padded_flag()) {
        payload_buffer.push_back(static_cast<std::byte>(frame.pad_length.value_or(0)));
    }
    if (frame.has_priority_flag()) {
        uint32_t stream_dep_val = frame.stream_dependency.value_or(0) & 0x7FFFFFFF;
        if (frame.exclusive_dependency.value_or(false)) {
            stream_dep_val |= (1U << 31);
        }
        write_uint32_big_endian(payload_buffer, stream_dep_val);
        payload_buffer.push_back(static_cast<std::byte>(frame.weight.value_or(0)));
    }

    auto [encoded_headers, hpack_err] = hpack_encoder.encode(frame.headers);
    // TODO: Handle hpack_err properly, maybe throw or return optional/pair
    if (hpack_err != HpackEncodingError::OK) {
        // This indicates an issue with HPACK encoding itself.
        // Depending on policy, could return empty or throw.
        return {};
    }
    payload_buffer.insert(payload_buffer.end(), encoded_headers.begin(), encoded_headers.end());

    if (frame.has_padded_flag() && frame.pad_length.value_or(0) > 0) {
        std::vector<std::byte> padding(frame.pad_length.value_or(0), static_cast<std::byte>(0));
        payload_buffer.insert(payload_buffer.end(), padding.begin(), padding.end());
    }

    header_to_write.length = static_cast<uint32_t>(payload_buffer.size());
    write_frame_header(buffer, header_to_write);
    buffer.insert(buffer.end(), payload_buffer.begin(), payload_buffer.end());

    return buffer;
}

std::vector<std::byte> serialize_priority_frame(const PriorityFrame& frame) {
    std::vector<std::byte> buffer;
    // PRIORITY frame payload is exactly 5 bytes. Header length must reflect this.
    FrameHeader header_to_write = frame.header;
    header_to_write.length = 5;
    write_frame_header(buffer, header_to_write);

    uint32_t stream_dep_val = frame.stream_dependency & 0x7FFFFFFF;
    if (frame.exclusive_dependency) {
        stream_dep_val |= (1U << 31);
    }
    write_uint32_big_endian(buffer, stream_dep_val);
    buffer.push_back(static_cast<std::byte>(frame.weight)); // Weight is weight-1, but spec means actual value here.
                                                          // RFC 7540 Section 6.3: "a single octet containing the weight for the stream (a value from 0 to 255)."
                                                          // The value stored is `weight - 1`. So sending `weight` field means `weight-1`.
                                                          // Let's assume frame.weight is the actual value (1-256). So send frame.weight -1.
                                                          // But the struct has uint8_t, so it should be value 0-255 representing weight 1-256.
                                                          // The field is "Weight: An 8-bit..." this is the value to send.
    return buffer;
}

std::vector<std::byte> serialize_rst_stream_frame(const RstStreamFrame& frame) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;
    header_to_write.length = 4; // RST_STREAM payload is 4 bytes (error code)
    write_frame_header(buffer, header_to_write);
    write_uint32_big_endian(buffer, static_cast<uint32_t>(frame.error_code));
    return buffer;
}

std::vector<std::byte> serialize_settings_frame(const SettingsFrame& frame) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;

    std::vector<std::byte> payload_buffer;
    if (!frame.has_ack_flag()) {
        for (const auto& setting : frame.settings) {
            write_uint16_big_endian(payload_buffer, setting.identifier);
            write_uint32_big_endian(payload_buffer, setting.value);
        }
    }
    // If ACK flag is set, payload must be empty. header.length should be 0.
    header_to_write.length = static_cast<uint32_t>(payload_buffer.size());
    write_frame_header(buffer, header_to_write);
    if (!payload_buffer.empty()) {
        buffer.insert(buffer.end(), payload_buffer.begin(), payload_buffer.end());
    }
    return buffer;
}

std::vector<std::byte> serialize_push_promise_frame(const PushPromiseFrame& frame, HpackEncoder& hpack_encoder) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;

    std::vector<std::byte> payload_buffer;
    if (frame.has_padded_flag()) {
        payload_buffer.push_back(static_cast<std::byte>(frame.pad_length.value_or(0)));
    }
    // Promised Stream ID (31 bits, R bit is 0)
    write_uint32_big_endian(payload_buffer, frame.promised_stream_id & 0x7FFFFFFF);

    auto [encoded_headers, hpack_err] = hpack_encoder.encode(frame.headers);
    if (hpack_err != HpackEncodingError::OK) {
        return {}; // Error during HPACK encoding
    }
    payload_buffer.insert(payload_buffer.end(), encoded_headers.begin(), encoded_headers.end());

    if (frame.has_padded_flag() && frame.pad_length.value_or(0) > 0) {
        std::vector<std::byte> padding(frame.pad_length.value_or(0), static_cast<std::byte>(0));
        payload_buffer.insert(payload_buffer.end(), padding.begin(), padding.end());
    }

    header_to_write.length = static_cast<uint32_t>(payload_buffer.size());
    write_frame_header(buffer, header_to_write);
    buffer.insert(buffer.end(), payload_buffer.begin(), payload_buffer.end());
    return buffer;
}

std::vector<std::byte> serialize_ping_frame(const PingFrame& frame) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;
    header_to_write.length = 8; // PING payload is 8 bytes
    write_frame_header(buffer, header_to_write);
    buffer.insert(buffer.end(), frame.opaque_data.begin(), frame.opaque_data.end());
    return buffer;
}

std::vector<std::byte> serialize_goaway_frame(const GoAwayFrame& frame) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;

    std::vector<std::byte> payload_buffer;
    write_uint32_big_endian(payload_buffer, frame.last_stream_id & 0x7FFFFFFF); // R bit must be 0
    write_uint32_big_endian(payload_buffer, static_cast<uint32_t>(frame.error_code));
    if (!frame.additional_debug_data.empty()) {
        payload_buffer.insert(payload_buffer.end(), frame.additional_debug_data.begin(), frame.additional_debug_data.end());
    }

    header_to_write.length = static_cast<uint32_t>(payload_buffer.size());
    write_frame_header(buffer, header_to_write);
    buffer.insert(buffer.end(), payload_buffer.begin(), payload_buffer.end());
    return buffer;
}

std::vector<std::byte> serialize_window_update_frame(const WindowUpdateFrame& frame) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;
    header_to_write.length = 4; // WINDOW_UPDATE payload is 4 bytes
    write_frame_header(buffer, header_to_write);
    // Window Size Increment (31 bits, R bit is 0)
    write_uint32_big_endian(buffer, frame.window_size_increment & 0x7FFFFFFF);
    return buffer;
}

std::vector<std::byte> serialize_continuation_frame(const ContinuationFrame& frame) {
    std::vector<std::byte> buffer;
    FrameHeader header_to_write = frame.header;
    // The payload is the raw header_block_fragment, already HPACKed.
    header_to_write.length = static_cast<uint32_t>(frame.header_block_fragment.size());
    write_frame_header(buffer, header_to_write);
    buffer.insert(buffer.end(), frame.header_block_fragment.begin(), frame.header_block_fragment.end());
    return buffer;
}


SerializedHeaderSequence serialize_header_block_with_continuation(
    const FrameHeader& initial_header_template, // Base header for the first (HEADERS/PUSH_PROMISE) frame
    const std::vector<HttpHeader>& headers_to_encode,
    HpackEncoder& hpack_encoder,
    uint32_t peer_max_frame_size,
    bool is_push_promise,
    stream_id_t promised_stream_id_if_push) {

    SerializedHeaderSequence result;
    auto [full_hpack_block, hpack_err] = hpack_encoder.encode(headers_to_encode);
    if (hpack_err != HpackEncodingError::OK) {
        // Handle error, perhaps return empty or throw
        return result;
    }

    FrameHeader current_header = initial_header_template; // Copy base
    current_header.flags &= ~HeadersFrame::END_HEADERS_FLAG; // Clear END_HEADERS initially

    size_t initial_payload_offset = 0;
    std::vector<std::byte> initial_payload_prefix;

    // Handle PUSH_PROMISE specific fields (Promised Stream ID)
    // Also PADDED and PRIORITY flags for both HEADERS and PUSH_PROMISE (not implemented here for simplicity)
    // This simplified version assumes no PADDING or PRIORITY on initial frame for now.
    if (is_push_promise) {
        // Add Promised Stream ID to the payload prefix
        write_uint32_big_endian(initial_payload_prefix, promised_stream_id_if_push & 0x7FFFFFFF);
        initial_payload_offset = 4; // Size of Promised Stream ID
    }

    size_t remaining_hpack_size = full_hpack_block.size();
    size_t hpack_offset = 0;

    // Max payload for the first frame (HEADERS or PUSH_PROMISE)
    uint32_t max_first_frame_payload = peer_max_frame_size - static_cast<uint32_t>(initial_payload_prefix.size());

    size_t first_frame_hpack_chunk_size = std::min(remaining_hpack_size, static_cast<size_t>(max_first_frame_payload));

    std::vector<std::byte> first_frame_payload = initial_payload_prefix;
    first_frame_payload.insert(first_frame_payload.end(),
                               full_hpack_block.begin() + hpack_offset,
                               full_hpack_block.begin() + hpack_offset + first_frame_hpack_chunk_size);

    current_header.length = static_cast<uint32_t>(first_frame_payload.size());
    hpack_offset += first_frame_hpack_chunk_size;
    remaining_hpack_size -= first_frame_hpack_chunk_size;

    if (remaining_hpack_size == 0) {
        current_header.flags |= HeadersFrame::END_HEADERS_FLAG;
    }

    // Serialize the first frame (HEADERS or PUSH_PROMISE)
    write_frame_header(result.headers_frame_bytes, current_header);
    result.headers_frame_bytes.insert(result.headers_frame_bytes.end(), first_frame_payload.begin(), first_frame_payload.end());

    // Serialize CONTINUATION frames if any
    while (remaining_hpack_size > 0) {
        ContinuationFrame cont_frame_obj;
        cont_frame_obj.header.type = FrameType::CONTINUATION;
        cont_frame_obj.header.stream_id = initial_header_template.stream_id; // Must be same stream
        cont_frame_obj.header.flags = 0; // Clear END_HEADERS initially for continuation

        size_t continuation_hpack_chunk_size = std::min(remaining_hpack_size, static_cast<size_t>(peer_max_frame_size));

        cont_frame_obj.header_block_fragment.assign(
            full_hpack_block.begin() + hpack_offset,
            full_hpack_block.begin() + hpack_offset + continuation_hpack_chunk_size
        );

        hpack_offset += continuation_hpack_chunk_size;
        remaining_hpack_size -= continuation_hpack_chunk_size;

        if (remaining_hpack_size == 0) {
            cont_frame_obj.header.flags |= ContinuationFrame::END_HEADERS_FLAG;
        }
        cont_frame_obj.header.length = static_cast<uint32_t>(cont_frame_obj.header_block_fragment.size());

        result.continuation_frames_bytes.push_back(serialize_continuation_frame(cont_frame_obj));
    }

    return result;
}


} // namespace FrameSerializer
} // namespace http2
