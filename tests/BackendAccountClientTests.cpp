#include "account/BackendAccountClient.h"
#include <boost/json.hpp>
#include <iostream>
#include <cstdlib>

using namespace metasequoia::linux_ime;
namespace
{
void require(bool value, const char *message)
{
    if (!value)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct FakeTransport final : online::HttpTransport
{
    online::HttpResponse response{200, "{}", {}};
    online::HttpRequest last;
    int calls = 0;
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &) override
    {
        last = request;
        ++calls;
        return response;
    }
};
std::string token(char c)
{
    return std::string(64, c);
}
std::string valid_tokens()
{
    return boost::json::serialize(boost::json::object{
        {"access_token", token('a')},
        {"refresh_token", token('b')},
        {"token_type", "Bearer"},
        {"expires_in", 900},
        {"user", boost::json::object{
                     {"id", "synthetic-user"}, {"display_name", "合成测试"}, {"created_at", "2026-01-01T00:00:00Z"}}}});
}
template <typename Action> void fails(Action action, long status)
{
    try
    {
        action();
        require(false, "request unexpectedly succeeded");
    }
    catch (const account::Failure &error)
    {
        require(error.status() == status, "wrong error status");
    }
}
} // namespace
int main()
{
    FakeTransport transport;
    account::BackendAccountClient client(transport);
    transport.response.body =
        R"({"providers":{"apple":true,"google":false,"wechat":false,"email":false,"phone":false}})";
    require(client.providers().at("apple"), "missing provider");
    require(transport.last.url == "https://api.msime.app/v1/auth/providers", "origin not fixed");
    require(transport.last.max_response_bytes == 1024 * 1024, "response not bounded");
    transport.response.body = R"({"challenge_id":"synthetic-challenge","expires_in":300,"nonce":"synthetic-nonce"})";
    require(client.challenge("apple").nonce == "synthetic-nonce", "missing nonce");
    require(boost::json::parse(transport.last.body).at("purpose").as_string() == "login", "wrong login purpose");
    (void)client.challenge("email", "synthetic@example.invalid", token('a'));
    require(boost::json::parse(transport.last.body).at("purpose").as_string() == "link", "wrong link purpose");
    require(transport.last.headers.back() == "Authorization: Bearer " + token('a'), "link credential missing");
    transport.response.body = valid_tokens();
    const auto session = client.login("synthetic-challenge", "synthetic-credential");
    require(session.user.id == "synthetic-user" && session.expires_in == 900, "bad session decoding");
    require(client.refresh(token('b')).access_token == token('a'), "bad refreshed token");
    require(boost::json::parse(transport.last.body).at("refresh_token").as_string() == token('b'),
            "wrong refresh body");
    transport.response.body =
        R"({"user":{"id":"synthetic-user","display_name":"测试","created_at":"now"},"identities":[{"provider":"email","subject":"synthetic@example.invalid"}]})";
    require(client.profile(token('a')).identities.size() == 1, "bad profile decoding");
    transport.response = {204, {}, {}};
    client.rename("新的合成名称", token('a'));
    require(transport.last.method == online::HttpMethod::Patch &&
                boost::json::parse(transport.last.body).at("display_name").as_string() == "新的合成名称",
            "rename not PATCH");
    client.logout(token('a'), true);
    require(boost::json::parse(transport.last.body).at("all").as_bool(), "logout all missing");
    client.delete_account(token('a'));
    require(transport.last.method == online::HttpMethod::Delete, "delete not DELETE");
    const int sent = transport.calls;
    fails([&] { client.profile("token\r\nInjected: header"); }, 400);
    fails([&] { client.challenge("unsupported"); }, 400);
    require(transport.calls == sent, "invalid input reached network");
    try
    {
        (void)client.providers([] { return true; });
        require(false, "cancel ignored");
    }
    catch (const account::Failure &error)
    {
        require(error.cancelled(), "cancel not distinguished");
    }
    require(transport.calls == sent, "cancelled request sent");
    transport.response = {401, "private upstream text", {}};
    try
    {
        (void)client.profile(token('a'));
        require(false, "401 accepted");
    }
    catch (const account::Failure &error)
    {
        require(error.status() == 401 && std::string(error.what()).find("private") == std::string::npos,
                "upstream detail leaked");
    }
    transport.response = {200, std::string(1024 * 1024 + 1, 'x'), {}};
    fails([&] { client.providers(); }, 0);
    for (const auto &body :
         {std::string("{}"), std::string("not JSON"), std::string("{\"providers\":{\"apple\":\"true\"}}")})
    {
        transport.response = {200, body, {}};
        fails([&] { client.providers(); }, 0);
    }
    auto malformed = boost::json::parse(valid_tokens()).as_object();
    malformed["access_token"] = "not-a-session-token";
    transport.response = {200, boost::json::serialize(malformed), {}};
    fails([&] { client.login("synthetic-challenge", "synthetic-credential"); }, 0);
    std::cout << "backend account protocol tests passed\n";
}
