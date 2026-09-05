#include "EndpointPolicy.h"

namespace metasequoia::linux_ime::online
{
namespace
{
constexpr std::size_t kMaximumEndpointLength = 2048;

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// A control character cannot appear in a valid URL. libcurl rejects one anyway,
// so this is not the last line of defence, but it keeps a malformed endpoint
// from reaching the transport in the first place and keeps the rule visible.
bool contains_control_character(std::string_view value)
{
    for (const char character : value)
    {
        if (static_cast<unsigned char>(character) < 0x20 || static_cast<unsigned char>(character) == 0x7f)
        {
            return true;
        }
    }
    return false;
}

bool is_loopback_http_endpoint(std::string_view endpoint)
{
    constexpr std::string_view prefixes[]{"http://127.0.0.1", "http://localhost", "http://[::1]"};
    for (const std::string_view prefix : prefixes)
    {
        if (starts_with(endpoint, prefix) &&
            (endpoint.size() == prefix.size() || endpoint[prefix.size()] == ':' || endpoint[prefix.size()] == '/'))
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool endpoint_allowed(std::string_view endpoint, bool allow_loopback_for_tests)
{
    if (endpoint.empty() || endpoint.size() > kMaximumEndpointLength || contains_control_character(endpoint))
    {
        return false;
    }
    return starts_with(endpoint, "https://") || (allow_loopback_for_tests && is_loopback_http_endpoint(endpoint));
}
} // namespace metasequoia::linux_ime::online
