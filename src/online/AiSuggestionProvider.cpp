#include "AiSuggestionProvider.h"

#include "EndpointPolicy.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace metasequoia::linux_ime::online
{
namespace
{
constexpr std::size_t kMaximumContextBytes = 2048;
constexpr std::size_t kMaximumPromptBytes = 8192;
constexpr std::size_t kMaximumCandidateBytes = 512;
constexpr std::size_t kMaximumSegments = 32;
constexpr std::size_t kMaximumSegmentBytes = 64;
constexpr std::size_t kMaximumCacheEntries = 128;

std::string provider_id(AiProvider provider)
{
    switch (provider)
    {
    case AiProvider::DeepSeek:
        return "deepseek";
    case AiProvider::OpenAI:
        return "openai";
    case AiProvider::SiliconFlow:
        return "siliconflow";
    case AiProvider::Groq:
        return "groq";
    case AiProvider::Custom:
        return "custom";
    }
    return "custom";
}

std::string bounded_utf8(std::string_view value, std::size_t maximum_bytes)
{
    if (value.size() <= maximum_bytes)
    {
        return std::string(value);
    }
    std::size_t end = maximum_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
    {
        --end;
    }
    return std::string(value.substr(0, end));
}

std::string trim_ascii(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

bool contains_control_character(std::string_view value)
{
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char character) { return character < 0x20U || character == 0x7fU; });
}

std::size_t bounded_candidate_limit(std::size_t limit)
{
    return std::clamp<std::size_t>(limit, 1, 10);
}

std::string resolved_endpoint(const AiSuggestionConfig &config)
{
    return config.endpoint.empty() ? AiSuggestionProvider::default_endpoint(config.provider) : config.endpoint;
}

std::string resolved_model(const AiSuggestionConfig &config)
{
    return config.model.empty() ? AiSuggestionProvider::default_model(config.provider) : config.model;
}

std::string resolved_prompt(const AiSuggestionConfig &config)
{
    return bounded_utf8(config.prompt.empty() ? AiSuggestionProvider::default_prompt() : config.prompt,
                        kMaximumPromptBytes);
}
} // namespace

AiSuggestionProvider::AiSuggestionProvider(std::shared_ptr<HttpTransport> transport,
                                           bool allow_insecure_loopback_for_tests, HttpTimeouts timeouts)
    : transport_(std::move(transport)), allow_insecure_loopback_for_tests_(allow_insecure_loopback_for_tests),
      timeouts_(timeouts)
{
    if (!transport_)
    {
        throw std::invalid_argument("AI suggestion provider requires an HTTP transport.");
    }
}

std::optional<std::string> AiSuggestionProvider::fetch(const OnlineQuery &query, std::string_view context,
                                                       const AiSuggestionConfig &config,
                                                       const CancellationCheck &cancelled) const
{
    if (!config.enabled || !query.ai_eligible || query.pinyin_segments.empty() || config.token.empty() ||
        !token_allowed(config.token) || (cancelled && cancelled()))
    {
        return std::nullopt;
    }

    const std::string endpoint = resolved_endpoint(config);
    const std::string model = resolved_model(config);
    if (!endpoint_allowed(endpoint, allow_insecure_loopback_for_tests_) || model.empty() || model.size() > 256)
    {
        return std::nullopt;
    }

    const std::string key = cache_key(query, context, config);
    std::uint64_t cache_credential_generation = 0;
    {
        std::lock_guard lock(cache_mutex_);
        if (cache_credential_ != config.token)
        {
            cache_.clear();
            cache_order_.clear();
            cache_credential_ = config.token;
            ++cache_credential_generation_;
        }
        cache_credential_generation = cache_credential_generation_;
        const auto cached = cache_.find(key);
        if (cached != cache_.end())
        {
            return cached->second;
        }
    }

    boost::json::array segments;
    const std::size_t segment_count = std::min(query.pinyin_segments.size(), kMaximumSegments);
    segments.reserve(segment_count);
    for (std::size_t index = 0; index < segment_count; ++index)
    {
        segments.push_back(boost::json::value(bounded_utf8(query.pinyin_segments[index], kMaximumSegmentBytes)));
    }

    boost::json::object input;
    input["segmented_pinyin"] = std::move(segments);
    input["context"] = bounded_utf8(context, kMaximumContextBytes);
    input["candidate_limit"] = bounded_candidate_limit(config.candidate_limit);

    boost::json::object system_message;
    system_message["role"] = "system";
    system_message["content"] = resolved_prompt(config);
    boost::json::object user_message;
    user_message["role"] = "user";
    user_message["content"] = boost::json::serialize(input);
    boost::json::array messages;
    messages.push_back(std::move(system_message));
    messages.push_back(std::move(user_message));

    boost::json::object response_format;
    response_format["type"] = "json_object";
    boost::json::object body;
    body["model"] = model;
    body["stream"] = false;
    body["temperature"] = 0.2;
    body["max_tokens"] = 512;
    body["response_format"] = std::move(response_format);
    body["messages"] = std::move(messages);
    if (config.provider == AiProvider::DeepSeek)
    {
        boost::json::object thinking;
        thinking["type"] = "disabled";
        body["thinking"] = std::move(thinking);
    }

    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = endpoint;
    request.headers = {"Content-Type: application/json", "Authorization: Bearer " + config.token};
    request.body = boost::json::serialize(body);
    request.connect_timeout = timeouts_.connect;
    request.total_timeout = timeouts_.total;
    const HttpResponse response = transport_->perform(request, cancelled);
    if (response.status_code < 200 || response.status_code >= 300 || response.body.empty() ||
        (cancelled && cancelled()))
    {
        return std::nullopt;
    }

    const auto candidate = parse_candidate(response.body);
    if (!candidate.has_value() || (cancelled && cancelled()))
    {
        return std::nullopt;
    }
    {
        std::lock_guard lock(cache_mutex_);
        if (cache_credential_generation_ != cache_credential_generation || cache_credential_ != config.token)
        {
            return candidate;
        }
        if (cache_.find(key) == cache_.end())
        {
            while (cache_order_.size() >= kMaximumCacheEntries)
            {
                cache_.erase(cache_order_.front());
                cache_order_.pop_front();
            }
            cache_order_.push_back(key);
        }
        cache_[key] = *candidate;
    }
    return candidate;
}

