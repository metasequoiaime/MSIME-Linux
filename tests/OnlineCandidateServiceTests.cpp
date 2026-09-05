#include "../src/InputController.h"
#include "../src/online/AiSuggestionProvider.h"
#include "../src/online/GoogleCloudProvider.h"
#include "../src/online/HttpTransport.h"
#include "../src/online/OnlineCandidateService.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"
#include "../vendor/MetasequoiaImeEngine/tests/src/test_directory_cleanup.h"

#include <boost/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <filesystem>
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
using metasequoia::linux_ime::online::AiProvider;
using metasequoia::linux_ime::online::AiSuggestionConfig;
using metasequoia::linux_ime::online::AiSuggestionProvider;
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
            cancelled_changed_.notify_all();
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

    bool wait_for_cancelled_calls(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return cancelled_changed_.wait_for(lock, timeout, [&] { return cancelled_calls_.size() >= count; });
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable calls_changed_;
    std::condition_variable cancelled_changed_;
    std::condition_variable release_changed_;
    std::deque<ScriptedResponse> scripts_;
    std::vector<HttpRequest> requests_;
    std::vector<std::size_t> cancelled_calls_;
};

class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the online-service test dictionary.");
        }
    }

    ~Database()
    {
        sqlite3_close(database_);
    }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3 *database_ = nullptr;
};

void set_data_directory(const std::filesystem::path &directory)
{
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(directory).c_str(), 1) != 0)
    {
        throw std::runtime_error("Failed to set the online-service test data directory.");
    }
}

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

OnlineQuery ai_query(std::string text, std::uint64_t generation = 1)
{
    OnlineQuery result = query(SchemeType::Quanpin, std::move(text), generation);
    result.pinyin_segments = result.query_text == "nihao" ? std::vector<std::string>{"ni", "hao"}
                                                          : std::vector<std::string>{result.query_text};
    result.ai_eligible = true;
    return result;
}

std::string ai_response(std::string candidate)
{
    boost::json::object item;
    item["text"] = std::move(candidate);
    boost::json::array candidates;
    candidates.push_back(std::move(item));
    boost::json::object content;
    content["candidates"] = std::move(candidates);
    boost::json::object message;
    message["content"] = boost::json::serialize(content);
    boost::json::object choice;
    choice["finish_reason"] = "stop";
    choice["message"] = std::move(message);
    boost::json::array choices;
    choices.push_back(std::move(choice));
    boost::json::object response;
    response["choices"] = std::move(choices);
    return boost::json::serialize(response);
}

AiSuggestionConfig ai_config(AiProvider provider = AiProvider::DeepSeek)
{
    AiSuggestionConfig config;
    config.enabled = true;
    config.provider = provider;
    config.token = "sk-test-secret";
    config.prompt = "Return JSON candidates for this input.";
    config.candidate_limit = 2;
    return config;
}

const std::string kSuccess = R"(["SUCCESS",[["ni",["你"],[],{}]]])";
} // namespace

