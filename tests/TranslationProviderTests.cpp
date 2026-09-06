#include "../src/online/GoogleCloudProvider.h"
#include "../src/online/TranslationProvider.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using metasequoia::OnlineQuery;
using metasequoia::linux_ime::online::CancellationCheck;
using metasequoia::linux_ime::online::GoogleCloudProvider;
using metasequoia::linux_ime::online::HttpMethod;
using metasequoia::linux_ime::online::HttpRequest;
using metasequoia::linux_ime::online::HttpResponse;
using metasequoia::linux_ime::online::HttpTimeouts;
using metasequoia::linux_ime::online::HttpTimeoutsHandle;
using metasequoia::linux_ime::online::HttpTransport;
using metasequoia::linux_ime::online::TranslationBackend;
using metasequoia::linux_ime::online::TranslationProvider;
using metasequoia::linux_ime::online::TranslationRequest;
using metasequoia::linux_ime::online::TranslationService;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class FakeTransport final : public HttpTransport
{
  public:
    HttpResponse perform(const HttpRequest &request, const CancellationCheck &) override
    {
        std::lock_guard lock(mutex);
        last_request = request;
        ++calls;
        changed.notify_all();
        return {200, R"({"data":"bonjour"})", {}};
    }

    bool wait_for_call(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return calls != 0; });
    }

    bool wait_for_calls(int minimum, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return calls >= minimum; });
    }

    std::mutex mutex;
    std::condition_variable changed;
    HttpRequest last_request;
    int calls = 0;
};

// A transport that holds every request until the test hands out a permit, so the worker can be observed part way
// through a candidate list instead of only after it has drained one.
class GatedTransport final : public HttpTransport
{
  public:
    HttpResponse perform(const HttpRequest &, const CancellationCheck &) override
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return open || permits > 0; });
        if (permits > 0)
        {
            --permits;
        }
        ++completed;
        changed.notify_all();
        return {200, R"({"data":"bonjour"})", {}};
    }

    void grant(int count)
    {
        {
            std::lock_guard lock(mutex);
            permits += count;
        }
        changed.notify_all();
    }

    void open_gate()
    {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }

    int completed_calls()
    {
        std::lock_guard lock(mutex);
        return completed;
    }

    std::mutex mutex;
    std::condition_variable changed;
    int permits = 0;
    int completed = 0;
    bool open = false;
};

// The cloud provider is exercised through fetch() rather than the static parser so the reply travels the same path a
// real inputtools.google.com answer would.
class CloudTransport final : public HttpTransport
{
  public:
    HttpResponse perform(const HttpRequest &request, const CancellationCheck &) override
    {
        last_request = request;
        ++calls;
        return {200, body, {}};
    }

    HttpRequest last_request;
    std::string body;
    int calls = 0;
};

std::string cloud_reply(const std::string &candidate)
{
    return R"(["SUCCESS",[["ni",[")" + candidate + R"("],[],{}]]])";
}

std::filesystem::path make_dictionary(const char *file_name = "metasequoia-translation-test.db")
{
    const auto path = std::filesystem::temp_directory_path() / file_name;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    sqlite3 *database = nullptr;
    require(sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database) == SQLITE_OK,
            "Failed to create translation dictionary.");
    const char *schema = "CREATE TABLE english_words(word TEXT PRIMARY KEY,display TEXT NOT NULL,weight INTEGER);"
                         "CREATE TABLE en_zh_glosses(english TEXT PRIMARY KEY,chinese_gloss TEXT NOT NULL);"
                         "CREATE TABLE zh_en_glosses(chinese TEXT PRIMARY KEY,english_gloss TEXT NOT NULL);"
                         "INSERT INTO en_zh_glosses VALUES('hello','你好');";
    require(sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK,
            "Failed to populate translation dictionary.");
    sqlite3_close(database);
    return path;
}
} // namespace

