#pragma once

#include "http2_types.h" // For HttpHeader
#include <vector>
#include <string>

namespace http2 {
namespace Hpack {

// RFC 7541 Appendix A: Static Table Definition
// The static table consists of a predefined list of common header fields.
// Entries are identified by a 1-based index.

const extern std::vector<HttpHeader> STATIC_TABLE;

// Function to get a header from the static table by its 1-based index.
// Returns std::nullopt if the index is invalid.
std::optional<HttpHeader> get_static_header(uint64_t index);

// Function to find a header in the static table.
// Returns a pair: <index, value_matches>.
// If name doesn't match, index is 0.
// If name matches but value doesn't, index is the entry's index and value_matches is false.
// If both name and value match, index is the entry's index and value_matches is true.
std::pair<int, bool> find_in_static_table(const std::string& name, const std::string& value);
std::pair<int, bool> find_in_static_table(const HttpHeader& header);


} // namespace Hpack
} // namespace http2
