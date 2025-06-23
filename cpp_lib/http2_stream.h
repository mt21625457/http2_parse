#pragma once

#include "http2_types.h"
#include "http2_frame.h" // For HttpHeader, though ideally stream might not directly parse frames
#include <cstdint>
#include <vector>
#include <string>
#include <deque>
#include <functional> // For callbacks, if needed at this level

namespace http2 {

// Stream States (RFC 7540 Section 5.1)
enum class StreamState {
    IDLE,
    RESERVED_LOCAL,
    RESERVED_REMOTE,
    OPEN,
    HALF_CLOSED_LOCAL,  // Local endpoint sent END_STREAM
    HALF_CLOSED_REMOTE, // Remote endpoint sent END_STREAM
    CLOSED
};

class Http2Stream {
public:
    Http2Stream(stream_id_t id, uint32_t initial_local_window, uint32_t initial_remote_window);

    stream_id_t get_id() const;
    StreamState get_state() const;

    // --- Window Management ---
    // Local window: how much data the remote peer can send to us
    // Remote window: how much data we can send to the remote peer

    // Called when we receive a WINDOW_UPDATE from the remote peer for this stream
    bool update_remote_window(window_size_t increment);
    // Called when we are ready to send more data (e.g., after application consumes data)
    // This would trigger sending a WINDOW_UPDATE frame to the peer.
    bool update_local_window(window_size_t increment);

    // Check if we can send data
    bool can_send_data(size_t data_size) const;
    // Record that we've sent data
    void record_data_sent(size_t data_size);

    // Check if remote can send data (based on our local window)
    // bool can_receive_data(size_t data_size) const; // This is more for the connection level
    // Record that we've received data
    void record_data_received(size_t data_size);


    int32_t get_local_window_size() const;
    int32_t get_remote_window_size() const; // Renamed from send_window_ for clarity

    // --- State Transitions ---
    // These methods would be called by the Http2Connection or Http2Parser
    // when specific frames are sent or received.

    void transition_to_open(); // On sending/receiving HEADERS
    void transition_to_half_closed_local(); // On sending END_STREAM
    void transition_to_half_closed_remote(); // On receiving END_STREAM
    void transition_to_closed(); // On RST_STREAM, or both sides half-closed

    // If the stream is reserved
    void transition_to_reserved_local();  // On sending PUSH_PROMISE
    void transition_to_reserved_remote(); // On receiving PUSH_PROMISE


    // --- Data Handling (Conceptual) ---
    // The stream might queue incoming data frames or outgoing data frames.
    // This is a simplified view; actual data handling might be more complex.
    // void enqueue_incoming_data(DataFrame data_frame);
    // std::optional<DataFrame> dequeue_outgoing_data();

    // --- Header Handling (Conceptual) ---
    // Store received headers, potentially in a structured way.
    // std::vector<HttpHeader> received_headers;
    // std::vector<HttpHeader> received_trailers;


private:
    stream_id_t id_;
    StreamState state_;

    // Flow control windows (RFC 7540 Section 6.9)
    // Local window: controlled by us, limits data peer can send on this stream.
    // We send WINDOW_UPDATE to increase it.
    int32_t local_window_size_; // Using int32_t as window can be < 0 temporarily before erroring

    // Remote window: controlled by peer, limits data we can send on this stream.
    // Peer sends WINDOW_UPDATE to increase it.
    int32_t remote_window_size_;

    // Other stream-specific properties:
    // - Priority information
    // - Queued frames (incoming/outgoing)
    // - Application-specific data associated with the stream
};

} // namespace http2
