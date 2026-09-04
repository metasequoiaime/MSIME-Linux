#include "../src/InputController.h"
#include "../src/online/GoogleCloudProvider.h"
#include "../src/online/HttpTransport.h"
#include "../src/online/OnlineCandidateService.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using metasequoia::OnlineQuery;
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::OnlineRequest;
using metasequoia::linux_ime::online::CancellationCheck;
using metasequoia::linux_ime::online::GoogleCloudProvider;
using metasequoia::linux_ime::online::HttpRequest;
using metasequoia::linux_ime::online::HttpResponse;
using metasequoia::linux_ime::online::HttpTransport;
using metasequoia::linux_ime::online::OnlineCandidateService;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct ScriptedResponse
{
    HttpResponse response;
    bool block_until_cancelled = false;
    bool throw_exception = false;
};

class FakeTransport final : public HttpTransport
{
  public:
    HttpResponse perform(const HttpRequest &request, const CancellationCheck &cancelled) override
    {
        std::unique_lock lock(mutex_);
        const std::size_t index = requests_.size();
        requests_.push_back(request);
        const ScriptedResponse script = scripts_.empty() ? ScriptedResponse{} : scripts_.front();
        if (!scripts_.empty())
        {
            scripts_.pop_front();
        }
        calls_changed_.notify_all();

        if (script.throw_exception)
        {
            throw std::runtime_error("scripted transport failure");
        }
        while (script.block_until_cancelled && !cancelled())
        {
            release_changed_.wait_for(lock, 10ms);
        }
        if (cancelled())
        {
            cancelled_calls_.push_back(index);
            return {0, {}, "cancelled"};
        }
        return script.response;
    }

    void queue(ScriptedResponse script)
    {
        std::lock_guard lock(mutex_);
        scripts_.push_back(std::move(script));
    }

    bool wait_for_calls(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return calls_changed_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
    }

    std::vector<HttpRequest> requests() const
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    std::size_t cancelled_call_count() const
    {
        std::lock_guard lock(mutex_);
        return cancelled_calls_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable calls_changed_;
    std::condition_variable release_changed_;
    std::deque<ScriptedResponse> scripts_;
    std::vector<HttpRequest> requests_;
    std::vector<std::size_t> cancelled_calls_;
};

struct DeliveredCandidate
{
    OnlineRequest request;
    std::string candidate;
    CandidateSource source = CandidateSource::Database;
};

class DeliveryLog
{
  public:
    void append(const OnlineRequest &request, std::string candidate, CandidateSource source)
    {
        {
            std::lock_guard lock(mutex_);
            values_.push_back({request, std::move(candidate), source});
        }
        changed_.notify_all();
    }

    bool wait_for_size(std::size_t size, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return values_.size() >= size; });
    }

    std::vector<DeliveredCandidate> values() const
    {
        std::lock_guard lock(mutex_);
        return values_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<DeliveredCandidate> values_;
};

class ThrowOnceDeliveryLog
{
  public:
    void append(const OnlineRequest &request, std::string candidate, CandidateSource source)
    {
        bool throw_now = false;
        {
            std::lock_guard lock(mutex_);
            ++attempts_;
            throw_now = attempts_ == 1;
            if (!throw_now)
            {
                values_.push_back({request, std::move(candidate), source});
            }
        }
        changed_.notify_all();
        if (throw_now)
        {
            throw std::runtime_error("scripted callback failure");
        }
    }

    bool wait_for_attempts(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return attempts_ >= count; });
    }

    std::vector<DeliveredCandidate> values() const
    {
        std::lock_guard lock(mutex_);
        return values_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t attempts_ = 0;
    std::vector<DeliveredCandidate> values_;
};

OnlineQuery query(SchemeType scheme, std::string text, std::uint64_t generation = 1)
{
    OnlineQuery result;
    result.scheme = scheme;
    result.generation = generation;
    result.identity = std::to_string(static_cast<int>(scheme)) + ":" + text;
    result.query_text = std::move(text);
    result.cache_key = result.query_text;
    result.cloud_eligible = true;
    result.ai_eligible = scheme != SchemeType::JapaneseRomaji;
    return result;
}

OnlineRequest request(std::uint64_t generation, SchemeType scheme, std::string text)
{
    return {generation, query(scheme, std::move(text), generation)};
}

const std::string kSuccess = R"(["SUCCESS",[["ni",["你"],[],{}]]])";
} // namespace

