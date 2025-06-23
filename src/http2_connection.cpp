#include "http2_connection.h"
#include "http2_parser.h" // Full definition needed
#include "http2_frame_serializer.h"
#include <algorithm> // for std::remove_if for stream cleanup
#include <iostream> // For debugging

// Helper to convert uint32_t to big-endian byte vector (length 4)
std::vector<std::byte> uint32_to_bytes_big_endian(uint32_t val) {
    std::vector<std::byte> bytes(4);
    bytes[0] = static_cast<std::byte>((val >> 24) & 0xFF);
    bytes[1] = static_cast<std::byte>((val >> 16) & 0xFF);
    bytes[2] = static_cast<std::byte>((val >> 8) & 0xFF);
    bytes[3] = static_cast<std::byte>(val & 0xFF);
    return bytes;
}
// Helper to convert uint16_t to big-endian byte vector (length 2)
std::vector<std::byte> uint16_to_bytes_big_endian(uint16_t val) {
    std::vector<std::byte> bytes(2);
    bytes[0] = static_cast<std::byte>((val >> 8) & 0xFF);
    bytes[1] = static_cast<std::byte>(val & 0xFF);
    return bytes;
}


namespace http2 {

Http2Connection::Http2Connection(bool is_server_connection)
    : is_server_(is_server_connection),
      hpack_decoder_(DEFAULT_HEADER_TABLE_SIZE), // Initial default, peer can change via SETTINGS
      // hpack_encoder_(DEFAULT_HEADER_TABLE_SIZE), // Similar for encoder
      local_settings_(), // Default constructed
      remote_settings_(), // Default constructed
      local_connection_window_size_(DEFAULT_INITIAL_WINDOW_SIZE),
      remote_connection_window_size_(DEFAULT_INITIAL_WINDOW_SIZE),
      expected_continuation_stream_id_(std::nullopt)
       {
    parser_ = std::make_unique<Http2Parser>(hpack_decoder_, *this);
    parser_->set_frame_callback([this](AnyHttp2Frame frame, const std::vector<std::byte>& payload){
        // The `payload` argument is ignored for now, but is required by the new callback signature.
        this->handle_parsed_frame(std::move(frame));
    });

    // Stream 0 (the connection itself) is implicitly present.
    // It doesn't use Http2Stream objects in the map usually, but its flow control
    // is managed by local_connection_window_size_ and remote_connection_window_size_.
    // Initialize HpackEncoder with our default/current dynamic table size setting.
    hpack_encoder_.set_own_max_dynamic_table_size(local_settings_.header_table_size);

}

Http2Connection::~Http2Connection() {
    // Cleanup, streams will be destroyed by map dtor.
}

void Http2Connection::set_frame_callback(FrameCallback cb) {
    frame_cb_ = std::move(cb);
}
void Http2Connection::set_settings_ack_callback(SettingsAckCallback cb) {
    settings_ack_cb_ = std::move(cb);
}
void Http2Connection::set_ping_ack_callback(PingAckCallback cb) {
    ping_ack_cb_ = std::move(cb);
}
void Http2Connection::set_goaway_callback(GoAwayCallback cb) {
    goaway_cb_ = std::move(cb);
}


size_t Http2Connection::process_incoming_data(std::span<const std::byte> data) {
    // TODO: Handle connection preface (client sends "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", server validates)
    // For now, assume preface is handled and we are processing frames.

    auto [consumed_bytes, error] = parser_->parse(data);

    if (error != ParserError::OK) {
        // Handle parsing error. This often means sending a GOAWAY frame.
        std::string error_message = "Parser error: code " + std::to_string(static_cast<int>(error));
        ErrorCode http2_error_code = ErrorCode::PROTOCOL_ERROR; // Default
        bool stream_specific_error = false;
        stream_id_t error_stream_id = 0; // For stream-specific errors if applicable from parser

        switch(error) {
            case ParserError::FRAME_SIZE_LIMIT_EXCEEDED:
                http2_error_code = ErrorCode::FRAME_SIZE_ERROR;
                // This is a connection error if it's about SETTINGS_MAX_FRAME_SIZE from peer.
                // If it's a frame that has a stream_id, it could be RST_STREAM too.
                // The parser's current FRAME_SIZE_LIMIT_EXCEEDED is before full frame parsing, so it's likely connection.
                break;
            case ParserError::HPACK_DECOMPRESSION_FAILED:
                http2_error_code = ErrorCode::COMPRESSION_ERROR;
                // This error occurs during header processing, which is stream-specific.
                // However, RFC 7540 Sec 4.3 says "An endpoint that detects a HPACK decoding error
                // MUST treat this as a connection error (Section 5.4.1) of type COMPRESSION_ERROR."
                // So, GOAWAY.
                break;
            case ParserError::INVALID_STREAM_ID: // e.g. DATA on stream 0
            case ParserError::CONTINUATION_WRONG_STREAM:
            case ParserError::CONTINUATION_EXPECTED: // These imply issues with specific streams or overall sequence.
                 // Generally PROTOCOL_ERROR. If a stream ID is identifiable, could be RST_STREAM,
                 // but often these are connection-level protocol violations.
                http2_error_code = ErrorCode::PROTOCOL_ERROR;
                break;
            // Other parser errors typically map to PROTOCOL_ERROR or INTERNAL_ERROR for the connection.
            default:
                http2_error_code = ErrorCode::PROTOCOL_ERROR;
                break;
        }

        // For now, most parser errors are treated as connection errors.
        if (on_send_goaway_) {
            on_send_goaway_(last_processed_stream_id_, http2_error_code, error_message);
        } else {
            std::cerr << "Http2Connection: Parser error: " << error_message
                      << ". Action: GOAWAY with code " << static_cast<int>(http2_error_code) << std::endl;
        }
        // After a GOAWAY due to parser error, the connection is likely unusable for new streams.
        // Consumed bytes might be 0 or partial if error occurred mid-frame.
        // The parser itself stops on error.
    }
    return consumed_bytes;
}


Http2Stream& Http2Connection::get_or_create_stream(stream_id_t stream_id) {
    if (stream_id == 0) {
        // This should ideally not be called for stream 0.
        // If it is, it implies a logic error or special handling needed.
        // For now, throw or return a static dummy stream for stream 0 if absolutely necessary.
        throw std::logic_error("get_or_create_stream called for stream ID 0");
    }

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        // Create new stream. Initial window sizes come from remote settings for local window,
        // and local settings for remote window.
        // This is subtle: our local_settings_.initial_window_size determines how much *we* can send initially on a new stream.
        // remote_settings_.initial_window_size determines how much *peer* can send initially on a new stream.
        uint32_t initial_local_win = remote_settings_.initial_window_size;
        uint32_t initial_remote_win = local_settings_.initial_window_size;

        // Check stream ID validity for creation
        // Client creates odd, server creates even (for push)
        bool client_initiated = (stream_id % 2 != 0);
        bool server_initiated = (stream_id % 2 == 0);

        if (is_server_) { // We are the server
            if (client_initiated) { // Client is opening a stream
                if (stream_id <= last_processed_stream_id_) {
                     // PROTOCOL_ERROR: stream ID less than or equal to previous from client
                     // This requires sending GOAWAY. For now, throw.
                     throw std::runtime_error("PROTOCOL_ERROR: Stream ID regression from client.");
                }
                last_processed_stream_id_ = stream_id;
            } else { // Server is trying to create/find a PUSH_PROMISE stream id
                 // This path is usually for finding an existing pushed stream, not initial creation by server here.
                 // Pushed streams are created differently (e.g. `create_pushed_stream`).
                 // If we reach here for an even ID the server didn't explicitly push, it's an issue.
            }
        } else { // We are the client
            if (server_initiated) { // Server is PUSH_PROMISE-ing a stream
                 if (stream_id <= last_processed_stream_id_) {
                    throw std::runtime_error("PROTOCOL_ERROR: Stream ID regression from server (PUSH_PROMISE).");
                 }
                 last_processed_stream_id_ = stream_id;
            } else { // Client is creating a stream, ID should be from next_client_stream_id_
                // This is fine.
            }
        }


        auto [new_it, inserted] = streams_.try_emplace(stream_id, stream_id, initial_local_win, initial_remote_win);
        // Initial state is IDLE. It will transition based on frames.
        return new_it->second;
    }
    return it->second;
}

