#include "gtest/gtest.h"
#include "http2_parser.h"
#include "http2_connection.h"
#include "hpack_decoder.h"    // Parser needs hpack decoder
#include <vector>
#include <cstring> // for memcpy

using namespace http2;

// Helper to create a std::span<const std::byte> from a uint8_t array
template<size_t N>
std::span<const std::byte> make_byte_span(const uint8_t (&arr)[N]) {
    return std::as_bytes(std::span<const uint8_t>(arr, N));
}

// Helper to construct frame bytes
std::vector<std::byte> construct_frame(uint32_t length, FrameType type, uint8_t flags, uint32_t stream_id, const std::vector<std::byte>& payload) {
    std::vector<std::byte> frame_bytes(9 + payload.size());
    frame_bytes[0] = static_cast<std::byte>((length >> 16) & 0xFF);
    frame_bytes[1] = static_cast<std::byte>((length >> 8) & 0xFF);
    frame_bytes[2] = static_cast<std::byte>(length & 0xFF);
    frame_bytes[3] = static_cast<std::byte>(type);
    frame_bytes[4] = static_cast<std::byte>(flags);
    frame_bytes[5] = static_cast<std::byte>((stream_id >> 24) & 0xFF); // Includes R bit (should be 0 for parser input)
    frame_bytes[6] = static_cast<std::byte>((stream_id >> 16) & 0xFF);
    frame_bytes[7] = static_cast<std::byte>((stream_id >> 8) & 0xFF);
    frame_bytes[8] = static_cast<std::byte>(stream_id & 0xFF);
    std::memcpy(frame_bytes.data() + 9, payload.data(), payload.size());
    return frame_bytes;
}

class Http2ParserTest : public ::testing::Test {
public:
    Http2ParserTest() : connection_context(true /*is_server*/), parser(hpack_decoder, connection_context) {
        parser.set_frame_callback([this](AnyHttp2Frame frame, const std::vector<std::byte>& payload){
            parsed_frames_store.push_back(std::move(frame));
        });
        // Set a reasonable max frame size for tests, e.g., default
        connection_context.apply_local_setting({http2::SettingsFrame::SETTINGS_MAX_FRAME_SIZE, DEFAULT_MAX_FRAME_SIZE});
        connection_context.apply_remote_setting({http2::SettingsFrame::SETTINGS_MAX_FRAME_SIZE, DEFAULT_MAX_FRAME_SIZE});
    }

protected:
    HpackDecoder hpack_decoder;
    // For parser tests, we can use the real connection object.
    Http2Connection connection_context;
    Http2Parser parser;
    std::vector<AnyHttp2Frame> parsed_frames_store;
    ParserError last_parser_error_ = ParserError::OK;

    void SetUp() override {
        parsed_frames_store.clear();
        last_parser_error_ = ParserError::OK;
        parser.reset(); // Reset parser state for each test
        connection_context.clear_header_block_buffer(); // Clear any continuation state
        connection_context.finish_continuation();
    }

    // Helper to feed data to the parser and store the last error
    size_t feed_parser(std::span<const std::byte> data) {
        auto [consumed, err] = parser.parse(data);
        last_parser_error_ = err;
        return consumed;
    }
};

TEST_F(Http2ParserTest, ParseDataFrameSimple) {
    std::vector<std::byte> payload = {std::byte('h'), std::byte('e'), std::byte('l'), std::byte('l'), std::byte('o')};
    uint32_t stream_id = 1;
    uint8_t flags = DataFrame::END_STREAM_FLAG;
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::DATA, flags, stream_id, payload);

    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);

    const auto* data_frame = std::get_if<DataFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(data_frame, nullptr);
    EXPECT_EQ(data_frame->header.type, FrameType::DATA);
    EXPECT_EQ(data_frame->header.length, payload.size());
    EXPECT_EQ(data_frame->header.flags, flags);
    EXPECT_EQ(data_frame->header.get_stream_id(), stream_id);
    EXPECT_TRUE(data_frame->has_end_stream_flag());
    EXPECT_FALSE(data_frame->has_padded_flag());
    ASSERT_EQ(data_frame->data.size(), payload.size());
    for(size_t i=0; i<payload.size(); ++i) {
        EXPECT_EQ(data_frame->data[i], payload[i]);
    }
}

