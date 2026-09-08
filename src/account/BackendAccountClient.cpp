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
void require_text(const std::string &text, std::size_t byte_limit)
{
    if (text.size() > byte_limit || text.find('\0') != std::string::npos)
        throw Failure(400);
    boost::system::error_code error;
    (void)boost::json::parse(boost::json::serialize(boost::json::value(text)), error);
    if (error)
        throw Failure(400);
}
void require_clipboard_text(const std::string &text)
{
    require_text(text, 16000);
    std::size_t units = 0;
    for (unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            units += c >= 0xf0 ? 2 : 1;
    if (units > 4000 || text.find_first_not_of(" \t\r\n") == std::string::npos)
        throw Failure(400);
}
ClipboardItem clipboard_item(const boost::json::object &source)
{
    ClipboardItem result{string(source, "id", 64), string(source, "text", 16000), string(source, "updated_at", 128)};
    if (!valid_token(result.id))
        throw Failure(0);
    require_clipboard_text(result.text);
    return result;
}
std::string query_component(const std::string &text)
{
    std::string result;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : text)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            result += static_cast<char>(c);
        else
        {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
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

Tokens BackendAccountClient::decode_tokens(const std::string &text)
{
    if (text.size() > 1024 * 1024)
    {
        throw Failure(0);
    }
    return tokens(text);
}

Failure::Failure(long status, bool cancelled)
    : std::runtime_error(cancelled ? "操作已取消" : "账号请求未完成，请稍后重试"), status_(status),
      cancelled_(cancelled)
{
}

std::string BackendAccountClient::request(online::HttpMethod method, const char *path, const std::string &body,
                                          const std::string &token, const online::CancellationCheck &cancelled,
                                          std::size_t response_limit)
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
    request.max_response_bytes = response_limit;
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
ClipboardSnapshot BackendAccountClient::clipboard(const std::string &token, const std::string &query,
                                                  const online::CancellationCheck &cancelled)
{
    require_token(token);
    require_text(query, 1024);
    const auto path = "/v1/users/me/clipboard?q=" + query_component(query);
    const auto source = object(request(online::HttpMethod::Get, path.c_str(), {}, token, cancelled, 2 * 1024 * 1024));
    const auto *enabled = source.if_contains("enabled");
    const auto *items = source.if_contains("items");
    if (!enabled || !enabled->is_bool() || !items || !items->is_array() || items->as_array().size() > 50)
        throw Failure(0);
    ClipboardSnapshot result{enabled->as_bool(), {}};
    for (const auto &value : items->as_array())
    {
        if (!value.is_object())
            throw Failure(0);
        result.items.push_back(clipboard_item(value.as_object()));
    }
    return result;
}
void BackendAccountClient::set_clipboard_enabled(bool enabled, const std::string &token,
                                                 const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto source =
        object(request(online::HttpMethod::Put, "/v1/users/me/clipboard/settings",
                       boost::json::serialize(boost::json::object{{"enabled", enabled}}), token, cancelled));
    const auto *value = source.if_contains("enabled");
    if (!value || !value->is_bool() || value->as_bool() != enabled)
        throw Failure(0);
}
ClipboardItem BackendAccountClient::add_clipboard(const std::string &text, const std::string &token,
                                                  const online::CancellationCheck &cancelled)
{
    require_token(token);
    require_clipboard_text(text);
    return clipboard_item(
        object(request(online::HttpMethod::Post, "/v1/users/me/clipboard",
                       boost::json::serialize(boost::json::object{{"text", text}}), token, cancelled)));
}
void BackendAccountClient::delete_clipboard(const std::string &id, const std::string &token,
                                            const online::CancellationCheck &cancelled)
{
    require_token(token);
    if (!id.empty())
        require_token(id);
    const auto path = std::string("/v1/users/me/clipboard") + (id.empty() ? "" : "/" + id);
    (void)request(online::HttpMethod::Delete, path.c_str(), {}, token, cancelled);
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