Http2Stream* Http2Connection::get_stream(stream_id_t stream_id) {
    if (stream_id == 0) return nullptr; // Stream 0 not in map
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return nullptr;
    }
    return &it->second;
}


void Http2Connection::handle_parsed_frame(AnyHttp2Frame any_frame) {
    // Dispatch to specific handlers
    // These handlers will update stream states, connection states, and call user callbacks.

    // First, call the generic frame callback if set
    if (frame_cb_) {
        frame_cb_(any_frame);
    }

    // Then, process based on type
    std::visit([this](auto&& typed_frame) {
        using T = std::decay_t<decltype(typed_frame)>;
        if constexpr (std::is_same_v<T, DataFrame>) {
            handle_data_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, HeadersFrame>) {
            handle_headers_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, PriorityFrame>) {
            handle_priority_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, RstStreamFrame>) {
            handle_rst_stream_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, SettingsFrame>) {
            handle_settings_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, PushPromiseFrame>) {
            handle_push_promise_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, PingFrame>) {
            handle_ping_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, GoAwayFrame>) {
            handle_goaway_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, WindowUpdateFrame>) {
            handle_window_update_frame(typed_frame);
        } else if constexpr (std::is_same_v<T, ContinuationFrame>) {
            handle_continuation_frame(typed_frame); // Already mostly handled by parser context
        }
    }, any_frame.frame_variant);

    // After handling, check for closed streams to clean up
    // This is a simple cleanup. More robust might be ref-counting or delayed cleanup.
    std::erase_if(streams_, [](const auto& item){
        return item.second.get_state() == StreamState::CLOSED;
    });
}


// --- Individual Frame Handlers ---

void Http2Connection::handle_data_frame(const DataFrame& frame) {
    if (frame.header.stream_id == 0) { /* Protocol error */ return; }
    Http2Stream& stream = get_or_create_stream(frame.header.stream_id);

    // Check stream state: Must be OPEN or HALF_CLOSED_REMOTE to receive DATA
    if (stream.get_state() != StreamState::OPEN && stream.get_state() != StreamState::HALF_CLOSED_REMOTE) {
        if (on_send_rst_stream_) {
            on_send_rst_stream_(frame.header.stream_id, ErrorCode::STREAM_CLOSED);
        } else {
            std::cerr << "CONN: DATA on closed/invalid stream " << frame.header.stream_id << ". Action: RST_STREAM(STREAM_CLOSED)" << std::endl;
        }
        stream.transition_to_closed(); // Ensure stream is marked closed locally
        return;
    }

    // Flow control:
    // Check if frame exceeds local window (stream or connection) BEFORE recording.
    // This is a simplification; precise flow control would check available window vs frame size.
    // RFC 7540 Section 6.9: "A receiver MAY respond with a WINDOW_UPDATE frame to restore the window."
    // "A receiver that receives a flow-controlled frame that exceeds its available flow-control window
    // for either the stream or the connection MUST treat this as a connection error (Section 5.4.1)
    // of type FLOW_CONTROL_ERROR."
    // Note: The spec also says in 6.1 (DATA frame): "If an endpoint receives a DATA frame for a stream
    // on which it has sent or received RST_STREAM, it MUST treat this as a stream error (Section 5.4.2)
    // of type STREAM_CLOSED." This is handled by the state check above.

    if (frame.data.size() > static_cast<size_t>(stream.get_local_window_size()) ||
        frame.data.size() > static_cast<size_t>(local_connection_window_size_)) {
        if (on_send_goaway_) {
            on_send_goaway_(last_processed_stream_id_, ErrorCode::FLOW_CONTROL_ERROR, "Received DATA frame exceeding flow control window");
        } else {
            std::cerr << "CONN: DATA frame for stream " << frame.header.stream_id << " exceeds flow control window. Action: GOAWAY(FLOW_CONTROL_ERROR)" << std::endl;
        }
        // The connection is now in an error state. Further processing might be limited.
        return;
    }


    // 1. Decrement stream-level window
    stream.record_data_received(frame.data.size());
    // 2. Decrement connection-level window
    record_connection_data_received(frame.data.size());

    // These checks are now slightly redundant due to the pre-check, but good for sanity.
    if (stream.get_local_window_size() < 0 ) { // Should not happen if pre-check is correct
        if (on_send_rst_stream_) {
            on_send_rst_stream_(frame.header.stream_id, ErrorCode::FLOW_CONTROL_ERROR);
        } else {
             std::cerr << "CONN: Stream " << frame.header.stream_id << " flow control error (local window < 0). Action: RST_STREAM(FLOW_CONTROL_ERROR)" << std::endl;
        }
        stream.transition_to_closed();
        return;
    }
    if (local_connection_window_size_ < 0) { // Should not happen
         if (on_send_goaway_) {
            on_send_goaway_(last_processed_stream_id_, ErrorCode::FLOW_CONTROL_ERROR, "Connection flow control error (window < 0)");
        } else {
            std::cerr << "CONN: Connection flow control error (window < 0). Action: GOAWAY(FLOW_CONTROL_ERROR)" << std::endl;
        }
        return;
    }


    // TODO: Pass data to application associated with the stream.

    if (frame.has_end_stream_flag()) {
        stream.transition_to_half_closed_remote(); // Peer has finished sending
        if (stream.get_state() == StreamState::CLOSED) {
            // Stream is now fully closed, will be cleaned up.
        }
    }
}