TEST_F(Http2ParserTest, ParseDataFrameWithPadding) {
    std::vector<std::byte> actual_data = {std::byte('h'), std::byte('i')};
    uint8_t pad_length = 5;
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(pad_length)); // Pad Length field
    payload.insert(payload.end(), actual_data.begin(), actual_data.end()); // Actual data
    for(uint8_t i=0; i<pad_length; ++i) payload.push_back(static_cast<std::byte>(0)); // Padding

    uint32_t stream_id = 3;
    uint8_t flags = DataFrame::PADDED_FLAG;
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::DATA, flags, stream_id, payload);

    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);

    const auto* data_frame = std::get_if<DataFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(data_frame, nullptr);
    EXPECT_TRUE(data_frame->has_padded_flag());
    ASSERT_TRUE(data_frame->pad_length.has_value());
    EXPECT_EQ(data_frame->pad_length.value(), pad_length);
    ASSERT_EQ(data_frame->data.size(), actual_data.size());
     for(size_t i=0; i<actual_data.size(); ++i) {
        EXPECT_EQ(data_frame->data[i], actual_data[i]);
    }
}

TEST_F(Http2ParserTest, ParseSettingsFrame) {
    // Setting: MAX_CONCURRENT_STREAMS (0x3) = 100 (0x64)
    // Setting: INITIAL_WINDOW_SIZE (0x4) = 65536 (0x10000)
    std::vector<std::byte> payload = {
        std::byte(0x00), std::byte(0x03), std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x64), // MAX_CONCURRENT_STREAMS = 100
        std::byte(0x00), std::byte(0x04), std::byte(0x00), std::byte(0x01), std::byte(0x00), std::byte(0x00)  // INITIAL_WINDOW_SIZE = 65536
    };
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::SETTINGS, 0, 0, payload);

    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);

    const auto* settings_frame = std::get_if<SettingsFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(settings_frame, nullptr);
    EXPECT_EQ(settings_frame->header.type, FrameType::SETTINGS);
    EXPECT_EQ(settings_frame->header.stream_id, 0u);
    ASSERT_EQ(settings_frame->settings.size(), 2u);
    EXPECT_EQ(settings_frame->settings[0].identifier, SettingsFrame::SETTINGS_MAX_CONCURRENT_STREAMS);
    EXPECT_EQ(settings_frame->settings[0].value, 100u);
    EXPECT_EQ(settings_frame->settings[1].identifier, SettingsFrame::SETTINGS_INITIAL_WINDOW_SIZE);
    EXPECT_EQ(settings_frame->settings[1].value, 65536u);
}

TEST_F(Http2ParserTest, ParseSettingsAck) {
    auto frame_bytes = construct_frame(0, FrameType::SETTINGS, SettingsFrame::ACK_FLAG, 0, {});
    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);
    const auto* settings_frame = std::get_if<SettingsFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(settings_frame, nullptr);
    EXPECT_TRUE(settings_frame->has_ack_flag());
    EXPECT_TRUE(settings_frame->settings.empty());
}

TEST_F(Http2ParserTest, ParsePingFrame) {
    std::vector<std::byte> payload(8);
    for(int i=0; i<8; ++i) payload[i] = static_cast<std::byte>(i);
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::PING, 0, 0, payload);

    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);
    const auto* ping_frame = std::get_if<PingFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(ping_frame, nullptr);
    for(int i=0; i<8; ++i) {
        EXPECT_EQ(ping_frame->opaque_data[i], static_cast<std::byte>(i));
    }
}

TEST_F(Http2ParserTest, ParseWindowUpdateFrame) {
    std::vector<std::byte> payload = {std::byte(0x00), std::byte(0x0F), std::byte(0x42), std::byte(0x40)}; // Increment = 1000000
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::WINDOW_UPDATE, 0, 1, payload);
    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);
    const auto* wu_frame = std::get_if<WindowUpdateFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(wu_frame, nullptr);
    EXPECT_EQ(wu_frame->window_size_increment, 1000000u);
}

