#include "gtest/gtest.h"
#include "http2_frame_serializer.h"
#include "http2_frame.h"
#include "hpack_encoder.h" // For tests involving HEADERS/PUSH_PROMISE
#include <vector>
#include <string>
#include <iomanip> // For std::setfill, std::setw with ostringstream
#include <sstream> // For ostringstream

using namespace http2;
using namespace FrameSerializer;

// Helper to convert vector of bytes to hex string for easy comparison
std::string bytes_to_hex_fs(const std::vector<std::byte>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::byte b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

// Helper to create HttpHeader vector
std::vector<HttpHeader> make_headers_fs(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::vector<HttpHeader> headers;
    for (const auto& p : pairs) {
        headers.push_back({p.first, p.second});
    }
    return headers;
}


TEST(FrameSerializerTest, SerializeDataFrame) {
    DataFrame df;
    df.header.type = FrameType::DATA;
    df.header.flags = DataFrame::END_STREAM_FLAG;
    df.header.stream_id = 1;
    df.data = {std::byte('h'), std::byte('e'), std::byte('l'), std::byte('l'), std::byte('o')};
    // Length should be 5. Serializer calculates this.

    auto bytes = serialize_data_frame(df);
    // Expected: 000005 (len) 00 (type DATA) 01 (flags END_STREAM) 00000001 (stream 1) 68656c6c6f (hello)
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000500010000000168656c6c6f");
}

TEST(FrameSerializerTest, SerializeDataFrameWithPadding) {
    DataFrame df;
    df.header.type = FrameType::DATA;
    df.header.flags = DataFrame::PADDED_FLAG;
    df.header.stream_id = 3;
    df.pad_length = 4;
    df.data = {std::byte('h'), std::byte('i')};
    // Payload: PadLen(1) + Data(2) + Padding(4) = 7 bytes
    // Expected: 000007 (len) 00 (type DATA) 08 (flags PADDED) 00000003 (stream 3) 04 (PadLen) 6869 (hi) 00000000 (padding)
    auto bytes = serialize_data_frame(df);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000700080000000304686900000000");
}

TEST(FrameSerializerTest, SerializeSettingsFrame) {
    SettingsFrame sf;
    sf.header.type = FrameType::SETTINGS;
    sf.header.flags = 0;
    sf.header.stream_id = 0;
    sf.settings.push_back({SettingsFrame::SETTINGS_MAX_CONCURRENT_STREAMS, 100}); // id 3, val 100
    sf.settings.push_back({SettingsFrame::SETTINGS_INITIAL_WINDOW_SIZE, 65536});// id 4, val 65536
    // Length = 2 * 6 = 12 bytes.
    // Expected: 00000c (len) 04 (type SETTINGS) 00 (flags) 00000000 (stream 0)
    //           0003 00000064 (setting 1)
    //           0004 00010000 (setting 2)
    auto bytes = serialize_settings_frame(sf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000c040000000000000300000064000400010000");
}

TEST(FrameSerializerTest, SerializeSettingsAck) {
    SettingsFrame sf;
    sf.header.type = FrameType::SETTINGS;
    sf.header.flags = SettingsFrame::ACK_FLAG;
    sf.header.stream_id = 0;
    // Length = 0
    // Expected: 000000 (len) 04 (type SETTINGS) 01 (flags ACK) 00000000 (stream 0)
    auto bytes = serialize_settings_frame(sf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "000000040100000000");
}

TEST(FrameSerializerTest, SerializePingFrame) {
    PingFrame pf;
    pf.header.type = FrameType::PING;
    pf.header.flags = 0;
    pf.header.stream_id = 0;
    pf.opaque_data = {std::byte(1), std::byte(2), std::byte(3), std::byte(4), std::byte(5), std::byte(6), std::byte(7), std::byte(8)};
    // Length = 8
    // Expected: 000008 (len) 06 (type PING) 00 (flags) 00000000 (stream 0) 0102030405060708 (data)
    auto bytes = serialize_ping_frame(pf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "0000080600000000000102030405060708");
}

TEST(FrameSerializerTest, SerializePingAck) {
    PingFrame pf;
    pf.header.type = FrameType::PING;
    pf.header.flags = PingFrame::ACK_FLAG;
    pf.header.stream_id = 0;
    pf.opaque_data = {std::byte(8), std::byte(7), std::byte(6), std::byte(5), std::byte(4), std::byte(3), std::byte(2), std::byte(1)};
    // Expected: 000008 (len) 06 (type PING) 01 (flags ACK) 00000000 (stream 0) 0807060504030201 (data)
    auto bytes = serialize_ping_frame(pf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "0000080601000000000807060504030201");
}

TEST(FrameSerializerTest, SerializeHeadersFrameSimple) {
    HpackEncoder encoder; // Default settings
    HeadersFrame hf;
    hf.header.type = FrameType::HEADERS;
    hf.header.flags = HeadersFrame::END_HEADERS_FLAG | HeadersFrame::END_STREAM_FLAG;
    hf.header.stream_id = 5;
    hf.headers = make_headers_fs({{":method", "GET"}}); // Encodes to 0x82
    // Length = 1 (for 0x82)
    // Expected: 000001 (len) 01 (type HEADERS) 05 (flags END_HEADERS|END_STREAM) 00000005 (stream 5) 82
    auto bytes = serialize_headers_frame(hf, encoder);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000101050000000582");
}

TEST(FrameSerializerTest, SerializeHeadersFrameWithPriority) {
    HpackEncoder encoder;
    HeadersFrame hf;
    hf.header.type = FrameType::HEADERS;
    hf.header.flags = HeadersFrame::END_HEADERS_FLAG | HeadersFrame::PRIORITY_FLAG;
    hf.header.stream_id = 7;
    hf.headers = make_headers_fs({{":status", "200"}}); // Encodes to 0x88
    hf.exclusive_dependency = true;
    hf.stream_dependency = 3;
    hf.weight = 15; // This is value 0-255, representing weight 1-256. So 15 means weight 16.
                     // Serializer should send 15.
    // Priority Payload: E(1)|StreamDep(3) + Weight(15) -> 80000003 (for dep) 0f (for weight) = 5 bytes
    // HPACK payload: 0x88 (1 byte)
    // Total payload length = 5 + 1 = 6
    // Expected: 000006 (len) 01 (type HEADERS) 24 (flags END_HEADERS|PRIORITY) 00000007 (stream 7)
    //           80000003 (Exclusive, Stream Dep 3)
    //           0f       (Weight 15 -> actual weight 16)
    //           88       (:status: 200)
    auto bytes = serialize_headers_frame(hf, encoder);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "000006012400000007800000030f88");
}


TEST(FrameSerializerTest, SerializeRstStreamFrame) {
    RstStreamFrame rsf;
    rsf.header.type = FrameType::RST_STREAM;
    rsf.header.flags = 0;
    rsf.header.stream_id = 9;
    rsf.error_code = ErrorCode::CANCEL; // 0x8
    // Expected: 000004 (len) 03 (type RST_STREAM) 00 (flags) 00000009 (stream 9) 00000008 (err code)
    auto bytes = serialize_rst_stream_frame(rsf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000403000000000900000008");
}

TEST(FrameSerializerTest, SerializeGoAwayFrame) {
    GoAwayFrame gaf;
    gaf.header.type = FrameType::GOAWAY;
    gaf.header.flags = 0;
    gaf.header.stream_id = 0;
    gaf.last_stream_id = 7;
    gaf.error_code = ErrorCode::PROTOCOL_ERROR; // 0x1
    std::string debug_msg = "bye";
    std::transform(debug_msg.begin(), debug_msg.end(), std::back_inserter(gaf.additional_debug_data),
                   [](char c){ return static_cast<std::byte>(c); });
    // Payload: LastStream(4) + ErrCode(4) + DebugData(3) = 11 bytes
    // Expected: 00000b (len) 07 (type GOAWAY) 00 (flags) 00000000 (stream 0)
    //           00000007 (last stream 7)
    //           00000001 (err code PROTOCOL_ERROR)
    //           627965   ("bye")
    auto bytes = serialize_goaway_frame(gaf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000b0700000000000000000700000001627965");
}

TEST(FrameSerializerTest, SerializeWindowUpdateFrame) {
    WindowUpdateFrame wuf;
    wuf.header.type = FrameType::WINDOW_UPDATE;
    wuf.header.flags = 0;
    wuf.header.stream_id = 11;
    wuf.window_size_increment = 100000; // 0x0186A0
    // Expected: 000004 (len) 08 (type WINDOW_UPDATE) 00 (flags) 0000000b (stream 11) 000186a0 (increment)
    auto bytes = serialize_window_update_frame(wuf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000408000000000b000186a0");
}

TEST(FrameSerializerTest, SerializeContinuationFrame) {
    ContinuationFrame cf;
    cf.header.type = FrameType::CONTINUATION;
    cf.header.flags = ContinuationFrame::END_HEADERS_FLAG;
    cf.header.stream_id = 13;
    cf.header_block_fragment = {std::byte(0x01), std::byte(0x02), std::byte(0x03)}; // Dummy HPACK fragment
    // Expected: 000003 (len) 09 (type CONTINUATION) 04 (flags END_HEADERS) 0000000d (stream 13) 010203
    auto bytes = serialize_continuation_frame(cf);
    EXPECT_EQ(bytes_to_hex_fs(bytes), "00000309040000000d010203");
}


TEST(FrameSerializerTest, SerializeHeaderBlockWithContinuationSmall) {
    HpackEncoder encoder;
    FrameHeader initial_header;
    initial_header.type = FrameType::HEADERS;
    initial_header.flags = HeadersFrame::END_STREAM_FLAG; // END_HEADERS will be added by func
    initial_header.stream_id = 1;

    auto headers = make_headers_fs({{":method", "POST"}, {":path", "/submit"}});
    // :method: POST -> 83
    // :path: /submit -> 44 07 2f7375626d6974 (idx 4 for :path, literal "/submit" len 7)
    // Total HPACK: 83 44 07 2f7375626d6974 (1 + 1 + 1 + 7 = 10 bytes)

    auto result = serialize_header_block_with_continuation(initial_header, headers, encoder, 100, false);

    EXPECT_TRUE(result.continuation_frames_bytes.empty());
    // Expected HEADERS frame:
    // Length 10, Type HEADERS (01), Flags END_STREAM | END_HEADERS (01 | 04 = 05), Stream 1
    // Payload: 8344072f7375626d6974
    // Full: 00000a 01 05 00000001 8344072f7375626d6974
    EXPECT_EQ(bytes_to_hex_fs(result.headers_frame_bytes), "00000a0105000000018344072f7375626d6974");
}

TEST(FrameSerializerTest, SerializeHeaderBlockWithContinuationLarge) {
    HpackEncoder encoder;
    FrameHeader initial_header;
    initial_header.type = FrameType::HEADERS;
    initial_header.flags = 0; // No END_STREAM initially
    initial_header.stream_id = 3;

    // Create a large header value that will force continuation
    std::string long_value(50, 'a'); // HPACK for this will be > 10 bytes
    auto headers = make_headers_fs({
        {":method", "GET"},                             // 82
        {"long-header", long_value}                     // 40 (new name) 0B 6c6f6e672d686561646572 (long-header) + (huffman for 50 'a's)
                                                        // literal 'a' is 0x61 (code 0x61, 7 bits). 50*7 bits = 350 bits ~ 44 bytes for value
                                                        // Name "long-header" (11 chars) -> 0x0B + 11 bytes
                                                        // Total HPACK: 1 (method) + 1(idx0) + 1(len_name) + 11(name) + 1(len_val_huff) + ~44 (val_huff) ~ 59 bytes
    });

    uint32_t peer_max_frame_size = 20; // Small max frame size to force continuation
    auto result = serialize_header_block_with_continuation(initial_header, headers, encoder, peer_max_frame_size, false);

    ASSERT_FALSE(result.headers_frame_bytes.empty());
    ASSERT_FALSE(result.continuation_frames_bytes.empty());

    // First frame (HEADERS)
    // Should have initial_header.flags (0) and NOT END_HEADERS
    // Length should be peer_max_frame_size
    FrameHeader first_fh;
    first_fh.length = static_cast<uint32_t>(result.headers_frame_bytes.size() - 9);
    first_fh.type = FrameType::HEADERS;
    first_fh.flags = static_cast<uint8_t>(result.headers_frame_bytes[4]);
    first_fh.stream_id = 1;

    EXPECT_EQ(first_fh.length, 16384 - 5);

    // Last CONTINUATION frame
    const auto& last_cont_bytes = result.continuation_frames_bytes.back();
    ASSERT_GE(last_cont_bytes.size(), 9u);
    FrameHeader last_cont_fh;
    last_cont_fh.length = static_cast<uint32_t>(last_cont_bytes.size() - 9);
    last_cont_fh.type = FrameType::CONTINUATION;
    last_cont_fh.flags = static_cast<uint8_t>(last_cont_bytes[4]);
    last_cont_fh.stream_id = 1;

    EXPECT_EQ(last_cont_fh.length, 1);

    // Verify total HPACKed data is reassembled correctly (conceptual check, hard to do byte-wise here without decoding)
    std::vector<std::byte> reassembled_hpack;
    reassembled_hpack.insert(reassembled_hpack.end(), result.headers_frame_bytes.begin() + 9, result.headers_frame_bytes.end());
    for(const auto& cont_payload_vec : result.continuation_frames_bytes) {
        reassembled_hpack.insert(reassembled_hpack.end(), cont_payload_vec.begin() + 9, cont_payload_vec.end());
    }

    auto [expected_full_hpack, _] = encoder.encode(headers); // Re-encode to get target
    EXPECT_EQ(reassembled_hpack, expected_full_hpack);

}

// int main(int argc, char **argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     return RUN_ALL_TESTS();
// }