void Http2Connection::handle_headers_frame(const HeadersFrame& frame) {
    if (frame.header.stream_id == 0) { /* Protocol error */ return; }
    Http2Stream& stream = get_or_create_stream(frame.header.stream_id);

    // State checks:
    // IDLE -> OPEN
    // RESERVED_REMOTE -> HALF_CLOSED_LOCAL (if END_STREAM set, peer sends HEADERS on our PUSH_PROMISE)
    // OPEN -> no state change on HEADERS itself (unless END_STREAM for trailers)
    // HALF_CLOSED_REMOTE -> no state change (unless END_STREAM for trailers)

    bool is_trailers = false; // Determine if these are trailing headers
    if (stream.get_state() == StreamState::OPEN || stream.get_state() == StreamState::HALF_CLOSED_REMOTE) {
        // If already received END_STREAM on data, these must be trailers.
        // This requires more state on the stream (e.g. `received_end_stream_data_flag`)
        // For now, let's assume if stream is HALF_CLOSED_REMOTE, these are trailers.
        if (stream.get_state() == StreamState::HALF_CLOSED_REMOTE) {
            is_trailers = true;
        }
    }


    if (stream.get_state() == StreamState::IDLE) {
        stream.transition_to_open();
    } else if (stream.get_state() == StreamState::RESERVED_REMOTE) { // Server receiving HEADERS for a PUSH_PROMISE it sent
        stream.transition_to_half_closed_local();
    } else if (stream.get_state() == StreamState::RESERVED_LOCAL) { // Client receiving HEADERS for a PUSH_PROMISE it received
         stream.transition_to_half_closed_remote(); // Client is now half-closed remote (it received END_STREAM with these HEADERS)
                                                  // Server is half-closed local (it sent END_STREAM with these HEADERS)
                                                  // This state transition might need review based on who sends what for PUSH.
                                                  // Let's assume for now client receives HEADERS for a promise, it opens.
                                                  // RFC 7540 Section 8.2: Client receives PUSH_PROMISE, then server sends HEADERS on new stream.
                                                  // So, if client gets HEADERS on a stream that was in RESERVED_LOCAL, it means server is sending.
                                                  // Client stream state should go to OPEN or HALF_CLOSED_REMOTE if END_STREAM is on these HEADERS.
        stream.transition_to_open(); // Simplified: receiving HEADERS on reserved stream opens it.
    }
     else if (stream.get_state() != StreamState::OPEN && stream.get_state() != StreamState::HALF_CLOSED_REMOTE) {
        // Receiving HEADERS in other states (e.g. HALF_CLOSED_LOCAL, CLOSED) is a PROTOCOL_ERROR
        if (on_send_rst_stream_) {
            on_send_rst_stream_(frame.header.stream_id, ErrorCode::PROTOCOL_ERROR);
        } else {
             std::cerr << "CONN: HEADERS on invalid stream " << frame.header.stream_id << " state " << static_cast<int>(stream.get_state()) << ". Action: RST_STREAM(PROTOCOL_ERROR)" << std::endl;
        }
        stream.transition_to_closed();
        return;
    }

    // The actual decoded headers are in frame.headers if END_HEADERS was set and no CONTINUATION.
    // If CONTINUATION was involved, the `finish_continuation` path would have triggered this callback
    // with the fully populated `pending_header_initiator_frame_`. So `frame.headers` here should be complete.

    // TODO: Validate header list size against SETTINGS_MAX_HEADER_LIST_SIZE (remote_settings_.max_header_list_size)
    // TODO: Process headers (e.g., pass to application, check for pseudo-headers order/validity, especially for trailers)
    if (is_trailers && !frame.has_end_stream_flag()) {
        // PROTOCOL_ERROR: Trailers must have END_STREAM
        if (on_send_rst_stream_) on_send_rst_stream_(frame.header.stream_id, ErrorCode::PROTOCOL_ERROR);
        stream.transition_to_closed();
        return;
    }


    if (frame.has_priority_flag()) {
        // Apply priority info: frame.stream_dependency, frame.exclusive_dependency, frame.weight
        // This is complex, involves updating dependency tree.
    }

    if (frame.has_end_stream_flag()) {
        stream.transition_to_half_closed_remote();
    }
}

void Http2Connection::handle_priority_frame(const PriorityFrame& frame) {
    if (frame.header.stream_id == 0) { /* Protocol error */ return; }
    // PRIORITY can be sent for any stream state except IDLE (if it creates the stream implicitly) or CLOSED.
    // For now, assume stream exists or is created.
    Http2Stream& stream = get_or_create_stream(frame.header.stream_id);
    if (stream.get_state() == StreamState::IDLE) {
        // "Receiving a PRIORITY frame on a stream that is in the "idle" state has no effect."
        // However, it *can* be used to implicitly open a stream for priority signaling.
        // The spec is a bit nuanced here. Let's assume it's fine.
    }
     if (stream.get_state() == StreamState::CLOSED) return; // Ignore on closed streams.


    // TODO: Apply priority information. This is complex.
    // frame.stream_dependency, frame.exclusive_dependency, frame.weight
}

void Http2Connection::handle_rst_stream_frame(const RstStreamFrame& frame) {
    if (frame.header.stream_id == 0) { /* Protocol error */ return; }
    Http2Stream* stream_ptr = get_stream(frame.header.stream_id);
    if (!stream_ptr) {
        // RST_STREAM on an unknown/idle/closed stream is not an error, just ignored.
        return;
    }
    if (stream_ptr->get_state() == StreamState::IDLE || stream_ptr->get_state() == StreamState::CLOSED) {
        return; // Ignore
    }

    stream_ptr->transition_to_closed();
    // TODO: Notify application about the stream reset with frame.error_code.
    // Stream will be cleaned up from the map.
}

void Http2Connection::handle_settings_frame(const SettingsFrame& frame) {
    if (frame.header.stream_id != 0) { /* Protocol error */ return; }

    if (frame.has_ack_flag()) {
        if (frame.header.length != 0) { /* Protocol error: ACK with payload */ return; }
        if (settings_ack_cb_) {
            settings_ack_cb_();
        }
        // TODO: If we had pending settings changes, mark them as acknowledged.
        return;
    }

    // This is a SETTINGS frame from the peer. Apply them.
    for (const auto& setting : frame.settings) {
        apply_remote_setting(setting);
    }

    // After applying all settings, send an ACK
    send_settings_ack_action();
}

