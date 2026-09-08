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
    std::cout << "Four dictionary CRUD, pagination and validation tests passed\n";
}
