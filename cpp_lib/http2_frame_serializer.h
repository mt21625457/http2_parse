#pragma once

#include "http2_frame.h"
#include "hpack_encoder.h" // Needed for serializing frames with headers
#include <vector>
#include <cstddef> // for std::byte

namespace http2 {

// Forward declaration of HpackEncoder if only used by .cpp,
// but it's likely needed for function signatures if passed around.

namespace FrameSerializer {

// --- Helper to write the 9-byte frame header ---
void write_frame_header(std::vector<std::byte>& buffer, const FrameHeader& header);

// --- Serialization functions for each frame type ---

// DATA Frame (RFC 7540 Section 6.1)
std::vector<std::byte> serialize_data_frame(const DataFrame& frame);

// HEADERS Frame (RFC 7540 Section 6.2)
// Requires an HPACK encoder instance.
std::vector<std::byte> serialize_headers_frame(const HeadersFrame& frame, HpackEncoder& hpack_encoder);

// PRIORITY Frame (RFC 7540 Section 6.3)
std::vector<std::byte> serialize_priority_frame(const PriorityFrame& frame);

// RST_STREAM Frame (RFC 7540 Section 6.4)
std::vector<std::byte> serialize_rst_stream_frame(const RstStreamFrame& frame);

// SETTINGS Frame (RFC 7540 Section 6.5)
std::vector<std::byte> serialize_settings_frame(const SettingsFrame& frame);

// PUSH_PROMISE Frame (RFC 7540 Section 6.6)
// Requires an HPACK encoder instance.
std::vector<std::byte> serialize_push_promise_frame(const PushPromiseFrame& frame, HpackEncoder& hpack_encoder);

// PING Frame (RFC 7540 Section 6.7)
std::vector<std::byte> serialize_ping_frame(const PingFrame& frame);

// GOAWAY Frame (RFC 7540 Section 6.8)
std::vector<std::byte> serialize_goaway_frame(const GoAwayFrame& frame);

// WINDOW_UPDATE Frame (RFC 7540 Section 6.9)
std::vector<std::byte> serialize_window_update_frame(const WindowUpdateFrame& frame);

// CONTINUATION Frame (RFC 7540 Section 6.10)
// Requires the header block fragment (already HPACKed) to be passed in,
// as CONTINUATION frames don't invoke HPACK themselves but carry its output.
// The HpackEncoder is not directly used by this function, but by the caller preparing fragments.
std::vector<std::byte> serialize_continuation_frame(const ContinuationFrame& frame);


// --- Helper for splitting large header blocks into HEADERS + CONTINUATION(s) ---
// Takes a fully populated HeadersFrame (with all HttpHeader objects) and peer's max frame size.
// Returns a vector of byte vectors, where the first is the HEADERS frame,
// and subsequent are CONTINUATION frames.
// The HpackEncoder is used internally here.
struct SerializedHeaderSequence {
    std::vector<std::byte> headers_frame_bytes;
    std::vector<std::vector<std::byte>> continuation_frames_bytes;
};

SerializedHeaderSequence serialize_header_block_with_continuation(
    const FrameHeader& initial_header, // Base header for the first (HEADERS/PUSH_PROMISE) frame
    const std::vector<HttpHeader>& headers_to_encode,
    HpackEncoder& hpack_encoder,
    uint32_t peer_max_frame_size,
    bool is_push_promise = false, // To determine if it's HEADERS or PUSH_PROMISE specific fields
    stream_id_t promised_stream_id_if_push = 0 // Only if is_push_promise
    // Potentially add padding fields if PADDED flag is intended for HEADERS/PUSH_PROMISE
);


} // namespace FrameSerializer
} // namespace http2