void Http2Connection::apply_remote_setting(const SettingsFrame::Setting& setting) {
    // Validate and apply settings from the peer. Returns false on error.
    // This updates remote_settings_ and influences connection behavior.
    bool changed_initial_window = false;
    uint32_t old_initial_window = remote_settings_.initial_window_size;

    switch (setting.identifier) {
        case SettingsFrame::SETTINGS_HEADER_TABLE_SIZE:
            remote_settings_.header_table_size = setting.value;
            hpack_decoder_.set_max_dynamic_table_size(setting.value); // Inform HPACK decoder
            break;
        case SettingsFrame::SETTINGS_ENABLE_PUSH:
            if (setting.value > 1) { /* Protocol error */ return; }
            remote_settings_.enable_push = (setting.value == 1);
            if (is_server_ && !remote_settings_.enable_push) {
                // Client disabled push. Server should not push.
            }
            break;
        case SettingsFrame::SETTINGS_MAX_CONCURRENT_STREAMS:
            remote_settings_.max_concurrent_streams = setting.value;
            break;
        case SettingsFrame::SETTINGS_INITIAL_WINDOW_SIZE:
            if (setting.value > MAX_ALLOWED_WINDOW_SIZE) { /* FLOW_CONTROL_ERROR */ return; }
            remote_settings_.initial_window_size = setting.value;
            changed_initial_window = true;
            break;
        case SettingsFrame::SETTINGS_MAX_FRAME_SIZE:
            if (setting.value < DEFAULT_MAX_FRAME_SIZE || setting.value > MAX_ALLOWED_FRAME_SIZE) {
                /* Protocol error */ return;
            }
            remote_settings_.max_frame_size = setting.value;
            // This value is used by the parser to check incoming frame sizes.
            break;
        case SettingsFrame::SETTINGS_MAX_HEADER_LIST_SIZE:
            remote_settings_.max_header_list_size = setting.value;
            break;
        default:
            // Unknown setting identifiers are ignored (RFC 7540 Section 6.5.2)
            break;
    }

    if (changed_initial_window) {
        // Adjust flow-control windows of all active streams (RFC 7540 Section 6.5.3)
        int32_t delta = static_cast<int32_t>(remote_settings_.initial_window_size) - static_cast<int32_t>(old_initial_window);
        for (auto& [id, stream] : streams_) {
            if (stream.get_state() != StreamState::IDLE && stream.get_state() != StreamState::CLOSED) {
                // This affects how much *data the peer can send on this stream*.
                // So it's an update to the stream's *local* window from the perspective of the stream object,
                // but it's driven by the peer's SETTINGS_INITIAL_WINDOW_SIZE.
                // This is subtle. The `initial_window_size` setting refers to the window size
                // that the *sender of the SETTINGS frame* will use for new streams.
                // So, when we receive SETTINGS_INITIAL_WINDOW_SIZE, it means the peer
                // is telling us *their* new initial window size for streams they initiate,
                // and also, it adjusts the window for data *we send them*.
                // This means it adjusts our `remote_window_size_` on each stream.
                stream.update_remote_window(delta); // `update_remote_window` adds, so delta works.
                                                    // Need to handle potential overflow if delta is huge.
                                                    // The stream method should cap at 2^31-1.
            }
        }
    }
}


void Http2Connection::handle_push_promise_frame(const PushPromiseFrame& frame) {
    if (frame.header.stream_id == 0) { /* Protocol error */ return; }
    if (!is_server_ && !local_settings_.enable_push) { /* Protocol error: client received PUSH_PROMISE but push disabled */ return;}
    if (is_server_) { /* Protocol error: server cannot receive PUSH_PROMISE */ return; }


    Http2Stream& parent_stream = get_or_create_stream(frame.header.stream_id);
    // Parent stream must be OPEN or HALF_CLOSED_REMOTE
    if (parent_stream.get_state() != StreamState::OPEN && parent_stream.get_state() != StreamState::HALF_CLOSED_REMOTE) {
        // PROTOCOL_ERROR
        return;
    }

    // Create or get the promised stream
    Http2Stream& promised_stream = get_or_create_stream(frame.promised_stream_id);
    if (promised_stream.get_state() != StreamState::IDLE) {
        // PROTOCOL_ERROR: Promised stream not in IDLE state
        return;
    }
    promised_stream.transition_to_reserved_remote(); // Client reserves it

    // TODO: Process promised headers (frame.headers). Application might decide to accept/reject the push.
    // If rejected, client sends RST_STREAM on the promised_stream_id.
}

void Http2Connection::handle_ping_frame(const PingFrame& frame) {
    if (frame.header.stream_id != 0) { /* Protocol error */ return; }
    if (frame.header.length != 8) { /* Protocol error */ return; }

    if (frame.has_ack_flag()) {
        // This is an ACK to our PING.
        if (ping_ack_cb_) {
            ping_ack_cb_(frame);
        }
    } else {
        // This is a PING from peer. Send ACK.
        if (on_send_ping_ack_) {
            PingFrame ack_response = frame; // Copy opaque data and other fields
            ack_response.header.flags |= PingFrame::ACK_FLAG;
            // ack_response.header.length remains 8
            // ack_response.header.stream_id remains 0
            on_send_ping_ack_(ack_response);
        } else {
            std::cerr << "CONN: Received PING, would send ACK." << std::endl;
        }
    }
}

void Http2Connection::handle_goaway_frame(const GoAwayFrame& frame) {
    this->going_away_ = true;
    this->last_peer_initiated_stream_id_in_goaway_ = frame.last_stream_id;
    if (goaway_cb_) {
        goaway_cb_(frame);
    }
}

