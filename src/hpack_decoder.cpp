#include "hpack_decoder.h"
#include "hpack_static_table.h"
#include "hpack_huffman.h"
#include <algorithm> // for std::reverse, std::min
#include <iterator> // for std::back_inserter
#include <iostream> // For temporary debugging

// Helper to read a big-endian integer from a span of bytes
template <typename T>
T read_big_endian(std::span<const std::byte>& data) {
    T value = 0;
    size_t bytes_to_read = std::min(data.size(), sizeof(T));
    if (bytes_to_read < sizeof(T)) { // Not enough data
        // This condition should be handled by caller, or throw error
        data = data.subspan(bytes_to_read); // Consume what's there
        return static_cast<T>(-1); // Indicate error
    }
    for (size_t i = 0; i < sizeof(T); ++i) {
        value = (value << 8) | static_cast<T>(data[i]);
    }
    data = data.subspan(sizeof(T));
    return value;
}


namespace http2 {

// --- HpackDecoder Implementation ---

HpackDecoder::HpackDecoder(uint32_t max_dynamic_table_size)
    : max_dynamic_table_size_(max_dynamic_table_size) {
    // Static table is globally defined in hpack_static_table.cpp
}

std::pair<std::vector<HttpHeader>, HpackError> HpackDecoder::decode(std::span<const std::byte> data) {
    std::vector<HttpHeader> headers;
    HpackError error_status = HpackError::OK;

    while (!data.empty()) {
        uint8_t first_byte = static_cast<uint8_t>(data[0]);

        if ((first_byte & 0b10000000) == 0b10000000) { // Indexed Header Field: 1xxxxxxx
            auto [index, err] = decode_integer(data, 7); // 7-bit prefix
            if (err != HpackError::OK) { error_status = err; break; }

            if (index == 0) { // Index 0 is not allowed
                error_status = HpackError::INDEX_OUT_OF_BOUNDS; break;
            }
            auto header_opt = get_header_from_tables(index);
            if (!header_opt) {
                error_status = HpackError::INDEX_OUT_OF_BOUNDS; break;
            }
            headers.push_back(header_opt.value());

        } else if ((first_byte & 0b11000000) == 0b01000000) { // Literal Header Field with Incremental Indexing: 01xxxxxx
            uint8_t prefix_bits = 6;
            auto [index, err_idx] = decode_integer(data, prefix_bits); // Consumes first byte with prefix
             if (err_idx != HpackError::OK) { error_status = err_idx; break; }

            HttpHeader header;
            if (index == 0) { // Literal name, literal value
                auto [name_str, err_name] = decode_string(data);
                if (err_name != HpackError::OK) { error_status = err_name; break; }
                header.name = std::move(name_str);
            } else {
                auto indexed_header_opt = get_header_from_tables(index);
                if (!indexed_header_opt) { error_status = HpackError::INDEX_OUT_OF_BOUNDS; break; }
                header.name = indexed_header_opt.value().name;
            }

            auto [value_str, err_val] = decode_string(data);
            if (err_val != HpackError::OK) { error_status = err_val; break; }
            header.value = std::move(value_str);

            headers.push_back(header);
            add_to_dynamic_table(header);

        } else if ((first_byte & 0b11110000) == 0b00000000) { // Literal Header Field without Indexing: 0000xxxx
            uint8_t prefix_bits = 4;
            auto [index, err_idx] = decode_integer(data, prefix_bits);
            if (err_idx != HpackError::OK) { error_status = err_idx; break; }

            HttpHeader header;
            if (index == 0) {
                auto [name_str, err_name] = decode_string(data);
                if (err_name != HpackError::OK) { error_status = err_name; break; }
                header.name = std::move(name_str);
            } else {
                auto indexed_header_opt = get_header_from_tables(index);
                if (!indexed_header_opt) { error_status = HpackError::INDEX_OUT_OF_BOUNDS; break; }
                header.name = indexed_header_opt.value().name;
            }

            auto [value_str, err_val] = decode_string(data);
            if (err_val != HpackError::OK) { error_status = err_val; break; }
            header.value = std::move(value_str);
            header.sensitive = false; // By default for this type
            headers.push_back(header);

        } else if ((first_byte & 0b11110000) == 0b00010000) { // Literal Header Field Never Indexed: 0001xxxx
            uint8_t prefix_bits = 4;
            auto [index, err_idx] = decode_integer(data, prefix_bits);
             if (err_idx != HpackError::OK) { error_status = err_idx; break; }

            HttpHeader header;
            if (index == 0) {
                auto [name_str, err_name] = decode_string(data);
                if (err_name != HpackError::OK) { error_status = err_name; break; }
                header.name = std::move(name_str);
            } else {
                auto indexed_header_opt = get_header_from_tables(index);
                if (!indexed_header_opt) { error_status = HpackError::INDEX_OUT_OF_BOUNDS; break; }
                header.name = indexed_header_opt.value().name;
            }

            auto [value_str, err_val] = decode_string(data);
            if (err_val != HpackError::OK) { error_status = err_val; break; }
            header.value = std::move(value_str);
            header.sensitive = true;
            headers.push_back(header);

        } else if ((first_byte & 0b11100000) == 0b00100000) { // Dynamic Table Size Update: 001xxxxx
            auto [size, err] = decode_integer(data, 5);
            if (err != HpackError::OK) { error_status = err; break; }

            // RFC 7541 Section 6.3: "A dynamic table size update MUST occur at the beginning
            // of a header block. It is an error if this is not the case."
            // This check should ideally be done by the caller or Http2Parser.
            // For now, we process it. If headers are already decoded, it's a protocol violation.
            if (!headers.empty()) {
                 error_status = HpackError::COMPRESSION_ERROR; // Or a more specific error
                 break;
            }

            if (size > HpackDecoder::DEFAULT_DYNAMIC_TABLE_SIZE) { // Using own default as a sanity check, though spec says max_dynamic_table_size_
                 // The spec says an encoder MUST NOT cause a dynamic table capacity to exceed this.
                 // If it does, "the decoder MUST treat this as a compression error."
                 error_status = HpackError::COMPRESSION_ERROR;
                 break;
            }
            set_max_dynamic_table_size(static_cast<uint32_t>(size));

        } else {
            // Should not happen if HPACK stream is valid
            error_status = HpackError::COMPRESSION_ERROR;
            break;
        }
        if (error_status != HpackError::OK) break;
    }

    return {headers, error_status};
}

void HpackDecoder::set_max_dynamic_table_size(uint32_t max_size) {
    max_dynamic_table_size_ = max_size;
    // Evict entries if current size exceeds new max size
    evict_from_dynamic_table(0); // Evict until current_size <= max_size
}

uint32_t HpackDecoder::get_current_dynamic_table_size() const {
    return current_dynamic_table_size_;
}
uint32_t HpackDecoder::get_max_dynamic_table_size() const {
    return max_dynamic_table_size_;
}


// Helper to decode an integer (RFC 7541, Section 5.1)
std::pair<uint64_t, HpackError> HpackDecoder::decode_integer(std::span<const std::byte>& data, uint8_t prefix_bits) {
    if (data.empty()) {
        return {0, HpackError::BUFFER_TOO_SMALL};
    }

    uint8_t mask = (1 << prefix_bits) - 1;
    uint64_t value = static_cast<uint64_t>(data[0]) & mask;
    data = data.subspan(1);

    if (value < mask) { // Value fits in the prefix
        return {value, HpackError::OK};
    }

    // Value is >= mask, multi-byte encoding
    uint64_t m = 0;
    uint8_t byte_val;
    do {
        if (data.empty()) {
            return {0, HpackError::BUFFER_TOO_SMALL}; // Need more bytes
        }
        byte_val = static_cast<uint8_t>(data[0]);
        data = data.subspan(1);

        if ((m / 128) > (UINT64_MAX / 128) ) { // Check for overflow before multiplication
             // Avoids (value + (byte_val & 127) * pow(2, m)) overflowing
             // This check is a bit tricky. Simpler: if m > 63 (or some threshold), error.
             // Max integer is 2^N - 1. If N is 7 (prefix_bits), max is 127.
             // If m gets too large, 2^m will overflow.
             // An integer is represented using I bits. If N < 8, I = N.
             // Otherwise, I = N + K*7 where K is number of octets.
             // If m exceeds 9*7 = 63 (for a 64-bit uint), it's an overflow.
            if (m >= 63 && (byte_val & 127) > 0) { // Heuristic, not perfect
                 return {0, HpackError::INTEGER_OVERFLOW};
            }
        }

        value += (static_cast<uint64_t>(byte_val & 127)) << m;
        m += 7;

        if (m > 70) { // Defensive check against extreme cases / malformed input
            return {0, HpackError::INTEGER_OVERFLOW};
        }

    } while ((byte_val & 128) == 128);

    return {value, HpackError::OK};
}

// Helper to decode a string (RFC 7541, Section 5.2)
std::pair<std::string, HpackError> HpackDecoder::decode_string(std::span<const std::byte>& data) {
    if (data.empty()) {
        return {"", HpackError::BUFFER_TOO_SMALL};
    }

    bool huffman_encoded = (static_cast<uint8_t>(data[0]) & 0b10000000) != 0;
    auto [length, err_len] = decode_integer(data, 7); // 7-bit prefix for length
    if (err_len != HpackError::OK) {
        return {"", err_len};
    }

    if (length > data.size()) {
        return {"", HpackError::BUFFER_TOO_SMALL}; // Not enough data for the string
    }

    std::string str_value;
    std::span<const std::byte> string_data = data.first(static_cast<size_t>(length));
    data = data.subspan(static_cast<size_t>(length));

    if (huffman_encoded) {
        auto [decoded_str, huff_err] = Hpack::huffman_decode(string_data);
        if (huff_err != Hpack::HuffmanError::OK) {
            // Map HuffmanError to HpackError
            return {"", HpackError::INVALID_HUFFMAN_CODE}; // Or COMPRESSION_ERROR
        }
        str_value = std::move(decoded_str);
    } else {
        str_value.reserve(static_cast<size_t>(length));
        for (std::byte b : string_data) {
            str_value += static_cast<char>(b);
        }
    }
    return {str_value, HpackError::OK};
}


void HpackDecoder::add_to_dynamic_table(HttpHeader header) {
    size_t entry_size = header.name.length() + header.value.length() + 32;

    if (entry_size > max_dynamic_table_size_) {
        // Entry is larger than the entire table capacity, so just clear the table.
        dynamic_table_.clear();
        current_dynamic_table_size_ = 0;
        return;
    }

    evict_from_dynamic_table(static_cast<uint32_t>(entry_size));

    dynamic_table_.push_front(DynamicTableEntry(std::move(header.name), std::move(header.value)));
    current_dynamic_table_size_ += static_cast<uint32_t>(entry_size);
}

void HpackDecoder::evict_from_dynamic_table(uint32_t required_space) {
    while (current_dynamic_table_size_ + required_space > max_dynamic_table_size_ && !dynamic_table_.empty()) {
        current_dynamic_table_size_ -= static_cast<uint32_t>(dynamic_table_.back().size);
        dynamic_table_.pop_back();
    }
}

std::optional<HttpHeader> HpackDecoder::get_header_from_tables(uint64_t index) {
    if (index == 0) return std::nullopt; // Index 0 is invalid

    // Check static table first (indices 1 to STATIC_TABLE.size())
    if (index <= Hpack::STATIC_TABLE.size()) {
        return Hpack::get_static_header(index);
    }

    // Adjust index for dynamic table
    uint64_t dynamic_index = index - Hpack::STATIC_TABLE.size();
    if (dynamic_index > dynamic_table_.size()) {
        return std::nullopt; // Index out of bounds for dynamic table
    }

    // Dynamic table indices are 1-based from the most recent entry
    return {{dynamic_table_[dynamic_index - 1].name, dynamic_table_[dynamic_index - 1].value}};
}


} // namespace http2
