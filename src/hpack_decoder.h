#pragma once

#include "http2_types.h"
#include <vector>
#include <string>
#include <deque>
#include <map> // For dynamic table, though a vector might be more performant with custom indexing

namespace http2 {

// HPACK Error Codes (can be specific or map to general HTTP/2 errors)
enum class HpackError {
    OK,
    COMPRESSION_ERROR,      // General compression error
    INDEX_OUT_OF_BOUNDS,    // Index refers to an entry not in the table
    INVALID_HUFFMAN_CODE,   // Error in Huffman decoding
    INTEGER_OVERFLOW,       // Integer representation overflow
    INVALID_STRING_LENGTH,  // String length mismatch or invalid
    BUFFER_TOO_SMALL,       // Input buffer too small for expected data
};


class HpackDecoder {
public:
    // RFC 7541 - Section 2.3.2. Maximum Header Table Size
    // Default size for the dynamic table (SETTINGS_HEADER_TABLE_SIZE)
    static constexpr uint32_t DEFAULT_DYNAMIC_TABLE_SIZE = 4096;

    HpackDecoder(uint32_t max_dynamic_table_size = DEFAULT_DYNAMIC_TABLE_SIZE);

    // Decodes a header block fragment.
    // Takes a span of bytes representing the header block.
    // Returns a pair: a vector of decoded headers and an HpackError.
    // In C++23, this could return std::expected<std::vector<HttpHeader>, HpackError>.
    std::pair<std::vector<HttpHeader>, HpackError> decode(std::span<const std::byte> data);

    // Updates the maximum size of the dynamic table.
    // This can be signaled by the peer via SETTINGS_HEADER_TABLE_SIZE.
    void set_max_dynamic_table_size(uint32_t max_size);
    uint32_t get_current_dynamic_table_size() const;
    uint32_t get_max_dynamic_table_size() const;


// private:
    struct DynamicTableEntry {
        std::string name;
        std::string value;
        size_t size; // Size as defined by HPACK (name_len + value_len + 32)

        DynamicTableEntry(std::string n, std::string v) :
            name(std::move(n)), value(std::move(v)) {
            size = name.length() + value.length() + 32;
        }
    };

    // Dynamic Table (RFC 7541 - Section 2.3)
    std::deque<DynamicTableEntry> dynamic_table_;
    uint32_t current_dynamic_table_size_ = 0; // Current sum of entry sizes
    uint32_t max_dynamic_table_size_;         // Max allowed size

    // Static Table (RFC 7541 - Appendix A)
    // This can be a static constexpr array or similar.
    static const std::vector<HttpHeader> STATIC_TABLE;

    // Helper methods for parsing different integer and string representations
    std::pair<uint64_t, HpackError> decode_integer(std::span<const std::byte>& data, uint8_t prefix_bits);
    std::pair<std::string, HpackError> decode_string(std::span<const std::byte>& data);
    std::pair<std::string, HpackError> huffman_decode(std::span<const std::byte> data);

    // Dynamic table management
    void add_to_dynamic_table(HttpHeader header);
    void evict_from_dynamic_table(uint32_t required_space);
    std::optional<HttpHeader> get_header_from_tables(uint64_t index);


    // --- Placeholder for Huffman Tree/Table ---
    // This would be a more complex structure for efficient Huffman decoding.
    // For now, we'll assume a helper function `huffman_decode` exists.
    // In a real implementation, this would involve precomputed Huffman codes.
    struct HuffmanNode { /* ... */ };
    // static const HuffmanNode* HUFFMAN_ROOT;
    // static const std::map<std::vector<bool>, char> REVERSE_HUFFMAN_TABLE; // Or similar structure
};

} // namespace http2
