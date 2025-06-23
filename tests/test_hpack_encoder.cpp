#include "gtest/gtest.h"
#include "cpp_lib/hpack_encoder.h"
#include "cpp_lib/hpack_static_table.h" // For checking against static table
#include "cpp_lib/hpack_huffman.h"   // For huffman details
#include <vector>
#include <string>
#include <numeric> // for std::iota

using namespace http2;

// Helper to convert vector of bytes to hex string for easy comparison
std::string bytes_to_hex(const std::vector<std::byte>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::byte b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

// Helper to create HttpHeader vector
std::vector<HttpHeader> make_headers(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::vector<HttpHeader> headers;
    for (const auto& p : pairs) {
        headers.push_back({p.first, p.second});
    }
    return headers;
}


class HpackEncoderTest : public ::testing::Test {
protected:
    HpackEncoder encoder; // Default dynamic table size (4096)

    // Helper to compare encoded output with expected hex string
    void expect_encoded(const std::vector<HttpHeader>& headers, const std::string& expected_hex) {
        auto [encoded_bytes, err] = encoder.encode(headers);
        ASSERT_EQ(err, HpackEncodingError::OK);
        EXPECT_EQ(bytes_to_hex(encoded_bytes), expected_hex);
    }
};

TEST_F(HpackEncoderTest, EncodeIndexedStatic) {
    // :method: GET (index 2) -> 82
    expect_encoded(make_headers({{":method", "GET"}}), "82");
    // :status: 200 (index 8) -> 88
    expect_encoded(make_headers({{":status", "200"}}), "88");
}

TEST_F(HpackEncoderTest, EncodeLiteralWithIncrementalIndexingNameIndexed) {
    // :path: /sample/path (name index 4 for :path)
    // Expected: 44 0C 2f73616d706c652f70617468 (no huffman for value)
    // 0x44 -> 01000100. Index 4.
    // 0x0C -> length 12 for "/sample/path"
    std::vector<HttpHeader> headers = {{":path", "/sample/path"}};
    expect_encoded(headers, "440c2f73616d706c652f70617468");

    // Verify dynamic table (size = 32 + 5 for :path + 12 for /sample/path = 49)
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 49);
    // Now try to encode it using dynamic table index (61 static + 1 = 62)
    // Index 62 -> 0xBE (10111110) -> 80 | 3E
    expect_encoded(headers, "be");
}

TEST_F(HpackEncoderTest, EncodeLiteralWithIncrementalIndexingNewName) {
    // custom-key: custom-value
    // Expected: 40 0A 637573746f6d2d6b6579 0C 637573746f6d2d76616c7565 (no huffman)
    // 0x40 -> 01000000. Index 0 (new name).
    // 0x0A "custom-key" (10 chars)
    // 0x0C "custom-value" (12 chars)
    std::vector<HttpHeader> headers = {{"custom-key", "custom-value"}};
    expect_encoded(headers, "400a637573746f6d2d6b65790c637573746f6d2d76616c7565");
    // Dynamic table size = 32 + 10 + 12 = 54
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 54);
}

TEST_F(HpackEncoderTest, EncodeLiteralWithoutIndexingNameIndexed) {
    // :path: /sample/path, but without indexing
    // Expected: 04 0C 2f73616d706c652f70617468
    // 0x04 -> 00000100. Index 4 (:path).
    uint32_t initial_size = encoder.get_current_dynamic_table_size();
    std::vector<HttpHeader> headers = {{":path", "/sample/path"}};
    // To force "Without Indexing", we can make it too large for current table, or set sensitive (but that's Never Indexed)
    // For this test, let's assume a strategy where it *chooses* not to index.
    // The current encoder strategy will try to incrementally index if possible.
    // To test this path, we might need to fill the dynamic table or have a specific flag on HttpHeader.
    // For now, let's assume the encoder has a reason to choose this (e.g. header is marked "don't index" by caller)
    // Or, we can test by making it not fit.
    encoder.set_own_max_dynamic_table_size(50); // :path (5) + /sample/path (12) + 32 = 49. It *would* fit.
                                               // Let's make it smaller: 40. It won't fit.
    encoder.set_own_max_dynamic_table_size(40);
    expect_encoded(headers, "040c2f73616d706c652f70617468");
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), initial_size); // Should not change if not indexed
    encoder.set_own_max_dynamic_table_size(HpackEncoder::DEFAULT_DYNAMIC_TABLE_SIZE); // Reset
}


TEST_F(HpackEncoderTest, EncodeLiteralNeverIndexedNameIndexed) {
    // cookie: secretvalue (sensitive)
    // "cookie" is static index 32.
    // Expected: 1F 11 0B 73656372657476616c7565 (no huffman for "secretvalue")
    //   1F 11 -> name index 32 (0001xxxx prefix for "Never Indexed", then integer 32)
    //           15 + (17%128) = 15+17 = 32. 1st byte (0x10 | 0xF), 2nd byte 0x11.
    //   0B -> length 11 for "secretvalue"
    std::vector<HttpHeader> headers = {{"cookie", "secretvalue", true}}; // Mark as sensitive
    expect_encoded(headers, "1f110b73656372657476616c7565");
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 0); // Never indexed
}

