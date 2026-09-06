#pragma once

#include "HttpTimeouts.h"
#include "HttpTransport.h"
#include <metasequoia/session.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace metasequoia::linux_ime::online
{
enum class AiProvider
{
    DeepSeek,
    OpenAI,
    SiliconFlow,
    Groq,
    Custom,
};

struct AiSuggestionConfig
{
    bool enabled = false;
    AiProvider provider = AiProvider::DeepSeek;
    std::string token;
    std::string endpoint;
    std::string model;
    std::string prompt;
    std::size_t candidate_limit = 3;
};

class AiSuggestionProvider
{
  public:
    explicit AiSuggestionProvider(std::shared_ptr<HttpTransport> transport,
                                  bool allow_insecure_loopback_for_tests = false, HttpTimeouts timeouts = {});

    std::optional<std::string> fetch(const OnlineQuery &query, std::string_view context,
                                     const AiSuggestionConfig &config, const CancellationCheck &cancelled) const;

    static std::string default_endpoint(AiProvider provider);
    static std::string default_model(AiProvider provider);
    static std::string default_prompt();
    static std::optional<std::string> parse_candidate(std::string_view response);
    static std::string cache_key(const OnlineQuery &query, std::string_view context, const AiSuggestionConfig &config);

  private:
    std::shared_ptr<HttpTransport> transport_;
    bool allow_insecure_loopback_for_tests_ = false;
    HttpTimeouts timeouts_;
    mutable std::mutex cache_mutex_;
    mutable std::string cache_credential_;
    mutable std::uint64_t cache_credential_generation_ = 0;
    mutable std::deque<std::string> cache_order_;
    mutable std::unordered_map<std::string, std::string> cache_;
};
} // namespace metasequoia::linux_ime::online
