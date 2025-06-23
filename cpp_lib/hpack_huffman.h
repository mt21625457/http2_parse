#pragma once

#include <vector>
#include <string>
#include <span> // C++20
#include <optional>

namespace http2 {
namespace Hpack {

// Represents an error during Huffman encoding/decoding
enum class HuffmanError {
    OK,
    INVALID_INPUT,      // e.g., character not in Huffman table for encoding
    INVALID_PADDING,    // EOS symbol used incorrectly or non-zero padding bits
    INCOMPLETE_CODE,    // Not enough bits to form a complete Huffman code
    BUFFER_TOO_SMALL,   // Output buffer too small for decoded string
};

// Encodes a string using the HPACK Huffman code.
// RFC 7541, Section 5.2 and Appendix B.
// Returns a pair: encoded data and HuffmanError.
// In C++23, std::expected<std::vector<std::byte>, HuffmanError> would be better.
std::pair<std::vector<std::byte>, HuffmanError> huffman_encode(const std::string& input);

// Decodes a string using the HPACK Huffman code.
// RFC 7541, Section 5.2 and Appendix B.
// Returns a pair: decoded string and HuffmanError.
// In C++23, std::expected<std::string, HuffmanError> would be better.
// The 'max_output_length' is a safeguard against decompression bombs.
std::pair<std::string, HuffmanError> huffman_decode(std::span<const std::byte> input, size_t max_output_length = 16384 * 4); // Default max e.g. 4x typical max header list size


// Helper function to get the length of the Huffman encoded version of a string.
// Useful for deciding whether to use Huffman encoding or literal representation.
size_t get_huffman_encoded_length(const std::string& input);

} // namespace Hpack
} // namespace http2
