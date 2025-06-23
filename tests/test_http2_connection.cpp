#include "gtest/gtest.h"
#include "cpp_lib/http2_connection.h"
#include "cpp_lib/http2_frame.h" // For constructing test frames
#include "cpp_lib/http2_parser.h" // For ParserError enum if needed
#include <vector>
#include <cstring> // for memcpy


using namespace http2;

// Helper to construct frame bytes (copied from test_http2_parser.cpp for convenience)
std::vector<std::byte> construct_frame_bytes(uint32_t length, FrameType type, uint8_t flags, uint32_t stream_id, const std::vector<std::byte>& payload) {
    std::vector<std::byte> frame_bytes(9 + payload.size());
    frame_bytes[0] = static_cast<std::byte>((length >> 16) & 0xFF);
    frame_bytes[1] = static_cast<std::byte>((length >> 8) & 0xFF);
    frame_bytes[2] = static_cast<std::byte>(length & 0xFF);
    frame_bytes[3] = static_cast<std::byte>(type);
    frame_bytes[4] = static_cast<std::byte>(flags);
    frame_bytes[5] = static_cast<std::byte>((stream_id >> 24) & 0xFF);
    frame_bytes[6] = static_cast<std::byte>((stream_id >> 16) & 0xFF);
    frame_bytes[7] = static_cast<std::byte>((stream_id >> 8) & 0xFF);
    frame_bytes[8] = static_cast<std::byte>(stream_id & 0xFF);
    if (!payload.empty()) {
        std::memcpy(frame_bytes.data() + 9, payload.data(), payload.size());
    }
    return frame_bytes;
}


class Http2ConnectionTest : public ::testing::Test {
protected:
    // Use a client connection for some tests, server for others if behavior differs.
    Http2Connection client_conn;
    Http2Connection server_conn;

    std::vector<AnyHttp2Frame> received_frames_client;
    std::vector<AnyHttp2Frame> received_frames_server;
    bool settings_ack_received_client = false;
    bool settings_ack_received_server = false;
    // Add more state for callbacks as needed

    Http2ConnectionTest() : client_conn(false /*is_server*/), server_conn(true /*is_server*/) {
        client_conn.set_frame_callback([this](const AnyHttp2Frame& frame){
            received_frames_client.push_back(frame);
        });
        client_conn.set_settings_ack_callback([this](){
            settings_ack_received_client = true;
        });

        server_conn.set_frame_callback([this](const AnyHttp2Frame& frame){
            received_frames_server.push_back(frame);
        });
         server_conn.set_settings_ack_callback([this](){
            settings_ack_received_server = true;
        });
    }

    void SetUp() override {
        received_frames_client.clear();
        received_frames_server.clear();
        settings_ack_received_client = false;
        settings_ack_received_server = false;
        // Reset connection states if necessary (though constructor does a lot)
        // Re-creating connections or having a dedicated reset method might be better for full isolation.
    }
};

TEST_F(Http2ConnectionTest, InitialSettings) {
    EXPECT_EQ(client_conn.get_local_settings().initial_window_size, DEFAULT_INITIAL_WINDOW_SIZE);
    EXPECT_EQ(client_conn.get_remote_settings().initial_window_size, DEFAULT_INITIAL_WINDOW_SIZE);
    EXPECT_EQ(client_conn.get_local_settings().max_frame_size, DEFAULT_MAX_FRAME_SIZE);

    EXPECT_FALSE(client_conn.is_server());
    EXPECT_TRUE(server_conn.is_server());
}