void Http2Connection::handle_window_update_frame(const WindowUpdateFrame& frame) {
    if (frame.window_size_increment == 0) {
        // RFC 7540 Section 6.9: "A WINDOW_UPDATE frame with a flow-control window increment of 0 MUST be
        // treated as a connection error (Section 5.4.1) of type PROTOCOL_ERROR"
        // ... "or a stream error (Section 5.4.2) of type PROTOCOL_ERROR if the stream identifier is non-zero."
        if (frame.header.stream_id == 0) {
            if (on_send_goaway_) on_send_goaway_(last_processed_stream_id_, ErrorCode::PROTOCOL_ERROR, "WINDOW_UPDATE with 0 increment on stream 0");
        } else {
            if (on_send_rst_stream_) on_send_rst_stream_(frame.header.stream_id, ErrorCode::PROTOCOL_ERROR);
            Http2Stream* stream = get_stream(frame.header.stream_id);
            if(stream) stream->transition_to_closed();
        }
        return;
    }

    if (frame.header.stream_id == 0) { // Connection-level window update
        if (static_cast<int64_t>(remote_connection_window_size_) + frame.window_size_increment > MAX_ALLOWED_WINDOW_SIZE) {
            if (on_send_goaway_) {
                on_send_goaway_(last_processed_stream_id_, ErrorCode::FLOW_CONTROL_ERROR, "WINDOW_UPDATE for connection overflowed");
            }
            return;
        }
        remote_connection_window_size_ += frame.window_size_increment;
    } else { // Stream-specific window update
        Http2Stream* stream_ptr = get_stream(frame.header.stream_id);
        if (!stream_ptr || stream_ptr->get_state() == StreamState::IDLE) { // Also ignore for RESERVED states
             // RFC 7540 Section 6.9: "WINDOW_UPDATE can be sent for a stream in the "open" or "half-closed (remote)" state.
             // WINDOW_UPDATE on a stream in the "idle" or "closed" state MUST be treated as a connection error
             // (Section 5.4.1) of type PROTOCOL_ERROR."
             // (Exception: Initial WINDOW_UPDATE for reserved streams might be okay, needs careful check)
             // For now, simplifying: if stream not in a state to receive flow control, it's an error.
            if (on_send_goaway_) { // Treat as connection error as per spec for idle/closed
                 on_send_goaway_(last_processed_stream_id_, ErrorCode::PROTOCOL_ERROR, "WINDOW_UPDATE on idle/closed stream");
            }
            return;
        }
        if (stream_ptr->get_state() == StreamState::CLOSED) { // Already covered by above, but explicit.
            return; // Ignore if somehow missed by above and already closed.
        }

        if (!stream_ptr->update_remote_window(frame.window_size_increment)) {
            // update_remote_window returns false if it would overflow 2^31-1
            if (on_send_rst_stream_) {
                on_send_rst_stream_(frame.header.stream_id, ErrorCode::FLOW_CONTROL_ERROR);
            }
            stream_ptr->transition_to_closed();
        }
    }
}

void Http2Connection::handle_continuation_frame(const ContinuationFrame& frame) {
    // Most logic for CONTINUATION is handled by the parser in conjunction with Http2Connection state
    // (expected_continuation_stream_id_, header_block_buffer_).
    // If END_HEADERS is set on this CONTINUATION frame, the parser would have triggered
    // HPACK decoding and populated the original HEADERS/PUSH_PROMISE frame's header list.
    // This handler here is mostly a notification that a CONTINUATION was processed.
    // If there was an error (e.g. unexpected CONTINUATION), the parser should have reported it.

    // The parser's `parse_continuation_payload` calls `connection_context_.populate_pending_headers`
    // when END_HEADERS is seen. We need to ensure that `populate_pending_headers` correctly
    // updates the original frame that was expecting continuation.

    // This is tricky because the `AnyHttp2Frame` passed to `handle_parsed_frame` for the *original*
    // HEADERS/PUSH_PROMISE frame might have been processed before all CONTINUATIONs arrived.
    // A better model:
    // 1. Parser sees HEADERS/PUSH_PROMISE without END_HEADERS. It stores it temporarily.
    // 2. Parser sees CONTINUATIONs, appends to buffer.
    // 3. Parser sees CONTINUATION with END_HEADERS. It decodes the full block.
    // 4. Parser then calls `handle_parsed_frame` with the *original* HEADERS/PUSH_PROMISE frame,
    //    now fully populated with all headers.

    // The current parser design calls `raw_frame_parsed_cb_` for each frame individually.
    // So, `handle_headers_frame` or `handle_push_promise_frame` might be called with an incomplete header list
    // if END_HEADERS is not set. The `frame.headers` in those initial calls would be empty.
    // When the final CONTINUATION with END_HEADERS arrives, its `handle_continuation_frame` (or rather, the
    // parser's logic for it) would need to signal that the header sequence is complete.

    // Let's refine: `Http2Parser::parse_continuation_payload` calls `connection_context_.populate_pending_headers`
    // `populate_pending_headers` should find the stream that was expecting these headers and update its pending header list.
    // Then, the application callback for that original HEADERS/PUSH_PROMISE frame should be triggered.

    // This means `pending_headers_for_continuation_` in Http2Connection should be per-stream or
    // the original frame object needs to be stored and updated.
    // For simplicity, parser now populates the original Header/PushPromise object via connection context.
    // So, by the time handle_headers_frame/handle_push_promise_frame is called for a frame that *was*
    // followed by CONTINUATIONs, its `frame.headers` list should be complete if the final
    // END_HEADERS has been processed.

    // If this CONTINUATION frame itself has END_HEADERS, the connection's `finish_continuation`
    // (called by parser) would have finalized the header block.
    // If not, we just wait for more.
}


// --- CONTINUATION state management (called by parser) ---
bool Http2Connection::is_expecting_continuation() const {
    return expected_continuation_stream_id_.has_value();
}
stream_id_t Http2Connection::get_expected_continuation_stream_id() const {
    return expected_continuation_stream_id_.value_or(0);
}

void Http2Connection::expect_continuation_for_stream(stream_id_t stream_id, FrameType initiator_type, AnyHttp2Frame initiator_frame) {
    expected_continuation_stream_id_ = stream_id;
    header_sequence_initiator_type_ = initiator_type;
    pending_header_initiator_frame_ = std::move(initiator_frame);
    // Header block buffer is NOT cleared here, it's appended to by the parser.
}

void Http2Connection::finish_continuation() {
    expected_continuation_stream_id_.reset();
    header_sequence_initiator_type_.reset();
    pending_header_initiator_frame_.reset();
    clear_header_block_buffer();
}

void Http2Connection::append_to_header_block_buffer(std::span<const std::byte> fragment) {
    header_block_buffer_.insert(header_block_buffer_.end(), fragment.begin(), fragment.end());
}
std::span<const std::byte> Http2Connection::get_header_block_buffer_span() const {
    return header_block_buffer_;
}
void Http2Connection::clear_header_block_buffer() {
    header_block_buffer_.clear();
}

void Http2Connection::populate_pending_headers(std::vector<HttpHeader> headers) {
    if (pending_header_initiator_frame_.has_value()) {
        std::visit([&](auto& frame_variant){
            using T = std::decay_t<decltype(frame_variant)>;
            if constexpr (std::is_same_v<T, HeadersFrame> || std::is_same_v<T, PushPromiseFrame>) {
                frame_variant.headers = std::move(headers);
            }
        }, pending_header_initiator_frame_->frame_variant);
    }
}

