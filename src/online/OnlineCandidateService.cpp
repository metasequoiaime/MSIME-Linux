#include "OnlineCandidateService.h"

#include <stdexcept>
#include <utility>

namespace metasequoia::linux_ime::online
{
OnlineCandidateService::OnlineCandidateService(std::shared_ptr<GoogleCloudProvider> cloud_provider,
                                               Callback callback, std::chrono::milliseconds debounce)
    : cloud_provider_(std::move(cloud_provider)), callback_(std::move(callback)), debounce_(debounce)
{
    if (!cloud_provider_ || !callback_ || debounce_ < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("Online candidate service requires a provider, callback, and non-negative debounce.");
    }
    worker_ = std::thread(&OnlineCandidateService::worker_loop, this);
}

OnlineCandidateService::~OnlineCandidateService()
{
    stop();
}

void OnlineCandidateService::submit(OnlineRequest request)
{
    if (!request.query.cloud_eligible || request.query.query_text.empty())
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
        latest_request_ = std::move(request);
        latest_update_ = std::chrono::steady_clock::now();
        request_token_.fetch_add(1);
    }
    changed_.notify_one();
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
    changed_.notify_one();
}

void OnlineCandidateService::stop()
{
    if (!stopping_.exchange(true))
    {
        request_token_.fetch_add(1);
        changed_.notify_all();
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
    {
        worker_.join();
    }
}

void OnlineCandidateService::worker_loop()
{
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

        const auto request = latest_request_;
        if (!request.has_value())
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
            candidate = cloud_provider_->fetch(request->query, cancelled);
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
                callback_(*request, *candidate, CandidateSource::CloudSuggestion);
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