TEST_F(Http2ConnectionTest, ProcessSettingsFrameAndAck) {
    // Server receives SETTINGS from client
    // Client sends: MAX_CONCURRENT_STREAMS (0x3) = 50
    //               HEADER_TABLE_SIZE (0x1) = 2048
    std::vector<std::byte> settings_payload = {
        0x00, 0x03, 0x00, 0x00, 0x00, 0x32, // MAX_CONCURRENT_STREAMS = 50
        0x00, 0x01, 0x00, 0x00, 0x08, 0x00  // HEADER_TABLE_SIZE = 2048
    };
    auto frame_bytes = construct_frame_bytes(static_cast<uint32_t>(settings_payload.size()), FrameType::SETTINGS, 0, 0, settings_payload);

    size_t consumed = server_conn.process_incoming_data(frame_bytes);
    EXPECT_EQ(consumed, frame_bytes.size());
    ASSERT_EQ(received_frames_server.size(), 1u);

    const auto* sf = std::get_if<SettingsFrame>(&received_frames_server[0].frame_variant);
    ASSERT_NE(sf, nullptr);
    EXPECT_FALSE(sf->has_ack_flag());

    // Check that server's remote_settings_ are updated
    EXPECT_EQ(server_conn.get_remote_settings().max_concurrent_streams, 50u);
    EXPECT_EQ(server_conn.get_remote_settings().header_table_size, 2048u);

    // TODO: Test that server sends a SETTINGS ACK. This requires an "output queue" on connection.
    // For now, we assume HttpConnection::handle_settings_frame would trigger send_settings_ack().
    // We can't easily test that output here without more infrastructure.

    // Test client receiving SETTINGS ACK from server (simulated)
    auto ack_bytes = construct_frame_bytes(0, FrameType::SETTINGS, SettingsFrame::ACK_FLAG, 0, {});
    client_conn.process_incoming_data(ack_bytes);
    EXPECT_TRUE(settings_ack_received_client);
}


TEST_F(Http2ConnectionTest, StreamCreationAndDataFrameHandling) {
    // Client sends HEADERS (Stream 1, END_HEADERS)
    // :method: GET (0x82 from static table)
    std::vector<std::byte> headers_payload = {static_cast<std::byte>(0x82)};
    auto headers_frame = construct_frame_bytes(static_cast<uint32_t>(headers_payload.size()),
                                             FrameType::HEADERS, HeadersFrame::END_HEADERS_FLAG, 1, headers_payload);
    server_conn.process_incoming_data(headers_frame);
    ASSERT_EQ(received_frames_server.size(), 1u); // Headers frame
    Http2Stream* stream1_server = server_conn.get_stream(1);
    ASSERT_NE(stream1_server, nullptr);
    EXPECT_EQ(stream1_server->get_state(), StreamState::OPEN);
    EXPECT_EQ(stream1_server->get_id(), 1u);

    // Client sends DATA on Stream 1 (payload "hello", END_STREAM)
    std::vector<std::byte> data_payload = {'h', 'e', 'l', 'l', 'o'};
    auto data_frame = construct_frame_bytes(static_cast<uint32_t>(data_payload.size()),
                                          FrameType::DATA, DataFrame::END_STREAM_FLAG, 1, data_payload);
    server_conn.process_incoming_data(data_frame);
    ASSERT_EQ(received_frames_server.size(), 2u); // DATA frame also received

    const auto* df_rcvd = std::get_if<DataFrame>(&received_frames_server[1].frame_variant);
    ASSERT_NE(df_rcvd, nullptr);
    EXPECT_EQ(df_rcvd->data.size(), 5u);

    EXPECT_EQ(stream1_server->get_state(), StreamState::HALF_CLOSED_REMOTE); // Client closed its sending side

    // Check flow control (initial window sizes are default 65535)
    EXPECT_EQ(stream1_server->get_local_window_size(), DEFAULT_INITIAL_WINDOW_SIZE - 5);
    EXPECT_EQ(server_conn.get_local_connection_window(), DEFAULT_INITIAL_WINDOW_SIZE - 5);
}

TEST_F(Http2ConnectionTest, RstStreamClosesStream) {
    // Setup: Stream 1 is open on server
    std::vector<std::byte> headers_payload = {static_cast<std::byte>(0x82)}; // :method: GET
    auto headers_frame = construct_frame_bytes(headers_payload.size(), FrameType::HEADERS, HeadersFrame::END_HEADERS_FLAG, 1, headers_payload);
    server_conn.process_incoming_data(headers_frame);
    Http2Stream* stream1_server = server_conn.get_stream(1);
    ASSERT_NE(stream1_server, nullptr);
    ASSERT_EQ(stream1_server->get_state(), StreamState::OPEN);

    // Client sends RST_STREAM on Stream 1
    std::vector<std::byte> rst_payload = {0x00, 0x00, 0x00, 0x08}; // CANCEL (0x8)
    auto rst_frame = construct_frame_bytes(rst_payload.size(), FrameType::RST_STREAM, 0, 1, rst_payload);
    server_conn.process_incoming_data(rst_frame);

    ASSERT_EQ(received_frames_server.size(), 2u); // RST_STREAM frame
    const auto* rst_rcvd = std::get_if<RstStreamFrame>(&received_frames_server[1].frame_variant);
    ASSERT_NE(rst_rcvd, nullptr);
    EXPECT_EQ(rst_rcvd->error_code, ErrorCode::CANCEL);

    // Stream should now be closed
    // The HttpConnection might remove closed streams from its map immediately or deferred.
    // Let's assume it's immediate for this test.
    // After `handle_parsed_frame` completes for RST_STREAM, it calls `erase_if`.
    Http2Stream* stream1_after_rst = server_conn.get_stream(1);
    EXPECT_EQ(stream1_after_rst, nullptr); // Stream should be gone from map
}

