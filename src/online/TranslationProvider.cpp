#include "TranslationProvider.h"
#include "contracts/assets/assets.h"
#include "core/data_path.h"

#include "EndpointPolicy.h"

#include "../../vendor/MetasequoiaImeEngine/english/english_dictionary.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace metasequoia::linux_ime::online
{
namespace
{
thread_local const TranslationService *active_translation_service = nullptr;

// A remote gloss costs one HTTPS round trip per candidate while the engine hands us up to a hundred of them, so the
// remote backend stops just past the first lookup table page (the largest page size the settings accept is nine)
// instead of shipping the whole list to a third party on every composition. The local dictionary keeps glossing
// everything because it never leaves the machine.
constexpr std::size_t kMaximumRemoteCandidates = 12;

// Glosses are published as they accumulate so an interrupted run still shows what it already resolved.
constexpr std::size_t kPublishBatchSize = 4;

std::string trim(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
    {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
    {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string upper_language(std::string_view language)
{
    std::string result(language);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    return result;
}

std::size_t utf8_prefix(std::string_view value, std::size_t maximum)
{
    if (value.size() <= maximum)
    {
        return value.size();
    }
    std::size_t end = maximum;
    while (end > 0)
    {
        std::size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(value[start]) & 0xc0U) == 0x80U)
        {
            --start;
        }
        const unsigned char lead = static_cast<unsigned char>(value[start]);
        const std::size_t width = lead < 0x80U ? 1 : lead < 0xe0U ? 2 : lead < 0xf0U ? 3 : 4;
        if (width <= end - start)
        {
            return end;
        }
        end = start;
    }
    return 0;
}
} // namespace

TranslationProvider::TranslationProvider(std::string dictionary_path, std::shared_ptr<HttpTransport> transport,
                                         std::shared_ptr<const HttpTimeoutsHandle> timeouts)
    : dictionary_path_(std::move(dictionary_path)), transport_(std::move(transport)),
      timeouts_(timeouts ? std::move(timeouts) : default_http_timeouts())
{
    if (!transport_)
    {
        throw std::invalid_argument("Translation provider requires an HTTP transport.");
    }
}

// Defined here because the cached dictionary is only a forward declaration in the header.
TranslationProvider::~TranslationProvider() = default;

// The overrides are assigned in the body rather than the initialiser list because the delegated constructor owns the
// members. That is still before any lookup can run, which is what matters now that the dictionary is opened once and
// cached: it reads translations_path_ when it is first built, not on every call.
TranslationProvider::TranslationProvider(const RuntimePaths &paths, std::shared_ptr<HttpTransport> transport,
                                         std::shared_ptr<const HttpTimeoutsHandle> timeouts)
    : TranslationProvider(path_to_utf8(paths.dictionary(assets::english_dictionary)), std::move(transport),
                          std::move(timeouts))
{
    paths.validate();
    translations_path_ = path_to_utf8(paths.resource(assets::translations));
}

std::optional<std::string> TranslationProvider::lookup(std::string_view candidate, std::string_view target_language,
                                                       std::string_view endpoint, std::string_view token,
                                                       const CancellationCheck &cancelled,
                                                       TranslationBackend backend) const
{
    if (candidate.empty() || candidate.size() > 256 || target_language.empty() || (cancelled && cancelled()))
    {
        return std::nullopt;
    }

    if (target_language == "en" && !dictionary_path_.empty())
    {
        std::lock_guard dictionary_lock(dictionary_mutex_);
        if (!dictionary_)
        {
            dictionary_ = std::make_unique<EnglishDictionary>(dictionary_path_, false, translations_path_);
        }
        std::string local = dictionary_->query_chinese_gloss(std::string(candidate));
        if (local.empty())
        {
            local = dictionary_->query_english_gloss(std::string(candidate));
        }
        local = format_gloss(local);
        if (!local.empty())
        {
            return local;
        }
    }

    if (backend == TranslationBackend::Local || !endpoint_allowed(endpoint) || !token_allowed(token) ||
        (cancelled && cancelled()))
    {
        return std::nullopt;
    }

    boost::json::object body;
    body["text"] = std::string(candidate);
    body["source_lang"] = "auto";
    body["target_lang"] = upper_language(target_language);

    HttpRequest request;
    request.method = HttpMethod::Post;
    request.url = std::string(endpoint);
    request.headers = {"Content-Type: application/json"};
    if (!token.empty())
    {
        request.headers.push_back("Authorization: Bearer " + std::string(token));
    }
    request.body = boost::json::serialize(body);
    const HttpTimeouts timeouts = timeouts_->get();
    request.connect_timeout = timeouts.connect;
    request.total_timeout = timeouts.total;
    const HttpResponse response = transport_->perform(request, cancelled);
    if (response.status_code < 200 || response.status_code >= 300 || response.body.empty() ||
        (cancelled && cancelled()))
    {
        return std::nullopt;
    }
    const auto parsed = parse_deeplx_response(response.body);
    if (!parsed.has_value())
    {
        return std::nullopt;
    }
    const std::string formatted = format_gloss(*parsed);
    return formatted.empty() ? std::nullopt : std::optional<std::string>(formatted);
}

std::string TranslationProvider::format_gloss(std::string_view raw)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= raw.size() && parts.size() < 2)
    {
        const std::size_t separator = raw.find_first_of(";,\n", start);
        const std::size_t end = separator == std::string_view::npos ? raw.size() : separator;
        std::string part = trim(raw.substr(start, end - start));
        if (!part.empty())
        {
            if (part.size() > 96)
            {
                part.resize(utf8_prefix(part, 96));
            }
            parts.push_back(std::move(part));
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        start = separator + 1;
    }
    if (parts.empty())
    {
        return {};
    }
    std::string result = parts.front();
    if (parts.size() == 2)
    {
        result += "; ";
        result += parts.back();
    }
    return result;
}

std::optional<std::string> TranslationProvider::parse_deeplx_response(std::string_view response)
{
    boost::system::error_code error;
    const boost::json::value root = boost::json::parse(response, error);
    if (error || !root.is_object())
    {
        return std::nullopt;
    }
    const auto &object = root.as_object();
    if (const auto data = object.if_contains("data"); data != nullptr && data->is_string())
    {
        return std::string(data->as_string().c_str(), data->as_string().size());
    }
    if (const auto translations = object.if_contains("translations");
        translations != nullptr && translations->is_array() && !translations->as_array().empty())
    {
        const auto &first = translations->as_array().front();
        if (first.is_object())
        {
            if (const auto text = first.as_object().if_contains("text"); text != nullptr && text->is_string())
            {
                return std::string(text->as_string().c_str(), text->as_string().size());
            }
        }
    }
    return std::nullopt;
}

struct TranslationService::State
{
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<TranslationRequest> latest_request;
    std::uint64_t token = 0;
    std::chrono::steady_clock::time_point last_update{};
    bool stopping = false;
};

TranslationService::TranslationService(std::shared_ptr<TranslationProvider> provider, Callback callback)
    : provider_(std::move(provider)), callback_(std::move(callback)), state_(std::make_unique<State>())
{
    if (!provider_ || !callback_)
    {
        throw std::invalid_argument("Translation service requires a provider and callback.");
    }
    worker_ = std::thread(&TranslationService::worker_loop, this);
}

TranslationService::~TranslationService()
{
    stop();
}

void TranslationService::submit(TranslationRequest request)
{
    if (!request.config.enabled || request.candidates.empty())
    {
        clear();
        return;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping)
        {
            return;
        }
        state_->latest_request = std::move(request);
        state_->last_update = std::chrono::steady_clock::now();
        ++state_->token;
    }
    state_->changed.notify_all();
}

