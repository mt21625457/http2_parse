#pragma once

#include "http2_types.h"
#include "http2_stream.h"
#include "http2_frame.h" // For HttpHeader, SettingsFrame etc.
#include "hpack_decoder.h"
#include "hpack_encoder.h" // Assuming an HpackEncoder will be created for sending headers

#include <map>
#include <vector>
#include <functional>
#include <optional>
#include <span>

namespace http2 {

// Forward declaration
class Http2Parser;

// Default connection settings (RFC 7540 Section 6.5.2)
constexpr uint32_t DEFAULT_HEADER_TABLE_SIZE = 4096;
constexpr bool DEFAULT_ENABLE_PUSH = true; // For server; client must not push.
constexpr uint32_t DEFAULT_MAX_CONCURRENT_STREAMS = -1; // No limit
constexpr uint32_t DEFAULT_INITIAL_WINDOW_SIZE = 65535; // 2^16 - 1
// constexpr uint32_t DEFAULT_MAX_FRAME_SIZE = 16384; // 2^14. This is also defined in http2_types.h
constexpr uint32_t DEFAULT_MAX_HEADER_LIST_SIZE = -1; // No limit

// Connection state and settings
struct ConnectionSettings {
    uint32_t header_table_size = DEFAULT_HEADER_TABLE_SIZE;
    bool enable_push = DEFAULT_ENABLE_PUSH; // Server's perspective
    uint32_t max_concurrent_streams = DEFAULT_MAX_CONCURRENT_STREAMS;
    uint32_t initial_window_size = DEFAULT_INITIAL_WINDOW_SIZE;
    uint32_t max_frame_size = DEFAULT_MAX_FRAME_SIZE;
    uint32_t max_header_list_size = DEFAULT_MAX_HEADER_LIST_SIZE; // Optional setting
};


class Http2Connection {
public:
    // Callback types for parsed frames and events
    using FrameCallback = std::function<void(const AnyHttp2Frame& frame)>;
    using SettingsAckCallback = std::function<void()>; // When SETTINGS ACK is received
    using PingAckCallback = std::function<void(const PingFrame& ping_ack_frame)>; // When PING ACK is received
    using GoAwayCallback = std::function<void(const GoAwayFrame& goaway_frame)>;
    // Add more callbacks as needed: e.g., for new stream, stream close, errors

    Http2Connection(bool is_server_connection);
    ~Http2Connection();

    // --- Callbacks Registration ---
    void set_frame_callback(FrameCallback cb);
    void set_settings_ack_callback(SettingsAckCallback cb);
    void set_ping_ack_callback(PingAckCallback cb);
    void set_goaway_callback(GoAwayCallback cb);
    // void set_new_stream_callback(...)
    // void set_stream_closed_callback(...)

    // --- Data Processing ---
    // Process incoming raw bytes from the transport layer
    // Returns number of bytes processed, or an error code/exception
    // In C++23, could return std::expected<size_t, ErrorCode>
    size_t process_incoming_data(std::span<const std::byte> data);

    // --- Frame Sending (High-Level API - to be implemented) ---
    // These methods would construct and serialize frames, then queue them for sending.
    // The actual sending mechanism (e.g., writing to a socket) is outside this class.
    // bool send_headers(stream_id_t stream_id, const std::vector<HttpHeader>& headers, bool end_stream);
    // bool send_data(stream_id_t stream_id, std::span<const std::byte> data, bool end_stream);
    // bool send_settings(const std::vector<SettingsFrame::Setting>& settings);
    // bool send_ping(const std::array<std::byte, 8>& opaque_data, bool ack = false);
    // bool send_goaway(stream_id_t last_stream_id, ErrorCode error_code, const std::string& debug_data = "");
    // bool send_window_update(stream_id_t stream_id, uint32_t increment);
    // bool send_rst_stream(stream_id_t stream_id, ErrorCode error_code);

    // --- Stream Management ---
    Http2Stream* get_stream(stream_id_t stream_id);
    // Http2Stream* create_stream(); // For client initiating a stream
    // Http2Stream* create_pushed_stream(stream_id_t parent_stream_id); // For server pushing a stream


