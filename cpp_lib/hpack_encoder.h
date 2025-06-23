#pragma once

#include "http2_types.h"
#include <vector>
#include <string>
#include <deque>
#include <map> // For reverse lookup in dynamic table, if needed for optimization

namespace http2 {

// HPACK Encoding Error (less common than decoding errors, mostly about table state)
enum class HpackEncodingError {
    OK,
    DYNAMIC_TABLE_UPDATE_FAILED, // If max size change causes issues
    STRING_ENCODING_FAILED,      // Should not happen with valid UTF-8/ASCII
    INTEGER_ENCODING_FAILED,     // Should not happen with valid integers
};

class HpackEncoder {
public:
    // Default size for the dynamic table (SETTINGS_HEADER_TABLE_SIZE)
    // This is the size *we* use for our dynamic table when encoding.
    // The peer will tell us *their* max dynamic table size via SETTINGS.
    static constexpr uint32_t DEFAULT_DYNAMIC_TABLE_SIZE = 4096;

    HpackEncoder(uint32_t max_dynamic_table_size = DEFAULT_DYNAMIC_TABLE_SIZE);

    // Encodes a list of headers into a byte vector.
    // Returns a pair: a vector of bytes and an HpackEncodingError.
    // In C++23, this could return std::expected<std::vector<std::byte>, HpackEncodingError>.
    std::pair<std::vector<std::byte>, HpackEncodingError> encode(const std::vector<HttpHeader>& headers);

    // Updates the maximum size of the dynamic table that the peer supports.
    // This is received via SETTINGS_HEADER_TABLE_SIZE from the peer.
    // This affects how we might choose to represent headers (e.g., if we can add to dynamic table).
    void set_peer_max_dynamic_table_size(uint32_t max_size);

    // Updates the maximum size of our *own* dynamic table.
    // This is what we would advertise in a SETTINGS frame (SETTINGS_HEADER_TABLE_SIZE).
    // Returns true if a dynamic table size update should be signaled to the peer.
    bool set_own_max_dynamic_table_size(uint32_t max_size);
    uint32_t get_current_dynamic_table_size() const;
    uint32_t get_own_max_dynamic_table_size() const { return own_max_dynamic_table_size_; }


private:
    struct DynamicTableEntry {
        std::string name;
        std::string value;
        size_t size; // Size as defined by HPACK (name_len + value_len + 32)

        DynamicTableEntry(std::string n, std::string v) :
            name(std::move(n)), value(std::move(v)) {
            size = name.length() + value.length() + 32;
        }
    };

    // Our dynamic table used for encoding
    std::deque<DynamicTableEntry> dynamic_table_;
    uint32_t current_dynamic_table_size_ = 0;
    uint32_t own_max_dynamic_table_size_; // Max size we allow for our table
    uint32_t own_max_dynamic_table_size_acknowledged_by_peer_ = DEFAULT_DYNAMIC_TABLE_SIZE; // The size we last signaled and peer acked (or initial)


    // The maximum dynamic table size supported by the peer.
    // This is received via SETTINGS_HEADER_TABLE_SIZE from the peer.
    uint32_t peer_max_dynamic_table_size_;


    // Static Table (RFC 7541 - Appendix A) - used for finding matches
    // static const std::vector<HttpHeader> STATIC_TABLE; // Already in hpack_static_table.h

    // Helper methods for encoding integers and strings
    void encode_integer(std::vector<std::byte>& buffer, uint8_t prefix_mask, uint8_t prefix_bits, uint64_t value);
    void encode_string(std::vector<std::byte>& buffer, const std::string& str, bool try_huffman);


    // Dynamic table management
    void add_to_dynamic_table(const HttpHeader& header);
    void evict_from_dynamic_table(uint32_t required_space);

    // Search for matches in static and dynamic tables
    // Returns <index (0 if no name match), value_matches (bool)>
    // Index is absolute (1 to 61 for static, 62+ for dynamic)
    std::pair<int, bool> find_header_in_tables(const HttpHeader& header);

    // --- Placeholder for Huffman Encoding logic ---
    // std::vector<std::byte> huffman_encode(const std::string& str); // In hpack_huffman.h
    // bool should_use_huffman(const std::string& str, const std::vector<std::byte>& huffman_encoded_str); // In hpack_huffman.h
};

} // namespace http2