TEST_F(Http2ConnectionTest, WindowUpdateConnectionLevel) {
    int32_t initial_remote_conn_window = client_conn.get_remote_connection_window();

    // Server sends WINDOW_UPDATE for connection (stream 0)
    uint32_t increment = 1000;
    std::vector<std::byte> wu_payload = {0x00, 0x00, 0x03, 0xE8}; // Increment 1000
    auto wu_frame = construct_frame_bytes(wu_payload.size(), FrameType::WINDOW_UPDATE, 0, 0, wu_payload);

    client_conn.process_incoming_data(wu_frame);
    ASSERT_EQ(received_frames_client.size(), 1u);
    EXPECT_EQ(client_conn.get_remote_connection_window(), initial_remote_conn_window + increment);
}

TEST_F(Http2ConnectionTest, WindowUpdateStreamLevel) {
    // Client opens stream 1 towards server
    std::vector<std::byte> headers_payload = {static_cast<std::byte>(0x82)}; // :method: GET
    auto hf = construct_frame_bytes(headers_payload.size(), FrameType::HEADERS, HeadersFrame::END_HEADERS_FLAG, 1, headers_payload);
    server_conn.process_incoming_data(hf); // Server gets stream 1
    Http2Stream* stream1_server = server_conn.get_stream(1);
    ASSERT_NE(stream1_server, nullptr);
    int32_t initial_remote_stream_window = stream1_server->get_remote_window_size();

    // Client sends WINDOW_UPDATE for stream 1
    uint32_t increment = 500;
    std::vector<std::byte> wu_payload = {0x00, 0x00, 0x01, (std::byte)0xF4}; // Increment 500
    auto wu_frame = construct_frame_bytes(wu_payload.size(), FrameType::WINDOW_UPDATE, 0, 1, wu_payload);

    server_conn.process_incoming_data(wu_frame);
    ASSERT_EQ(received_frames_server.size(), 2u); // Headers + WindowUpdate
    EXPECT_EQ(stream1_server->get_remote_window_size(), initial_remote_stream_window + increment);
}


TEST_F(Http2ConnectionTest, PingPong) {
    // Client sends PING
    std::array<std::byte, 8> ping_data_client;
    for(int i=0; i<8; ++i) ping_data_client[i] = static_cast<std::byte>(i+10);
    std::vector<std::byte> ping_payload_client(ping_data_client.begin(), ping_data_client.end());
    auto ping_frame_from_client = construct_frame_bytes(ping_payload_client.size(), FrameType::PING, 0, 0, ping_payload_client);

    server_conn.process_incoming_data(ping_frame_from_client);
    ASSERT_EQ(received_frames_server.size(), 1u);
    const auto* ping_rcvd_by_server = std::get_if<PingFrame>(&received_frames_server[0].frame_variant);
    ASSERT_NE(ping_rcvd_by_server, nullptr);
    EXPECT_FALSE(ping_rcvd_by_server->has_ack_flag());
    EXPECT_EQ(ping_rcvd_by_server->opaque_data, ping_data_client);

    // TODO: Test that server sends PING ACK. Requires output queue.
    // For now, simulate client receiving the ACK.
    std::vector<std::byte> ping_ack_payload_from_server(ping_data_client.begin(), ping_data_client.end()); // Same data
    auto ping_ack_frame = construct_frame_bytes(ping_ack_payload_from_server.size(), FrameType::PING, PingFrame::ACK_FLAG, 0, ping_ack_payload_from_server);

    bool client_ping_ack_cb_fired = false;
    std::array<std::byte, 8> client_ping_ack_data;
    client_conn.set_ping_ack_callback([&](const PingFrame& pf){
        client_ping_ack_cb_fired = true;
        client_ping_ack_data = pf.opaque_data;
    });

    client_conn.process_incoming_data(ping_ack_frame);
    EXPECT_TRUE(client_ping_ack_cb_fired);
    EXPECT_EQ(client_ping_ack_data, ping_data_client);
}