int main()
{
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path data_directory =
        std::filesystem::temp_directory_path() / ("metasequoia-online-service-" + suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(data_directory);
    std::filesystem::create_directories(data_directory);
    {
        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',200);");
    }
    set_data_directory(data_directory);

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
    require(japanese_requests.size() == 2 && japanese_requests[1].url.find("itc=ja-t-i0-und") != std::string::npos,
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
        require(calls.size() == 1 && calls[0].url.find("text=nihao") != std::string::npos && deliveries.size() == 1 &&
                    deliveries[0].request.generation == 11 && deliveries[0].candidate == "你" &&
                    deliveries[0].source == CandidateSource::CloudSuggestion,
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
            [destroyed_deliveries](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
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
            [recovered_deliveries](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
                recovered_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(50, SchemeType::Quanpin, "ni"));
        require(throwing_transport->wait_for_calls(1, 1s), "The throwing transport request never started.");
        service.submit(request(51, SchemeType::Quanpin, "nihao"));
        require(throwing_transport->wait_for_calls(2, 1s), "The online worker stopped after a transport exception.");
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
            [callback_deliveries](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
                callback_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service.submit(request(60, SchemeType::Quanpin, "ni"));
        require(callback_deliveries->wait_for_attempts(1, 1s), "The throwing callback was not invoked.");
        service.submit(request(61, SchemeType::Quanpin, "nihao"));
        require(callback_deliveries->wait_for_attempts(2, 1s), "The online worker stopped after a callback exception.");
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
            [&service_pointer, reentrant_stop_deliveries](const OnlineRequest &online_request, std::string candidate,
                                                          CandidateSource source) {
                service_pointer->stop();
                reentrant_stop_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms);
        service_pointer = &service;
        service.submit(request(70, SchemeType::Quanpin, "ni"));
        require(reentrant_stop_deliveries->wait_for_size(1, 1s),
                "Stopping from the worker callback did not return safely.");
    }

    require(AiSuggestionProvider::default_endpoint(AiProvider::DeepSeek) ==
                    "https://api.deepseek.com/chat/completions" &&
                AiSuggestionProvider::default_model(AiProvider::DeepSeek) == "deepseek-v4-flash" &&
                AiSuggestionProvider::default_endpoint(AiProvider::OpenAI) ==
                    "https://api.openai.com/v1/chat/completions" &&
                AiSuggestionProvider::default_model(AiProvider::OpenAI) == "gpt-4o-mini" &&
                AiSuggestionProvider::default_endpoint(AiProvider::SiliconFlow) ==
                    "https://api.siliconflow.cn/v1/chat/completions" &&
                AiSuggestionProvider::default_model(AiProvider::SiliconFlow) == "Qwen/Qwen3-8B" &&
                AiSuggestionProvider::default_endpoint(AiProvider::Groq) ==
                    "https://api.groq.com/openai/v1/chat/completions" &&
                AiSuggestionProvider::default_model(AiProvider::Groq) == "openai/gpt-oss-120b" &&
                AiSuggestionProvider::default_endpoint(AiProvider::Custom).empty() &&
                AiSuggestionProvider::default_model(AiProvider::Custom).empty(),
            "AI provider defaults do not match their canonical OpenAI-compatible services.");

    auto ai_transport = std::make_shared<FakeTransport>();
    ai_transport->queue({{200, ai_response("你好"), {}}});
    AiSuggestionProvider ai_provider(ai_transport);
    const auto deepseek_config = ai_config();
    require(ai_provider.fetch(ai_query("nihao"), "前文", deepseek_config, [] { return false; }) == "你好",
            "The DeepSeek-compatible provider did not parse its first candidate.");
    const auto ai_requests = ai_transport->requests();
    require(ai_requests.size() == 1 && ai_requests[0].method == metasequoia::linux_ime::online::HttpMethod::Post &&
                ai_requests[0].url == "https://api.deepseek.com/chat/completions" &&
                ai_requests[0].headers.size() == 2 && ai_requests[0].headers[0] == "Content-Type: application/json" &&
                ai_requests[0].headers[1] == "Authorization: Bearer sk-test-secret",
            "The AI provider did not build the expected authenticated POST request.");
    const auto ai_body = boost::json::parse(ai_requests[0].body).as_object();
    const auto &ai_messages = ai_body.at("messages").as_array();
    const auto ai_input = boost::json::parse(ai_messages[1].as_object().at("content").as_string()).as_object();
    require(ai_body.at("model").as_string() == "deepseek-v4-flash" && !ai_body.at("stream").as_bool() &&
                ai_body.at("temperature").as_double() == 0.2 && ai_body.at("max_tokens").as_int64() == 512 &&
                ai_body.at("response_format").as_object().at("type").as_string() == "json_object" &&
                ai_body.at("thinking").as_object().at("type").as_string() == "disabled" &&
                ai_messages[0].as_object().at("role").as_string() == "system" &&
                ai_messages[0].as_object().at("content").as_string() == deepseek_config.prompt &&
                ai_input.at("segmented_pinyin").as_array().size() == 2 &&
                ai_input.at("context").as_string() == "前文" && ai_input.at("candidate_limit").as_int64() == 2,
            "The AI request JSON lost its deterministic generation contract.");
    require(AiSuggestionProvider::cache_key(ai_query("nihao"), "前文", deepseek_config).find(deepseek_config.token) ==
                std::string::npos,
            "The AI cache key exposed an API token.");

    auto invalid_ai_transport = std::make_shared<FakeTransport>();
    AiSuggestionProvider invalid_ai_provider(invalid_ai_transport);
    auto custom_config = ai_config(AiProvider::Custom);
    custom_config.endpoint = "http://example.com/v1/chat/completions";
    custom_config.model = "custom-model";
    require(!invalid_ai_provider.fetch(ai_query("ni"), {}, custom_config, [] { return false; }).has_value() &&
                invalid_ai_transport->requests().empty(),
            "The AI provider accepted a non-HTTPS custom endpoint.");

    auto loopback_transport = std::make_shared<FakeTransport>();
    loopback_transport->queue({{200, ai_response("本地"), {}}});
    AiSuggestionProvider loopback_provider(loopback_transport, true);
    custom_config.endpoint = "http://127.0.0.1:18080/v1/chat/completions";
    require(loopback_provider.fetch(ai_query("ni"), {}, custom_config, [] { return false; }) == "本地",
            "The explicit loopback test endpoint was not accepted.");

    auto cache_transport = std::make_shared<FakeTransport>();
    AiSuggestionProvider cache_provider(cache_transport);
    auto cached_config = ai_config(AiProvider::OpenAI);
    cache_transport->queue({{200, ai_response("缓存一"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "甲", cached_config, [] { return false; }) == "缓存一" &&
                cache_provider.fetch(ai_query("ni"), "甲", cached_config, [] { return false; }) == "缓存一" &&
                cache_transport->requests().size() == 1,
            "An identical AI request did not reuse its bounded cache.");
    auto changed_config = cached_config;
    changed_config.endpoint = "https://example.com/v1/chat/completions";
    cache_transport->queue({{200, ai_response("端点"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "甲", changed_config, [] { return false; }) == "端点",
            "The AI cache was not isolated by endpoint.");
    changed_config = cached_config;
    changed_config.model = "different-model";
    cache_transport->queue({{200, ai_response("模型"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "甲", changed_config, [] { return false; }) == "模型",
            "The AI cache was not isolated by model.");
    changed_config = cached_config;
    changed_config.prompt = "A different JSON prompt.";
    cache_transport->queue({{200, ai_response("提示词"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "甲", changed_config, [] { return false; }) == "提示词",
            "The AI cache was not isolated by prompt.");
    changed_config = ai_config(AiProvider::Groq);
    cache_transport->queue({{200, ai_response("提供商"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "甲", changed_config, [] { return false; }) == "提供商",
            "The AI cache was not isolated by provider.");
    cache_transport->queue({{200, ai_response("上下文"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "乙", cached_config, [] { return false; }) == "上下文",
            "The AI cache was not isolated by surrounding context.");
    changed_config = cached_config;
    changed_config.token = "sk-second-account-secret";
    cache_transport->queue({{200, ai_response("第二账号"), {}}});
    require(cache_provider.fetch(ai_query("ni"), "甲", changed_config, [] { return false; }) == "第二账号" &&
                cache_transport->requests().size() == 7 &&
                AiSuggestionProvider::cache_key(ai_query("ni"), "甲", changed_config).find(changed_config.token) ==
                    std::string::npos,
            "Changing AI credentials reused another account's cached candidate or exposed the token.");

    auto eviction_transport = std::make_shared<FakeTransport>();
    AiSuggestionProvider eviction_provider(eviction_transport);
    for (std::size_t index = 0; index < 129; ++index)
    {
        eviction_transport->queue({{200, ai_response("候选-" + std::to_string(index)), {}}});
        require(eviction_provider.fetch(ai_query("ni"), "上下文-" + std::to_string(index), cached_config,
                                        [] { return false; }) == "候选-" + std::to_string(index),
                "The bounded AI cache fixture failed before reaching capacity.");
    }
    eviction_transport->queue({{200, ai_response("重新获取"), {}}});
    require(eviction_provider.fetch(ai_query("ni"), "上下文-0", cached_config, [] { return false; }) == "重新获取" &&
                eviction_transport->requests().size() == 130,
            "The AI cache did not evict its oldest entry at the configured bound.");

    auto bounded_ai_transport = std::make_shared<FakeTransport>();
    bounded_ai_transport->queue({{200, ai_response("有界"), {}}});
    AiSuggestionProvider bounded_ai_provider(bounded_ai_transport);
    auto bounded_config = ai_config(AiProvider::OpenAI);
    bounded_config.prompt.assign(9000, 'p');
    bounded_config.candidate_limit = 999;
    require(bounded_ai_provider.fetch(ai_query("ni"), std::string(3000, 'c'), bounded_config, [] { return false; }) ==
                "有界",
            "A bounded AI request did not complete.");
    const auto bounded_body = boost::json::parse(bounded_ai_transport->requests()[0].body).as_object();
    const auto &bounded_messages = bounded_body.at("messages").as_array();
    const auto bounded_input =
        boost::json::parse(bounded_messages[1].as_object().at("content").as_string()).as_object();
    require(bounded_messages[0].as_object().at("content").as_string().size() == 8192 &&
                bounded_input.at("context").as_string().size() == 2048 &&
                bounded_input.at("candidate_limit").as_int64() == 10,
            "The AI provider did not bound prompt, context, and candidate count.");

    auto failed_ai_transport = std::make_shared<FakeTransport>();
    AiSuggestionProvider failed_ai_provider(failed_ai_transport);
    failed_ai_transport->queue({{500, ai_response("错误"), {}}});
    require(!failed_ai_provider.fetch(ai_query("ni"), {}, deepseek_config, [] { return false; }).has_value(),
            "An AI HTTP failure produced a candidate.");
    failed_ai_transport->queue({{200, "not-json", {}}});
    require(!failed_ai_provider.fetch(ai_query("nihao"), {}, deepseek_config, [] { return false; }).has_value(),
            "An invalid AI response produced a candidate.");
    failed_ai_transport->queue({{200, ai_response(std::string(513, 'x')), {}}});
    require(!failed_ai_provider.fetch(ai_query("hao"), {}, deepseek_config, [] { return false; }).has_value(),
            "An oversized AI candidate escaped the output limit.");
    std::string controlled_candidate = "可见";
    controlled_candidate.push_back('\0');
    controlled_candidate += "隐藏";
    failed_ai_transport->queue({{200, ai_response(std::move(controlled_candidate)), {}}});
    require(!failed_ai_provider.fetch(ai_query("kongzhi"), {}, deepseek_config, [] { return false; }).has_value(),
            "An AI candidate containing a C0 control character was accepted.");
    auto ineligible_query = ai_query("ni");
    ineligible_query.ai_eligible = false;
    const auto failed_call_count = failed_ai_transport->requests().size();
    require(!failed_ai_provider.fetch(ineligible_query, {}, deepseek_config, [] { return false; }).has_value() &&
                failed_ai_transport->requests().size() == failed_call_count,
            "An AI-ineligible composition reached the transport.");

    InputController ordered_controller(SchemeType::Quanpin, 5);
    for (const char character : std::string("nihao"))
    {
        require(ordered_controller.handle_key({FrontendKey::Character, character}).handled,
                "The AI ordering fixture could not build a composition.");
    }
    const auto ordered_request = ordered_controller.online_request();
    require(ordered_request.has_value(), "The AI ordering fixture did not expose an online request.");
    auto ordered_cloud_transport = std::make_shared<FakeTransport>();
    ordered_cloud_transport->queue({{200, R"(["SUCCESS",[["nihao",["云候选"],[],{}]]])", {}}});
    auto ordered_ai_transport = std::make_shared<FakeTransport>();
    ordered_ai_transport->queue({{200, ai_response("智能候选"), {}}});
    auto ordered_cloud_provider = std::make_shared<GoogleCloudProvider>(ordered_cloud_transport);
    auto ordered_ai_provider = std::make_shared<AiSuggestionProvider>(ordered_ai_transport);
    auto ordered_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService service(
            ordered_cloud_provider,
            [ordered_deliveries](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
                ordered_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms, ordered_ai_provider);
        service.submit(*ordered_request, "前文", deepseek_config);
        require(ordered_deliveries->wait_for_size(2, 1s), "Cloud and AI providers did not run independently.");
    }
    const auto ordered_values = ordered_deliveries->values();
    for (auto iterator = ordered_values.rbegin(); iterator != ordered_values.rend(); ++iterator)
    {
        require(ordered_controller.apply_online_candidate(iterator->request.generation, iterator->request.query,
                                                          iterator->candidate, iterator->source),
                "A current online candidate was rejected during ordering.");
    }
    require(ordered_controller.candidates().size() >= 3 &&
                ordered_controller.candidates()[1].source == CandidateSource::CloudSuggestion &&
                ordered_controller.candidates()[2].source == CandidateSource::AiSuggestion,
            "Cloud and AI candidates did not keep slots 2 and 3 regardless of arrival order.");
    require(!ordered_controller.apply_online_candidate(ordered_request->generation, ordered_request->query, "云候选",
                                                       CandidateSource::AiSuggestion) &&
                !ordered_controller.apply_online_candidate(ordered_request->generation - 1, ordered_request->query,
                                                           "过期智能候选", CandidateSource::AiSuggestion),
            "AI candidate deduplication or generation rejection was bypassed.");

    auto ai_replacement_cloud_transport = std::make_shared<FakeTransport>();
    auto ai_replacement_transport = std::make_shared<FakeTransport>();
    ai_replacement_transport->queue({{200, ai_response("旧智能候选"), {}}, true});
    ai_replacement_transport->queue({{200, ai_response("新智能候选"), {}}});
    auto ai_replacement_cloud_provider = std::make_shared<GoogleCloudProvider>(ai_replacement_cloud_transport);
    auto ai_replacement_provider = std::make_shared<AiSuggestionProvider>(ai_replacement_transport);
    auto ai_replacement_deliveries = std::make_shared<DeliveryLog>();
    {
        OnlineCandidateService service(
            ai_replacement_cloud_provider,
            [ai_replacement_deliveries](const OnlineRequest &online_request, std::string candidate,
                                        CandidateSource source) {
                ai_replacement_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms, ai_replacement_provider);
        service.submit({90, ai_query("ni", 90)}, {}, deepseek_config);
        require(ai_replacement_transport->wait_for_calls(1, 1s), "The replaceable AI request never started.");
        service.submit({91, ai_query("nihao", 91)}, {}, deepseek_config);
        require(ai_replacement_transport->wait_for_calls(2, 1s), "The replacement AI request never started.");
        require(ai_replacement_deliveries->wait_for_size(1, 1s), "The replacement AI result was not delivered.");
        const auto deliveries = ai_replacement_deliveries->values();
        require(ai_replacement_transport->cancelled_call_count() == 1 && deliveries.size() == 1 &&
                    deliveries[0].request.generation == 91 && deliveries[0].source == CandidateSource::AiSuggestion,
                "A newer generation did not cancel and replace the active AI request.");
    }

    auto concurrent_stop_cloud_transport = std::make_shared<FakeTransport>();
    concurrent_stop_cloud_transport->queue({{200, kSuccess, {}}});
    auto concurrent_stop_ai_transport = std::make_shared<FakeTransport>();
    concurrent_stop_ai_transport->queue({{200, ai_response("不会交付"), {}}, true});
    auto concurrent_stop_cloud_provider = std::make_shared<GoogleCloudProvider>(concurrent_stop_cloud_transport);
    auto concurrent_stop_ai_provider = std::make_shared<AiSuggestionProvider>(concurrent_stop_ai_transport);
    auto concurrent_stop_deliveries = std::make_shared<DeliveryLog>();
    std::mutex callback_mutex;
    std::condition_variable callback_changed;
    bool callback_entered = false;
    OnlineCandidateService *concurrent_service_pointer = nullptr;
    {
        OnlineCandidateService service(
            concurrent_stop_cloud_provider,
            [&](const OnlineRequest &online_request, std::string candidate, CandidateSource source) {
                {
                    std::lock_guard lock(callback_mutex);
                    callback_entered = true;
                }
                callback_changed.notify_all();
                require(concurrent_stop_ai_transport->wait_for_cancelled_calls(1, 1s),
                        "The external stop did not cancel the parallel AI request.");
                concurrent_service_pointer->stop();
                concurrent_stop_deliveries->append(online_request, std::move(candidate), source);
            },
            0ms, concurrent_stop_ai_provider);
        concurrent_service_pointer = &service;
        service.submit({100, ai_query("nihao", 100)}, {}, deepseek_config);
        require(concurrent_stop_cloud_transport->wait_for_calls(1, 1s) &&
                    concurrent_stop_ai_transport->wait_for_calls(1, 1s),
                "The concurrent-stop providers did not both start.");
        {
            std::unique_lock lock(callback_mutex);
            require(callback_changed.wait_for(lock, 1s, [&] { return callback_entered; }),
                    "The concurrent-stop callback did not start.");
        }
        std::thread external_stopper([&] { service.stop(); });
        external_stopper.join();
        require(concurrent_stop_deliveries->wait_for_size(1, 1s),
                "A worker callback could not return from stop while another thread joined it.");
    }

    return 0;
}
