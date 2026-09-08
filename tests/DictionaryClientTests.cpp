#include "account/BackendAccountClient.h"
#include <boost/json.hpp>
#include <iostream>
#include <stdexcept>
using namespace metasequoia::linux_ime;
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
struct Transport final : online::HttpTransport
{
    int calls = 0;
    online::HttpRequest last;
    online::HttpResponse response{200, "{}", {}};
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &) override
    {
        ++calls;
        last = request;
        return response;
    }
};
template <class F> void fails(F action, long status)
{
    try
    {
        action();
    }
    catch (const account::Failure &error)
    {
        require(error.status() == status, "wrong failure");
        return;
    }
    throw std::runtime_error("unexpected success");
}
boost::json::object entry(const std::string &kind, int revision)
{
    return {{"id", std::string(64, 'c')},
            {"kind", kind},
            {"code", "ni hao"},
            {"word", "你好"},
            {"weight", 10},
            {"revision", revision},
            {"updated_at", "2026-09-09T00:00:00Z"}};
}
} // namespace
int main()
{
    Transport http;
    account::BackendAccountClient client(http);
    const std::string token(64, 'a'), id(64, 'c');
    for (const auto &kind : {"pinyin", "wubi", "english", "quick"})
    {
        auto previous = entry(kind, 2), replacement = entry(kind, 4);
        http.response.body = boost::json::serialize(
            boost::json::object{{"entries", boost::json::array{previous}}, {"has_more", true}, {"offset", 3}});
        auto page = client.dictionary(kind, "你 &+?", 3, 1, token);
        require(page.entries.size() == 1 && page.has_more && page.offset == 3, "page not decoded");
        require(http.last.url == std::string("https://api.msime.app/v1/users/me/dictionaries/") + kind +
                                     "?q=%E4%BD%A0%20%26%2B%3F&offset=3&limit=1",
                "query not encoded");
        require(http.last.headers.back() == "Authorization: Bearer " + token, "missing authorization");
        account::DictionaryEntry draft;
        draft.code = "ni hao";
        draft.word = "你好";
        http.response.body = boost::json::serialize(
            boost::json::object{{"revision", 4}, {"previous", nullptr}, {"replacement", replacement}});
        auto created = client.edit_dictionary(kind, "", 0, draft, token);
        require(created.replacement->revision == 4 && http.last.method == online::HttpMethod::Post &&
                    !boost::json::parse(http.last.body).as_object().contains("revision"),
                "create precondition wrong");
        http.response.body = boost::json::serialize(
            boost::json::object{{"revision", 4}, {"previous", previous}, {"replacement", replacement}});
        client.edit_dictionary(kind, id, 2, draft, token);
        require(http.last.method == online::HttpMethod::Put &&
                    boost::json::parse(http.last.body).at("revision").as_int64() == 2,
                "update lost entry revision");
        http.response.body = boost::json::serialize(
            boost::json::object{{"revision", 4}, {"previous", previous}, {"replacement", nullptr}});
        client.edit_dictionary(kind, id, 2, std::nullopt, token);
        require(http.last.method == online::HttpMethod::Delete && http.last.body == "{\"revision\":2}",
                "delete lost precondition");
        http.response.status_code = 409;
        const auto before = http.calls;
        fails([&] { client.edit_dictionary(kind, id, 2, draft, token); }, 409);
        require(http.calls == before + 1, "conflict retried");
        http.response.status_code = 200;
        replacement["kind"] = "other";
        http.response.body = boost::json::serialize(
            boost::json::object{{"revision", 4}, {"previous", previous}, {"replacement", replacement}});
        fails([&] { client.edit_dictionary(kind, id, 2, draft, token); }, 0);
    }
    for (const auto &kind : {"pinyin", "wubi", "english", "quick"})
    {
        boost::json::object base{{"kind", kind}, {"code", "test"}, {"word", "基础词条"}, {"weight", 8}};
        http.response.body = boost::json::serialize(boost::json::object{
            {"entries", boost::json::array{base}}, {"has_more", false}, {"offset", 0}, {"revision", 0}});
        const auto page = client.dictionary_catalog(kind, "test&+", 0, 50, token);
        require(page.entries.size() == 1 && page.entries[0].id.empty() && page.revision == 0 &&
                    http.last.url.find("/catalog?q=test%26%2B&offset=0&limit=50") != std::string::npos,
                "base catalog entry or revision was lost");
        const auto old = page.entries[0];
        auto changed = old;
        changed.weight = 42;
        auto next = base;
        next["weight"] = 42;
        http.response.body =
            boost::json::serialize(boost::json::object{{"previous", base}, {"replacement", next}, {"revision", 1}});
        client.manage_dictionary(kind, 0, old, changed, token);
        const auto body = boost::json::parse(http.last.body).as_object();
        require(http.last.method == online::HttpMethod::Post && http.last.url.find("/edit") != std::string::npos &&
                    body.at("revision").as_int64() == 0 && body.at("previous").at("code").as_string() == "test" &&
                    body.at("replacement").at("weight").as_int64() == 42,
                "managed edit did not carry catalog CAS or exact key");
        http.response.body =
            boost::json::serialize(boost::json::object{{"previous", base}, {"replacement", nullptr}, {"revision", 2}});
        client.manage_dictionary(kind, 1, old, std::nullopt, token);
        require(boost::json::parse(http.last.body).at("replacement").is_null(), "managed deletion lost null");
        http.response.status_code = 409;
        const auto calls = http.calls;
        fails([&] { client.manage_dictionary(kind, 1, old, changed, token); }, 409);
        require(http.calls == calls + 1, "managed conflict retried");
        http.response.status_code = 200;
        http.response.body = boost::json::serialize(boost::json::object{
            {"entries", boost::json::array{base, base}}, {"has_more", false}, {"offset", 0}, {"revision", 0}});
        fails([&] { client.dictionary_catalog(kind, "test", 0, 50, token); }, 0);
    }
    for (const auto *profile : {"xiaohe", "ziranma", "shoudao", "microsoft"})
    {
        http.response.body = R"({"entries":[],"has_more":false,"offset":0,"revision":0})";
        client.dictionary_catalog("pinyin", "nihc", 0, 50, token, {}, {"shuangpin", profile});
        require(http.last.url.find(std::string("&scheme=shuangpin&profile=") + profile) != std::string::npos,
                "double-pinyin options lost");
    }
    {
        const auto calls = http.calls;
        fails([&] { client.dictionary_catalog("pinyin", "", 0, 20, token); }, 400);
        fails([&] { client.dictionary_catalog("bad", "test", 0, 20, token); }, 400);
        fails([&] { client.dictionary_catalog("quick", "", -1, 20, token); }, 400);
        fails([&] { client.dictionary_catalog("english", "test", 0, 20, token, [] { return true; }); }, 0);
        fails([&] { client.dictionary_catalog("pinyin", "ni", 0, 20, token, {}, {"bad", "xiaohe"}); }, 400);
        fails([&] { client.dictionary_catalog("pinyin", "ni", 0, 20, token, {}, {"shuangpin", "bad&other=1"}); }, 400);
        require(http.calls == calls, "invalid or cancelled catalog reached transport");
    }
    const auto before = http.calls;
    fails([&] { client.dictionary("../preferences", "", 0, 20, token); }, 400);
    fails([&] { client.dictionary("pinyin", "", 0, 201, token); }, 400);
    fails([&] { client.dictionary("pinyin", std::string("\xff"), 0, 20, token); }, 400);
    fails([&] { client.dictionary("pinyin", "", -1, 20, token); }, 400);
    fails([&] { client.dictionary("pinyin", "", 0, 20, ""); }, 400);
    fails([&] { client.edit_dictionary("pinyin", id, 0, std::nullopt, token); }, 400);
    fails([&] { client.edit_dictionary("pinyin", "", 0, std::nullopt, token); }, 400);
    account::DictionaryEntry draft;
    draft.word = std::string(200, 'x');
    fails([&] { client.edit_dictionary("quick", "", 0, draft, token); }, 400);
    draft.word = "bad\nentry";
    fails([&] { client.edit_dictionary("pinyin", "", 0, draft, token); }, 400);
    require(http.calls == before, "invalid input reached transport");
    fails([&] { client.dictionary("quick", "", 0, 1, token, [] { return true; }); }, 0);
    require(http.calls == before, "cancelled request reached transport");
    auto item = entry("pinyin", 1);
    http.response.body = boost::json::serialize(
        boost::json::object{{"entries", boost::json::array{item, item}}, {"has_more", false}, {"offset", 0}});
    fails([&] { client.dictionary("pinyin", "", 0, 2, token); }, 0);
    http.response.body = R"({"entries":[],"has_more":true,"offset":0})";
    fails([&] { client.dictionary("pinyin", "", 0, 2, token); }, 0);
    for (const auto &kind : {"pinyin", "wubi", "english", "quick"})
    {
        for (const auto &format : {"standard", "windows"})
        {
            const std::string tsv =
                std::string(format) == "windows" && (std::string(kind) == "english" || std::string(kind) == "quick")
                    ? "test\tTest\t10\n"
                    : "Test\ttest\t10\n";
            http.response.body = R"({"imported":1,"revision":8})";
            const auto result = client.import_dictionary(kind, tsv, format, token);
            require(result.imported == 1 && result.revision == 8, "import result wrong");
            require(boost::json::parse(http.last.body).at("text").as_string() == tsv, "import column order changed");
            http.response.body = tsv;
            require(client.export_dictionary(kind, format, token) == tsv, "export changed bytes");
        }
    }
    const auto calls = http.calls;
    for (const auto &bad : {"", "bad", "word\tcode\t-1", "word\tcode\t1oops", "word\tcode\t1\textra"})
        fails([&] { client.import_dictionary("pinyin", bad, "standard", token); }, 400);
    fails([&] { client.import_dictionary("pinyin", "word\tcode\t1", "other", token); }, 400);
    std::string too_many;
    for (int i = 0; i < 501; ++i)
        too_many += "word\tcode\t1\n";
    fails([&] { client.import_dictionary("pinyin", too_many, "standard", token); }, 400);
    std::string escaped;
    for (int i = 0; i < 400; ++i)
        escaped += std::string(100, '"') + "\tcode\t1\n";
    fails([&] { client.import_dictionary("quick", escaped, "standard", token); }, 400);
    fails([&] { client.export_dictionary("quick", "standard&other=1", token); }, 400);
    require(http.calls == calls, "invalid import reached transport");
    http.response.body = R"({"error":"upstream failure"})";
    fails([&] { client.export_dictionary("pinyin", "standard", token); }, 0);
    http.response.body = "word\tcode\t1";
    fails([&] { client.export_dictionary("pinyin", "standard", token); }, 0);
    http.response.body = "";
    require(client.export_dictionary("quick", "windows", token).empty(), "empty export rejected");
    http.response.body = R"({"imported":2,"revision":8})";
    fails([&] { client.import_dictionary("quick", "word\tcode\t1", "standard", token); }, 0);
    http.response.body = R"({"imported":2,"revision":9})";
    auto annotated = client.import_han_dictionary("中国\n学习", 20, token);
    require(annotated.imported == 2 && annotated.revision == 9 &&
                http.last.url == "https://api.msime.app/v1/users/me/dictionaries/pinyin/import-hans",
            "wrong Han import route");
    require(boost::json::parse(http.last.body).at("text").as_string() == "中国\n学习" &&
                boost::json::parse(http.last.body).at("weight").as_int64() == 20,
            "Han import changed phrases or weight");
    const auto han_calls = http.calls;
    fails([&] { client.import_han_dictionary("", 10, token); }, 400);
    fails([&] { client.import_han_dictionary("中国", -1, token); }, 400);
    fails([&] { client.import_han_dictionary(std::string(65537, 'x'), 10, token); }, 400);
    require(http.calls == han_calls, "invalid Han import reached transport");
    http.response.body = R"({"imported":501,"revision":9})";
    fails([&] { client.import_han_dictionary("中国", 10, token); }, 0);
    http.response.status_code = 400;
    fails([&] { client.import_han_dictionary("English", 10, token); }, 400);
    require(http.calls == han_calls + 2, "Han import failure retried");
    std::cout << "Four dictionary CRUD, pagination and validation tests passed\n";
}
