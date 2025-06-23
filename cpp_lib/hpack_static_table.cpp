#include "hpack_static_table.h"
#include <optional>

namespace http2 {
namespace Hpack {

// Definition of the static table (RFC 7541 Appendix A)
// Note: Index is 1-based. static_table[0] corresponds to index 1.
const std::vector<HttpHeader> STATIC_TABLE = {
    {":authority", ""}, // 1
    {":method", "GET"}, // 2
    {":method", "POST"}, // 3
    {":path", "/"}, // 4
    {":path", "/index.html"}, // 5
    {":scheme", "http"}, // 6
    {":scheme", "https"}, // 7
    {":status", "200"}, // 8
    {":status", "204"}, // 9
    {":status", "206"}, // 10
    {":status", "304"}, // 11
    {":status", "400"}, // 12
    {":status", "404"}, // 13
    {":status", "500"}, // 14
    {"accept-charset", ""}, // 15
    {"accept-encoding", "gzip, deflate"}, // 16
    {"accept-language", ""}, // 17
    {"accept-ranges", ""}, // 18
    {"accept", ""}, // 19
    {"access-control-allow-origin", ""}, // 20
    {"age", ""}, // 21
    {"allow", ""}, // 22
    {"authorization", ""}, // 23
    {"cache-control", ""}, // 24
    {"content-disposition", ""}, // 25
    {"content-encoding", ""}, // 26
    {"content-language", ""}, // 27
    {"content-length", ""}, // 28
    {"content-location", ""}, // 29
    {"content-range", ""}, // 30
    {"content-type", ""}, // 31
    {"cookie", ""}, // 32
    {"date", ""}, // 33
    {"etag", ""}, // 34
    {"expect", ""}, // 35
    {"expires", ""}, // 36
    {"from", ""}, // 37
    {"host", ""}, // 38
    {"if-match", ""}, // 39
    {"if-modified-since", ""}, // 40
    {"if-none-match", ""}, // 41
    {"if-range", ""}, // 42
    {"if-unmodified-since", ""}, // 43
    {"last-modified", ""}, // 44
    {"link", ""}, // 45
    {"location", ""}, // 46
    {"max-forwards", ""}, // 47
    {"proxy-authenticate", ""}, // 48
    {"proxy-authorization", ""}, // 49
    {"range", ""}, // 50
    {"referer", ""}, // 51
    {"refresh", ""}, // 52
    {"retry-after", ""}, // 53
    {"server", ""}, // 54
    {"set-cookie", ""}, // 55
    {"strict-transport-security", ""}, // 56
    {"transfer-encoding", ""}, // 57
    {"user-agent", ""}, // 58
    {"vary", ""}, // 59
    {"via", ""}, // 60
    {"www-authenticate", ""} // 61
};

std::optional<HttpHeader> get_static_header(uint64_t index) {
    if (index == 0 || index > STATIC_TABLE.size()) {
        return std::nullopt;
    }
    return STATIC_TABLE[index - 1]; // 1-based index
}

std::pair<int, bool> find_in_static_table(const std::string& name, const std::string& value) {
    for (size_t i = 0; i < STATIC_TABLE.size(); ++i) {
        if (STATIC_TABLE[i].name == name) {
            if (STATIC_TABLE[i].value == value) {
                return {static_cast<int>(i + 1), true}; // Found name and value
            }
            // Name matches, but value doesn't. Return first name match.
            // (HPACK prefers smallest index for name match if value differs)
            bool found_name_only = false;
            int first_name_match_idx = 0;
            for(size_t j=0; j < STATIC_TABLE.size(); ++j) {
                if(STATIC_TABLE[j].name == name) {
                    if(!found_name_only) {
                        first_name_match_idx = static_cast<int>(j+1);
                        found_name_only = true;
                    }
                    if(STATIC_TABLE[j].value == value) { // Should have been caught above
                         return {static_cast<int>(j + 1), true};
                    }
                }
            }
            if(found_name_only) return {first_name_match_idx, false};
        }
    }
    return {0, false}; // Not found
}

std::pair<int, bool> find_in_static_table(const HttpHeader& header) {
    return find_in_static_table(header.name, header.value);
}


} // namespace Hpack
} // namespace http2