std::string AiSuggestionProvider::default_endpoint(AiProvider provider)
{
    switch (provider)
    {
    case AiProvider::DeepSeek:
        return "https://api.deepseek.com/chat/completions";
    case AiProvider::OpenAI:
        return "https://api.openai.com/v1/chat/completions";
    case AiProvider::SiliconFlow:
        return "https://api.siliconflow.cn/v1/chat/completions";
    case AiProvider::Groq:
        return "https://api.groq.com/openai/v1/chat/completions";
    case AiProvider::Custom:
        return {};
    }
    return {};
}

std::string AiSuggestionProvider::default_model(AiProvider provider)
{
    switch (provider)
    {
    case AiProvider::DeepSeek:
        return "deepseek-v4-flash";
    case AiProvider::OpenAI:
        return "gpt-4o-mini";
    case AiProvider::SiliconFlow:
        return "Qwen/Qwen3-8B";
    case AiProvider::Groq:
        return "openai/gpt-oss-120b";
    case AiProvider::Custom:
        return {};
    }
    return {};
}

std::string AiSuggestionProvider::default_prompt()
{
    return "你是一个中文全拼输入法联想引擎。输入为已经切分好的拼音数组、前文上下文和候选数量。"
           "优先生成与拼音严格对应的常用中文候选，并结合上下文、语义和固定搭配排序。"
           "若拼音明显组成英文单词、缩写、产品名或技术术语，也可返回英文，但不要生造。"
           "只输出合法 JSON，格式为 {\"candidates\":[{\"text\":\"候选内容\"}]}。"
           "候选数量不得超过指定上限；没有合理结果时返回空数组。";
}

std::optional<std::string> AiSuggestionProvider::parse_candidate(std::string_view response)
{
    boost::system::error_code error;
    const boost::json::value outer = boost::json::parse(response, error);
    if (error || !outer.is_object())
    {
        return std::nullopt;
    }
    const auto &outer_object = outer.as_object();
    const auto *choices = outer_object.if_contains("choices");
    if (choices == nullptr || !choices->is_array() || choices->as_array().empty() ||
        !choices->as_array()[0].is_object())
    {
        return std::nullopt;
    }
    const auto &choice = choices->as_array()[0].as_object();
    const auto *finish_reason = choice.if_contains("finish_reason");
    if (finish_reason != nullptr && finish_reason->is_string() && finish_reason->as_string() == "length")
    {
        return std::nullopt;
    }
    const auto *message = choice.if_contains("message");
    if (message == nullptr || !message->is_object())
    {
        return std::nullopt;
    }
    const auto *content = message->as_object().if_contains("content");
    if (content == nullptr || !content->is_string())
    {
        return std::nullopt;
    }
    const boost::json::value result = boost::json::parse(content->as_string(), error);
    if (error || !result.is_object())
    {
        return std::nullopt;
    }
    const auto *candidates = result.as_object().if_contains("candidates");
    if (candidates == nullptr || !candidates->is_array())
    {
        return std::nullopt;
    }
    for (const auto &entry : candidates->as_array())
    {
        if (!entry.is_object())
        {
            continue;
        }
        const auto *text = entry.as_object().if_contains("text");
        if (text == nullptr || !text->is_string())
        {
            continue;
        }
        const std::string candidate = trim_ascii(text->as_string());
        if (!candidate.empty() && candidate.size() <= kMaximumCandidateBytes && !contains_control_character(candidate))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string AiSuggestionProvider::cache_key(const OnlineQuery &query, std::string_view context,
                                            const AiSuggestionConfig &config)
{
    boost::json::array segments;
    const std::size_t segment_count = std::min(query.pinyin_segments.size(), kMaximumSegments);
    segments.reserve(segment_count);
    for (std::size_t index = 0; index < segment_count; ++index)
    {
        segments.push_back(boost::json::value(bounded_utf8(query.pinyin_segments[index], kMaximumSegmentBytes)));
    }
    boost::json::object key;
    key["provider"] = provider_id(config.provider);
    key["endpoint"] = resolved_endpoint(config);
    key["model"] = resolved_model(config);
    key["prompt"] = resolved_prompt(config);
    key["candidate_limit"] = bounded_candidate_limit(config.candidate_limit);
    key["scheme"] = static_cast<int>(query.scheme);
    key["identity"] = query.identity;
    key["query"] = query.cache_key;
    key["segments"] = std::move(segments);
    key["context"] = bounded_utf8(context, kMaximumContextBytes);
    return boost::json::serialize(key);
}
} // namespace metasequoia::linux_ime::online