TEST_F(Http2ParserTest, ParseHeadersFrameSimple) {
    // HEADERS for stream 1, END_HEADERS
    // Payload: example HPACK data for a simple header list
    // e.g., :method: GET (index 2 -> 0x82)
    std::vector<std::byte> hpack_payload = {static_cast<std::byte>(0x82)};
    uint8_t flags = HeadersFrame::END_HEADERS_FLAG | HeadersFrame::END_STREAM_FLAG;
    auto frame_bytes = construct_frame(static_cast<uint32_t>(hpack_payload.size()), FrameType::HEADERS, flags, 1, hpack_payload);

    feed_parser(frame_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);

    const auto* headers_frame = std::get_if<HeadersFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(headers_frame, nullptr);
    EXPECT_TRUE(headers_frame->has_end_headers_flag());
    EXPECT_TRUE(headers_frame->has_end_stream_flag());
    ASSERT_EQ(headers_frame->headers.size(), 1u);
    EXPECT_EQ(headers_frame->headers[0].name, ":method");
    EXPECT_EQ(headers_frame->headers[0].value, "GET");
}

TEST_F(Http2ParserTest, ParseContinuationFrames) {
    // HEADERS frame, stream 1, NO END_HEADERS
    // Payload: :method: GET (0x82)
    std::vector<std::byte> hpack_payload1 = {static_cast<std::byte>(0x82)};
    uint8_t flags_h = 0; // No END_HEADERS
    auto headers_frame_bytes = construct_frame(static_cast<uint32_t>(hpack_payload1.size()), FrameType::HEADERS, flags_h, 1, hpack_payload1);

    // CONTINUATION frame, stream 1, END_HEADERS
    // Payload: :path: / (0x84)
    std::vector<std::byte> hpack_payload2 = {static_cast<std::byte>(0x84)};
    uint8_t flags_c = ContinuationFrame::END_HEADERS_FLAG;
    auto cont_frame_bytes = construct_frame(static_cast<uint32_t>(hpack_payload2.size()), FrameType::CONTINUATION, flags_c, 1, hpack_payload2);

    std::vector<std::byte> all_bytes;
    all_bytes.insert(all_bytes.end(), headers_frame_bytes.begin(), headers_frame_bytes.end());
    all_bytes.insert(all_bytes.end(), cont_frame_bytes.begin(), cont_frame_bytes.end());

    feed_parser(all_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    // We expect two frames to be "parsed" at raw level by parser
    // But the HttpConnection logic should assemble them.
    // The callback to Http2ParserTest is for *raw* frames.
    // The HeadersFrame object from the first callback will have empty headers.
    // The ContinuationFrame object from the second callback will have the END_HEADERS flag.
    // The HttpConnection is responsible for calling HPACK on the combined buffer.
    // For this test, we verify the parser correctly identifies both frames.
    // And that HttpConnection's state for continuation is correctly managed.

    ASSERT_EQ(parsed_frames_store.size(), 2u);

    const auto* hf = std::get_if<HeadersFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(hf, nullptr);
    EXPECT_FALSE(hf->has_end_headers_flag());
    // At this raw parsing stage, hf->headers might be empty because END_HEADERS wasn't set.
    // The connection context would hold the fragment.

    const auto* cf = std::get_if<ContinuationFrame>(&parsed_frames_store[1].frame_variant);
    ASSERT_NE(cf, nullptr);
    EXPECT_TRUE(cf->has_end_headers_flag());

    // To test the final assembled headers, we need to inspect HttpConnection or have it emit a final event.
    // For now, let's assume the connection logic in parser `parse_headers_payload` and `parse_continuation_payload`
    // correctly calls HPACK decoder. We can check the *last* frame that caused END_HEADERS.
    // The `HeadersFrame` that is eventually "completed" by the `ContinuationFrame` is not directly
    // emitted by the parser again. The parser emits raw frames.
    // The HttpConnection would be responsible for this.
    // This test shows the parser part works. A Connection test would verify assembly.
    // However, our current parser design calls HPACK from within parse_xxx_payload if END_HEADERS is set.
    // So, the HeadersFrame object (hf above) SHOULD have the headers if the CONTINUATION path works.
    // Let's check connection's pending headers (if it had such a public field for tests)
    // OR, the parser test could be more integrated with connection.

    // Let's re-evaluate: parser's parse_continuation_payload, if it has END_HEADERS,
    // calls hpack_decoder_.decode(connection_context_.get_header_block_buffer_span());
    // and then connection_context_.populate_pending_headers(decoded_headers);
    // This `populate_pending_headers` is the tricky part for testing via parser alone.

    // For this test, let's verify the state of the HPACK decoder in the connection context
    // after all bytes are processed.
    // The `HeadersFrame` object in `parsed_frames_store[0]` was created when the first part was parsed.
    // If `populate_pending_headers` was to update *that object*, it's complex.
    // It's more likely the connection would emit a "headers_complete" event.

    // Given the current parser design, the `hf->headers` will be populated by the time
    // the CONTINUATION frame with END_HEADERS is processed and its callback completes.
    // This means the `HeadersFrame` object initially created for the first frame *is* the one
    // that eventually gets its `headers` member populated.
    // This depends on `Http2Connection::populate_pending_headers` having a reference to it.
    // This is not the case. The parser emits distinct frame objects.
    // The `connection_context_.populate_pending_headers` is a placeholder.

    // What we *can* test here:
    // 1. First frame is HEADERS, no END_HEADERS.
    // 2. Connection expects continuation for stream 1.
    // 3. Second frame is CONTINUATION, stream 1, with END_HEADERS.
    // 4. Connection no longer expects continuation.
    // 5. HPACK decoder was called with combined buffer. (Hard to check directly here without mocking HPACK)

    EXPECT_TRUE(connection_context.is_expecting_continuation() == false); // Should be false after END_HEADERS
}


TEST_F(Http2ParserTest, ErrorFrameSizeExceeded) {
    connection_context.apply_remote_setting({http2::SettingsFrame::SETTINGS_MAX_FRAME_SIZE, 10}); // Small max frame size
    
    std::vector<std::byte> payload(11, std::byte('A')); // Payload is 11 bytes > 10
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::DATA, 0, 1, {}); // Payload not actually sent here
                                                                                                        // The header declares length 11.

    // Create frame header declaring length 11
    std::vector<std::byte> oversized_header_bytes = {
        std::byte(0x00), std::byte(0x00), std::byte(0x0B), // Length 11
        std::byte(FrameType::DATA), std::byte(0x00),
        std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x01) // Stream 1
    };
    // We don't even need to send payload, header itself is the problem.
    feed_parser(oversized_header_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::FRAME_SIZE_LIMIT_EXCEEDED);
    EXPECT_TRUE(parsed_frames_store.empty()); // Parser should stop before forming a frame object
}