TEST_F(Http2ConnectionTest, GoAwayProcessing) {
    // Server sends GOAWAY to client
    uint32_t last_stream_id = 5;
    ErrorCode error_code = ErrorCode::NO_ERROR;
    std::vector<std::byte> debug_data_payload = {'b', 'y', 'e'};

    std::vector<std::byte> goaway_payload;
    goaway_payload.push_back(static_cast<std::byte>((last_stream_id >> 24) & 0xFF));
    goaway_payload.push_back(static_cast<std::byte>((last_stream_id >> 16) & 0xFF));
    goaway_payload.push_back(static_cast<std::byte>((last_stream_id >> 8) & 0xFF));
    goaway_payload.push_back(static_cast<std::byte>(last_stream_id & 0xFF));
    uint32_t err_val = static_cast<uint32_t>(error_code);
    goaway_payload.push_back(static_cast<std::byte>((err_val >> 24) & 0xFF));
    goaway_payload.push_back(static_cast<std::byte>((err_val >> 16) & 0xFF));
    goaway_payload.push_back(static_cast<std::byte>((err_val >> 8) & 0xFF));
    goaway_payload.push_back(static_cast<std::byte>(err_val & 0xFF));
    goaway_payload.insert(goaway_payload.end(), debug_data_payload.begin(), debug_data_payload.end());

    auto goaway_frame = construct_frame_bytes(goaway_payload.size(), FrameType::GOAWAY, 0, 0, goaway_payload);

    bool goaway_cb_fired = false;
    GoAwayFrame goaway_info_store;
    client_conn.set_goaway_callback([&](const GoAwayFrame& gf){
        goaway_cb_fired = true;
        goaway_info_store = gf;
    });

    client_conn.process_incoming_data(goaway_frame);
    EXPECT_TRUE(goaway_cb_fired);
    EXPECT_EQ(goaway_info_store.last_stream_id, last_stream_id);
    EXPECT_EQ(goaway_info_store.error_code, error_code);
    EXPECT_EQ(goaway_info_store.additional_debug_data, debug_data_payload);
    EXPECT_TRUE(client_conn.is_going_away()); // Check internal state if exposed, or behavior
}


TEST_F(Http2ConnectionTest, ContinuationFrameSequence) {
    // Client sends HEADERS (Stream 1, NO END_HEADERS) for ":method: GET"
    std::vector<std::byte> h_payload1 = {static_cast<std::byte>(0x82)}; // :method: GET
    auto h_frame1 = construct_frame_bytes(h_payload1.size(), FrameType::HEADERS, 0, 1, h_payload1);
    server_conn.process_incoming_data(h_frame1);

    ASSERT_EQ(received_frames_server.size(), 1u);
    const auto* hf_rcvd = std::get_if<HeadersFrame>(&received_frames_server[0].frame_variant);
    ASSERT_NE(hf_rcvd, nullptr);
    EXPECT_FALSE(hf_rcvd->has_end_headers_flag());
    EXPECT_TRUE(hf_rcvd->headers.empty()); // Headers not decoded yet
    EXPECT_TRUE(server_conn.is_expecting_continuation());
    EXPECT_EQ(server_conn.get_expected_continuation_stream_id(), 1u);

    // Client sends CONTINUATION (Stream 1, END_HEADERS) for ":path: /"
    std::vector<std::byte> c_payload1 = {static_cast<std::byte>(0x84)}; // :path: /
    auto c_frame1 = construct_frame_bytes(c_payload1.size(), FrameType::CONTINUATION, ContinuationFrame::END_HEADERS_FLAG, 1, c_payload1);
    server_conn.process_incoming_data(c_frame1);

    ASSERT_EQ(received_frames_server.size(), 2u); // Continuation frame also received
    const auto* cf_rcvd = std::get_if<ContinuationFrame>(&received_frames_server[1].frame_variant);
    ASSERT_NE(cf_rcvd, nullptr);
    EXPECT_TRUE(cf_rcvd->has_end_headers_flag());

    EXPECT_FALSE(server_conn.is_expecting_continuation());

    // Now, the original HeadersFrame object that was passed to the callback is NOT updated.
    // The callback for HEADERS was already fired with an empty header list.
    // This highlights a nuance in testing/using the raw frame callback.
    // A higher-level "headers complete" callback on the connection would be better.
    // For this test, we can check the HpackDecoder state if it was affected, or the stream.
    Http2Stream* stream1 = server_conn.get_stream(1);
    ASSERT_NE(stream1, nullptr);
    // If stream stored headers, we'd check here. It doesn't currently.

    // The test for parser's continuation handling implicitly tests that HpackDecoder is called.
    // What we can verify here is that the connection's continuation state is cleared.
    // To verify the headers, we'd need to inspect what handle_parsed_frame does with the decoded headers
    // from the CONTINUATION path. The current HttpConnection::populate_pending_headers is a stub.

    // Let's assume the HttpParser correctly populates the *ContinuationFrame* object's `headers` field
    // if it's the one with END_HEADERS and it decodes. This is not standard.
    // The standard way is the HEADERS frame conceptually contains all headers.
    // The current parser's `parse_continuation_payload` calls `connection_context_.populate_pending_headers(decoded_headers);`
    // Let's make `populate_pending_headers` store them in a test-accessible way for now.
    // This is still not ideal. The HEADERS frame object itself should be the one with headers.

    // A more robust test would be to have the connection emit a "Headers готова" (Headers Ready) event
    // with the fully assembled list of headers.
    // For now, this test just checks the continuation state logic on the connection.
}


