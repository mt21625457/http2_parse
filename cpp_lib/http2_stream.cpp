#include "http2_stream.h"
#include <algorithm> // for std::min, std::max

namespace http2 {

Http2Stream::Http2Stream(stream_id_t id, uint32_t initial_local_window, uint32_t initial_remote_window)
    : id_(id),
      state_(StreamState::IDLE),
      local_window_size_(static_cast<int32_t>(initial_local_window)),
      remote_window_size_(static_cast<int32_t>(initial_remote_window)) {
    if (id == 0) {
        // Stream ID 0 is for connection control, not a typical stream.
        // It doesn't really have a state like other streams.
        // For simplicity, we might not even create Http2Stream objects for ID 0,
        // or handle it specially.
        // If created, its state might be considered always "OPEN" in a sense,
        // but it doesn't transition like other streams.
        // For now, let's assume stream 0 is not managed by this class directly
        // or has very specific handling if it is.
        // This constructor is more for client/server initiated streams (ID > 0).
    }
}

stream_id_t Http2Stream::get_id() const {
    return id_;
}

StreamState Http2Stream::get_state() const {
    return state_;
}

// --- Window Management ---

bool Http2Stream::update_remote_window(window_size_t increment) {
    // Remote window is how much *we* can send. Peer sends WINDOW_UPDATE to increase this.
    if (increment == 0) {
        // A WINDOW_UPDATE frame with a 0 increment can be a PROTOCOL_ERROR
        // for stream-specific updates, but not for connection-level.
        // Let's assume for now it's an error at stream level.
        // transition_to_closed(); // Or signal error
        // return false; // This indicates an error for the stream
        // For now, let's be lenient and just say it had no effect.
        return true; // No change, but not a failure of the update operation itself.
    }

    // Check for window overflow (RFC 7540 Section 6.9.1)
    // "A sender MUST NOT allow a flow-control window to exceed 2^31 - 1 octets."
    if (static_cast<int64_t>(remote_window_size_) + increment > ((1LL << 31) - 1)) {
        // This is a FLOW_CONTROL_ERROR, stream should be reset.
        // transition_to_closed(); // Or signal error to connection to send RST_STREAM
        // For now, just indicate failure to update.
        return false; // Error: window overflow
    }
    remote_window_size_ += static_cast<int32_t>(increment);
    return true;
}

bool Http2Stream::update_local_window(window_size_t increment) {
    // Local window is how much *peer* can send. We send WINDOW_UPDATE to increase this.
    // This function is called when *we* are ready for more data.
    if (increment == 0) { // This should not happen if we are generating the update.
        return true; // No change.
    }
    // Similar overflow check as above, though we are the ones generating it.
    if (static_cast<int64_t>(local_window_size_) + increment > ((1LL << 31) - 1)) {
        // This would be an internal logic error if we try to set it this high.
        return false; // Error: internal logic trying to set window too high
    }
    local_window_size_ += static_cast<int32_t>(increment);
    return true;
}

bool Http2Stream::can_send_data(size_t data_size) const {
    if (state_ != StreamState::OPEN && state_ != StreamState::HALF_CLOSED_REMOTE) {
        return false; // Cannot send data on non-open or locally closed stream
    }
    return remote_window_size_ >= static_cast<int32_t>(data_size);
}

void Http2Stream::record_data_sent(size_t data_size) {
    if (data_size == 0) return;
    remote_window_size_ -= static_cast<int32_t>(data_size);
    // remote_window_size_ can become negative if we send more than allowed.
    // The spec says: "A sender MAY send data that exceeds the flow control window of a stream..."
    // "...However, the receiver treats this as a FLOW_CONTROL_ERROR."
    // So, local accounting can go negative, but the peer will likely reset.
}

void Http2Stream::record_data_received(size_t data_size) {
    if (data_size == 0) return;
    local_window_size_ -= static_cast<int32_t>(data_size);
    // If local_window_size_ becomes negative, it means the peer sent too much data.
    // This should be handled as a FLOW_CONTROL_ERROR by the connection.
}

int32_t Http2Stream::get_local_window_size() const {
    return local_window_size_;
}

int32_t Http2Stream::get_remote_window_size() const {
    return remote_window_size_;
}


// --- State Transitions ---

void Http2Stream::transition_to_open() {
    // Valid transitions: IDLE -> OPEN (on sending/receiving HEADERS)
    //                  RESERVED_LOCAL -> OPEN (on sending HEADERS for the promise)
    //                  RESERVED_REMOTE -> OPEN (on receiving HEADERS for the promise)
    if (state_ == StreamState::IDLE || state_ == StreamState::RESERVED_LOCAL || state_ == StreamState::RESERVED_REMOTE) {
        state_ = StreamState::OPEN;
    }
    // Other transitions to OPEN are protocol errors.
}

void Http2Stream::transition_to_half_closed_local() {
    // Valid transitions: OPEN -> HALF_CLOSED_LOCAL (on sending END_STREAM)
    //                  RESERVED_LOCAL -> HALF_CLOSED_LOCAL (sending END_STREAM on reserved stream, less common)
    if (state_ == StreamState::OPEN || state_ == StreamState::RESERVED_LOCAL) {
        state_ = StreamState::HALF_CLOSED_LOCAL;
    }
    // If already HALF_CLOSED_REMOTE, then sending END_STREAM transitions to CLOSED.
    else if (state_ == StreamState::HALF_CLOSED_REMOTE) {
        state_ = StreamState::CLOSED;
    }
}

void Http2Stream::transition_to_half_closed_remote() {
    // Valid transitions: OPEN -> HALF_CLOSED_REMOTE (on receiving END_STREAM)
    //                  RESERVED_REMOTE -> HALF_CLOSED_REMOTE (receiving END_STREAM on reserved stream)
    if (state_ == StreamState::OPEN || state_ == StreamState::RESERVED_REMOTE) {
        state_ = StreamState::HALF_CLOSED_REMOTE;
    }
    // If already HALF_CLOSED_LOCAL, then receiving END_STREAM transitions to CLOSED.
    else if (state_ == StreamState::HALF_CLOSED_LOCAL) {
        state_ = StreamState::CLOSED;
    }
}

void Http2Stream::transition_to_closed() {
    // Can be called due to:
    // - Sending/Receiving RST_STREAM
    // - Both sides half-closing (OPEN -> HALF_CLOSED_LOCAL -> CLOSED or OPEN -> HALF_CLOSED_REMOTE -> CLOSED)
    // - Errors leading to stream closure
    state_ = StreamState::CLOSED;
    // When a stream is closed, its resources can be reclaimed.
    // Flow control windows are irrelevant.
    local_window_size_ = 0;
    remote_window_size_ = 0;
}

void Http2Stream::transition_to_reserved_local() {
    // Valid transition: IDLE -> RESERVED_LOCAL (on sending PUSH_PROMISE)
    if (state_ == StreamState::IDLE) {
        state_ = StreamState::RESERVED_LOCAL;
    }
}

void Http2Stream::transition_to_reserved_remote() {
    // Valid transition: IDLE -> RESERVED_REMOTE (on receiving PUSH_PROMISE)
    if (state_ == StreamState::IDLE) {
        state_ = StreamState::RESERVED_REMOTE;
    }
}


} // namespace http2