TEST_F(Http2ParserTest, PartialFrameThenComplete) {
    std::vector<std::byte> payload = {std::byte('h'), std::byte('e'), std::byte('l'), std::byte('l'), std::byte('o')};
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::DATA, 0, 1, payload);

    // Feed only first 5 bytes (incomplete header)
    feed_parser(std::span<const std::byte>(frame_bytes.data(), 5));
    ASSERT_EQ(last_parser_error_, ParserError::OK); // OK, but needs more data
    ASSERT_TRUE(parsed_frames_store.empty());

    // Feed remaining bytes
    feed_parser(std::span<const std::byte>(frame_bytes.data() + 5, frame_bytes.size() - 5));
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 1u);
    const auto* data_frame = std::get_if<DataFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(data_frame, nullptr);
    EXPECT_EQ(data_frame->header.length, payload.size());
}

TEST_F(Http2ParserTest, TwoFramesConcatenated) {
    std::vector<std::byte> payload1 = {std::byte('f'), std::byte('1')};
    auto frame1_bytes = construct_frame(static_cast<uint32_t>(payload1.size()), FrameType::DATA, 0, 1, payload1);
    std::vector<std::byte> payload2 = {std::byte('f'), std::byte('2'), std::byte('d')};
    auto frame2_bytes = construct_frame(static_cast<uint32_t>(payload2.size()), FrameType::DATA, DataFrame::END_STREAM_FLAG, 1, payload2);

    std::vector<std::byte> all_bytes;
    all_bytes.insert(all_bytes.end(), frame1_bytes.begin(), frame1_bytes.end());
    all_bytes.insert(all_bytes.end(), frame2_bytes.begin(), frame2_bytes.end());

    feed_parser(all_bytes);
    ASSERT_EQ(last_parser_error_, ParserError::OK);
    ASSERT_EQ(parsed_frames_store.size(), 2u);

    const auto* df1 = std::get_if<DataFrame>(&parsed_frames_store[0].frame_variant);
    ASSERT_NE(df1, nullptr);
    EXPECT_EQ(df1->data.size(), 2u);
    EXPECT_EQ(df1->data[0], static_cast<std::byte>('f'));

    const auto* df2 = std::get_if<DataFrame>(&parsed_frames_store[1].frame_variant);
    ASSERT_NE(df2, nullptr);
    EXPECT_EQ(df2->data.size(), 3u);
    EXPECT_TRUE(df2->has_end_stream_flag());
}