// --- Sending API Tests ---
struct FrameSentInfo {
    FrameType type;
    uint32_t stream_id;
    uint8_t flags;
    std::vector<std::byte> payload; // Only payload, not 9-byte header
    std::vector<std::byte> full_frame_bytes;

    // Helper to parse frame header from full_frame_bytes
    FrameSentInfo(const std::vector<std::byte>& raw_bytes) : full_frame_bytes(raw_bytes) {
        if (raw_bytes.size() < 9) return; // Invalid
        uint32_t length = (static_cast<uint32_t>(raw_bytes[0]) << 16) |
                          (static_cast<uint32_t>(raw_bytes[1]) << 8)  |
                          (static_cast<uint32_t>(raw_bytes[2]));
        type = static_cast<FrameType>(raw_bytes[3]);
        flags = static_cast<uint8_t>(raw_bytes[4]);
        stream_id = (static_cast<uint32_t>(raw_bytes[5]) << 24) |
                    (static_cast<uint32_t>(raw_bytes[6]) << 16) |
                    (static_cast<uint32_t>(raw_bytes[7]) << 8)  |
                    (static_cast<uint32_t>(raw_bytes[8]));
        stream_id &= 0x7FFFFFFF; // Mask R bit

        if (raw_bytes.size() == 9 + length) {
            payload.assign(raw_bytes.begin() + 9, raw_bytes.end());
        }
    }
};


TEST_F(Http2ConnectionTest, SendSettingsFrame) {
    std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    std::vector<SettingsFrame::Setting> settings = {
        {SettingsFrame::SETTINGS_MAX_FRAME_SIZE, 20000},
        {SettingsFrame::SETTINGS_ENABLE_PUSH, 0}
    };
    bool success = client_conn.send_settings(settings);
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 1u);

    const auto& frame_info = sent_frames_capture[0];
    EXPECT_EQ(frame_info.type, FrameType::SETTINGS);
    EXPECT_EQ(frame_info.stream_id, 0u);
    EXPECT_EQ(frame_info.flags, 0);
    EXPECT_EQ(frame_info.payload.size(), 12u); // 2 settings * 6 bytes/setting
    // TODO: Deeper payload validation if needed
}

