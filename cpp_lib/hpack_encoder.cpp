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
    // This might influence encoding choices, e.g., whether to use "With Incremental Indexing"
    // if the entry might not fit in the peer's table. For now, not strictly enforced here,
    // as the primary constraint is our *own* table size for adding entries.
    // However, the encoder MUST NOT cause the dynamic table capacity to exceed this value.
    // This means if peer_max_dynamic_table_size_ is smaller than our intended additions,
    // we should use non-indexing or never-indexed literals.
    // The current logic in `encode` primarily considers `own_max_dynamic_table_size_` for additions.
    // A stricter implementation would also check against `peer_max_dynamic_table_size_`
    // before choosing "Literal With Incremental Indexing".
    // For now, this mainly informs the context, but `encode` logic needs to be aware.
}

bool HpackEncoder::set_own_max_dynamic_table_size(uint32_t max_size) {
    // This is called when we intend to change our dynamic table size,
    // typically by sending a SETTINGS_HEADER_TABLE_SIZE.
    // The actual change to `own_max_dynamic_table_size_` and eviction
    // should happen *after* the peer acknowledges this size via its own
    // SETTINGS frame (or if we send a Dynamic Table Size Update instruction in HPACK).
    // For now, this method will set the intended size and evict immediately.
    // A more compliant model would be:
    // 1. Application calls this to set *intended* max size.
    // 2. Connection sends SETTINGS frame with this new size.
    // 3. Encoder *could* preemptively evict entries to meet this new *intended* size,
    //    OR it waits for peer's ACK.
    // 4. RFC 7541 Section 4.2: "This mechanism can be used to completely clear
    //    the dynamic table by setting a maximum size of 0, which can be
    //    followed by a maximum size update to a larger value."
    //
    // This method will also determine if a Dynamic Table Size Update *instruction*
    // needs to be encoded in the next HPACK block.
    // For now, simpler: update own_max_dynamic_table_size_ and evict.
    // The return value indicates if a SETTINGS frame should reflect this change.

    bool size_changed_for_settings = (own_max_dynamic_table_size_ != max_size);

    own_max_dynamic_table_size_ = max_size;
    evict_from_dynamic_table(0); // Evict until current_size <= new max_size

    // The need to send an HPACK dynamic table size update instruction (001xxxxx)
    // is if `max_size` is less than `own_max_dynamic_table_size_acknowledged_by_peer_`.
    // If we increase the size, we just start using it up to the new max, and peer learns via SETTINGS.
    // If we decrease it below what peer thinks we have, we MUST signal this via HPACK instruction.
    // This logic is complex and usually part of `encode()` beginning.
    // For now, `set_own_max_dynamic_table_size` directly applies the size.
    // The `own_max_dynamic_table_size_acknowledged_by_peer_` would be updated when we receive SETTINGS ACK for our size.

    return size_changed_for_settings; // Indicates if SETTINGS frame needs to be sent by connection layer.
}

uint32_t HpackEncoder::get_current_dynamic_table_size() const {
    return current_dynamic_table_size_;
}


void HpackEncoder::add_to_dynamic_table(const HttpHeader& header) {
    size_t entry_size = header.name.length() + header.value.length() + 32;

    if (entry_size > own_max_dynamic_table_size_) {
        // Entry is too large to ever fit, clear the table
        dynamic_table_.clear();
        current_dynamic_table_size_ = 0;
        return;
    }

    evict_from_dynamic_table(static_cast<uint32_t>(entry_size));

    dynamic_table_.push_front({header.name, header.value, entry_size});
    current_dynamic_table_size_ += static_cast<uint32_t>(entry_size);
}

void HpackEncoder::evict_from_dynamic_table(uint32_t required_space) {
    while (current_dynamic_table_size_ + required_space > own_max_dynamic_table_size_ && !dynamic_table_.empty()) {
        current_dynamic_table_size_ -= static_cast<uint32_t>(dynamic_table_.back().size);
        dynamic_table_.pop_back();
    }
}

std::pair<int, bool> HpackEncoder::find_in_static_table(const HttpHeader& header) {
    // Returns <absolute_index, value_matches>
    // Absolute index means 1-61 for static, 62+ for dynamic.
    // If name not found, index is 0.
    // If name found but value not, index is for the first name match, value_matches is false.

    // Check static table first
    auto [static_idx, static_value_match] = Hpack::find_in_static_table(header.name, header.value);
    if (static_idx != 0) { // Name found in static table
        return {static_idx, static_value_match};
    }

    // Check dynamic table
    // Dynamic table indices are 1-based internally to the dynamic table.
    // Convert to absolute index by adding static table size.
    for (size_t i = 0; i < dynamic_table_.size(); ++i) {
        if (dynamic_table_[i].name == header.name) {
            if (dynamic_table_[i].value == header.value) {
                return {static_cast<int>(Hpack::STATIC_TABLE.size() + i + 1), true}; // Value also matches
            }
            // Name matches, value doesn't. Return this as first name match from dynamic table.
            // (HPACK prefers smallest index for name match if value differs)
            // If we already found a name match in static table (static_idx != 0 but !static_value_match),
            // that one would take precedence. But current logic returns early if static_idx !=0.
            // So, if we are here, name was not in static table.
            return {static_cast<int>(Hpack::STATIC_TABLE.size() + i + 1), false};
        }
    }
    return {0, false}; // Not found in either table
}


// Huffman related methods are in hpack_huffman.cpp / .h
// bool HpackEncoder::should_use_huffman(const std::string& str, const std::vector<std::byte>& huffman_encoded_str) {
//     if (huffman_encoded_str.empty() && !str.empty()) return false;
//     return huffman_encoded_str.size() < str.length();
// }


} // namespace http2
