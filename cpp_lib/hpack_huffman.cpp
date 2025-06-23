#include "hpack_huffman.h"
#include <map>
#include <vector>
#include <algorithm> // For std::fill

// This is a large file. The actual Huffman codes and tree structure
// are taken directly from RFC 7541, Appendix B.

namespace http2 {
namespace Hpack {

// Huffman Codes (RFC 7541, Appendix B)
// Pair: <code, number_of_bits>
static const std::map<uint8_t, std::pair<uint32_t, uint8_t>> HUFFMAN_CODES = {
    {0, {0x1ff8, 13}}, {1, {0x7fffd8, 23}}, {2, {0xfffffe2, 28}}, {3, {0xfffffe3, 28}},
    {4, {0xfffffe4, 28}}, {5, {0xfffffe5, 28}}, {6, {0xfffffe6, 28}}, {7, {0xfffffe7, 28}},
    {8, {0xfffffe8, 28}}, {9, {0xffffea, 24}}, {10, {0x3ffffffc, 30}}, {11, {0xfffffe9, 28}},
    {12, {0xfffffea, 28}}, {13, {0x3ffffffd, 30}}, {14, {0xfffffeb, 28}}, {15, {0xfffffec, 28}},
    {16, {0xfffffed, 28}}, {17, {0xfffffee, 28}}, {18, {0xfffffef, 28}}, {19, {0xffffff0, 28}},
    {20, {0xffffff1, 28}}, {21, {0xffffff2, 28}}, {22, {0x3ffffffe, 30}}, {23, {0xffffff3, 28}},
    {24, {0xffffff4, 28}}, {25, {0xffffff5, 28}}, {26, {0xffffff6, 28}}, {27, {0xffffff7, 28}},
    {28, {0xffffff8, 28}}, {29, {0xffffff9, 28}}, {30, {0xffffffa, 28}}, {31, {0xffffffb, 28}},
    {32, {0x14, 6}}, {33, {0x3f8, 10}}, {34, {0x3f9, 10}}, {35, {0x3fa, 10}},
    {36, {0x3fb, 10}}, {37, {0x3fc, 10}}, {38, {0x7fa, 11}}, {39, {0x3fd, 10}},
    {40, {0x3fe, 10}}, {41, {0x3ff, 10}}, {42, {0x7fb, 11}}, {43, {0x7fc, 11}},
    {44, {0x7fd, 11}}, {45, {0x7fe, 11}}, {46, {0x7ff0, 15}}, {47, {0x7ff1, 15}},
    {48, {0x0, 5}}, {49, {0x1, 5}}, {50, {0x2, 5}}, {51, {0x15, 6}},
    {52, {0x16, 6}}, {53, {0x17, 6}}, {54, {0x18, 6}}, {55, {0x19, 6}},
    {56, {0x1a, 6}}, {57, {0x1b, 6}}, {58, {0x1c, 6}}, {59, {0x1d, 6}},
    {60, {0x1e, 6}}, {61, {0x1f, 6}}, {62, {0x5c, 7}}, {63, {0x5d, 7}},
    {64, {0x5e, 7}}, {65, {0x5f, 7}}, {66, {0x60, 7}}, {67, {0x61, 7}},
    {68, {0x62, 7}}, {69, {0x63, 7}}, {70, {0x64, 7}}, {71, {0x65, 7}},
    {72, {0x66, 7}}, {73, {0x67, 7}}, {74, {0x68, 7}}, {75, {0x69, 7}},
    {76, {0x6a, 7}}, {77, {0x6b, 7}}, {78, {0x6c, 7}}, {79, {0x6d, 7}},
    {80, {0x6e, 7}}, {81, {0x6f, 7}}, {82, {0x70, 7}}, {83, {0x71, 7}},
    {84, {0x72, 7}}, {85, {0x7f1, 11}}, {86, {0x7f2, 11}}, {87, {0x7f3, 11}},
    {88, {0x7f4, 11}}, {89, {0x7f5, 11}}, {90, {0x7f6, 11}}, {91, {0x7f7, 11}},
    {92, {0x7f8, 11}}, {93, {0x7f9, 11}}, {94, {0x7fa0, 15}}, {95, {0x7fa1, 15}},
    {96, {0x7fa2, 15}}, {97, {0x3, 5}}, {98, {0x20, 6}}, {99, {0x21, 6}},
    {100, {0x22, 6}}, {101, {0x4, 5}}, {102, {0x23, 6}}, {103, {0x24, 6}},
    {104, {0x25, 6}}, {105, {0x5, 5}}, {106, {0x26, 6}}, {107, {0x27, 6}},
    {108, {0x28, 6}}, {109, {0x29, 6}}, {110, {0x2a, 6}}, {111, {0x2b, 6}},
    {112, {0x2c, 6}}, {113, {0x2d, 6}}, {114, {0x2e, 6}}, {115, {0x6, 5}},
    {116, {0x7, 5}}, {117, {0x2f, 6}}, {118, {0x73, 7}}, {119, {0x74, 7}},
    {120, {0x30, 6}}, {121, {0x31, 6}}, {122, {0x32, 6}}, {123, {0x7fa3, 15}},
    {124, {0x7fa4, 15}}, {125, {0x7fa5, 15}}, {126, {0x7fa6, 15}}, {127, {0x7fa7, 15}},
    {128, {0x7fa8, 15}}, {129, {0x7fa9, 15}}, {130, {0x7faa, 15}}, {131, {0x7fab, 15}},
    {132, {0x7fac, 15}}, {133, {0x7fad, 15}}, {134, {0x7fae, 15}}, {135, {0x7faf, 15}},
    {136, {0x7fb0, 15}}, {137, {0x7fb1, 15}}, {138, {0x7fb2, 15}}, {139, {0x7fb3, 15}},
    {140, {0x7fb4, 15}}, {141, {0x7fb5, 15}}, {142, {0x7fb6, 15}}, {143, {0x7fb7, 15}},
    {144, {0x7fb8, 15}}, {145, {0x7fb9, 15}}, {146, {0x7fba, 15}}, {147, {0x7fbb, 15}},
    {148, {0x7fbc, 15}}, {149, {0x7fbd, 15}}, {150, {0x7fbe, 15}}, {151, {0x7fbf, 15}},
    {152, {0x7fc0, 15}}, {153, {0x7fc1, 15}}, {154, {0x7fc2, 15}}, {155, {0x7fc3, 15}},
    {156, {0x7fc4, 15}}, {157, {0x7fc5, 15}}, {158, {0x7fc6, 15}}, {159, {0x7fc7, 15}},
    {160, {0x7fc8, 15}}, {161, {0x7fc9, 15}}, {162, {0x7fca, 15}}, {163, {0x7fcb, 15}},
    {164, {0x7fcc, 15}}, {165, {0x7fcd, 15}}, {166, {0x7fce, 15}}, {167, {0x7fcf, 15}},
    {168, {0x7fd0, 15}}, {169, {0x7fd1, 15}}, {170, {0x7fd2, 15}}, {171, {0x7fd3, 15}},
    {172, {0x7fd4, 15}}, {173, {0x7fd5, 15}}, {174, {0x7fd6, 15}}, {175, {0x7fd7, 15}},
    {176, {0x7fd8, 15}}, {177, {0x7fd9, 15}}, {178, {0x7fda, 15}}, {179, {0x7fdb, 15}},
    {180, {0x7fdc, 15}}, {181, {0x7fdd, 15}}, {182, {0x7fde, 15}}, {183, {0x7fdf, 15}},
    {184, {0x7fe0, 15}}, {185, {0x7fe1, 15}}, {186, {0x7fe2, 15}}, {187, {0x7fe3, 15}},
    {188, {0x7fe4, 15}}, {189, {0x7fe5, 15}}, {190, {0x7fe6, 15}}, {191, {0x7fe7, 15}},
    {192, {0x7fe8, 15}}, {193, {0x7fe9, 15}}, {194, {0x7fea, 15}}, {195, {0x7feb, 15}},
    {196, {0x7fec, 15}}, {197, {0x7fed, 15}}, {198, {0x7fee, 15}}, {199, {0x7fef, 15}},
    {200, {0x7ff2, 15}}, {201, {0x7ff3, 15}}, {202, {0x7ff4, 15}}, {203, {0x7ff5, 15}},
    {204, {0x7ff6, 15}}, {205, {0x7ff7, 15}}, {206, {0x7ff80, 19}}, {207, {0x7ff81, 19}},
    {208, {0x7ff82, 19}}, {209, {0x7ff83, 19}}, {210, {0x7ff84, 19}}, {211, {0x7ff85, 19}},
    {212, {0x7ff86, 19}}, {213, {0x7ff87, 19}}, {214, {0x7ff88, 19}}, {215, {0x7ff89, 19}},
    {216, {0x7ff8a, 19}}, {217, {0x7ff8b, 19}}, {218, {0x7ff8c, 19}}, {219, {0x7ff8d, 19}},
    {220, {0x7ff8e, 19}}, {221, {0x7ff8f, 19}}, {222, {0x7ff90, 19}}, {223, {0x7ff91, 19}},
    {224, {0x7ff92, 19}}, {225, {0x7ff93, 19}}, {226, {0x7ff94, 19}}, {227, {0x7ff95, 19}},
    {228, {0x7ff96, 19}}, {229, {0x7ff97, 19}}, {230, {0x7ff98, 19}}, {231, {0x7ff99, 19}},
    {232, {0x7ff9a, 19}}, {233, {0x7ff9b, 19}}, {234, {0x7ff9c, 19}}, {235, {0x7ff9d, 19}},
    {236, {0x7ff9e, 19}}, {237, {0x7ff9f, 19}}, {238, {0x7ffa0, 19}}, {239, {0x7ffa1, 19}},
    {240, {0x7ffa2, 19}}, {241, {0x7ffa3, 19}}, {242, {0x7ffa4, 19}}, {243, {0x7ffa5, 19}},
    {244, {0x7ffa6, 19}}, {245, {0x7ffa7, 19}}, {246, {0x7ffa8, 19}}, {247, {0x7ffa9, 19}},
    {248, {0x7ffaa, 19}}, {249, {0x7ffab, 19}}, {250, {0x7ffac, 19}}, {251, {0x7ffad, 19}},
    {252, {0x7ffae, 19}}, {253, {0x7ffaf, 19}}, {254, {0x7ffb0, 19}}, {255, {0x7ffb1, 19}},
    {256, {0x7ffb2, 19}} // EOS (End of String) - Not used for encoding actual chars
    // The actual EOS for padding is all 1s up to 30 bits.
};
const uint32_t HUFFMAN_EOS = 0x3fffffff; // 30 bits of 1s

// For decoding, a tree or a multi-level array lookup is more efficient.
// For decoding, a tree or a multi-level array lookup is more efficient.
// A more optimized version would use a prebuilt static tree, possibly with state for faster lookup.
struct HuffmanDecodeNode {
    std::unique_ptr<HuffmanDecodeNode> children[2]; // 0 and 1
    std::optional<uint8_t> symbol = std::nullopt;   // Decoded symbol if this is a leaf
    bool is_eos_prefix = false; // True if this node represents a prefix of the EOS symbol