// --- Flow Control for Connection ---
int32_t Http2Connection::get_local_connection_window() const {
    return local_connection_window_size_;
}
int32_t Http2Connection::get_remote_connection_window() const {
    return remote_connection_window_size_;
}
void Http2Connection::update_local_connection_window(uint32_t increment) {
    // Called when we are ready to receive more data at connection level
    // This would trigger sending a WINDOW_UPDATE for stream 0.
    if (static_cast<int64_t>(local_connection_window_size_) + increment > MAX_ALLOWED_WINDOW_SIZE) {
        // Internal error or trying to set too high
        return;
    }
    local_connection_window_size_ += increment;
}
void Http2Connection::record_connection_data_sent(size_t size) {
    remote_connection_window_size_ -= static_cast<int32_t>(size);
}
void Http2Connection::record_connection_data_received(size_t size) {
    local_connection_window_size_ -= size;
}

const ConnectionSettings& Http2Connection::get_local_settings() const {
    return local_settings_;
}
const ConnectionSettings& Http2Connection::get_remote_settings() const {
    return remote_settings_;
}

stream_id_t Http2Connection::get_next_available_stream_id() {
    if (is_server_) {
        return 0; // Server does not initiate streams like this
    }
    if (next_client_stream_id_ > MAX_STREAM_ID - 2) { // Check for overflow before incrementing
        return 0; // No more stream IDs available
    }
    stream_id_t id = next_client_stream_id_;
    next_client_stream_id_ += 2;
    return id;
}


// --- Frame Sending API Implementations ---

bool Http2Connection::send_settings(const std::vector<SettingsFrame::Setting>& settings) {
    SettingsFrame sf;
    sf.settings = settings;
    auto frame_bytes = FrameSerializer::serialize_settings_frame(sf);
    if(on_send_bytes_) {
        on_send_bytes_(frame_bytes);
    }
    return true;
}

bool Http2Connection::send_settings_ack_action() {
    if (!on_send_bytes_) return false;

    SettingsFrame sf;
    sf.header.type = FrameType::SETTINGS;
    sf.header.flags = SettingsFrame::ACK_FLAG;
    sf.header.stream_id = 0;
    // sf.settings is empty for ACK
    // Length will be 0, serializer handles this.

    auto frame_bytes = FrameSerializer::serialize_settings_frame(sf);
     if (frame_bytes.empty()) { // Should not happen for ACK unless serializer is broken
        return false;
    }
    on_send_bytes_(std::move(frame_bytes));
    return true;
}


bool Http2Connection::send_ping(const std::array<std::byte, 8>& opaque_data, bool ack) {
    if (!on_send_bytes_) return false;

    PingFrame pf;
    pf.header.type = FrameType::PING;
    pf.header.flags = ack ? PingFrame::ACK_FLAG : 0;
    pf.header.stream_id = 0;
    pf.opaque_data = opaque_data;
    // Length is fixed at 8, serializer handles this.

    auto frame_bytes = FrameSerializer::serialize_ping_frame(pf);
    if (frame_bytes.empty()) return false; // Serialization error
    on_send_bytes_(std::move(frame_bytes));
    return true;
}

bool Http2Connection::send_ping_ack_action(const PingFrame& received_ping) {
    // This is called by handle_ping_frame when a non-ACK PING is received.
    // We need to send back a PING with ACK flag and same opaque data.
    return send_ping(received_ping.opaque_data, true);
}


bool Http2Connection::send_rst_stream_frame_action(stream_id_t stream_id, ErrorCode error_code) {
    if (stream_id == 0) return false; // Cannot RST stream 0
    if (!on_send_bytes_) return false;

    Http2Stream* stream = get_stream(stream_id);
    if (stream) {
        // Per RFC 7540 Section 6.4:
        // "RST_STREAM terminates a single stream [...] After receiving a RST_STREAM on a stream,
        // the receiver MUST NOT send additional frames for that stream, with the exception of PRIORITY."
        // "An endpoint MUST NOT send a RST_STREAM frame on a stream that is already "closed"..."
        if (stream->get_state() == StreamState::CLOSED || stream->get_state() == StreamState::IDLE) {
            // Or if it was never created (stream == nullptr and not IDLE state logic)
            // For IDLE, sending RST_STREAM is a PROTOCOL_ERROR.
            // For now, simple check: if stream is already closed, don't send.
            // A more robust check would be needed if we allow sending RST_STREAM for IDLE streams
            // (which would be a protocol violation by the sender of this RST_STREAM).
            if(stream->get_state() == StreamState::CLOSED) return true; // Nothing to do
        }
    }
    // If stream is nullptr, it might be IDLE or already cleaned up. Sending RST_STREAM for an IDLE stream
    // that the peer doesn't know about is not useful and might be a protocol error if it was never opened.
    // If it's for a stream we know is active or half-closed, proceed.

    RstStreamFrame rsf;
    rsf.header.type = FrameType::RST_STREAM;
    rsf.header.flags = 0;
    rsf.header.stream_id = stream_id;
    rsf.error_code = error_code;
    // Length is fixed at 4.

    auto frame_bytes = FrameSerializer::serialize_rst_stream_frame(rsf);
    if (frame_bytes.empty()) return false;
    on_send_bytes_(std::move(frame_bytes));

    // Update local stream state to closed
    if (stream) {
        stream->transition_to_closed();
    }
    return true;
}

bool Http2Connection::send_goaway_action(stream_id_t last_stream_id, ErrorCode error_code, const std::string& debug_data) {
    if (!on_send_bytes_) return false;

    GoAwayFrame gaf;
    gaf.header.type = FrameType::GOAWAY;
    gaf.header.flags = 0;
    gaf.header.stream_id = 0;
    gaf.last_stream_id = last_stream_id;
    gaf.error_code = error_code;
    std::transform(debug_data.begin(), debug_data.end(), std::back_inserter(gaf.additional_debug_data),
                   [](char c){ return static_cast<std::byte>(c); });

    auto frame_bytes = FrameSerializer::serialize_goaway_frame(gaf);
    if (frame_bytes.empty()) return false;
    on_send_bytes_(std::move(frame_bytes));

    this->going_away_ = true; // Mark connection as going away from our side.
    // Further stream creation might be blocked based on this flag.
    return true;
}

bool Http2Connection::send_window_update_action(stream_id_t stream_id, uint32_t increment) {
    if (increment == 0 || increment > MAX_ALLOWED_WINDOW_SIZE) return false; // Invalid increment
    if (!on_send_bytes_) return false;

    WindowUpdateFrame wuf;
    wuf.header.type = FrameType::WINDOW_UPDATE;
    wuf.header.flags = 0;
    wuf.header.stream_id = stream_id;
    wuf.window_size_increment = increment;

    auto frame_bytes = FrameSerializer::serialize_window_update_frame(wuf);
    if (frame_bytes.empty()) return false;
    on_send_bytes_(std::move(frame_bytes));

    // We sent a WINDOW_UPDATE, this means we are increasing *our* local window for the peer.
    // So, the peer can send us more data. This affects local_window_size_ on stream/connection.
    if (stream_id == 0) {
        local_connection_window_size_ += increment; // This seems counter-intuitive here.
                                                // When *we* send WINDOW_UPDATE, it's because *our* application
                                                // consumed data, freeing up *our* receive buffer for the *peer*.
                                                // So, this action *increases* the window for the *peer to send to us*.
                                                // The `local_connection_window_size_` is *our* receive window.
                                                // This needs to be called when *our application* signals it has processed data.
                                                // For now, let's assume this function is called *after* such processing.
    } else {
        Http2Stream* stream = get_stream(stream_id);
        if (stream) {
            // stream->local_window_size_ += increment; // Similar logic as above.
            // This function just sends the frame. The window variable itself
            // should be incremented by logic that decides to send this frame.
        } else {
            return false; // Cannot send WU for unknown stream
        }
    }
    return true;
}

