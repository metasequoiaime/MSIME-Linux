#pragma once

#include "HttpTimeouts.h"
#include "HttpTransport.h"
#include <metasequoia/session.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

class EnglishDictionary;

namespace metasequoia::linux_ime::online
{
enum class TranslationBackend
{
    Local,
    DeepLX,
};

struct TranslationConfig
{
    bool enabled = false;
    TranslationBackend backend = TranslationBackend::Local;
    std::string endpoint;
    std::string token;
    std::string target_language = "en";
};

struct TranslationRequest
{
    std::uint64_t generation = 0;
    std::vector<std::string> candidates;
    TranslationConfig config;
};

class TranslationProvider
{
  public:
    explicit TranslationProvider(std::string dictionary_path, std::shared_ptr<HttpTransport> transport,
                                 std::shared_ptr<const HttpTimeoutsHandle> timeouts = {});
    TranslationProvider(const RuntimePaths &paths, std::shared_ptr<HttpTransport> transport,
                        std::shared_ptr<const HttpTimeoutsHandle> timeouts = {});
    ~TranslationProvider();

    TranslationProvider(const TranslationProvider &) = delete;
    TranslationProvider &operator=(const TranslationProvider &) = delete;

    std::optional<std::string> lookup(std::string_view candidate, std::string_view target_language,
                                      std::string_view endpoint, std::string_view token,
                                      const CancellationCheck &cancelled,
                                      TranslationBackend backend = TranslationBackend::DeepLX) const;
    static std::string format_gloss(std::string_view raw);
    static std::optional<std::string> parse_deeplx_response(std::string_view response);

  private:
    std::string dictionary_path_;
    std::string translations_path_;
    std::shared_ptr<HttpTransport> transport_;
    // Read on every request rather than copied here; see HttpTimeoutsHandle.
    std::shared_ptr<const HttpTimeoutsHandle> timeouts_;
    // Opening the dictionary re-reads the custom translation sidecar and re-opens SQLite, which is far too expensive to
    // repeat for every candidate of every composition, so one handle is kept alive and serialised here.
    mutable std::mutex dictionary_mutex_;
    mutable std::unique_ptr<EnglishDictionary> dictionary_;
};

class TranslationService
{
  public:
    using Callback = std::function<void(std::uint64_t, std::vector<std::pair<std::string, std::string>>)>;

    TranslationService(std::shared_ptr<TranslationProvider> provider, Callback callback);
    ~TranslationService();

    TranslationService(const TranslationService &) = delete;
    TranslationService &operator=(const TranslationService &) = delete;

    void submit(TranslationRequest request);
    void clear();
    void stop();

  private:
    void worker_loop();

    std::shared_ptr<TranslationProvider> provider_;
    Callback callback_;
    std::thread worker_;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace metasequoia::linux_ime::online