    HuffmanDecodeNode() = default;
};

static std::unique_ptr<HuffmanDecodeNode> HUFFMAN_DECODE_TREE_ROOT_INSTANCE = nullptr;

// Function to build the Huffman decoding tree (RFC 7541, Appendix B)
static void initialize_huffman_decode_tree() {
    if (HUFFMAN_DECODE_TREE_ROOT_INSTANCE) return;

    HUFFMAN_DECODE_TREE_ROOT_INSTANCE = std::make_unique<HuffmanDecodeNode>();

    for (uint32_t i = 0; i <= 256; ++i) { // Iterate through all possible symbols (0-255 for chars, 256 for EOS)
        if (HUFFMAN_CODES.count(static_cast<uint8_t>(i)) == 0 && i != 256) { // EOS (256) is not in HUFFMAN_CODES map directly
            continue;
        }

        uint32_t code;
        uint8_t bits;

        if (i == 256) { // EOS symbol
            code = HUFFMAN_EOS; // 0x3fffffff (30 bits of 1s)
            bits = 30;
        } else {
            const auto& entry = HUFFMAN_CODES.at(static_cast<uint8_t>(i));
            code = entry.first;
            bits = entry.second;
        }

        HuffmanDecodeNode* current = HUFFMAN_DECODE_TREE_ROOT_INSTANCE.get();
        for (int j = bits - 1; j >= 0; --j) {
            int bit = (code >> j) & 1;
            if (!current->children[bit]) {
                current->children[bit] = std::make_unique<HuffmanDecodeNode>();
            }
            current = current->children[bit].get();
            if (i == 256) { // Mark all nodes on EOS path as EOS prefixes
                current->is_eos_prefix = true;
            }
        }
        if (i != 256) { // Don't store EOS as a decodable symbol in the same way
            current->symbol = static_cast<uint8_t>(i);
        }
    }
}


std::pair<std::vector<std::byte>, HuffmanError> huffman_encode(const std::string& input) {
    std::vector<std::byte> encoded_data;
    uint64_t current_byte_accumulator = 0;
    int bits_in_accumulator = 0;

    for (char ch_signed : input) {
        uint8_t ch = static_cast<uint8_t>(ch_signed);
        auto it = HUFFMAN_CODES.find(ch);
        if (it == HUFFMAN_CODES.end()) {
            return {{}, HuffmanError::INVALID_INPUT}; // Character not in Huffman table
        }

        uint32_t code = it->second.first;
        uint8_t num_bits = it->second.second;

        current_byte_accumulator <<= num_bits;
        current_byte_accumulator |= code;
        bits_in_accumulator += num_bits;

        while (bits_in_accumulator >= 8) {
            bits_in_accumulator -= 8;
            encoded_data.push_back(static_cast<std::byte>((current_byte_accumulator >> bits_in_accumulator) & 0xFF));
        }
    }

    if (bits_in_accumulator > 0) {
        // Add EOS prefix (most significant bits of EOS code)
        // RFC 7541: "The EOS symbol is coded using the code of length 30 bits"
        // "...any bits of the EOS symbol not consumed are padding and MUST be set to 1"
        uint8_t remaining_bits_in_byte = 8 - bits_in_accumulator;
        // Get the required number of MSBs from the EOS code.
        // HUFFMAN_EOS is 30 bits of 1s.
        uint32_t eos_prefix_mask = (1 << remaining_bits_in_byte) - 1; // e.g., if remaining_bits_in_byte is 3, mask is 0b111
        uint32_t eos_prefix = (HUFFMAN_EOS >> (30 - remaining_bits_in_byte)) & eos_prefix_mask;
        // Since EOS is all 1s, this prefix will also be all 1s.

        current_byte_accumulator <<= remaining_bits_in_byte;
        current_byte_accumulator |= eos_prefix;
        encoded_data.push_back(static_cast<std::byte>(current_byte_accumulator & 0xFF));
    }

    return {encoded_data, HuffmanError::OK};
}

std::pair<std::string, HuffmanError> huffman_decode(std::span<const std::byte> input, size_t max_output_length) {
    if (!HUFFMAN_DECODE_TREE_ROOT_INSTANCE) {
        initialize_huffman_decode_tree();
    }

    std::string decoded_string;
    decoded_string.reserve(input.size() * 2); // Pre-allocate, rough estimate

    const HuffmanDecodeNode* current_node = HUFFMAN_DECODE_TREE_ROOT_INSTANCE.get();
    if (!current_node) return { "", HuffmanError::INTERNAL_ERROR}; // Should not happen

    int bit_count_in_last_byte = 0; // To track how many bits of the last byte are padding

    for (size_t byte_idx = 0; byte_idx < input.size(); ++byte_idx) {
        std::byte b = input[byte_idx];
        for (int bit_idx = 7; bit_idx >= 0; --bit_idx) {
            if (decoded_string.length() > max_output_length) {
                return {decoded_string, HuffmanError::BUFFER_TOO_SMALL}; // Output limit exceeded
            }

            int bit = (static_cast<uint8_t>(b) >> bit_idx) & 1;
            if (!current_node->children[bit]) {
                 // This sequence of bits does not correspond to any valid Huffman code or EOS prefix.
                return {decoded_string, HuffmanError::INVALID_INPUT};
            }
            current_node = current_node->children[bit].get();

            if (current_node->symbol) {
                decoded_string += static_cast<char>(current_node->symbol.value());
                current_node = HUFFMAN_DECODE_TREE_ROOT_INSTANCE.get(); // Reset for next char
                bit_count_in_last_byte = 0; // Reset padding counter after a full symbol
            } else {
                 bit_count_in_last_byte++;
                 if (byte_idx == input.size() -1 && current_node->is_eos_prefix) {
                    // We are in the last byte, and the current sequence is a prefix of EOS.
                    // This is valid padding. We can stop.
                    // The number of padding bits is (8 - bit_idx) or (7-bit_idx +1)
                    // The spec requires padding to be the MSBs of EOS.
                    // If we are on an EOS prefix path, and it's the end of input, it's valid.
                 } else if (byte_idx == input.size() -1 && !current_node->is_eos_prefix) {
                    // End of input, current path is not an EOS prefix, and not a full symbol.
                    return {decoded_string, HuffmanError::INVALID_PADDING};
                 }
                 // If not end of input, and not a symbol, just continue.
            }
        }
    }

    // After all bytes are processed, if current_node is not the root,
    // it means the input ended mid-sequence.
    // This is valid if the partial sequence is a prefix of the EOS symbol.
    if (current_node != HUFFMAN_DECODE_TREE_ROOT_INSTANCE.get()) {
        if (!current_node->is_eos_prefix) {
            // The final bits did not form a complete character AND are not a valid EOS prefix.
            return {decoded_string, HuffmanError::INVALID_PADDING};
        }
        // If it IS an EOS prefix, then the padding is valid.
    }

    return {decoded_string, HuffmanError::OK};
}


size_t get_huffman_encoded_length(const std::string& input) {
    size_t total_bits = 0;
    for (char ch_signed : input) {
        uint8_t ch = static_cast<uint8_t>(ch_signed);
        auto it = HUFFMAN_CODES.find(ch);
        if (it == HUFFMAN_CODES.end()) {
            // This case should ideally not happen if input is valid.
            // Or, it means this character cannot be Huffman encoded.
            // For length calculation, this is problematic. Assume error or very large length.
            return std::string::npos; // Indicate error or unsuitability
        }
        total_bits += it->second.second;
    }
    return (total_bits + 7) / 8; // Round up to the nearest byte
}


} // namespace Hpack
} // namespace http2