// Implementations for send_data, send_headers, send_priority, send_push_promise will follow.

bool Http2Connection::send_data(stream_id_t stream_id, std::span<const std::byte> data, bool end_stream) {
    if (stream_id == 0) return false; // DATA must be on a non-zero stream
    if (!on_send_bytes_) return false;

    Http2Stream* stream = get_stream(stream_id);
    if (!stream || (stream->get_state() != StreamState::OPEN && stream->get_state() != StreamState::HALF_CLOSED_REMOTE)) {
        // Cannot send DATA on idle, closed, or locally half-closed streams
        return false;
    }

    // Flow Control and Segmentation
    size_t data_offset = 0;
    bool all_data_sent = false;

    while (data_offset < data.size() || (data.empty() && data_offset == 0 && end_stream /* allow empty DATA with END_STREAM */) ) {
        uint32_t available_stream_window = static_cast<uint32_t>(std::max(0, stream->get_remote_window_size()));
        uint32_t available_conn_window = static_cast<uint32_t>(std::max(0, remote_connection_window_size_));

        if (available_stream_window == 0 || available_conn_window == 0) {
            if (data.empty() && data_offset == 0 && end_stream) { // Sending empty DATA frame with END_STREAM
                // This is allowed even if window is 0.
            } else if (data_offset < data.size()) { // Actual data to send, but no window
                std::cerr << "CONN: send_data blocked by flow control for stream " << stream_id << ". StreamWin: " << available_stream_window << " ConnWin: " << available_conn_window << std::endl;
                return data_offset > 0; // Return true if some data was sent before blocking
            } else {
                // data_offset == data.size(), means all data sent in previous iterations.
                break;
            }
        }

        size_t remaining_data_size = data.size() - data_offset;
        uint32_t current_chunk_size = static_cast<uint32_t>(remaining_data_size);

        // Limit chunk size by peer's max frame size
        current_chunk_size = std::min(current_chunk_size, remote_settings_.max_frame_size);

        // Limit by flow control windows
        current_chunk_size = std::min(current_chunk_size, available_stream_window);
        current_chunk_size = std::min(current_chunk_size, available_conn_window);

        if (current_chunk_size == 0 && remaining_data_size > 0) {
            // No space in window for more data, and there is data left.
             std::cerr << "CONN: send_data blocked by flow control (chunk size 0) for stream " << stream_id << ". StreamWin: " << available_stream_window << " ConnWin: " << available_conn_window << std::endl;
            return data_offset > 0; // True if some data sent, false if nothing could be sent.
        }
        // If data is empty, current_chunk_size will be 0. This is for the empty DATA with END_STREAM case.
        if (data.empty() && data_offset == 0 && end_stream) {
            current_chunk_size = 0; // Ensure it's zero for empty DATA frame
        }


        DataFrame df;
        df.header.type = FrameType::DATA;
        df.header.stream_id = stream_id;
        // Padding not implemented in this send_data yet.

        df.data.assign(data.begin() + data_offset, data.begin() + data_offset + current_chunk_size);
        data_offset += current_chunk_size;
        all_data_sent = (data_offset == data.size());

        df.header.flags = (end_stream && all_data_sent) ? DataFrame::END_STREAM_FLAG : 0;
        // df.header.length is set by serializer

        auto frame_bytes = FrameSerializer::serialize_data_frame(df);
        // Serialization error for DATA is unlikely unless extreme memory issues, but check:
        if (frame_bytes.empty() && (current_chunk_size > 0 || (data.empty() && end_stream))) {
            std::cerr << "CONN: send_data serialization failed for stream " << stream_id << std::endl;
            return data_offset > current_chunk_size; // Return true if previous chunks were sent
        }

        on_send_bytes_(std::move(frame_bytes));

        // Update flow control windows
        stream->record_data_sent(current_chunk_size);
        record_connection_data_sent(current_chunk_size);

        if (end_stream && all_data_sent) {
            stream->transition_to_half_closed_local();
        }

        if (data.empty() && data_offset == 0 && end_stream) { // Sent the one empty DATA frame
            break;
        }
        if (all_data_sent) break; // All data has been segmented and sent
    }
    return true; // Successfully sent all (or part if blocked mid-way and error returned earlier)
}

bool Http2Connection::send_headers(stream_id_t stream_id,
                                 const std::vector<HttpHeader>& headers,
                                 bool end_stream,
                                 std::optional<PriorityData> priority,
                                 std::optional<uint8_t> padding) {
    if (!is_server_) { // Is a client
        if ((stream_id % 2) == 0 || stream_id < next_client_stream_id_) { // Stream ID must be odd and increasing
            return false; // PROTOCOL_ERROR
        }
        next_client_stream_id_ = stream_id + 2;
    }

    // Create stream
    auto& stream = get_or_create_stream(stream_id);
    if (stream.get_state() == StreamState::CLOSED) {
        return false; // Cannot send HEADERS on a closed stream
    }

    FrameHeader initial_header;
    initial_header.type = FrameType::HEADERS;
    initial_header.stream_id = stream_id;
    initial_header.flags = 0;
    if (end_stream) initial_header.flags |= HeadersFrame::END_STREAM_FLAG;
    if (padding.has_value()) initial_header.flags |= HeadersFrame::PADDED_FLAG;
    if (priority.has_value()) initial_header.flags |= HeadersFrame::PRIORITY_FLAG;
    // END_HEADERS will be set by serialize_header_block_with_continuation on the last frame.

    // Construct the HeadersFrame object for the serializer (even if it gets split)
    // The serializer helper will use parts of this.
    HeadersFrame hf_template; // Template for priority/padding data for the first frame.
    hf_template.header = initial_header; // Base flags, stream_id, type. Length is TBD by serializer.
    if (padding.has_value()) hf_template.pad_length = padding.value();
    if (priority.has_value()) {
        hf_template.exclusive_dependency = priority.value().exclusive_dependency;
        hf_template.stream_dependency = priority.value().stream_dependency;
        hf_template.weight = priority.value().weight;
    }
    // hf_template.headers is not used by serialize_header_block_with_continuation, it takes headers_to_encode separately.

    auto sequence = FrameSerializer::serialize_header_block_with_continuation(
        hf_template.header, // Pass the header with flags for padding/priority
        headers,
        hpack_encoder_,
        remote_settings_.max_frame_size, // Peer's max frame size
        false // is_push_promise
    );

    if (sequence.headers_frame_bytes.empty()) return false; // Serialization or HPACK error

    on_send_bytes_(std::move(sequence.headers_frame_bytes));
    for (auto& cont_bytes : sequence.continuation_frames_bytes) {
        on_send_bytes_(std::move(cont_bytes));
    }

    // Update stream state
    if (stream.get_state() == StreamState::IDLE) { // Client sending initial HEADERS
        stream.transition_to_open();
    } else if (is_server_ && stream.get_state() == StreamState::RESERVED_REMOTE) { // Server responding to PUSH_PROMISE
        stream.transition_to_half_closed_local();
    }

    if (end_stream) {
        stream.transition_to_half_closed_local();
    }
    return true;
}

