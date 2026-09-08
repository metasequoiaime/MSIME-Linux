#include "account/AccountSession.h"
#include <boost/json.hpp>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace metasequoia::linux_ime;
namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct Store final : SecretStore
{
    SecretLookupResult record;
    bool available = true;
    SecretLookupResult lookup(SecretKind, std::string_view) const override
    {
        return available ? record : SecretLookupResult{SecretStatus::Unavailable, {}, {}};
    }
    bool store(SecretKind, std::string_view, std::string_view value, std::string *) override
    {
        if (!available)
            return false;
        record = {SecretStatus::Found, std::string(value), {}};
        return true;
    }
    bool erase(SecretKind, std::string_view, std::string *) override
    {
        if (!available)
            return false;
        record = {};
        return true;
    }
};
struct Transport final : online::HttpTransport
{
    int calls = 0;
    online::HttpRequest last;
    online::HttpResponse response;
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &) override
    {
        last = request;
        ++calls;
        return response;
    }
    void tokens(const char *id, char access = 'c')
    {
        response = {200,
                    boost::json::serialize(boost::json::object{
                        {"access_token", std::string(64, access)},
                        {"refresh_token", std::string(64, 'd')},
                        {"token_type", "Bearer"},
                        {"expires_in", 900},
                        {"user", boost::json::object{{"id", id}, {"display_name", "测试"}, {"created_at", "now"}}}}),
                    {}};
    }
};
template <typename Action> void fails(Action action)
{
    try
    {
        action();
    }
    catch (const account::Failure &)
    {
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}
} // namespace
int main()
{
    Store secrets;
    account::AccountCredentialStore storage(secrets);
    Transport transport;
    account::BackendAccountClient client(transport);
    std::int64_t now = 1000;
    account::AccountSession session(client, storage, [&] { return now; });
    secrets.available = false;
    fails([&] { session.restore(); });
    secrets.available = true;
    const auto initial = session.restore();
    require(!initial.user && initial.generation != 0, "restore did not initialize");
    transport.tokens("one");
    secrets.available = false;
    fails([&] { session.login(initial.generation, "challenge", "credential"); });
    require(!session.snapshot().user, "failed persistence published login");
    secrets.available = true;
    auto logged = session.login(initial.generation, "challenge", "credential");
    require(logged.user->id == "one" && logged.generation != initial.generation, "login missing generation change");
    transport.response = {201, R"({"challenge_id":"link-challenge","expires_in":300})", {}};
    const auto challenge = session.begin_link(logged.generation, "email", "synthetic@example.invalid");
    require(boost::json::parse(transport.last.body).at("purpose").as_string() == "link", "binding purpose missing");
    require(transport.last.headers.back() == "Authorization: Bearer " + std::string(64, 'c'),
            "binding authorization missing");
    transport.tokens("wrong-account");
    fails([&] { session.link(logged.generation, challenge.id, "123456"); });
    require(session.snapshot().user->id == "one", "binding switched account");
    transport.tokens("one");
    secrets.available = false;
    fails([&] { session.link(logged.generation, challenge.id, "123456"); });
    require(session.snapshot().generation == logged.generation, "failed binding save published session");
    secrets.available = true;
    const auto before_link = logged.generation;
    logged = session.link(logged.generation, challenge.id, "123456");
    require(logged.generation != before_link && logged.user->id == "one", "binding did not rotate generation");
    const auto calls_after_link = transport.calls;
    fails([&] { session.link(before_link, challenge.id, "123456"); });
    require(transport.calls == calls_after_link, "stale binding reached network");
    transport.response = {
        200,
        R"({"user":{"id":"one","display_name":"更新昵称","created_at":"now"},"identities":[{"provider":"email","subject":"synthetic@example.invalid"}]})",
        {}};
    require(session.profile(logged.generation).identities.size() == 1, "profile identities missing");
    require(storage.load()->tokens.user.display_name == "更新昵称", "profile cache not saved");
    require(session.rename(logged.generation, "更新昵称").user.display_name == "更新昵称", "rename profile missing");
    transport.response = {
        200, R"({"user":{"id":"different-user","display_name":"错误账号","created_at":"now"},"identities":[]})", {}};
    fails([&] { session.profile(logged.generation); });
    require(storage.load()->tokens.user.id == "one", "profile switched account");
    const int before_stale = transport.calls;
    fails([&] { session.login(initial.generation, "stale", "credential"); });
    fails([&] { session.access_token(initial.generation); });
    fails([&] { session.profile(initial.generation); });
    fails([&] { session.clipboard(initial.generation); });
    fails([&] { session.add_clipboard(initial.generation, "旧账号不得上传"); });
    fails([&] { session.delete_clipboard(initial.generation, {}); });
    require(transport.calls == before_stale, "stale operation reached network");
    require(session.restore().generation == logged.generation, "restore replaced live session");
    now = 1900;
    transport.tokens("one", 'e');
    std::vector<std::future<std::string>> futures;
    const int before_refresh = transport.calls;
    for (int i = 0; i < 8; ++i)
        futures.push_back(std::async(std::launch::async, [&] { return session.access_token(logged.generation); }));
    for (auto &future : futures)
        require(future.get() == std::string(64, 'e'), "wrong refreshed token");
    require(transport.calls == before_refresh + 1, "concurrent refresh duplicated request");
    require(storage.load()->tokens.access_token == std::string(64, 'e'), "refresh not persisted");
    transport.response = {503, {}, {}};
    fails([&] { session.logout(logged.generation); });
    require(session.snapshot().user->id == "one", "logout failure lost session");
    now = 2800;
    fails([&] { session.access_token(logged.generation); });
    require(session.snapshot().user->id == "one", "temporary refresh failure lost session");
    transport.tokens("wrong-account");
    fails([&] { session.access_token(logged.generation); });
    require(storage.load()->tokens.user.id == "one", "refresh switched account");
    transport.tokens("one");
    secrets.available = false;
    fails([&] { session.access_token(logged.generation); });
    secrets.available = true;
    require(storage.load()->tokens.access_token == std::string(64, 'e'), "failed refresh save changed record");
    transport.response = {401, {}, {}};
    fails([&] { session.access_token(logged.generation); });
    require(!session.snapshot().user && !storage.load(), "revoked refresh credential survived");
    transport.tokens("two");
    const auto second = session.login(session.snapshot().generation, "challenge", "credential");
    const int before_cancel = transport.calls;
    fails([&] { session.delete_account(second.generation, [] { return true; }); });
    require(transport.calls == before_cancel && session.snapshot().user, "cancel changed session");
    transport.response = {204, {}, {}};
    secrets.available = false;
    fails([&] { session.delete_account(second.generation); });
    require(!session.snapshot().user, "revoked account kept usable after keyring failure");
    secrets.available = true;
    storage.clear();
    std::cout << "account session lifecycle tests passed\n";
}
