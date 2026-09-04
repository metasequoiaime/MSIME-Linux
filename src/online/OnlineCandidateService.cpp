#include "OnlineCandidateService.h"

#include <stdexcept>
#include <utility>

namespace metasequoia::linux_ime::online
{
namespace
{
thread_local const OnlineCandidateService *active_worker_service = nullptr;
}

OnlineCandidateService::OnlineCandidateService(std::shared_ptr<GoogleCloudProvider> cloud_provider,
                                               Callback callback, std::chrono::milliseconds debounce,
                                               std::shared_ptr<AiSuggestionProvider> ai_provider)
    : cloud_provider_(std::move(cloud_provider)), ai_provider_(std::move(ai_provider)),
      callback_(std::move(callback)), debounce_(debounce)
{
    if (!cloud_provider_ || !callback_ || debounce_ < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("Online candidate service requires a provider, callback, and non-negative debounce.");
    }
    try
    {
        cloud_worker_ = std::thread(&OnlineCandidateService::worker_loop, this, CandidateSource::CloudSuggestion);
        if (ai_provider_)
        {
            ai_worker_ = std::thread(&OnlineCandidateService::worker_loop, this, CandidateSource::AiSuggestion);
        }
    }
    catch (...)
    {
        {
            std::lock_guard lock(mutex_);
            stopping_.store(true);
        }
        changed_.notify_all();
        if (cloud_worker_.joinable())
        {
            cloud_worker_.join();
        }
        throw;
    }
}

OnlineCandidateService::~OnlineCandidateService()
{
    stop();
}

void OnlineCandidateService::submit(OnlineRequest request, std::string context,
                                    std::optional<AiSuggestionConfig> ai_config)
{
    const bool cloud_eligible = request.query.cloud_eligible;
    const bool ai_eligible = ai_provider_ && ai_config.has_value() && ai_config->enabled && request.query.ai_eligible;
    if ((!cloud_eligible && !ai_eligible) || request.query.query_text.empty())
    {
        clear();
        return;
    }
    if (stopping_.load())
    {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopping_.load())
        {
            return;
        }
        latest_request_ = PendingRequest{std::move(request), std::move(context), std::move(ai_config)};
        latest_update_ = std::chrono::steady_clock::now();
        request_token_.fetch_add(1);
    }
    changed_.notify_all();
}

void OnlineCandidateService::clear()
{
    if (stopping_.load())
    {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        latest_request_.reset();
        latest_update_ = std::chrono::steady_clock::now();
        request_token_.fetch_add(1);
    }
    changed_.notify_all();
}

void OnlineCandidateService::stop()
{
    {
        std::lock_guard lock(mutex_);
        if (!stopping_.exchange(true))
        {
            request_token_.fetch_add(1);
        }
    }
    changed_.notify_all();
    if (active_worker_service == this)
    {
        return;
    }
    std::lock_guard join_lock(join_mutex_);
    if (cloud_worker_.joinable())
    {
        cloud_worker_.join();
    }
    if (ai_worker_.joinable())
    {
        ai_worker_.join();
    }
}

void OnlineCandidateService::worker_loop(CandidateSource source)
{
    struct WorkerMarker
    {
        const OnlineCandidateService *previous;
        ~WorkerMarker() { active_worker_service = previous; }
    } marker{active_worker_service};
    active_worker_service = this;

    std::uint64_t observed_token = 0;
    std::unique_lock lock(mutex_);
    while (!stopping_.load())
    {
        changed_.wait(lock, [&] { return stopping_.load() || request_token_.load() != observed_token; });
        if (stopping_.load())
        {
            return;
        }

        observed_token = request_token_.load();
        auto deadline = latest_update_ + debounce_;
        while (changed_.wait_until(lock, deadline,
                                   [&] { return stopping_.load() || request_token_.load() != observed_token; }))
        {
            if (stopping_.load())
            {
                return;
            }
            observed_token = request_token_.load();
            deadline = latest_update_ + debounce_;
        }

        const auto pending = latest_request_;
        if (!pending.has_value())
        {
            continue;
        }
        const bool eligible = source == CandidateSource::CloudSuggestion
                                  ? pending->request.query.cloud_eligible
                                  : ai_provider_ && pending->ai_config.has_value() &&
                                        pending->ai_config->enabled && pending->request.query.ai_eligible;
        if (!eligible)
        {
            continue;
        }

        lock.unlock();
        const auto cancelled = [this, observed_token] {
            return stopping_.load() || request_token_.load() != observed_token;
        };
        std::optional<std::string> candidate;
        try
        {
            if (source == CandidateSource::CloudSuggestion)
            {
                candidate = cloud_provider_->fetch(pending->request.query, cancelled);
            }
            else
            {
                candidate = ai_provider_->fetch(pending->request.query, pending->context, *pending->ai_config,
                                                cancelled);
            }
        }
        catch (...)
        {
            // Online failures must not terminate or block local input.
            lock.lock();
            continue;
        }
        if (candidate.has_value() && !cancelled())
        {
            try
            {
                callback_(pending->request, *candidate, source);
            }
            catch (...)
            {
                // Frontend delivery is also isolated from the worker lifecycle.
            }
        }
        lock.lock();
    }
}
} // namespace metasequoia::linux_ime::online
