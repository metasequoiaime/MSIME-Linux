#include "GoogleCloudProvider.h"

#include <boost/json.hpp>

#include <stdexcept>
#include <utility>

namespace metasequoia::linux_ime::online
{
namespace
{
std::string url_encode(const std::string &input)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(input.size() * 3);
    for (const unsigned char character : input)
    {
        const bool unreserved = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                                (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                                character == '.' || character == '~';
        if (unreserved)
        {
            encoded.push_back(static_cast<char>(character));
        }
        else
        {
            encoded.push_back('%');
            encoded.push_back(hex[(character >> 4) & 0x0f]);
            encoded.push_back(hex[character & 0x0f]);
        }
    }
    return encoded;
}
} // namespace

GoogleCloudProvider::GoogleCloudProvider(std::shared_ptr<HttpTransport> transport) : transport_(std::move(transport))
{
    if (!transport_)
    {
        throw std::invalid_argument("Google cloud provider requires an HTTP transport.");
    }
}

std::optional<std::string> GoogleCloudProvider::fetch(const OnlineQuery &query,
                                                      const CancellationCheck &cancelled) const
{
    if (!query.cloud_eligible || query.query_text.empty() || (cancelled && cancelled()))
    {
        return std::nullopt;
    }

    HttpRequest request;
    request.url = build_url(query);
    const HttpResponse response = transport_->perform(request, cancelled);
    if (response.status_code != 200 || response.body.empty() || (cancelled && cancelled()))
    {
        return std::nullopt;
    }
    return parse_candidate(response.body);
}

std::string GoogleCloudProvider::build_url(const OnlineQuery &query)
{
    const char *input_tool = query.scheme == SchemeType::JapaneseRomaji ? "ja-t-i0-und" : "zh-t-i0-pinyin";
    return "https://inputtools.google.com/request?text=" + url_encode(query.query_text) + "&itc=" + input_tool +
           "&num=1&ie=utf-8&oe=utf-8";
}

std::optional<std::string> GoogleCloudProvider::parse_candidate(std::string_view response)
{
    boost::system::error_code error;
    const boost::json::value root = boost::json::parse(response, error);
    if (error || !root.is_array())
    {
        return std::nullopt;
    }
    const auto &root_array = root.as_array();
    if (root_array.size() < 2 || !root_array[0].is_string() || root_array[0].as_string() != "SUCCESS" ||
        !root_array[1].is_array())
    {
        return std::nullopt;
    }
    const auto &results = root_array[1].as_array();
    if (results.empty() || !results[0].is_array())
    {
        return std::nullopt;
    }
    const auto &first_result = results[0].as_array();
    if (first_result.size() < 2 || !first_result[1].is_array())
    {
        return std::nullopt;
    }
    const auto &candidates = first_result[1].as_array();
    if (candidates.empty() || !candidates[0].is_string() || candidates[0].as_string().empty())
    {
        return std::nullopt;
    }
    const auto &candidate = candidates[0].as_string();
    return std::string(candidate.data(), candidate.size());
}
} // namespace metasequoia::linux_ime::online