TEST_F(Http2ConnectionTest, SendDataFrameRespectsFlowControl) {
    std::vector<FrameSentInfo> sent_frames_capture;
    server_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    // Setup: Client opens stream 1
    auto headers_payload = {std::byte(0x82)}; // :method: GET
    auto hf = construct_frame_bytes(headers_payload.size(), FrameType::HEADERS, HeadersFrame::END_HEADERS_FLAG, 1, headers_payload);
    server_conn.process_incoming_data(hf); // Server gets stream 1
    Http2Stream* stream1 = server_conn.get_stream(1);
    ASSERT_NE(stream1, nullptr);

    // Reduce client's (peer for server_conn) window for stream 1 and connection
    // Server's remote_window_size for stream 1 and connection is what client allows.
    // We can't directly set server's remote_window_size here easily.
    // Instead, let's set server's local_settings.max_frame_size to be small,
    // and test segmentation based on that (as send_data uses local_settings for segmentation limit for now).
    // The test for remote_settings_.max_frame_size is also important.
    // The refined send_data uses remote_settings_.max_frame_size.
    // Let's simulate client setting a small max_frame_size for the server.
    SettingsFrame client_settings_val;
    client_settings_val.settings.push_back({SettingsFrame::SETTINGS_MAX_FRAME_SIZE, 5});
    server_conn.apply_remote_setting(client_settings_val.settings[0]); // Server now knows client wants small frames
    EXPECT_EQ(server_conn.get_remote_settings().max_frame_size, 5u);


    // Try to send 12 bytes of data. Should be split into 3 frames (5, 5, 2).
    std::vector<std::byte> data_to_send(12);
    std::iota(data_to_send.begin(), data_to_send.end(), std::byte(1));

    // Ensure server has enough window from client (for this test, assume large enough)
    // To test this properly, we'd need to simulate client sending WINDOW_UPDATE.
    // For now, let's assume default window (65535) is fine for 12 bytes.

    bool success = server_conn.send_data(1, data_to_send, true /* end_stream */);
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 3u);

    EXPECT_EQ(sent_frames_capture[0].type, FrameType::DATA);
    EXPECT_EQ(sent_frames_capture[0].payload.size(), 5u);
    EXPECT_EQ(sent_frames_capture[0].flags, 0); // Not END_STREAM

    EXPECT_EQ(sent_frames_capture[1].type, FrameType::DATA);
    EXPECT_EQ(sent_frames_capture[1].payload.size(), 5u);
    EXPECT_EQ(sent_frames_capture[1].flags, 0);

    EXPECT_EQ(sent_frames_capture[2].type, FrameType::DATA);
    EXPECT_EQ(sent_frames_capture[2].payload.size(), 2u);
    EXPECT_EQ(sent_frames_capture[2].flags, DataFrame::END_STREAM_FLAG); // END_STREAM on last frame

    // Check stream state
    EXPECT_EQ(stream1->get_state(), StreamState::HALF_CLOSED_LOCAL); // Server closed its sending side
}


TEST_F(Http2ConnectionTest, SendHeadersWithContinuation) {
    std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    // Set peer's (server for client_conn) max_frame_size to be small
    client_conn.remote_settings_.max_frame_size = 30;

    std::string long_val(50, 'b');
    std::vector<HttpHeader> headers = {
        {":method", "POST"},
        {"user-agent", "TestClient/1.0"},
        {"custom-long-header", long_val}
    };
    // HPACK for this will be > 30 bytes.
    // :method: POST -> 83 (1 byte)
    // user-agent: TestClient/1.0 -> 5F 0E 54657374436c69656e742f312e30 (idx 5F for ua, literal value, ~17 bytes)
    // custom-long-header: bbb... (new name, literal value) -> 40 + name_len_enc + name + val_len_enc + val_huffman
    // Name "custom-long-header" (18 chars) -> literal name ~20 bytes
    // Value (50 'b's) -> huffman 'b' is 6 bits (0x20). 50*6 = 300 bits = 37.5 -> 38 bytes.
    // Total HPACK estimated: 1 + 17 + 20 + 38 = ~76 bytes.
    // With max_frame_size = 30, this should require HEADERS + 2 CONTINUATIONs.

    bool success = client_conn.send_headers(1, headers, true /*end_stream*/);
    EXPECT_TRUE(success);
    ASSERT_GE(sent_frames_capture.size(), 2u); // At least HEADERS + 1 CONTINUATION

    EXPECT_EQ(sent_frames_capture[0].type, FrameType::HEADERS);
    EXPECT_EQ(sent_frames_capture[0].stream_id, 1u);
    EXPECT_FALSE(sent_frames_capture[0].flags & HeadersFrame::END_HEADERS_FLAG); // First HEADERS should not have END_HEADERS
    EXPECT_TRUE(sent_frames_capture[0].flags & HeadersFrame::END_STREAM_FLAG); // END_STREAM should be on first frame if set

    for(size_t i = 1; i < sent_frames_capture.size() -1; ++i) {
        EXPECT_EQ(sent_frames_capture[i].type, FrameType::CONTINUATION);
        EXPECT_EQ(sent_frames_capture[i].stream_id, 1u);
        EXPECT_FALSE(sent_frames_capture[i].flags & ContinuationFrame::END_HEADERS_FLAG);
    }

    EXPECT_EQ(sent_frames_capture.back().type, FrameType::CONTINUATION);
    EXPECT_EQ(sent_frames_capture.back().stream_id, 1u);
    EXPECT_TRUE(sent_frames_capture.back().flags & ContinuationFrame::END_HEADERS_FLAG);
}

