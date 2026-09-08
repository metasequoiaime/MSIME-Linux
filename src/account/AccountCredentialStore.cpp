#include "AccountCredentialStore.h"
#include <boost/json.hpp>

namespace metasequoia::linux_ime::account
{
namespace
{
constexpr const char *provider = "msime";
constexpr std::size_t maximum_bytes = 4096;

SavedSession decode(const std::string &text)
{
    if (text.empty() || text.size() > maximum_bytes)
    {
        throw Failure(0);
    }
    boost::system::error_code error;
    const auto value = boost::json::parse(text, error);
    if (error || !value.is_object())
    {
        throw Failure(0);
    }
    const auto *version = value.as_object().if_contains("version");
    const auto *expiry = value.as_object().if_contains("expires_at");
    if (!version || !version->is_int64() || version->as_int64() != 1 || !expiry || !expiry->is_int64() ||
        expiry->as_int64() <= 0 || expiry->as_int64() > 253402300799LL)
    {
        throw Failure(0);
    }
    return {BackendAccountClient::decode_tokens(text), expiry->as_int64()};
}
} // namespace

std::optional<SavedSession> AccountCredentialStore::load() const
{
    const auto result = store_.lookup(SecretKind::AccountSession, provider);
    if (result.status == SecretStatus::NotFound)
    {
        return std::nullopt;
    }
    if (result.status != SecretStatus::Found)
    {
        throw Failure(0);
    }
    // Keep expired access tokens: their refresh token can still restore the session.
    return decode(result.value);
}

void AccountCredentialStore::save(const SavedSession &session)
{
    const auto &tokens = session.tokens;
    const auto text = boost::json::serialize(
        boost::json::object{{"version", 1},
                            {"expires_at", session.expires_at},
                            {"token_type", "Bearer"},
                            {"access_token", tokens.access_token},
                            {"refresh_token", tokens.refresh_token},
                            {"expires_in", tokens.expires_in},
                            {"user", boost::json::object{{"id", tokens.user.id},
                                                         {"display_name", tokens.user.display_name},
                                                         {"created_at", tokens.user.created_at}}}});
    (void)decode(text);
    // One item holds the entire session; a reader cannot mix tokens from two accounts.
    if (!store_.store(SecretKind::AccountSession, provider, text))
    {
        throw Failure(0);
    }
}

void AccountCredentialStore::clear()
{
    if (!store_.erase(SecretKind::AccountSession, provider))
    {
        throw Failure(0);
    }
}
} // namespace metasequoia::linux_ime::account
