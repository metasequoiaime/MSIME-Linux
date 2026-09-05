#include "online/EndpointPolicy.h"

#include <stdexcept>
#include <string>

namespace
{
using metasequoia::linux_ime::online::endpoint_allowed;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void accepts_https()
{
    require(endpoint_allowed("https://api.openai.com/v1/chat/completions"), "a plain HTTPS endpoint was rejected");
    require(endpoint_allowed("https://example.invalid"), "an HTTPS endpoint without a path was rejected");
}

void rejects_anything_that_is_not_https()
{
    require(!endpoint_allowed("http://example.invalid/v1"), "plain HTTP was accepted");
    require(!endpoint_allowed("ftp://example.invalid"), "a non-HTTP scheme was accepted");
    require(!endpoint_allowed("example.invalid/v1"), "a schemeless endpoint was accepted");
    require(!endpoint_allowed(""), "an empty endpoint was accepted");
    // Scheme comparison is case sensitive on purpose: curl would accept HTTPS://
    // but nothing in this project produces it, so treat it as malformed input.
    require(!endpoint_allowed("HTTPS://example.invalid"), "an upper case scheme was accepted");
}

// The voice provider used to have no limit here while the other two capped at
// 2048, which is the kind of difference that only shows up in production.
void rejects_an_overlong_endpoint()
{
    const std::string endpoint = "https://example.invalid/" + std::string(2048, 'a');
    require(!endpoint_allowed(endpoint), "an endpoint longer than the limit was accepted");
    const std::string at_limit = "https://a/" + std::string(2048 - 10, 'b');
    require(endpoint_allowed(at_limit), "an endpoint at the limit was rejected");
}

// Only the translation provider used to reject these.
void rejects_control_characters()
{
    require(!endpoint_allowed("https://example.invalid/v1\r\nX-Injected: 1"), "CRLF was accepted");
    require(!endpoint_allowed("https://example.invalid/v1\n"), "a newline was accepted");
    require(!endpoint_allowed(std::string("https://example.invalid/v\0 1", 27)), "a NUL was accepted");
    require(!endpoint_allowed("https://example.invalid/\x7f"), "a delete character was accepted");
}

void allows_loopback_only_when_asked()
{
    require(!endpoint_allowed("http://127.0.0.1:8080/v1"), "loopback was allowed without opting in");
    require(endpoint_allowed("http://127.0.0.1:8080/v1", true), "loopback was rejected after opting in");
    require(endpoint_allowed("http://localhost:8080/v1", true), "loopback by name was rejected");
    require(endpoint_allowed("http://[::1]:8080/v1", true), "IPv6 loopback was rejected");
    // A host that merely starts with the loopback spelling is a different host.
    require(!endpoint_allowed("http://127.0.0.1.example.invalid/v1", true), "a lookalike host was accepted");
    require(!endpoint_allowed("http://localhost.example.invalid/v1", true), "a lookalike name was accepted");
}
} // namespace

int main()
{
    accepts_https();
    rejects_anything_that_is_not_https();
    rejects_an_overlong_endpoint();
    rejects_control_characters();
    allows_loopback_only_when_asked();
    return 0;
}