    // --- Settings Management ---
    const ConnectionSettings& get_local_settings() const;
    const ConnectionSettings& get_remote_settings() const;
    void apply_local_setting(const SettingsFrame::Setting& setting); // Apply a setting we intend to send
    void apply_remote_setting(const SettingsFrame::Setting& setting); // Apply a setting received from peer

    // --- Flow Control ---
    // Connection-level flow control window (for stream 0)
    int32_t get_local_connection_window() const;
    int32_t get_remote_connection_window() const;
    void update_local_connection_window(uint32_t increment);
    void record_connection_data_sent(size_t size);
    void record_connection_data_received(size_t size);

    // --- Continuation Handling ---
    bool is_expecting_continuation() const;
    stream_id_t get_expected_continuation_stream_id() const;
    void expect_continuation_for_stream(stream_id_t stream_id, FrameType initiator_type, AnyHttp2Frame initiator_frame);
    void finish_continuation();
    void append_to_header_block_buffer(std::span<const std::byte> fragment);
    void populate_pending_headers(std::vector<HttpHeader> headers);
    std::span<const std::byte> get_header_block_buffer_span() const;
    void clear_header_block_buffer();

    // --- State Information ---
    bool is_server() const { return is_server_; }
    bool is_going_away() const { return going_away_; }
    stream_id_t get_next_available_stream_id(); // For client to create new streams
    uint32_t get_max_frame_size_remote() const { return remote_settings_.max_frame_size; }
    uint32_t get_max_frame_size_local() const { return local_settings_.max_frame_size; }


private:
    friend class Http2Parser; // Allow parser to call private methods like handle_parsed_frame

    void handle_parsed_frame(AnyHttp2Frame frame);
    void handle_data_frame(const DataFrame& frame);
    void handle_headers_frame(const HeadersFrame& frame);
    void handle_priority_frame(const PriorityFrame& frame);
    void handle_rst_stream_frame(const RstStreamFrame& frame);
    void handle_settings_frame(const SettingsFrame& frame);
    void handle_push_promise_frame(const PushPromiseFrame& frame);
    void handle_ping_frame(const PingFrame& frame);
    void handle_goaway_frame(const GoAwayFrame& frame);
    void handle_window_update_frame(const WindowUpdateFrame& frame);
    void handle_continuation_frame(const ContinuationFrame& frame);

    // Helper to get or create a stream
    Http2Stream& get_or_create_stream(stream_id_t stream_id);


    bool is_server_;
    std::map<stream_id_t, Http2Stream> streams_;
    stream_id_t next_client_stream_id_ = 1; // For client-initiated streams (odd numbers)
    stream_id_t next_server_stream_id_ = 2; // For server-initiated streams (push promise, even numbers)
    stream_id_t last_processed_stream_id_ = 0; // For GOAWAY processing
    bool going_away_ = false; // Set to true when GOAWAY has been sent or received
    stream_id_t last_peer_initiated_stream_id_in_goaway_ = 0;

    ConnectionSettings local_settings_;  // Settings we have sent or will send
    ConnectionSettings remote_settings_; // Settings received from the peer

    HpackDecoder hpack_decoder_;
    // HpackEncoder hpack_encoder_; // For sending headers

    // Parser instance - connection owns the parser
    std::unique_ptr<Http2Parser> parser_;

    // Callbacks
    FrameCallback frame_cb_;
    SettingsAckCallback settings_ack_cb_;
    PingAckCallback ping_ack_cb_;
    GoAwayCallback goaway_cb_;

    // Connection-level flow control windows (RFC 7540 Section 6.9.1)
    // These are separate from stream-level windows.
    // They apply to data sent on stream 0 (which is implicitly all streams for DATA frames)
    int32_t local_connection_window_size_;
    int32_t remote_connection_window_size_;


