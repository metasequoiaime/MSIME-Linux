#include "BackendAccountClient.h"

#include <boost/json.hpp>
#include <algorithm>
#include <array>

namespace metasequoia::linux_ime::account
{
namespace
{
const std::array<const char *, 5> providers_list{"apple", "google", "wechat", "phone", "email"};
bool valid_token(const std::string &token)
{
    return token.size() == 64 && std::all_of(token.begin(), token.end(),
                                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
boost::json::object object(const std::string &text)
{
    boost::system::error_code error;
    auto value = boost::json::parse(text, error);
    if (error || !value.is_object())
    {
        throw Failure(0);
    }
    return std::move(value.as_object());
}
std::string string(const boost::json::object &value, const char *key, std::size_t limit, bool optional = false)
{
    const auto *item = value.if_contains(key);
    if (optional && (item == nullptr || item->is_null()))
    {
        return {};
    }
    if (item == nullptr || !item->is_string() || item->as_string().size() > limit ||
        item->as_string().find('\0') != boost::json::string::npos)
    {
        throw Failure(0);
    }
    return std::string(item->as_string());
}
int lifetime(const boost::json::object &value)
{
    const auto *item = value.if_contains("expires_in");
    if (item == nullptr || !item->is_int64() || item->as_int64() <= 0 || item->as_int64() > 86400)
    {
        throw Failure(0);
    }
    return static_cast<int>(item->as_int64());
}
User user(const boost::json::object &value)
{
    const auto *item = value.if_contains("user");
    if (item == nullptr || !item->is_object())
    {
        throw Failure(0);
    }
    const auto &source = item->as_object();
    User result{string(source, "id", 128), string(source, "display_name", 512), string(source, "created_at", 128)};
    if (result.id.empty())
    {
        throw Failure(0);
    }
    return result;
}
Tokens tokens(const std::string &text)
{
    const auto source = object(text);
    Tokens result{string(source, "access_token", 64), string(source, "refresh_token", 64), lifetime(source),
                  user(source)};
    if (!valid_token(result.access_token) || !valid_token(result.refresh_token) ||
        string(source, "token_type", 16) != "Bearer")
    {
        throw Failure(0);
    }
    return result;
}
void require_token(const std::string &token)
{
    if (!valid_token(token))
    {
        throw Failure(400);
    }
}
} // namespace

Failure::Failure(long status, bool cancelled)
    : std::runtime_error(cancelled ? "操作已取消" : "账号请求未完成，请稍后重试"), status_(status),
      cancelled_(cancelled)
{
}

std::string BackendAccountClient::request(online::HttpMethod method, const char *path, const std::string &body,
                                          const std::string &token, const online::CancellationCheck &cancelled)
{
    if (!token.empty())
    {
        require_token(token);
    }
    if (body.size() > 65536)
    {
        throw Failure(400);
    }
    if (cancelled && cancelled())
    {
        throw Failure(0, true);
    }
    online::HttpRequest request;
    request.method = method;
    request.url = std::string("https://api.msime.app") + path;
    request.body = body;
    request.total_timeout = std::chrono::seconds(30);
    request.max_response_bytes = 1024 * 1024;
    request.headers = {"Accept: application/json", "Cache-Control: no-store"};
    if (!body.empty())
    {
        request.headers.emplace_back("Content-Type: application/json");
    }
    if (!token.empty())
    {
        request.headers.emplace_back("Authorization: Bearer " + token);
    }
    auto response = transport_.perform(request, cancelled);
    if (cancelled && cancelled())
    {
        throw Failure(0, true);
    }
    if (!response.error.empty() || response.body.size() > request.max_response_bytes)
    {
        throw Failure(0);
    }
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw Failure(response.status_code);
    }
    return response.body;
}

std::map<std::string, bool> BackendAccountClient::providers(const online::CancellationCheck &cancelled)
{
    const auto response = object(request(online::HttpMethod::Get, "/v1/auth/providers", {}, {}, cancelled));
    const auto *value = response.if_contains("providers");
    if (value == nullptr || !value->is_object() || value->as_object().size() > 16)
    {
        throw Failure(0);
    }
    std::map<std::string, bool> result;
    for (const char *provider : providers_list)
    {
        const auto *enabled = value->as_object().if_contains(provider);
        if (enabled == nullptr || !enabled->is_bool())
        {
            throw Failure(0);
        }
        result.emplace(provider, enabled->as_bool());
    }
    return result;
}
Challenge BackendAccountClient::challenge(const std::string &provider, const std::string &target,
                                          const std::string &link_token, const online::CancellationCheck &cancelled)
{
    if (std::find(providers_list.begin(), providers_list.end(), provider) == providers_list.end() ||
        target.size() > 512)
    {
        throw Failure(400);
    }
    const auto response = object(
        request(online::HttpMethod::Post, "/v1/auth/challenges",
                boost::json::serialize(boost::json::object{
                    {"provider", provider}, {"target", target}, {"purpose", link_token.empty() ? "login" : "link"}}),
                link_token, cancelled));
    Challenge result{string(response, "challenge_id", 128), lifetime(response), string(response, "nonce", 256, true),
                     string(response, "authorization_url", 4096, true)};
    if (result.id.empty())
    {
        throw Failure(0);
    }
    return result;
}
Tokens BackendAccountClient::login(const std::string &challenge_id, const std::string &credential,
                                   const std::string &link_token, const online::CancellationCheck &cancelled)
{
    if (challenge_id.empty() || challenge_id.size() > 128 || credential.empty() || credential.size() > 16384)
    {
        throw Failure(400);
    }
    return tokens(
        request(online::HttpMethod::Post, "/v1/auth/login",
                boost::json::serialize(boost::json::object{{"challenge_id", challenge_id}, {"credential", credential}}),
                link_token, cancelled));
}
Tokens BackendAccountClient::refresh(const std::string &refresh_token, const online::CancellationCheck &cancelled)
{
    require_token(refresh_token);
    return tokens(request(online::HttpMethod::Post, "/v1/auth/refresh",
                          boost::json::serialize(boost::json::object{{"refresh_token", refresh_token}}), {},
                          cancelled));
}
Profile BackendAccountClient::profile(const std::string &token, const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto response = object(request(online::HttpMethod::Get, "/v1/users/me", {}, token, cancelled));
    Profile result{user(response), {}};
    const auto *identities = response.if_contains("identities");
    if (identities == nullptr || !identities->is_array() || identities->as_array().size() > 32)
    {
        throw Failure(0);
    }
    for (const auto &identity : identities->as_array())
    {
        if (!identity.is_object())
        {
            throw Failure(0);
        }
        result.identities.push_back(
            {string(identity.as_object(), "provider", 32), string(identity.as_object(), "subject", 512)});
    }
    return result;
}
void BackendAccountClient::rename(const std::string &name, const std::string &token,
                                  const online::CancellationCheck &cancelled)
{
    require_token(token);
    if (name.empty() || name.size() > 128)
    {
        throw Failure(400);
    }
    (void)request(online::HttpMethod::Patch, "/v1/users/me",
                  boost::json::serialize(boost::json::object{{"display_name", name}}), token, cancelled);
}
void BackendAccountClient::logout(const std::string &token, bool all, const online::CancellationCheck &cancelled)
{
    require_token(token);
    (void)request(online::HttpMethod::Post, "/v1/auth/logout",
                  boost::json::serialize(boost::json::object{{"all", all}}), token, cancelled);
}
void BackendAccountClient::delete_account(const std::string &token, const online::CancellationCheck &cancelled)
{
    require_token(token);
    (void)request(online::HttpMethod::Delete, "/v1/users/me", {}, token, cancelled);
}
} // namespace metasequoia::linux_ime::account
