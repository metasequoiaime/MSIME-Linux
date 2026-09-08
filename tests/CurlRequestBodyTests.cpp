#include "online/HttpTransport.h"
#include <iostream>
#include <curl/curl.h>
#include <cstdlib>
#include <string>
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
    std::cout << "Real curl HTTP methods and JSON bodies passed\n";
}
