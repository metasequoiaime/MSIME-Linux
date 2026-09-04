#pragma once

#include "GoogleCloudProvider.h"
#include "../InputController.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace metasequoia::linux_ime::online
{
class OnlineCandidateService
{
  public:
    // Runs on the owned worker thread. It may call stop(), but must not destroy this service;
    // frontends should marshal the immutable request to their UI/main thread.
    using Callback = std::function<void(const OnlineRequest &, std::string, CandidateSource)>;

    explicit OnlineCandidateService(std::shared_ptr<GoogleCloudProvider> cloud_provider, Callback callback,
                                    std::chrono::milliseconds debounce = std::chrono::milliseconds(500));
    ~OnlineCandidateService();

    OnlineCandidateService(const OnlineCandidateService &) = delete;
    OnlineCandidateService &operator=(const OnlineCandidateService &) = delete;

    void submit(OnlineRequest request);
    void clear();
    void stop();

  private:
    void worker_loop();

    std::shared_ptr<GoogleCloudProvider> cloud_provider_;
    Callback callback_;
    std::chrono::milliseconds debounce_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<OnlineRequest> latest_request_;
    std::chrono::steady_clock::time_point latest_update_{};
    std::atomic<std::uint64_t> request_token_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};
} // namespace metasequoia::linux_ime::online
