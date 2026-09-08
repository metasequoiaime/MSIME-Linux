#include "online/HttpTransport.h"
#include <iostream>
#include <curl/curl.h>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cstring>
#include <stdexcept>
using namespace metasequoia::linux_ime::online;
// Test-only linker wrapper: trust the generated loopback CA on these handles.
// Production transport still performs hostname and peer certificate verification.
extern "C" CURL *__real_curl_easy_init();
extern "C" CURL *__wrap_curl_easy_init()
{
    auto *handle = __real_curl_easy_init();
    if (handle)
        if (const char *ca = std::getenv("MSIME_TEST_CA"))
            curl_easy_setopt(handle, CURLOPT_CAINFO, ca);
    return handle;
}
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    CurlHttpTransport transport;
    unsetenv("MSIME_TEST_CA");
    HttpRequest untrusted;
    untrusted.url = argv[1];
    if (transport.perform(untrusted, {}).error.empty())
        return 5;
    setenv("MSIME_TEST_CA", argv[2], 1);
    for (const auto method :
         {HttpMethod::Get, HttpMethod::Post, HttpMethod::Put, HttpMethod::Patch, HttpMethod::Delete})
    {
        HttpRequest request;
        request.url = argv[1];
        request.method = method;
        request.body = method == HttpMethod::Get ? "" : "{\"revision\":7}";
        request.headers = {"Content-Type: application/json"};
        const auto response = transport.perform(request, {});
        const auto name = method == HttpMethod::Get     ? "GET"
                          : method == HttpMethod::Post  ? "POST"
                          : method == HttpMethod::Put   ? "PUT"
                          : method == HttpMethod::Patch ? "PATCH"
                                                        : "DELETE";
        if (!response.error.empty() || response.status_code != 200 ||
            response.body != std::string(name) + "\n" + request.body)
            return 3;
    }
    HttpRequest empty;
    empty.url = argv[1];
    empty.method = HttpMethod::Delete;
    const auto response = transport.perform(empty, {});
    if (!response.error.empty() || response.status_code != 200 || response.body != "DELETE\n")
        return 4;
    const std::string payload(256 * 1024, 'x');
    for (const auto method : {HttpMethod::Post, HttpMethod::Put, HttpMethod::Patch, HttpMethod::Delete})
    {
        HttpRequest stream;
        stream.url = argv[1];
        stream.method = method;
        stream.headers = {"Content-Type: application/x-ndjson", "Expect:"};
        stream.body_size = payload.size();
        std::size_t supplied = 0;
        stream.body_source = [&](std::size_t offset, char *data, std::size_t capacity) {
            if (offset != supplied)
                throw std::runtime_error("unexpected upload offset");
            const auto count = std::min<std::size_t>(capacity, 997);
            std::memcpy(data, payload.data() + offset, count);
            supplied += count;
            return count;
        };
        std::string received;
        stream.max_response_bytes = payload.size() + 16;
        stream.response_sink = [&](const char *data, std::size_t size) {
            received.append(data, size);
            return true;
        };
        auto result = transport.perform(stream, {});
        const char *name = method == HttpMethod::Post    ? "POST"
                           : method == HttpMethod::Put   ? "PUT"
                           : method == HttpMethod::Patch ? "PATCH"
                                                         : "DELETE";
        if (!result.error.empty() || result.status_code != 200 || !result.body.empty() ||
            received != std::string(name) + "\n" + payload || supplied != payload.size())
            return 6;
    }
    HttpRequest stream;
    stream.url = argv[1];
    stream.method = HttpMethod::Put;
    stream.headers = {"Expect:"};
    stream.body_size = 100;
    stream.body_source = [](std::size_t, char *, std::size_t) { return std::size_t(0); };
    if (transport.perform(stream, {}).error.empty())
        return 7;
    stream.body_source = [](std::size_t, char *, std::size_t capacity) { return capacity + 1; };
    if (transport.perform(stream, {}).error.empty())
        return 8;
    stream.body_source = [](std::size_t, char *, std::size_t) -> std::size_t { throw std::runtime_error("source"); };
    if (transport.perform(stream, {}).error.empty())
        return 9;
    stream.body = "ambiguous";
    if (transport.perform(stream, {}).error.empty())
        return 10;
    stream.body.clear();
    stream.body_size = 0;
    // Zero-length sources are legal and are never read.
    if (!transport.perform(stream, {}).error.empty())
        return 11;
    HttpRequest sink;
    sink.url = argv[1];
    sink.max_response_bytes = 2;
    std::size_t accepted = 0;
    sink.response_sink = [&](const char *, std::size_t size) {
        accepted += size;
        return true;
    };
    if (transport.perform(sink, {}).error.empty() || accepted > 2)
        return 12;
    sink.max_response_bytes = 1024;
    sink.response_sink = [](const char *, std::size_t) { return false; };
    if (transport.perform(sink, {}).error.empty())
        return 13;
    sink.response_sink = [](const char *, std::size_t) -> bool { throw std::runtime_error("sink"); };
    if (transport.perform(sink, {}).error.empty())
        return 14;
    if (transport.perform(sink, [] { return true; }).error.empty())
        return 15;
    std::cout << "Real curl methods, bounded streaming, callback failures and TLS passed\n";
}