void TranslationService::clear()
{
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping)
        {
            return;
        }
        state_->latest_request.reset();
        ++state_->token;
    }
    state_->changed.notify_all();
}

void TranslationService::stop()
{
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->stopping)
        {
            state_->stopping = true;
            ++state_->token;
        }
    }
    state_->changed.notify_all();
    if (active_translation_service == this)
    {
        return;
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void TranslationService::worker_loop()
{
    struct Marker
    {
        const TranslationService *previous;
        ~Marker()
        {
            active_translation_service = previous;
        }
    } marker{active_translation_service};
    active_translation_service = this;

    std::uint64_t observed_token = 0;
    std::unique_lock lock(state_->mutex);
    while (!state_->stopping)
    {
        state_->changed.wait(lock, [&] { return state_->stopping || state_->token != observed_token; });
        if (state_->stopping)
        {
            return;
        }
        observed_token = state_->token;
        auto deadline = state_->last_update + std::chrono::milliseconds(500);
        while (state_->changed.wait_until(lock, deadline,
                                          [&] { return state_->stopping || state_->token != observed_token; }))
        {
            if (state_->stopping)
            {
                return;
            }
            observed_token = state_->token;
            deadline = state_->last_update + std::chrono::milliseconds(500);
        }
        const auto request = state_->latest_request;
        if (!request.has_value())
        {
            continue;
        }
        lock.unlock();

        const auto cancelled = [state = state_.get(), observed_token] {
            std::lock_guard state_lock(state->mutex);
            return state->stopping || state->token != observed_token;
        };
        std::vector<std::pair<std::string, std::string>> results;
        std::size_t published = 0;
        // Every delivery carries the whole accumulated set because the frontend replaces its gloss map with what it
        // receives.
        const auto publish = [&] {
            try
            {
                callback_(request->generation, results);
            }
            catch (...)
            {
            }
            published = results.size();
        };
        for (std::size_t index = 0; index < request->candidates.size(); ++index)
        {
            if (cancelled())
            {
                break;
            }
            // Candidates past the remote budget still get a local gloss, which costs nothing beyond a cached dictionary
            // query; only the network round trips are rationed.
            const TranslationBackend backend =
                index < kMaximumRemoteCandidates ? request->config.backend : TranslationBackend::Local;
            const auto &candidate = request->candidates[index];
            const auto gloss = provider_->lookup(candidate, request->config.target_language, request->config.endpoint,
                                                 request->config.token, cancelled, backend);
            if (gloss.has_value())
            {
                results.emplace_back(candidate, *gloss);
            }
            if (results.size() - published >= kPublishBatchSize && !cancelled())
            {
                publish();
            }
        }
        if (results.size() > published && !cancelled())
        {
            publish();
        }
        lock.lock();
    }
}
} // namespace metasequoia::linux_ime::online