int main()
{
    const auto dictionary = make_dictionary();
    auto transport = std::make_shared<FakeTransport>();
    TranslationProvider provider(metasequoia::path_to_utf8(dictionary), transport);
    const auto local = provider.lookup("hello", "en", "", "", {});
    require(local.has_value() && *local == "你好", "Local English gloss was not preferred.");

    {
        namespace fs = std::filesystem;
        const auto root =
            fs::temp_directory_path() /
            ("msime-translation-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto resources = root / "resources";
        const auto working = root / "user/dictionaries/initial";
        fs::create_directories(resources);
        fs::create_directories(working);
        fs::copy_file(dictionary, working / "english.db");
        std::ofstream(resources / "custom_translations.txt") << "hello\t资源译文\n";
        std::ofstream(working / "custom_translations.txt") << "hello\t错误目录\n";
        const metasequoia::RuntimePaths paths{resources, root / "user", root / "cache", working};
        TranslationProvider separated(paths, transport);
        require(separated.lookup("hello", "en", "", "", {}, TranslationBackend::Local) == "资源译文",
                "Translation overrides were read beside the working database instead of from resources.");
        require(transport->calls == 0, "A resource translation unexpectedly used the network.");
        fs::remove_all(root);
    }

    const auto remote = provider.lookup("世界", "fr", "https://translation.example.test/translate", "secret", {});
    require(remote.has_value() && *remote == "bonjour", "DeepLX fallback was not parsed.");
    require(transport->wait_for_call(std::chrono::milliseconds(100)), "DeepLX request was not issued.");
    require(transport->last_request.method == HttpMethod::Post && transport->last_request.headers.size() == 2 &&
                transport->last_request.headers[1] == "Authorization: Bearer secret",
            "DeepLX request did not preserve the expected secure contract.");
    const int calls_before_local_only = transport->calls;
    const auto local_only = provider.lookup("未收录", "fr", "https://translation.example.test/translate", "secret", {},
                                            TranslationBackend::Local);
    require(!local_only.has_value() && transport->calls == calls_before_local_only,
            "Local translation mode unexpectedly sent a remote request.");
    require(TranslationProvider::format_gloss(" first meaning; second meaning; third meaning ") ==
                "first meaning; second meaning",
            "Gloss formatting did not retain at most two short meanings.");

    auto timeout_transport = std::make_shared<FakeTransport>();
    TranslationProvider timed_provider(
        metasequoia::path_to_utf8(dictionary), timeout_transport,
        HttpTimeoutsHandle::fixed({std::chrono::milliseconds(300), std::chrono::milliseconds(1500)}));
    const auto timed = timed_provider.lookup("世界", "fr", "https://translation.example.test/translate", "secret", {});
    require(timed.has_value() && *timed == "bonjour", "The configured translation provider stopped parsing replies.");
    require(timeout_transport->last_request.connect_timeout == std::chrono::milliseconds(300) &&
                timeout_transport->last_request.total_timeout == std::chrono::milliseconds(1500),
            "The configured translation timeouts never reached the request, which kept the 2500/8000 ms defaults.");

    // What the shared handle exists for. A provider that captured its deadlines at construction could only pick up an
    // edited connect-timeout-ms or total-timeout-ms by being replaced, and replacing one means joining a worker that
    // may be inside a request of up to the total timeout -- a wait the IBus main loop cannot take, which is why those
    // two settings used to apply only at the next engine start.
    const auto live_timeouts = std::make_shared<HttpTimeoutsHandle>(
        HttpTimeouts{std::chrono::milliseconds(300), std::chrono::milliseconds(1500)});
    auto live_transport = std::make_shared<FakeTransport>();
    TranslationProvider live_provider(metasequoia::path_to_utf8(dictionary), live_transport, live_timeouts);
    (void)live_provider.lookup("世界", "fr", "https://translation.example.test/translate", "secret", {});
    require(live_transport->last_request.connect_timeout == std::chrono::milliseconds(300) &&
                live_transport->last_request.total_timeout == std::chrono::milliseconds(1500),
            "The provider did not read its initial deadlines out of the shared handle.");
    live_timeouts->set({std::chrono::milliseconds(700), std::chrono::milliseconds(2200)});
    (void)live_provider.lookup("世界", "fr", "https://translation.example.test/translate", "secret", {});
    require(live_transport->last_request.connect_timeout == std::chrono::milliseconds(700) &&
                live_transport->last_request.total_timeout == std::chrono::milliseconds(2200),
            "A deadline changed after the provider was built never reached the next request.");

    // Deleting the database between two lookups is the only externally visible difference between a cached handle and
    // one rebuilt per candidate: the open SQLite connection keeps answering, a fresh one cannot open the missing file.
    const auto cached_dictionary = make_dictionary("metasequoia-translation-cache-test.db");
    auto cache_transport = std::make_shared<FakeTransport>();
    TranslationProvider cached_provider(metasequoia::path_to_utf8(cached_dictionary), cache_transport);
    const auto first_gloss = cached_provider.lookup("hello", "en", "", "", {});
    require(first_gloss.has_value() && *first_gloss == "你好",
            "The cached dictionary did not answer the first lookup.");
    std::error_code cache_error;
    std::filesystem::remove(cached_dictionary, cache_error);
    require(!std::filesystem::exists(cached_dictionary), "The translation dictionary copy could not be removed.");
    const auto second_gloss = cached_provider.lookup("hello", "en", "", "", {});
    require(second_gloss.has_value() && *second_gloss == "你好",
            "The provider rebuilt its EnglishDictionary for the second candidate instead of reusing the open handle.");
    require(cache_transport->calls == 0, "A local gloss reached the network.");

    auto cloud_transport = std::make_shared<CloudTransport>();
    GoogleCloudProvider cloud_provider(
        cloud_transport, HttpTimeoutsHandle::fixed({std::chrono::milliseconds(400), std::chrono::milliseconds(1200)}));
    OnlineQuery cloud_query;
    cloud_query.query_text = "ni";
    cloud_query.cloud_eligible = true;
    cloud_transport->body = cloud_reply("  你好  ");
    const auto cloud_candidate = cloud_provider.fetch(cloud_query, {});
    require(cloud_candidate.has_value() && *cloud_candidate == "你好",
            "A padded cloud candidate was not trimmed before it reached the lookup table.");
    require(cloud_transport->last_request.connect_timeout == std::chrono::milliseconds(400) &&
                cloud_transport->last_request.total_timeout == std::chrono::milliseconds(1200),
            "The configured cloud timeouts never reached the request, which kept the 2500/8000 ms defaults.");
    cloud_transport->body = cloud_reply(std::string(512, 'a'));
    const auto cloud_at_limit = cloud_provider.fetch(cloud_query, {});
    require(cloud_at_limit.has_value() && *cloud_at_limit == std::string(512, 'a'),
            "A cloud candidate at the length budget was rejected.");
    cloud_transport->body = cloud_reply(std::string(513, 'a'));
    require(!cloud_provider.fetch(cloud_query, {}).has_value(),
            "An over-long cloud candidate was accepted straight from the network.");
    cloud_transport->body = cloud_reply(R"(ni\u0007hao)");
    require(!cloud_provider.fetch(cloud_query, {}).has_value(),
            "A cloud candidate carrying a control character was accepted straight from the network.");
    cloud_transport->body = cloud_reply(R"(你好\u001b[31m)");
    require(!cloud_provider.fetch(cloud_query, {}).has_value(),
            "A cloud candidate carrying an escape sequence was accepted straight from the network.");
    cloud_transport->body = cloud_reply(R"(第一行\n第二行)");
    require(!cloud_provider.fetch(cloud_query, {}).has_value(),
            "A cloud candidate carrying a newline was accepted straight from the network.");
    cloud_transport->body = cloud_reply("   ");
    require(!cloud_provider.fetch(cloud_query, {}).has_value(), "A blank cloud candidate was accepted.");

    auto budget_transport = std::make_shared<FakeTransport>();
    std::mutex budget_mutex;
    std::condition_variable budget_changed;
    std::vector<std::pair<std::string, std::string>> budget_results;
    auto budget_service = std::make_unique<TranslationService>(
        std::make_shared<TranslationProvider>(metasequoia::path_to_utf8(dictionary), budget_transport),
        [&](std::uint64_t generation, std::vector<std::pair<std::string, std::string>> values) {
            std::lock_guard lock(budget_mutex);
            if (generation == 7)
            {
                budget_results = std::move(values);
            }
            budget_changed.notify_all();
        });
    TranslationRequest budget_request;
    budget_request.generation = 7;
    for (int index = 0; index < 30; ++index)
    {
        budget_request.candidates.push_back("候选" + std::to_string(index));
    }
    budget_request.config.enabled = true;
    budget_request.config.backend = TranslationBackend::DeepLX;
    budget_request.config.endpoint = "https://translation.example.test/translate";
    budget_request.config.token = "secret";
    budget_request.config.target_language = "fr";
    budget_service->submit(std::move(budget_request));
    require(budget_transport->wait_for_calls(12, std::chrono::seconds(5)),
            "The remote budget did not cover the first lookup table page.");
    require(!budget_transport->wait_for_calls(13, std::chrono::milliseconds(500)),
            "Candidate translation issued one remote request per candidate instead of stopping at the budget.");
    {
        std::unique_lock budget_lock(budget_mutex);
        require(
            budget_changed.wait_for(budget_lock, std::chrono::seconds(2), [&] { return budget_results.size() == 12; }),
            "The remote budget did not deliver exactly the candidates it was allowed to translate.");
    }
    budget_service->stop();

    auto gated_transport = std::make_shared<GatedTransport>();
    std::mutex gated_mutex;
    std::condition_variable gated_changed;
    std::vector<std::pair<std::string, std::string>> gated_results;
    int gated_deliveries = 0;
    int gated_first_delivery = 0;
    auto gated_service = std::make_unique<TranslationService>(
        std::make_shared<TranslationProvider>(metasequoia::path_to_utf8(dictionary), gated_transport),
        [&](std::uint64_t generation, std::vector<std::pair<std::string, std::string>> values) {
            std::lock_guard lock(gated_mutex);
            if (generation == 9)
            {
                ++gated_deliveries;
                if (gated_deliveries == 1)
                {
                    gated_first_delivery = static_cast<int>(values.size());
                }
                gated_results = std::move(values);
            }
            gated_changed.notify_all();
        });
    TranslationRequest gated_request;
    gated_request.generation = 9;
    for (int index = 0; index < 10; ++index)
    {
        gated_request.candidates.push_back("分段" + std::to_string(index));
    }
    gated_request.config.enabled = true;
    gated_request.config.backend = TranslationBackend::DeepLX;
    gated_request.config.endpoint = "https://translation.example.test/translate";
    gated_request.config.token = "secret";
    gated_request.config.target_language = "fr";
    gated_service->submit(std::move(gated_request));
    gated_transport->grant(4);
    {
        std::unique_lock gated_lock(gated_mutex);
        require(gated_changed.wait_for(gated_lock, std::chrono::seconds(5), [&] { return gated_deliveries >= 1; }),
                "Candidate translation withheld every gloss until the last request had finished.");
    }
    require(gated_transport->completed_calls() == 4,
            "The first delivery arrived only once every lookup had completed.");
    require(gated_first_delivery == 4, "The first delivery did not carry the glosses that were already resolved.");
    gated_transport->open_gate();
    {
        std::unique_lock gated_lock(gated_mutex);
        require(gated_changed.wait_for(gated_lock, std::chrono::seconds(5), [&] { return gated_results.size() == 10; }),
                "The remaining glosses were never delivered.");
        require(gated_deliveries >= 2, "Every gloss arrived in a single delivery instead of incrementally.");
    }
    gated_service->stop();

    std::mutex callback_mutex;
    std::condition_variable callback_changed;
    std::vector<std::pair<std::string, std::string>> results;
    auto service = std::make_unique<TranslationService>(
        std::make_shared<TranslationProvider>(metasequoia::path_to_utf8(dictionary), transport),
        [&](std::uint64_t generation, std::vector<std::pair<std::string, std::string>> values) {
            std::lock_guard lock(callback_mutex);
            if (generation == 2)
                results = std::move(values);
            callback_changed.notify_all();
        });
    TranslationRequest request;
    request.generation = 2;
    request.candidates = {"hello"};
    request.config.enabled = true;
    request.config.target_language = "en";
    service->submit(std::move(request));
    std::unique_lock callback_lock(callback_mutex);
    require(callback_changed.wait_for(callback_lock, std::chrono::seconds(2), [&] { return !results.empty(); }),
            "Translation service did not deliver a current generation.");
    require(results.front().second == "你好", "Translation service changed the local gloss.");
    service->stop();
    std::filesystem::remove(dictionary);
    return 0;
}