bool Http2Connection::send_priority(stream_id_t stream_id, const PriorityData& priority_data) {
    if (stream_id == 0) return false;
    if (!on_send_bytes_) return false;

    // PRIORITY can be sent for idle streams to register them.
    Http2Stream* stream = get_stream(stream_id);
    if (!stream) { // If stream doesn't exist, it's IDLE. Create it for accounting if needed, but PRIORITY itself doesn't change state from IDLE.
        // stream = &get_or_create_stream(stream_id); // Optional: create if you want to track all signaled streams.
    } else {
        if (stream->get_state() == StreamState::CLOSED) return false; // Cannot send PRIORITY for closed stream
    }


    PriorityFrame pf;
    pf.header.type = FrameType::PRIORITY;
    pf.header.flags = 0;
    pf.header.stream_id = stream_id;
    pf.exclusive_dependency = priority_data.exclusive_dependency;
    pf.stream_dependency = priority_data.stream_dependency;
    pf.weight = priority_data.weight; // This should be the value 0-255.
    // Length is fixed at 5.

    auto frame_bytes = FrameSerializer::serialize_priority_frame(pf);
    if (frame_bytes.empty()) return false;
    on_send_bytes_(std::move(frame_bytes));
    return true;
}


bool Http2Connection::send_push_promise(stream_id_t associated_stream_id,
                                      stream_id_t promised_stream_id,
                                      const std::vector<HttpHeader>& headers,
                                      std::optional<uint8_t> padding_length) {
    if (!is_server_) return false; // Client cannot send PUSH_PROMISE
    if (associated_stream_id == 0 || promised_stream_id == 0 || (promised_stream_id % 2 != 0)) {
        return false; // Invalid stream IDs
    }
    if (!on_send_bytes_) return false;

    Http2Stream* assoc_stream = get_stream(associated_stream_id);
    if (!assoc_stream || (assoc_stream->get_state() != StreamState::OPEN && assoc_stream->get_state() != StreamState::HALF_CLOSED_LOCAL)) {
        return false; // Associated stream must be open or half-closed (local)
    }

    // Create and reserve the promised stream
    Http2Stream* promised_s = get_stream(promised_stream_id);
    if (promised_s) return false; // Promised stream already exists

    promised_s = &get_or_create_stream(promised_stream_id); // Creates in IDLE
    promised_s->transition_to_reserved_local(); // Server reserves it locally


    FrameHeader initial_header;
    initial_header.type = FrameType::PUSH_PROMISE;
    initial_header.stream_id = associated_stream_id;
    initial_header.flags = 0; // END_HEADERS handled by serializer helper
    if (padding_length.has_value()) initial_header.flags |= PushPromiseFrame::PADDED_FLAG;

    // Similar to send_headers, use serialize_header_block_with_continuation
    PushPromiseFrame ppf_template;
    ppf_template.header = initial_header;
    if (padding_length.has_value()) ppf_template.pad_length = padding_length.value();
    ppf_template.promised_stream_id = promised_stream_id;
    // ppf_template.headers not used by helper, takes headers_to_encode separately.

    auto sequence = FrameSerializer::serialize_header_block_with_continuation(
        ppf_template.header,
        headers,
        hpack_encoder_,
        remote_settings_.max_frame_size,
        true, // is_push_promise = true
        promised_stream_id
    );

    if (sequence.headers_frame_bytes.empty()) {
        promised_s->transition_to_closed(); // Clean up reserved stream if PUSH_PROMISE fails to send
        return false;
    }

    on_send_bytes_(std::move(sequence.headers_frame_bytes));
    for (auto& cont_bytes : sequence.continuation_frames_bytes) {
        on_send_bytes_(std::move(cont_bytes));
    }

    return true;
}

void Http2Connection::apply_local_setting(const SettingsFrame::Setting& setting) {
    // Apply a setting for our local configuration. This influences frames we send.
    // This is simpler than apply_remote_setting as it doesn't usually trigger
    // complex state changes like adjusting all stream windows.

    switch (setting.identifier) {
        case SettingsFrame::SETTINGS_HEADER_TABLE_SIZE:
            local_settings_.header_table_size = setting.value;
            // We should also inform our own HPACK encoder about this change.
            hpack_encoder_.set_own_max_dynamic_table_size(setting.value);
            break;
        case SettingsFrame::SETTINGS_ENABLE_PUSH:
            // This is a setting for the peer. A client sends this to a server.
            // If we are a client, we'd set this. If we are a server, we'd read it.
            if (!is_server_) {
                local_settings_.enable_push = (setting.value != 0);
            }
            break;
        case SettingsFrame::SETTINGS_MAX_CONCURRENT_STREAMS:
            local_settings_.max_concurrent_streams = setting.value;
            break;
        case SettingsFrame::SETTINGS_INITIAL_WINDOW_SIZE:
            if (setting.value > MAX_ALLOWED_WINDOW_SIZE) {
                // This is an internal configuration error, should not happen in valid usage.
                return;
            }
            // Changing our own initial window size setting will affect new streams we create.
            local_settings_.initial_window_size = setting.value;
            break;
        case SettingsFrame::SETTINGS_MAX_FRAME_SIZE:
            if (setting.value < DEFAULT_MAX_FRAME_SIZE || setting.value > MAX_ALLOWED_FRAME_SIZE) {
                return; // Invalid configuration
            }
            local_settings_.max_frame_size = setting.value;
            break;
        case SettingsFrame::SETTINGS_MAX_HEADER_LIST_SIZE:
            local_settings_.max_header_list_size = setting.value;
            break;
        default:
            // Ignore unknown settings
            break;
    }
}

} // namespace http2