TEST_F(HpackEncoderTest, EncodeWithHuffman) {
    // :authority: www.example.com (Huffman for value)
    // :authority is static index 1.
    // Expected: 41 8C F1 E3 C2 E5 F2 3A 6B A0 Ab 90 F4 FF (From RFC C.4.1, assuming it's added to table)
    //   41 -> 01000001. Index 1 (:authority), incremental indexing for value.
    //   8C -> Huffman, length 12.
    //   F1... -> Huffman for "www.example.com"
    std::vector<HttpHeader> headers = {{":authority", "www.example.com"}};
    expect_encoded(headers, "418cf1e3c2e5f23a6ba0ab90f4ff");
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 32 + 10 + 15); // :authority(10) + www.example.com(15) + 32 = 57
}


TEST_F(HpackEncoderTest, DynamicTableEvictionOnEncode) {
    encoder.set_own_max_dynamic_table_size(100); // Small table

    // header1: "name1" (5), "value1" (6) -> size 43. Table: [h1] (43/100)
    expect_encoded(make_headers({{"name1", "value1"}}), "40056e616d65310676616c756531");
    ASSERT_EQ(encoder.get_current_dynamic_table_size(), 43);

    // header2: "name2" (5), "value2" (6) -> size 43. Table: [h2, h1] (86/100)
    expect_encoded(make_headers({{"name2", "value2"}}), "40056e616d65320676616c756532");
    ASSERT_EQ(encoder.get_current_dynamic_table_size(), 86);

    // header3: "name3" (5), "value3" (6) -> size 43.
    // Needs to evict h1 (oldest, size 43). New total = 43(h2) + 43(h3) = 86.
    // Table: [h3, h2] (86/100)
    expect_encoded(make_headers({{"name3", "value3"}}), "40056e616d65330676616c756533");
    ASSERT_EQ(encoder.get_current_dynamic_table_size(), 86);

    // Try to index h1 again, it should be a new entry (not found in dynamic table)
    // It will evict h2. Table: [h1_new, h3] (86/100)
    expect_encoded(make_headers({{"name1", "value1"}}), "40056e616d65310676616c756531");
    ASSERT_EQ(encoder.get_current_dynamic_table_size(), 86);
}

TEST_F(HpackEncoderTest, SetOwnMaxDynamicTableSize) {
    // Add some entries
    expect_encoded(make_headers({{"key1", "val1"}}), "40046b6579310476616c31"); // size 32+4+4=40
    expect_encoded(make_headers({{"key2", "val2"}}), "40046b6579320476616c32"); // size 40. Total 80
    ASSERT_EQ(encoder.get_current_dynamic_table_size(), 80);

    bool changed = encoder.set_own_max_dynamic_table_size(50); // Reduce size
    EXPECT_TRUE(changed);
    EXPECT_EQ(encoder.get_own_max_dynamic_table_size(), 50);
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 40); // key1 evicted, key2 remains

    // Try to add key1 again, it will evict key2
    expect_encoded(make_headers({{"key1", "val1"}}), "40046b6579310476616c31");
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 40);
}

// RFC Appendix D examples
TEST_F(HpackEncoderTest, RFCApdxD24_LiteralIncremental) {
    // D.2.4 First Request
    // :method: GET              -> 82
    // :scheme: http             -> 86
    // :path: /                  -> 84
    // :authority: www.example.com -> 41 0f 7777772e6578616d706c652e636f6d (name idx 1, literal value, add to table)
    // (Assuming no huffman for this example as per D.2 "without Huffman")
    std::vector<HttpHeader> headers1 = {
        {":method", "GET"}, {":scheme", "http"}, {":path", "/"}, {":authority", "www.example.com"}
    };
    // Encoder will choose to incrementally index :authority
    expect_encoded(headers1, "828684410f7777772e6578616d706c652e636f6d");
    // Dynamic table: [(:authority, www.example.com)] size 57
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 57);


    // D.2.4 Second Request
    // :method: GET              -> 82
    // :scheme: http             -> 86
    // :path: /                  -> 84
    // :authority: www.example.com -> C1 (idx 62) (from dynamic table)
    // cache-control: no-cache   -> 58 08 6e6f2d6361636865 (name idx 24, literal value "no-cache", add to table)
    std::vector<HttpHeader> headers2 = {
        {":method", "GET"}, {":scheme", "http"}, {":path", "/"},
        {":authority", "www.example.com"}, {"cache-control", "no-cache"}
    };
    expect_encoded(headers2, "828684c158086e6f2d6361636865");
    // Dynamic table: [ (cc,no-cache), (:auth,www) ] sizes 53 + 57 = 110
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 57 + (32+13+8));


    // D.2.4 Third Request
    // :method: GET                     -> 82
    // :scheme: https                   -> 87
    // :path: /index.html               -> 85
    // :authority: www.example.com      -> C1 (idx 62)
    // custom-key: custom-value         -> 40 0a 637573746f6d2d6b6579 0c 637573746f6d2d76616c7565 (new name, add to table)
    std::vector<HttpHeader> headers3 = {
        {":method", "GET"}, {":scheme", "https"}, {":path", "/index.html"},
        {":authority", "www.example.com"}, {"custom-key", "custom-value"}
    };
    expect_encoded(headers3, "828785c1400a637573746f6d2d6b65790c637573746f6d2d76616c7565");
    // Dynamic table: [ (ck,cv), (cc,nc), (:auth,www) ] sizes 54 + 53 + 57 = 164
    EXPECT_EQ(encoder.get_current_dynamic_table_size(), 57 + (32+13+8) + (32+10+12));
}

// Add more tests, especially for Huffman decision making and edge cases for table sizes.

// This main is needed if we build this test file as a standalone executable.
// If part of a larger test suite with one main (e.g. in test_main.cpp), this can be removed.
// int main(int argc, char **argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     return RUN_ALL_TESTS();
// }