TEST_F(Http2ConnectionTest, SendPriorityFrame) {
    std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    PriorityData pd = {true, 3, 150}; // Exclusive, depends on stream 3, weight 151
    bool success = client_conn.send_priority(5, pd); // Send PRIORITY for stream 5
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 1u);

    const auto& fi = sent_frames_capture[0];
    EXPECT_EQ(fi.type, FrameType::PRIORITY);
    EXPECT_EQ(fi.stream_id, 5u);
    EXPECT_EQ(fi.payload.size(), 5u); // Priority payload is 5 bytes
    // Expected payload: E(1)|StreamDep(3) + Weight(150) -> 80000003 96
    // Stream Dependency: (1 << 31) | 3 = 0x80000003
    // Weight: 150 (0x96)
    std::vector<std::byte> expected_payload = {
        std::byte(0x80), std::byte(0x00), std::byte(0x00), std::byte(0x03), // Stream Dependency
        std::byte(0x96)  // Weight
    };
    EXPECT_EQ(fi.payload, expected_payload);
}

TEST_F(Http2ConnectionTest, SendRstStreamFrameAction) {
    std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    // Open a stream first to make RST meaningful for state change
    client_conn.send_headers(1, make_headers_fs({{":method", "GET"}}), false);
    sent_frames_capture.clear(); // Clear headers frame

    Http2Stream* stream1 = client_conn.get_stream(1);
    ASSERT_NE(stream1, nullptr);
    ASSERT_EQ(stream1->get_state(), StreamState::OPEN);

    bool success = client_conn.send_rst_stream_frame_action(1, ErrorCode::CANCEL);
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 1u);

    const auto& fi = sent_frames_capture[0];
    EXPECT_EQ(fi.type, FrameType::RST_STREAM);
    EXPECT_EQ(fi.stream_id, 1u);
    EXPECT_EQ(fi.payload.size(), 4u);
    // ErrorCode::CANCEL is 0x8
    std::vector<std::byte> expected_payload = {std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x08)};
    EXPECT_EQ(fi.payload, expected_payload);

    ASSERT_NE(client_conn.get_stream(1), nullptr); // Stream object might still exist until connection cleans up
    EXPECT_EQ(client_conn.get_stream(1)->get_state(), StreamState::CLOSED);
}

TEST_F(Http2ConnectionTest, SendPingAckAction) {
    std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    PingFrame received_ping; // Simulate a received PING
    std::iota(received_ping.opaque_data.begin(), received_ping.opaque_data.end(), std::byte(1));

    bool success = client_conn.send_ping_ack_action(received_ping);
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 1u);

    const auto& fi = sent_frames_capture[0];
    EXPECT_EQ(fi.type, FrameType::PING);
    EXPECT_EQ(fi.stream_id, 0u);
    EXPECT_TRUE(fi.flags & PingFrame::ACK_FLAG);
    EXPECT_EQ(fi.payload.size(), 8u);
    std::vector<std::byte> expected_payload_vec(received_ping.opaque_data.begin(), received_ping.opaque_data.end());
    EXPECT_EQ(fi.payload, expected_payload_vec);
}

TEST_F(Http2ConnectionTest, SendGoAwayAction) {
     std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    std::string debug_msg = "test";
    bool success = client_conn.send_goaway_action(5, ErrorCode::ENHANCE_YOUR_CALM, debug_msg);
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 1u);

    const auto& fi = sent_frames_capture[0];
    EXPECT_EQ(fi.type, FrameType::GOAWAY);
    EXPECT_EQ(fi.stream_id, 0u);
    // Last Stream ID (5), Error Code (ENHANCE_YOUR_CALM = 0xb), Debug Data ("test")
    // Payload: 00000005 0000000b 74657374
    std::vector<std::byte> expected_payload = {
        std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x05),
        std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x0b),
        std::byte('t'), std::byte('e'), std::byte('s'), std::byte('t')
    };
    EXPECT_EQ(fi.payload, expected_payload);
    EXPECT_TRUE(client_conn.is_going_away());
}

