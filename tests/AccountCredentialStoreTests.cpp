#include "account/AccountCredentialStore.h"
#include <boost/json.hpp>
#include <iostream>
#include <stdexcept>

using namespace metasequoia::linux_ime;
namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
struct Store final : SecretStore
{
    SecretLookupResult record;
    bool available = true;
    int writes = 0;
    static void check(SecretKind kind, std::string_view provider)
    {
        require(kind == SecretKind::AccountSession && provider == "msime", "wrong credential namespace");
    }
    SecretLookupResult lookup(SecretKind kind, std::string_view provider) const override
    {
        check(kind, provider);
        return available ? record : SecretLookupResult{SecretStatus::Unavailable, {}, "private service detail"};
    }
    bool store(SecretKind kind, std::string_view provider, std::string_view text, std::string *) override
    {
        check(kind, provider);
        ++writes;
        if (!available)
            return false;
        record = {SecretStatus::Found, std::string(text), {}};
        return true;
    }
    bool erase(SecretKind kind, std::string_view provider, std::string *) override
    {
        check(kind, provider);
        if (!available)
            return false;
        record = {};
        return true;
    }
};
template <typename Action> void fails(Action action)
{
    try
    {
        action();
    }
    catch (const account::Failure &error)
    {
        require(std::string(error.what()).find("private") == std::string::npos, "diagnostic leaked");
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}
} // namespace
int main()
{
    Store secrets;
    account::AccountCredentialStore store(secrets);
    require(!store.load(), "missing session not distinguished");
    const account::SavedSession first{
        {std::string(64, 'a'), std::string(64, 'b'), 900, {"first", "合成用户", "2026-01-01T00:00:00Z"}}, 1000};
    store.save(first);
    account::AccountCredentialStore reopened(secrets);
    const auto restored = reopened.load();
    require(restored && restored->expires_at == 1000 && restored->tokens.access_token == first.tokens.access_token &&
                restored->tokens.refresh_token == first.tokens.refresh_token && restored->tokens.user.id == "first" &&
                restored->tokens.user.display_name == "合成用户",
            "expired session failed to round trip");
    const auto original = secrets.record;
    auto second = first;
    second.tokens.user.id = "second";
    second.tokens.access_token = std::string(64, 'c');
    second.tokens.refresh_token = std::string(64, 'd');
    secrets.available = false;
    fails([&] { store.load(); });
    fails([&] { store.save(second); });
    fails([&] { store.clear(); });
    require(secrets.record.value == original.value, "failed persistence changed record");
    secrets.available = true;
    store.save(second);
    require(store.load()->tokens.user.id == "second" &&
                store.load()->tokens.refresh_token == second.tokens.refresh_token,
            "account replacement mixed credentials");
    const int writes = secrets.writes;
    second.tokens.access_token = "invalid";
    fails([&] { store.save(second); });
    require(secrets.writes == writes, "invalid credentials reached storage");
    for (const char *field : {"version", "expires_at", "refresh_token", "user"})
    {
        auto corrupted = boost::json::parse(original.value).as_object();
        corrupted.erase(field);
        secrets.record.value = boost::json::serialize(corrupted);
        fails([&] { store.load(); });
        require(secrets.record.status == SecretStatus::Found, "corrupt record silently erased");
    }
    secrets.record.value = std::string(4097, 'x');
    fails([&] { store.load(); });
    store.clear();
    store.clear();
    require(!store.load(), "clear failed");
    std::cout << "account credential persistence tests passed\n";
}