TEST_F(Http2ParserTest, ErrorRstStreamInvalidLength) {
    std::vector<std::byte> payload = {std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x01), std::byte(0x00)}; // 5 bytes, expected 4
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::RST_STREAM, 0, 1, payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_FRAME_SIZE);
}

TEST_F(Http2ParserTest, ErrorWindowUpdateInvalidLength) {
    std::vector<std::byte> payload = {std::byte(0x00), std::byte(0x00), std::byte(0x01)}; // 3 bytes, expected 4
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::WINDOW_UPDATE, 0, 0, payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_FRAME_SIZE);
}

TEST_F(Http2ParserTest, ErrorWindowUpdateZeroIncrement) {
    std::vector<std::byte> payload = {std::byte(0x00), std::byte(0x00), std::byte(0x00), std::byte(0x00)}; // Increment 0
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::WINDOW_UPDATE, 0, 1, payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_WINDOW_UPDATE_INCREMENT);
}

TEST_F(Http2ParserTest, ErrorSettingsAckWithPayload) {
    std::vector<std::byte> payload = {std::byte(0x01)}; // Payload length 1
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::SETTINGS, SettingsFrame::ACK_FLAG, 0, payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_FRAME_SIZE);
}

TEST_F(Http2ParserTest, ErrorSettingsInvalidLength) {
    std::vector<std::byte> payload(5); // Length 5, not multiple of 6
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::SETTINGS, 0, 0, payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_FRAME_SIZE);
}

TEST_F(Http2ParserTest, ErrorDataFrameOnStreamZero) {
    std::vector<std::byte> payload = {std::byte('a')};
    auto frame_bytes = construct_frame(static_cast<uint32_t>(payload.size()), FrameType::DATA, 0, 0, payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_STREAM_ID);
}

TEST_F(Http2ParserTest, ErrorHeadersFrameOnStreamZero) {
    std::vector<std::byte> hpack_payload = {std::byte(0x82)}; // :method: GET
    auto frame_bytes = construct_frame(static_cast<uint32_t>(hpack_payload.size()), FrameType::HEADERS, HeadersFrame::END_HEADERS_FLAG, 0, hpack_payload);
    feed_parser(frame_bytes);
    EXPECT_EQ(last_parser_error_, ParserError::INVALID_STREAM_ID);
}

// TODO: More tests for padding errors (pad length too large, etc.)
// TODO: Tests for PRIORITY frame specifics
// TODO: Tests for PUSH_PROMISE frame specifics (and server vs client context)
// TODO: Tests for GOAWAY frame specifics
// TODO: Deeper tests for CONTINUATION error scenarios (wrong stream, no preceding HEADERS, etc.)
// TODO: Test HPACK errors propagating from HpackDecoder through Http2Parser

// This main is already in test_hpack_decoder.cpp, GTest needs one main.
// If running tests separately, each would need one.
// For a single test executable, only one test_main.cpp (or one of the test files) should have main().
// int main(int argc, char **argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     return RUN_ALL_TESTS();
// }