TEST_F(Http2ConnectionTest, SendWindowUpdateAction) {
    std::vector<FrameSentInfo> sent_frames_capture;
    client_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    // For stream 0 (connection)
    bool success_conn = client_conn.send_window_update_action(0, 10000);
    EXPECT_TRUE(success_conn);
    ASSERT_EQ(sent_frames_capture.size(), 1u);
    const auto& fi_conn = sent_frames_capture[0];
    EXPECT_EQ(fi_conn.type, FrameType::WINDOW_UPDATE);
    EXPECT_EQ(fi_conn.stream_id, 0u);
    // Increment 10000 (0x2710)
    std::vector<std::byte> expected_payload_conn = {std::byte(0x00), std::byte(0x00), std::byte(0x27), std::byte(0x10)};
    EXPECT_EQ(fi_conn.payload, expected_payload_conn);
    sent_frames_capture.clear();

    // For a specific stream
    client_conn.send_headers(1, make_headers_fs({{":method", "GET"}}), false); // Open stream 1
    sent_frames_capture.clear();
    bool success_stream = client_conn.send_window_update_action(1, 5000);
    EXPECT_TRUE(success_stream);
    ASSERT_EQ(sent_frames_capture.size(), 1u);
    const auto& fi_stream = sent_frames_capture[0];
    EXPECT_EQ(fi_stream.type, FrameType::WINDOW_UPDATE);
    EXPECT_EQ(fi_stream.stream_id, 1u);
    // Increment 5000 (0x1388)
    std::vector<std::byte> expected_payload_stream = {std::byte(0x00), std::byte(0x00), std::byte(0x13), std::byte(0x88)};
    EXPECT_EQ(fi_stream.payload, expected_payload_stream);
}

TEST_F(Http2ConnectionTest, SendPushPromiseFrame) {
    std::vector<FrameSentInfo> sent_frames_capture;
    server_conn.set_on_send_bytes([&](std::vector<std::byte> bytes){
        sent_frames_capture.emplace_back(bytes);
    });

    // Associated stream 1 needs to be open or half-closed(local) on server
    server_conn.process_incoming_data( // Simulate client opening stream 1
        construct_frame_bytes(1, FrameType::HEADERS, HeadersFrame::END_HEADERS_FLAG, 1, {std::byte(0x82)})
    );
    sent_frames_capture.clear(); // Clear the received HEADERS from server's perspective

    stream_id_t associated_stream_id = 1;
    stream_id_t promised_stream_id = 2; // Server promises an even stream
    std::vector<HttpHeader> headers = make_headers_fs({{":method", "GET"}, {":path", "/promised.js"}});
    // HPACK for :method:GET (82), :path:/promised.js (44 + len + path_val)
    // :path:/promised.js -> 44 0c 2f70726f6d697365642e6a73 (idx 4, literal len 12)
    // HPACK payload: 82 44 0c 2f70726f6d697365642e6a73  (1 + 1+1+12 = 15 bytes)

    bool success = server_conn.send_push_promise(associated_stream_id, promised_stream_id, headers);
    EXPECT_TRUE(success);
    ASSERT_EQ(sent_frames_capture.size(), 1u); // Assuming fits in one PUSH_PROMISE frame

    const auto& fi = sent_frames_capture[0];
    EXPECT_EQ(fi.type, FrameType::PUSH_PROMISE);
    EXPECT_EQ(fi.stream_id, associated_stream_id); // PP is on associated stream
    EXPECT_TRUE(fi.flags & HeadersFrame::END_HEADERS_FLAG); // Should be set by serializer if fits

    // Payload: Promised Stream ID (4 bytes) + HPACK (15 bytes) = 19 bytes
    // Promised Stream ID: 00000002
    // Full payload: 00000002 82440c2f70726f6d697365642e6a73
    EXPECT_EQ(fi.payload.size(), 4u + 15u);
    std::vector<std::byte> expected_pp_payload_prefix = {std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x02)};
    EXPECT_TRUE(std::equal(expected_pp_payload_prefix.begin(), expected_pp_payload_prefix.end(), fi.payload.begin()));

    Http2Stream* promised_stream = server_conn.get_stream(promised_stream_id);
    ASSERT_NE(promised_stream, nullptr);
    EXPECT_EQ(promised_stream->get_state(), StreamState::RESERVED_LOCAL);
}


// TODO: Test flow control error scenarios (sending too much data)
// TODO: Test SETTINGS_INITIAL_WINDOW_SIZE change affecting existing stream windows
// TODO: Test MAX_CONCURRENT_STREAMS limit (requires sending frames from connection)
// TODO: Test PUSH_PROMISE scenarios (client receiving, server sending)

// Main is in test_hpack_decoder.cpp or test_http2_parser.cpp
// int main(int argc, char **argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     return RUN_ALL_TESTS();
// }