int main()
{
    auto transport = std::make_shared<FakeTransport>();
    GoogleCloudProvider provider(transport);

    transport->queue({{200, kSuccess, {}}});
    const auto chinese = provider.fetch(query(SchemeType::Quanpin, "ni hao/ü"), [] { return false; });
    require(chinese == "你", "The Google provider did not parse the first Chinese candidate.");
    const auto chinese_requests = transport->requests();
    require(chinese_requests.size() == 1 &&
                chinese_requests[0].url.find("text=ni%20hao%2F%C3%BC") != std::string::npos &&
                chinese_requests[0].url.find("itc=zh-t-i0-pinyin") != std::string::npos &&
                chinese_requests[0].connect_timeout == 2500ms && chinese_requests[0].total_timeout == 8000ms &&
                chinese_requests[0].max_response_bytes > 0,
            "The Chinese Google request did not encode input or enforce transport limits.");

    transport->queue({{200, kSuccess, {}}});
    require(provider.fetch(query(SchemeType::JapaneseRomaji, "ka"), [] { return false; }) == "你",
            "The Google provider did not parse a Japanese cloud candidate.");
    const auto japanese_requests = transport->requests();
    require(japanese_requests.size() == 2 &&
                japanese_requests[1].url.find("itc=ja-t-i0-und") != std::string::npos,
            "The Japanese Google request used the wrong input tool code.");

    transport->queue({{500, kSuccess, {}}});
    require(!provider.fetch(query(SchemeType::Quanpin, "ni"), [] { return false; }).has_value(),
            "An HTTP error produced a cloud candidate.");
    transport->queue({{200, "not-json", {}}});
    require(!provider.fetch(query(SchemeType::Quanpin, "ni"), [] { return false; }).has_value(),
            "Invalid JSON produced a cloud candidate.");
    transport->queue({{200, {}, {}}});
    require(!provider.fetch(query(SchemeType::Quanpin, "ni"), [] { return false; }).has_value(),
            "An empty response produced a cloud candidate.");
    transport->queue({{0, {}, "timeout"}});
    require(!provider.fetch(query(SchemeType::Quanpin, "ni"), [] { return false; }).has_value(),
            "A transport timeout produced a cloud candidate.");

    auto debounce_transport = std::make_shared<FakeTransport>();
    debounce_transport->queue({{200, kSuccess, {}}});
    auto debounce_provider = std::make_shared<GoogleCloudProvider>(debounce_transport);
    auto debounce_deliveries = std::make_shared<DeliveryLog>();
    const auto debounce_started = std::chrono::steady_clock::now();
    {
        OnlineCandidateService service(
            debounce_provider,
            [debounce_deliveries](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
                debounce_deliveries->append(online_request, std::move(candidate), source);
            });
        service.submit(request(10, SchemeType::Quanpin, "ni"));
        service.submit(request(11, SchemeType::Quanpin, "nihao"));
        require(debounce_transport->wait_for_calls(1, 2s), "The debounced cloud request never started.");
        const auto elapsed = std::chrono::steady_clock::now() - debounce_started;
        require(elapsed >= 500ms, "The cloud request started before the 500 ms debounce elapsed.");
        require(debounce_deliveries->wait_for_size(1, 1s), "The debounced cloud result was not delivered.");
        const auto calls = debounce_transport->requests();
        const auto deliveries = debounce_deliveries->values();
        require(calls.size() == 1 && calls[0].url.find("text=nihao") != std::string::npos &&
                    deliveries.size() == 1 && deliveries[0].request.generation == 11 &&
                    deliveries[0].candidate == "你" && deliveries[0].source == CandidateSource::CloudSuggestion,
                "Debounce did not replace the older online request.");
    }

    auto replacement_transport = std::make_shared<FakeTransport>();
    replacement_transport->queue({{200, kSuccess, {}}, true});
    replacement_transport->queue({{200, kSuccess, {}}});
    auto replacement_provider = std::make_shared<GoogleCloudProvider>(replacement_transport);
    auto replacement_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService service(
            replacement_provider,
            [replacement_deliveries](const OnlineRequest &online_request, std::string candidate,
                                     CandidateSource source) {
                replacement_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(20, SchemeType::Quanpin, "ni"));
        require(replacement_transport->wait_for_calls(1, 1s), "The replaceable cloud request never started.");
        service.submit(request(21, SchemeType::Quanpin, "nihao"));
        require(replacement_transport->wait_for_calls(2, 1s), "The replacement cloud request never started.");
        require(replacement_deliveries->wait_for_size(1, 1s), "The replacement cloud result was not delivered.");
        const auto deliveries = replacement_deliveries->values();
        require(replacement_transport->cancelled_call_count() == 1 && deliveries.size() == 1 &&
                    deliveries[0].request.generation == 21,
                "A newer generation did not cancel and replace the active cloud request.");
    }

    auto stopped_transport = std::make_shared<FakeTransport>();
    stopped_transport->queue({{200, kSuccess, {}}, true});
    auto stopped_provider = std::make_shared<GoogleCloudProvider>(stopped_transport);
    auto stopped_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService service(
            stopped_provider,
            [stopped_deliveries](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
                stopped_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(30, SchemeType::Quanpin, "ni"));
        require(stopped_transport->wait_for_calls(1, 1s), "The stoppable cloud request never started.");

        InputController local_controller(SchemeType::Quanpin, 3);
        const auto local_result = local_controller.handle_key({FrontendKey::Character, 'n'});
        require(local_result.handled && local_controller.preedit() == "n",
                "Local input waited for or depended on the blocked cloud transport.");
        service.stop();
        require(stopped_transport->cancelled_call_count() == 1 && stopped_deliveries->values().empty(),
                "Stopping the online service delivered an in-flight result.");
    }

    auto destroyed_transport = std::make_shared<FakeTransport>();
    destroyed_transport->queue({{200, kSuccess, {}}, true});
    auto destroyed_provider = std::make_shared<GoogleCloudProvider>(destroyed_transport);
    auto destroyed_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService service(
            destroyed_provider,
            [destroyed_deliveries](const OnlineRequest &online_request, std::string candidate,
                                   CandidateSource source) {
                destroyed_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(40, SchemeType::Quanpin, "ni"));
        require(destroyed_transport->wait_for_calls(1, 1s), "The destruction fixture request never started.");
    }
    require(destroyed_transport->cancelled_call_count() == 1 && destroyed_deliveries->values().empty(),
            "Destroying the online service invoked a late callback.");

    auto throwing_transport = std::make_shared<FakeTransport>();
    throwing_transport->queue({{}, false, true});
    throwing_transport->queue({{200, kSuccess, {}}});
    auto throwing_provider = std::make_shared<GoogleCloudProvider>(throwing_transport);
    auto recovered_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService service(
            throwing_provider,
            [recovered_deliveries](const OnlineRequest &online_request, std::string candidate,
                                   CandidateSource source) {
                recovered_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(50, SchemeType::Quanpin, "ni"));
        require(throwing_transport->wait_for_calls(1, 1s), "The throwing transport request never started.");
        service.submit(request(51, SchemeType::Quanpin, "nihao"));
        require(throwing_transport->wait_for_calls(2, 1s),
                "The online worker stopped after a transport exception.");
        require(recovered_deliveries->wait_for_size(1, 1s),
                "The online worker did not recover from a transport exception.");
        require(recovered_deliveries->values()[0].request.generation == 51,
                "The recovered worker delivered the wrong request.");
    }

    auto callback_transport = std::make_shared<FakeTransport>();
    callback_transport->queue({{200, kSuccess, {}}});
    callback_transport->queue({{200, kSuccess, {}}});
    auto callback_provider = std::make_shared<GoogleCloudProvider>(callback_transport);
    auto callback_deliveries = std::make_shared<ThrowOnceDeliveryLog>();
    {
        OnlineCandidateService service(
            callback_provider,
            [callback_deliveries](const OnlineRequest &online_request, std::string candidate,
                                  CandidateSource source) {
                callback_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(60, SchemeType::Quanpin, "ni"));
        require(callback_deliveries->wait_for_attempts(1, 1s), "The throwing callback was not invoked.");
        service.submit(request(61, SchemeType::Quanpin, "nihao"));
        require(callback_deliveries->wait_for_attempts(2, 1s),
                "The online worker stopped after a callback exception.");
        const auto deliveries = callback_deliveries->values();
        require(deliveries.size() == 1 && deliveries[0].request.generation == 61,
                "The worker did not recover from a callback exception.");
    }

    auto reentrant_stop_transport = std::make_shared<FakeTransport>();
    reentrant_stop_transport->queue({{200, kSuccess, {}}});
    auto reentrant_stop_provider = std::make_shared<GoogleCloudProvider>(reentrant_stop_transport);
    auto reentrant_stop_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService *service_pointer = nullptr;
        OnlineCandidateService service(
            reentrant_stop_provider,
            [&service_pointer, reentrant_stop_deliveries](const OnlineRequest &online_request,
                                                          std::string candidate, CandidateSource source) {
                service_pointer->stop();
                reentrant_stop_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service_pointer = &service;
        service.submit(request(70, SchemeType::Quanpin, "ni"));
        require(reentrant_stop_deliveries->wait_for_size(1, 1s),
                "Stopping from the worker callback did not return safely.");
    }

    return 0;
}
