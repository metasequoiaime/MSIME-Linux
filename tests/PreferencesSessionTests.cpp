#include "account/AccountSession.h"
#include <boost/json.hpp>
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
    SecretLookupResult lookup(SecretKind, std::string_view) const override
    {
        return record;
    }
    bool store(SecretKind, std::string_view, std::string_view value, std::string *) override
    {
        record = {SecretStatus::Found, std::string(value), {}};
        return true;
    }
    bool erase(SecretKind, std::string_view, std::string *) override
    {
        record = {};
        return true;
    }
};
struct Transport final : online::HttpTransport
{
    std::int64_t revision = 3;
    bool rejected = false;
    int requests = 0;
    boost::json::object settings{{"general.cloud_candidates", true}, {"platform.ios.custom_keyboard_skin", "preserve"}};
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &) override
    {
        ++requests;
        require(request.headers.back() == "Authorization: Bearer " + std::string(64, 'a'),
                "missing user authorization");
        if (rejected)
            return {401, "{}", {}};
        if (request.url.find("/schema") != std::string::npos)
            return {
                200,
                R"({"fields":{"general.cloud_candidates":{"type":"boolean"},"platform.ios.custom_keyboard_skin":{"type":"string","maxLength":786432}},"maximum_bytes":1048576,"update_mode":"replace","revision_required":true})",
                {}};
        if (request.method == online::HttpMethod::Put)
        {
            const auto body = boost::json::parse(request.body);
            if (body.at("revision").as_int64() != revision)
                return {409, "{}", {}};
            settings = body.at("settings").as_object();
            ++revision;
        }
        return {200, boost::json::serialize(boost::json::object{{"revision", revision}, {"settings", settings}}), {}};
    }
};
template <typename Action> void fails(Action action, long status)
{
    try
    {
        action();
    }
    catch (const account::Failure &error)
    {
        require(error.status() == status, "wrong failure status");
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}
} // namespace
int main()
{
    Store secrets;
    account::AccountCredentialStore storage(secrets);
    storage.save({{std::string(64, 'a'), std::string(64, 'b'), 900, {"alpha", "合成用户", "now"}}, 5000});
    Transport transport;
    account::BackendAccountClient client(transport);
    account::AccountSession session(client, storage, [] { return 1000; });
    const auto account = session.restore();
    auto review = session.review_preferences(account.generation);
    require(review.user_id == "alpha" && review.remote.revision == 3, "review identity/revision missing");
    const auto uploaded = session.upload_preferences(review, {{"general.cloud_candidates", false}});
    require(uploaded.revision == 4 &&
                transport.settings.at("platform.ios.custom_keyboard_skin").as_string() == "preserve",
            "upload did not preserve other platform fields");
    const auto before_conflict = transport.requests;
    fails([&] { session.upload_preferences(review, {{"general.cloud_candidates", true}}); }, 409);
    require(transport.requests == before_conflict + 1, "conflict automatically retried");
    bool applied = false;
    fails([&] { session.apply_preferences(review, [&] { applied = true; }); }, 409);
    require(!applied, "stale review applied locally");
    review = session.review_preferences(account.generation);
    transport.settings["general.cloud_candidates"] = true; // A broken server that forgot to increment revision.
    fails([&] { session.apply_preferences(review, [&] { applied = true; }); }, 409);
    require(!applied, "changed contents with same revision applied");
    review = session.review_preferences(account.generation);
    int apply_calls = 0;
    try
    {
        session.apply_preferences(review, [&] {
            ++apply_calls;
            throw std::runtime_error("synthetic write failure");
        });
    }
    catch (const std::runtime_error &)
    {
    }
    require(apply_calls == 1 && session.snapshot().user->id == "alpha",
            "failed local write retried or changed account");
    session.apply_preferences(review, [&] { applied = true; });
    require(applied, "valid review not applied");
    auto wrong_user = review;
    wrong_user.user_id = "beta";
    const auto before_stale = transport.requests;
    fails([&] { session.upload_preferences(wrong_user, {}); }, 0);
    fails([&] { session.apply_preferences(wrong_user, [] {}); }, 0);
    fails([&] { session.apply_preferences(review, [] {}, [] { return true; }); }, 0);
    require(transport.requests == before_stale, "wrong account or cancelled review reached network");
    transport.rejected = true;
    fails([&] { session.review_preferences(account.generation); }, 401);
    require(!session.snapshot().user && !storage.load(), "401 retained session");
    const auto after_revoke = transport.requests;
    fails([&] { session.upload_preferences(review, {}); }, 0);
    require(transport.requests == after_revoke, "revoked account review reached network");
    std::cout << "preferences review, conflict and account isolation tests passed\n";
}
