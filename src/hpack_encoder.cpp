#include "hpack_encoder.h"
#include "hpack_static_table.h"
#include "hpack_huffman.h"
#include <algorithm> // For std::find_if, std::min

namespace http2 {

// --- HpackEncoder Implementation ---

HpackEncoder::HpackEncoder(uint32_t max_dynamic_table_size)
    : own_max_dynamic_table_size_(max_dynamic_table_size),
      peer_max_dynamic_table_size_(DEFAULT_DYNAMIC_TABLE_SIZE) { // Peer's default, updated by SETTINGS
}

// Helper to encode an integer (RFC 7541, Section 5.1)
void HpackEncoder::encode_integer(std::vector<std::byte>& buffer, uint8_t prefix_mask, uint8_t prefix_bits, uint64_t value) {
    uint8_t initial_byte_prefix = prefix_mask; // The bits of the prefix itself (e.g., 1xxxxxxx -> 0x80)
    uint8_t N = (1 << prefix_bits) - 1; // Max value that fits in prefix_bits (e.g., 0x7F for 7 bits)

    if (value < N) {
        buffer.push_back(static_cast<std::byte>(initial_byte_prefix | static_cast<uint8_t>(value)));
    } else {
        buffer.push_back(static_cast<std::byte>(initial_byte_prefix | N));
        value -= N;
        while (value >= 128) {
            buffer.push_back(static_cast<std::byte>(0x80 | (value % 128)));
            value /= 128;
        }
        buffer.push_back(static_cast<std::byte>(value));
    }
}

// Helper to encode a string (RFC 7541, Section 5.2)
void HpackEncoder::encode_string(std::vector<std::byte>& buffer, const std::string& str, bool try_huffman) {
    std::vector<std::byte> huffman_encoded_bytes;
    bool use_huffman_actual = false;

    if (try_huffman) {
        auto [encoded_res, huff_err] = Hpack::huffman_encode(str);
        if (huff_err == Hpack::HuffmanError::OK) {
            // Simple heuristic: use Huffman if it's shorter.
            // More complex heuristics could be used (e.g. considering CPU cost).
            if (encoded_res.size() < str.length()) {
                huffman_encoded_bytes = std::move(encoded_res);
                use_huffman_actual = true;
            }
        }
    }

    uint8_t prefix = use_huffman_actual ? 0x80 : 0x00; // H bit (1 for Huffman, 0 for literal)
    size_t length = use_huffman_actual ? huffman_encoded_bytes.size() : str.length();

    encode_integer(buffer, prefix, 7, length); // 7-bit prefix for length

    if (use_huffman_actual) {
        buffer.insert(buffer.end(), huffman_encoded_bytes.begin(), huffman_encoded_bytes.end());
    } else {
        for (char c : str) {
            buffer.push_back(static_cast<std::byte>(c));
        }
    }
}


std::pair<std::vector<std::byte>, HpackEncodingError> HpackEncoder::encode(const std::vector<HttpHeader>& headers) {
    std::vector<std::byte> output_buffer;

    for (const auto& header : headers) {
        // Strategy:
        // 1. Try full match (name and value) in static table.
        // 2. Try full match in dynamic table.
        // 3. Try name match in static table.
        // 4. Try name match in dynamic table.
        // 5. If sensitive, use Literal Never Indexed.
        // 6. Else, use Literal With Incremental Indexing (if it fits in dynamic table).
        // 7. Else, use Literal Without Indexing.

    // Step 1 & 2: Check for exact match (name and value) in static or dynamic table.
    // Static table indices are 1 to STATIC_TABLE.size().
    // Dynamic table indices are STATIC_TABLE.size() + 1 to STATIC_TABLE.size() + dynamic_table_.size().
    auto [static_idx, static_value_match] = Hpack::find_in_static_table(header);
    if (static_idx != 0 && static_value_match) {
        encode_integer(output_buffer, 0x80, 7, static_idx); // Indexed Header Field: 1xxxxxxx
            continue;
        }

    auto [dyn_idx, dyn_value_match] = find_in_dynamic_table(header); // dyn_idx is 1-based for dynamic part
    if (dyn_idx != 0 && dyn_value_match) {
        encode_integer(output_buffer, 0x80, 7, Hpack::STATIC_TABLE.size() + dyn_idx);
            continue;
        }

        // Determine indexing strategy
    size_t entry_size = header.name.length() + header.value.length() + 32;
    bool can_be_added_to_dynamic_table = entry_size <= own_max_dynamic_table_size_;

    // Step 5: If sensitive, use Literal Never Indexed.
    if (header.sensitive) {
        // Try to find name in static or dynamic table for indexed name part
        int name_table_idx = 0;
        if (static_idx != 0) { // Name matched in static table (value didn't or was empty)
            name_table_idx = static_idx;
        } else if (dyn_idx != 0) { // Name matched in dynamic table
            name_table_idx = static_cast<int>(Hpack::STATIC_TABLE.size()) + dyn_idx;
            }

        encode_integer(output_buffer, 0x10, 4, name_table_idx); // Literal Never Indexed: 0001xxxx
        if (name_table_idx == 0) { // Name is also literal
            encode_string(output_buffer, header.name, true); // try_huffman = true
            }
        encode_string(output_buffer, header.value, true);
    }
    // Step 6: Else, if it can be added, use Literal With Incremental Indexing.
    else if (can_be_added_to_dynamic_table) {
        int name_table_idx = 0;
        if (static_idx != 0) {
            name_table_idx = static_idx;
        } else if (dyn_idx != 0) {
            name_table_idx = static_cast<int>(Hpack::STATIC_TABLE.size()) + dyn_idx;
            }

        encode_integer(output_buffer, 0x40, 6, name_table_idx); // Literal With Incremental Indexing: 01xxxxxx
        if (name_table_idx == 0) {
                encode_string(output_buffer, header.name, true);
            }
            encode_string(output_buffer, header.value, true);
            add_to_dynamic_table(header); // Add to our dynamic table
    }
    // Step 7: Else (cannot be added or not sensitive and not chosen for indexing), use Literal Without Indexing.
    else {
        int name_table_idx = 0;
        if (static_idx != 0) {
            name_table_idx = static_idx;
        } else if (dyn_idx != 0) {
            name_table_idx = static_cast<int>(Hpack::STATIC_TABLE.size()) + dyn_idx;
            }

        encode_integer(output_buffer, 0x00, 4, name_table_idx); // Literal Without Indexing: 0000xxxx
        if (name_table_idx == 0) {
                encode_string(output_buffer, header.name, true);
            }
            encode_string(output_buffer, header.value, true);
        }
    }
    return {output_buffer, HpackEncodingError::OK};
}


void HpackEncoder::set_peer_max_dynamic_table_size(uint32_t max_size) {
    peer_max_dynamic_table_size_ = max_size;
}

bool HpackEncoder::set_own_max_dynamic_table_size(uint32_t max_size) {
    bool size_changed_for_settings = (own_max_dynamic_table_size_ != max_size);

    own_max_dynamic_table_size_ = max_size;
    evict_from_dynamic_table(0); // Evict until current_size <= new max_size

    return size_changed_for_settings;
}

uint32_t HpackEncoder::get_current_dynamic_table_size() const {
    return current_dynamic_table_size_;
}


void HpackEncoder::add_to_dynamic_table(const HttpHeader& header) {
    size_t entry_size = header.name.length() + header.value.length() + 32;

    if (entry_size > own_max_dynamic_table_size_) {
        dynamic_table_.clear();
        current_dynamic_table_size_ = 0;
        return;
    }

    evict_from_dynamic_table(static_cast<uint32_t>(entry_size));

    dynamic_table_.push_front(DynamicTableEntry(header.name, header.value));
    current_dynamic_table_size_ += static_cast<uint32_t>(dynamic_table_.front().size);
}

void HpackEncoder::evict_from_dynamic_table(uint32_t required_space) {
    while (current_dynamic_table_size_ + required_space > own_max_dynamic_table_size_ && !dynamic_table_.empty()) {
        current_dynamic_table_size_ -= static_cast<uint32_t>(dynamic_table_.back().size);
        dynamic_table_.pop_back();
    }
}

std::pair<int, bool> HpackEncoder::find_in_dynamic_table(const HttpHeader& header) {
    for (size_t i = 0; i < dynamic_table_.size(); ++i) {
        if (dynamic_table_[i].name == header.name) {
            bool value_matches = (dynamic_table_[i].value == header.value);
            return {static_cast<int>(i + 1), value_matches};
        }
    }
    return {0, false}; // No match found
}

} // namespace http2
