#include "../src/online/TranslationProvider.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using metasequoia::linux_ime::online::CancellationCheck;
using metasequoia::linux_ime::online::HttpMethod;
using metasequoia::linux_ime::online::HttpRequest;
using metasequoia::linux_ime::online::HttpResponse;
using metasequoia::linux_ime::online::HttpTransport;
using metasequoia::linux_ime::online::TranslationProvider;
using metasequoia::linux_ime::online::TranslationRequest;
using metasequoia::linux_ime::online::TranslationService;
using metasequoia::linux_ime::online::TranslationBackend;

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

    std::mutex mutex;
    std::condition_variable changed;
    HttpRequest last_request;
    int calls = 0;
};

std::filesystem::path make_dictionary()
{
    const auto path = std::filesystem::temp_directory_path() / "metasequoia-translation-test.db";
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