    // Buffer for incomplete frames or header sequences
    std::vector<std::byte> incoming_buffer_;
    // State for handling CONTINUATION frames
    std::optional<stream_id_t> expected_continuation_stream_id_;
    std::vector<HttpHeader> pending_headers_for_continuation_;
    // Temp buffer for header block fragments across multiple frames (HEADERS/PUSH_PROMISE + CONTINUATION)
    std::vector<std::byte> header_block_buffer_;
    // Store the type of frame that initiated the header sequence (HEADERS or PUSH_PROMISE)
    // This helps in correctly populating the original frame object after all continuations.
    std::optional<FrameType> header_sequence_initiator_type_;
    // Store the original HeadersFrame/PushPromiseFrame (or its relevant parts)
    // This is a simplification. A more robust way might involve storing a variant or pointer.
    std::optional<AnyHttp2Frame> pending_header_initiator_frame_;


    // Max dynamic table size signaled by peer, to configure HPACK decoder
    // uint32_t remote_header_table_size_ = DEFAULT_HEADER_TABLE_SIZE;


    // --- Output/Action Queue (Conceptual) ---
    // In a real implementation, actions like sending frames would be queued
    // and processed by a separate I/O component.
    // For now, we can log or use placeholder functions.
    std::function<void(stream_id_t, ErrorCode)> on_send_rst_stream_;
    std::function<void(stream_id_t, ErrorCode, const std::string&)> on_send_goaway_;
    std::function<void(const SettingsFrame&)> on_send_settings_ack_; // Placeholder for sending settings ACK
    std::function<void(const PingFrame&)> on_send_ping_ack_; // Placeholder for sending ping ACK
    std::function<void(stream_id_t, uint32_t)> on_send_window_update_;
    std::function<void(std::vector<std::byte>)> on_send_bytes_; // Callback to application to send raw bytes


public:
    // Setters for action callbacks (for testing/integration)
    void set_on_send_rst_stream(std::function<void(stream_id_t, ErrorCode)> cb) { on_send_rst_stream_ = std::move(cb); }
    void set_on_send_goaway(std::function<void(stream_id_t, ErrorCode, const std::string&)> cb) { on_send_goaway_ = std::move(cb); }
    void set_on_send_settings_ack(std::function<void(const SettingsFrame&)> cb) { on_send_settings_ack_ = std::move(cb); }
    void set_on_send_ping_ack(std::function<void(const PingFrame&)> cb) { on_send_ping_ack_ = std::move(cb); }
    void set_on_send_window_update(std::function<void(stream_id_t, uint32_t)> cb) { on_send_window_update_ = std::move(cb); }
    void set_on_send_bytes(std::function<void(std::vector<std::byte>)> cb) { on_send_bytes_ = std::move(cb); }

    // --- Frame Sending API ---
    // Return bool indicating success/failure or specific error codes. For now, bool.
    // These methods will construct the frame, serialize it, and call on_send_bytes_.

    bool send_data(stream_id_t stream_id, std::span<const std::byte> data, bool end_stream);

    bool send_headers(stream_id_t stream_id,
                      const std::vector<HttpHeader>& headers,
                      bool end_stream,
                      std::optional<PriorityData> priority = std::nullopt,
                      std::optional<uint8_t> padding = std::nullopt);

    bool send_priority(stream_id_t stream_id, const PriorityData& priority);

    bool send_rst_stream_frame_action(stream_id_t stream_id, ErrorCode error_code); // Renamed from internal handler

    bool send_settings(const std::vector<SettingsFrame::Setting>& settings);
    bool send_settings_ack_action(); // Renamed from internal handler

    bool send_ping(const std::array<std::byte, 8>& opaque_data, bool ack);
    bool send_ping_ack_action(const PingFrame& received_ping); // Renamed from internal handler

    bool send_goaway_action(stream_id_t last_stream_id, ErrorCode error_code, const std::string& debug_data); // Renamed

    bool send_window_update_action(stream_id_t stream_id, uint32_t increment); // Renamed

    bool send_push_promise(stream_id_t associated_stream_id,
                           stream_id_t promised_stream_id,
                           const std::vector<HttpHeader>& headers,
                           std::optional<uint8_t> padding = std::nullopt);

private:
    // HpackEncoder instance for sending headers
    HpackEncoder hpack_encoder_;

};

} // namespace http2
