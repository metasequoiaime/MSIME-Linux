#include "HttpTransport.h"

#include <curl/curl.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace metasequoia::linux_ime::online
{
namespace
{
struct CurlGlobalState
{
    CurlGlobalState() : initialized(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK)
    {
    }
    ~CurlGlobalState()
    {
        if (initialized)
        {
            curl_global_cleanup();
        }
    }

    bool initialized = false;
};

struct ResponseBuffer
{
    std::string body;
    std::size_t limit = 0;
    bool exceeded = false;
    bool failed = false;
};

std::size_t write_response(char *data, std::size_t size, std::size_t count, void *user_data) noexcept
{
    auto &buffer = *static_cast<ResponseBuffer *>(user_data);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
    {
        buffer.exceeded = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    if (bytes > buffer.limit - std::min(buffer.limit, buffer.body.size()))
    {
        buffer.exceeded = true;
        return 0;
    }
    try
    {
        buffer.body.append(data, bytes);
    }
    catch (...)
    {
        buffer.failed = true;
        return 0;
    }
    return bytes;
}

int transfer_progress(void *user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept
{
    const auto &cancelled = *static_cast<const CancellationCheck *>(user_data);
    try
    {
        return cancelled && cancelled() ? 1 : 0;
    }
    catch (...)
    {
        return 1;
    }
}

struct CurlHandleDeleter
{
    void operator()(CURL *handle) const
    {
        curl_easy_cleanup(handle);
    }
};
} // namespace

HttpResponse CurlHttpTransport::perform(const HttpRequest &request, const CancellationCheck &cancelled)
{
    static CurlGlobalState global;
    if (!global.initialized)
    {
        return {0, {}, "libcurl initialization failed"};
    }
    if (request.url.rfind("https://", 0) != 0)
    {
        return {0, {}, "only HTTPS URLs are allowed"};
    }

    std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
    if (!handle)
    {
        return {0, {}, "libcurl handle creation failed"};
    }

    ResponseBuffer response_buffer{{}, request.max_response_bytes, false, false};
    const long connect_timeout = static_cast<long>(request.connect_timeout.count());
    const long total_timeout = static_cast<long>(request.total_timeout.count());
    const bool configured = curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str()) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "HTTPS") == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, connect_timeout) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, total_timeout) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "MetasequoiaImeLinux/0.1") == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_response) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response_buffer) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, transfer_progress) == CURLE_OK &&
                            curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &cancelled) == CURLE_OK;
    if (!configured)
    {
        return {0, {}, "libcurl request configuration failed"};
    }

    curl_slist *header_list = nullptr;
    for (const auto &header : request.headers)
    {
        curl_slist *updated = curl_slist_append(header_list, header.c_str());
        if (updated == nullptr)
        {
            curl_slist_free_all(header_list);
            return {0, {}, "libcurl header allocation failed"};
        }
        header_list = updated;
    }
    const auto free_headers = [&] { curl_slist_free_all(header_list); };
    if (header_list != nullptr && curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, header_list) != CURLE_OK)
    {
        free_headers();
        return {0, {}, "libcurl header configuration failed"};
    }

    if (request.method == HttpMethod::Post &&
        (curl_easy_setopt(handle.get(), CURLOPT_POST, 1L) != CURLE_OK ||
         curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request.body.data()) != CURLE_OK ||
         curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size())) !=
             CURLE_OK))
    {
        free_headers();
        return {0, {}, "libcurl POST configuration failed"};
    }

    const CURLcode result = curl_easy_perform(handle.get());
    long status_code = 0;
    if (result == CURLE_OK)
    {
        (void)curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status_code);
    }
    free_headers();

    if (response_buffer.exceeded)
    {
        return {0, {}, "HTTP response exceeded configured limit"};
    }
    if (response_buffer.failed)
    {
        return {0, {}, "HTTP response buffering failed"};
    }
    if (result != CURLE_OK)
    {
        return {0, {}, cancelled && cancelled() ? "cancelled" : curl_easy_strerror(result)};
    }
    return {status_code, std::move(response_buffer.body), {}};
}
} // namespace metasequoia::linux_ime::online
