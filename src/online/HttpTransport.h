#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace metasequoia::linux_ime::online
{
enum class HttpMethod
{
    Get,
    Post,
    Patch,
    Put,
    Delete,
};

using HttpBodySource = std::function<std::size_t(std::size_t, char *, std::size_t)>;
using HttpResponseSink = std::function<bool(const char *, std::size_t)>;
struct HttpRequest
{
    HttpMethod method = HttpMethod::Get;
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{2500};
    std::chrono::milliseconds total_timeout{8000};
    std::size_t max_response_bytes = 256 * 1024;
    // Streaming callbacks are synchronous and must not retain provided buffers.
    // A source supplies exactly body_size bytes, independently addressable by offset.
    // A sink receives provisional bytes; callers publish only after a successful response.
    HttpBodySource body_source;
    std::size_t body_size = 0;
    HttpResponseSink response_sink;
};

struct HttpResponse
{
    long status_code = 0;
    std::string body;
    std::string error;
};

using CancellationCheck = std::function<bool()>;

class HttpTransport
{
  public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse perform(const HttpRequest &request, const CancellationCheck &cancelled) = 0;
};

class CurlHttpTransport final : public HttpTransport
{
  public:
    HttpResponse perform(const HttpRequest &request, const CancellationCheck &cancelled) override;
};
} // namespace metasequoia::linux_ime::online
