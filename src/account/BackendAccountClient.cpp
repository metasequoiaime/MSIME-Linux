#include "BackendAccountClient.h"

#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <limits>

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
std::string dictionary_path(const std::string &kind)
{
    if (kind != "pinyin" && kind != "wubi" && kind != "english" && kind != "quick")
        throw Failure(400);
    return "/v1/users/me/dictionaries/" + kind;
}
void require_dictionary_entry(const DictionaryEntry &entry, const std::string &kind)
{
    require_text(entry.code, 512);
    require_text(entry.word, 2048);
    if (entry.weight < 0 || entry.word.find_first_not_of(" \t\r\n") == std::string::npos ||
        entry.word.find_first_of("\t\r\n") != std::string::npos ||
        entry.code.find_first_of("\t\r\n") != std::string::npos)
        throw Failure(400);
    if (kind == "quick")
    {
        std::size_t units = 0;
        for (unsigned char c : entry.word)
            if ((c & 0xc0) != 0x80)
                units += c >= 0xf0 ? 2 : 1;
        if (units > 199)
            throw Failure(400);
    }
}
void dictionary_format(const std::string &format)
{
    if (format != "standard" && format != "windows")
        throw Failure(400);
}
std::size_t dictionary_tsv(const std::string &text, const std::string &kind, const std::string &format,
                           std::size_t maximum, bool exported)
{
    std::size_t count = 0, start = text.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    while (start < text.size())
    {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
        {
            if (exported)
                throw Failure(400);
            end = text.size();
        }
        auto size = end - start;
        if (size && text[start + size - 1] == '\r')
            --size;
        if (size)
        {
            if (++count > maximum || size > 2624)
                throw Failure(400);
            const auto line = text.substr(start, size);
            const auto first = line.find('\t');
            const auto second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
            if (second == std::string::npos || line.find('\t', second + 1) != std::string::npos)
                throw Failure(400);
            DictionaryEntry entry;
            entry.word = line.substr(0, first);
            entry.code = line.substr(first + 1, second - first - 1);
            if (format == "windows" && (kind == "english" || kind == "quick"))
                std::swap(entry.word, entry.code);
            auto weight = line.substr(second + 1);
            const auto begin = weight.find_first_not_of(" \r");
            const auto last = weight.find_last_not_of(" \r");
            if (begin == std::string::npos)
                throw Failure(400);
            weight = weight.substr(begin, last - begin + 1);
            const auto parsed = std::from_chars(weight.data(), weight.data() + weight.size(), entry.weight);
            if (parsed.ec != std::errc{} || parsed.ptr != weight.data() + weight.size())
                throw Failure(400);
            require_dictionary_entry(entry, kind);
        }
        start = end == text.size() ? end : end + 1;
    }
    return count;
}
std::int64_t dictionary_number(const boost::json::object &source, const char *key, std::int64_t minimum)
{
    const auto *value = source.if_contains(key);
    if (!value || !value->is_int64() || value->as_int64() < minimum)
        throw Failure(0);
    return value->as_int64();
}
DictionaryEntry dictionary_entry(const boost::json::object &source, const std::string &kind)
{
    DictionaryEntry entry{string(source, "id", 64),
                          string(source, "kind", 16),
                          string(source, "code", 512),
                          string(source, "word", 2048),
                          dictionary_number(source, "weight", 0),
                          dictionary_number(source, "revision", 1),
                          string(source, "updated_at", 128)};
    if (!valid_token(entry.id) || entry.kind != kind || entry.updated_at.empty())
        throw Failure(0);
    try
    {
        require_dictionary_entry(entry, kind);
    }
    catch (const Failure &)
    {
        throw Failure(0);
    }
    return entry;
}
std::optional<DictionaryEntry> optional_dictionary_entry(const boost::json::object &source, const char *key,
                                                         const std::string &kind)
{
    const auto *value = source.if_contains(key);
    if (!value)
        throw Failure(0);
    if (value->is_null())
        return std::nullopt;
    if (!value->is_object())
        throw Failure(0);
    return dictionary_entry(value->as_object(), kind);
}
DictionaryEntry catalog_entry(const boost::json::object &source, const std::string &kind)
{
    DictionaryEntry entry;
    entry.kind = string(source, "kind", 16);
    entry.code = string(source, "code", 512);
    entry.word = string(source, "word", 2048);
    entry.weight = dictionary_number(source, "weight", 0);
    if (entry.kind != kind)
        throw Failure(0);
    try
    {
        require_dictionary_entry(entry, kind);
    }
    catch (const Failure &)
    {
        throw Failure(0);
    }
    return entry;
}
Preferences preferences_value(const boost::json::object &source)
{
    const auto *revision = source.if_contains("revision");
    const auto *settings = source.if_contains("settings");
    if (!revision || !revision->is_int64() || revision->as_int64() < 0 || !settings || !settings->is_object() ||
        settings->as_object().size() > 1024)
        throw Failure(0);
    Preferences result{revision->as_int64(), {}};
    for (const auto &entry : settings->as_object())
    {
        const std::string key(entry.key());
        require_text(key, 256);
        const auto &value = entry.value();
        if (value.is_bool())
            result.settings.emplace(key, value.as_bool());
        else if (value.is_int64() && value.as_int64() >= 0 && value.as_int64() <= 1000000)
            result.settings.emplace(key, value.as_int64());
        else if (value.is_double() && std::isfinite(value.as_double()) && value.as_double() >= 0 &&
                 value.as_double() <= 1000000)
            result.settings.emplace(key, value.as_double());
        else if (value.is_string())
        {
            auto text = std::string(value.as_string());
            require_text(text, 1024 * 1024);
            result.settings.emplace(key, std::move(text));
        }
        else
            throw Failure(0);
    }
    return result;
}
boost::json::object preference_settings(const Preferences &value, const PreferencesSchema &schema)
{
    if (value.revision < 0 || value.revision == std::numeric_limits<std::int64_t>::max() || schema.maximum_bytes == 0 ||
        schema.maximum_bytes > 1024 * 1024 || value.settings.size() > 1024)
        throw Failure(400);
    boost::json::object settings;
    for (const auto &[key, setting] : value.settings)
    {
        const auto it = schema.fields.find(key);
        if (it == schema.fields.end())
            throw Failure(400);
        const auto &field = it->second;
        const bool type_matches = (field.type == "boolean" && std::holds_alternative<bool>(setting)) ||
                                  (field.type == "integer" && std::holds_alternative<std::int64_t>(setting)) ||
                                  (field.type == "number" && (std::holds_alternative<std::int64_t>(setting) ||
                                                              std::holds_alternative<double>(setting))) ||
                                  (field.type == "string" && std::holds_alternative<std::string>(setting));
        if (!type_matches)
            throw Failure(400);
        if (const auto *text = std::get_if<std::string>(&setting))
            require_text(*text, field.maximum_length);
        if (const auto *number = std::get_if<std::int64_t>(&setting))
            if (*number < 0 || *number > 1000000)
                throw Failure(400);
        if (const auto *number = std::get_if<double>(&setting))
            if (!std::isfinite(*number) || *number < 0 || *number > 1000000)
                throw Failure(400);
        std::visit([&, name = key](const auto &item) { settings[name] = item; }, setting);
    }
    return settings;
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
                                          std::size_t response_limit, std::size_t request_limit)
{
    if (!token.empty())
    {
        require_token(token);
    }
    if (body.size() > request_limit)
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
Preferences BackendAccountClient::preferences(const std::string &token, const online::CancellationCheck &cancelled)
{
    require_token(token);
    return preferences_value(
        object(request(online::HttpMethod::Get, "/v1/users/me/preferences", {}, token, cancelled, 2 * 1024 * 1024)));
}
PreferencesSchema BackendAccountClient::preferences_schema(const std::string &token,
                                                           const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto source =
        object(request(online::HttpMethod::Get, "/v1/users/me/preferences/schema", {}, token, cancelled));
    const auto *maximum = source.if_contains("maximum_bytes");
    const auto *required = source.if_contains("revision_required");
    const auto *fields = source.if_contains("fields");
    if (!maximum || !maximum->is_int64() || maximum->as_int64() <= 0 || maximum->as_int64() > 1024 * 1024 ||
        !required || !required->is_bool() || !required->as_bool() || string(source, "update_mode", 32) != "replace" ||
        !fields || !fields->is_object() || fields->as_object().size() > 1024)
        throw Failure(0);
    PreferencesSchema result{static_cast<std::size_t>(maximum->as_int64()), {}};
    for (const auto &entry : fields->as_object())
    {
        if (!entry.value().is_object())
            throw Failure(0);
        const std::string key(entry.key());
        require_text(key, 256);
        const auto &field = entry.value().as_object();
        auto type = string(field, "type", 32);
        if (type != "boolean" && type != "integer" && type != "number" && type != "string")
            throw Failure(0);
        std::size_t length = 1024;
        if (const auto *limit = field.if_contains("maxLength"))
        {
            if (!limit->is_int64() || limit->as_int64() <= 0 || limit->as_int64() > 1024 * 1024)
                throw Failure(0);
            length = static_cast<std::size_t>(limit->as_int64());
        }
        if (key.find("prompt") != std::string::npos)
            length = 8192;
        result.fields.emplace(key, PreferenceField{std::move(type), length});
    }
    return result;
}
Preferences BackendAccountClient::put_preferences(const Preferences &value, const PreferencesSchema &schema,
                                                  const std::string &token, const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto body = boost::json::serialize(
        boost::json::object{{"revision", value.revision}, {"settings", preference_settings(value, schema)}});
    if (body.size() > schema.maximum_bytes)
        throw Failure(400);
    auto result = preferences_value(object(request(online::HttpMethod::Put, "/v1/users/me/preferences", body, token,
                                                   cancelled, 2 * 1024 * 1024, schema.maximum_bytes)));
    if (result.revision != value.revision + 1)
        throw Failure(0);
    return result;
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
DictionaryPage BackendAccountClient::dictionary(const std::string &kind, const std::string &query, int offset,
                                                int limit, const std::string &token,
                                                const online::CancellationCheck &cancelled)
{
    require_token(token);
    auto path = dictionary_path(kind);
    require_text(query, 1024);
    if (offset < 0 || offset > 1000000 || limit < 1 || limit > 200)
        throw Failure(400);
    path += "?q=" + query_component(query) + "&offset=" + std::to_string(offset) + "&limit=" + std::to_string(limit);
    const auto source = object(request(online::HttpMethod::Get, path.c_str(), {}, token, cancelled, 4 * 1024 * 1024));
    const auto *entries = source.if_contains("entries");
    const auto *more = source.if_contains("has_more");
    if (!entries || !entries->is_array() || entries->as_array().size() > static_cast<std::size_t>(limit) || !more ||
        !more->is_bool() || dictionary_number(source, "offset", 0) != offset)
        throw Failure(0);
    DictionaryPage page{{}, more->as_bool(), offset};
    for (const auto &entry : entries->as_array())
    {
        if (!entry.is_object())
            throw Failure(0);
        auto decoded = dictionary_entry(entry.as_object(), kind);
        if (std::any_of(page.entries.begin(), page.entries.end(),
                        [&](const DictionaryEntry &old) { return old.id == decoded.id; }))
            throw Failure(0);
        page.entries.push_back(std::move(decoded));
    }
    if (page.has_more && page.entries.size() != static_cast<std::size_t>(limit))
        throw Failure(0);
    return page;
}
DictionaryPage BackendAccountClient::dictionary_catalog(const std::string &kind, const std::string &query, int offset,
                                                        int limit, const std::string &token,
                                                        const online::CancellationCheck &cancelled,
                                                        DictionaryCatalogOptions options)
{
    require_token(token);
    if ((options.scheme != "pinyin" && options.scheme != "shuangpin") ||
        (options.profile != "xiaohe" && options.profile != "ziranma" && options.profile != "shoudao" &&
         options.profile != "microsoft"))
        throw Failure(400);
    auto path = dictionary_path(kind);
    require_text(query, 1024);
    if ((query.empty() && kind != "quick") || offset < 0 || offset > 1000000 || limit < 1 || limit > 200)
        throw Failure(400);
    path += "/catalog?q=" + query_component(query) + "&offset=" + std::to_string(offset) +
            "&limit=" + std::to_string(limit);
    path += "&scheme=" + options.scheme + "&profile=" + options.profile;
    const auto source = object(request(online::HttpMethod::Get, path.c_str(), {}, token, cancelled, 4 * 1024 * 1024));
    const auto *entries = source.if_contains("entries"), *more = source.if_contains("has_more");
    if (!entries || !entries->is_array() || entries->as_array().size() > static_cast<std::size_t>(limit) || !more ||
        !more->is_bool() || dictionary_number(source, "offset", 0) != offset)
        throw Failure(0);
    DictionaryPage page{{}, more->as_bool(), offset, dictionary_number(source, "revision", 0)};
    for (const auto &value : entries->as_array())
    {
        if (!value.is_object())
            throw Failure(0);
        auto entry = catalog_entry(value.as_object(), kind);
        if (std::any_of(page.entries.begin(), page.entries.end(),
                        [&](const auto &old) { return old.code == entry.code && old.word == entry.word; }))
            throw Failure(0);
        page.entries.push_back(std::move(entry));
    }
    if (page.has_more && page.entries.size() != static_cast<std::size_t>(limit))
        throw Failure(0);
    return page;
}
DictionaryChange BackendAccountClient::manage_dictionary(const std::string &kind, std::int64_t revision,
                                                         const DictionaryEntry &previous,
                                                         const std::optional<DictionaryEntry> &replacement,
                                                         const std::string &token,
                                                         const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto path = dictionary_path(kind) + "/edit";
    if (revision < 0 || revision == std::numeric_limits<std::int64_t>::max() || previous.kind != kind)
        throw Failure(400);
    require_dictionary_entry(previous, kind);
    boost::json::object body{{"revision", revision},
                             {"previous", boost::json::object{{"code", previous.code}, {"word", previous.word}}},
                             {"replacement", nullptr}};
    if (replacement)
    {
        require_dictionary_entry(*replacement, kind);
        body["replacement"] = boost::json::object{
            {"code", replacement->code}, {"word", replacement->word}, {"weight", replacement->weight}};
    }
    const auto source =
        object(request(online::HttpMethod::Post, path.c_str(), boost::json::serialize(body), token, cancelled));
    DictionaryChange change;
    change.revision = dictionary_number(source, "revision", 1);
    const auto *old = source.if_contains("previous"), *next = source.if_contains("replacement");
    if (change.revision != revision + 1 || !old || !old->is_object() || !next ||
        (replacement ? !next->is_object() : !next->is_null()))
        throw Failure(0);
    change.previous = catalog_entry(old->as_object(), kind);
    if (change.previous->code != previous.code || change.previous->word != previous.word)
        throw Failure(0);
    if (replacement)
        change.replacement = catalog_entry(next->as_object(), kind);
    return change;
}
DictionaryChange BackendAccountClient::edit_dictionary(const std::string &kind, const std::string &id,
                                                       std::int64_t revision,
                                                       const std::optional<DictionaryEntry> &replacement,
                                                       const std::string &token,
                                                       const online::CancellationCheck &cancelled)
{
    require_token(token);
    auto path = dictionary_path(kind);
    if ((id.empty() && (revision != 0 || !replacement)) ||
        (!id.empty() && (!valid_token(id) || revision < 1 || revision == std::numeric_limits<std::int64_t>::max())))
        throw Failure(400);
    boost::json::object body;
    if (!id.empty())
    {
        path += "/" + id;
        body["revision"] = revision;
    }
    if (replacement)
    {
        require_dictionary_entry(*replacement, kind);
        body["code"] = replacement->code;
        body["word"] = replacement->word;
        body["weight"] = replacement->weight;
    }
    const auto method = id.empty()    ? online::HttpMethod::Post
                        : replacement ? online::HttpMethod::Put
                                      : online::HttpMethod::Delete;
    const auto source = object(request(method, path.c_str(), boost::json::serialize(body), token, cancelled));
    DictionaryChange change{dictionary_number(source, "revision", 1),
                            optional_dictionary_entry(source, "previous", kind),
                            optional_dictionary_entry(source, "replacement", kind)};
    if (bool(change.previous) != !id.empty() || bool(change.replacement) != bool(replacement) ||
        change.revision <= revision ||
        (change.previous && (change.previous->id != id || change.previous->revision != revision)) ||
        (change.replacement &&
         (change.replacement->revision != change.revision || (!id.empty() && change.replacement->id != id))))
        throw Failure(0);
    return change;
}
DictionaryImportResult BackendAccountClient::import_dictionary(const std::string &kind, const std::string &text,
                                                               const std::string &format, const std::string &token,
                                                               const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto path = dictionary_path(kind) + "/import";
    dictionary_format(format);
    if (text.size() > 65536)
        throw Failure(400);
    const auto count = dictionary_tsv(text, kind, format, 500, false);
    if (count == 0)
        throw Failure(400);
    const auto body = boost::json::serialize(boost::json::object{{"text", text}, {"format", format}});
    const auto result = object(request(online::HttpMethod::Post, path.c_str(), body, token, cancelled));
    const auto imported = dictionary_number(result, "imported", 1);
    if (imported != static_cast<std::int64_t>(count))
        throw Failure(0);
    return {static_cast<int>(imported), dictionary_number(result, "revision", 1)};
}
DictionaryImportResult BackendAccountClient::import_han_dictionary(const std::string &text, std::int64_t weight,
                                                                   const std::string &token,
                                                                   const online::CancellationCheck &cancelled)
{
    require_token(token);
    require_text(text, 65536);
    if (text.empty() || weight < 0)
        throw Failure(400);
    // The backend validates Chinese phrases and generates canonical pinyin with
    // the native Engine. Do not reproduce its annotation rules in this client.
    const auto body = boost::json::serialize(boost::json::object{{"text", text}, {"weight", weight}});
    const auto result = object(
        request(online::HttpMethod::Post, "/v1/users/me/dictionaries/pinyin/import-hans", body, token, cancelled));
    const auto count = dictionary_number(result, "imported", 1);
    if (count > 500)
        throw Failure(0);
    return {static_cast<int>(count), dictionary_number(result, "revision", 1)};
}
std::string BackendAccountClient::export_dictionary(const std::string &kind, const std::string &format,
                                                    const std::string &token,
                                                    const online::CancellationCheck &cancelled)
{
    require_token(token);
    const auto path = dictionary_path(kind) + "/export?format=" + format;
    dictionary_format(format);
    auto text = request(online::HttpMethod::Get, path.c_str(), {}, token, cancelled, 256 * 1024 * 1024);
    try
    {
        (void)dictionary_tsv(text, kind, format, 100000, true);
    }
    catch (const Failure &)
    {
        throw Failure(0);
    }
    return text;
}
std::string BackendAccountClient::snapshot_request(online::HttpRequest &request, const std::string &token,
                                                   const online::CancellationCheck &cancelled)
{
    require_token(token);
    if (cancelled && cancelled())
        throw Failure(0, true);
    request.total_timeout = std::chrono::minutes(2);
    request.headers.emplace_back("Authorization: Bearer " + token);
    request.headers.emplace_back("Cache-Control: no-store");
    auto response = transport_.perform(request, cancelled);
    if (cancelled && cancelled())
        throw Failure(0, true);
    if (!response.error.empty() || response.body.size() > request.max_response_bytes)
        throw Failure(0);
    if (response.status_code != 200)
        throw Failure(response.status_code);
    return response.body;
}
void BackendAccountClient::download_snapshot(const online::HttpResponseSink &sink, const std::string &token,
                                             const online::CancellationCheck &cancelled)
{
    if (!sink)
        throw Failure(400);
    online::HttpRequest request;
    request.url = "https://api.msime.app/v1/users/me/dictionary/snapshot";
    request.headers = {"Accept: application/x-ndjson"};
    request.max_response_bytes = 512U * 1024U * 1024U;
    std::size_t received = 0;
    bool failed = false;
    request.response_sink = [&](const char *data, std::size_t bytes) {
        if (bytes > request.max_response_bytes - received || (bytes && !data))
        {
            failed = true;
            return false;
        }
        if (!sink(data, bytes))
        {
            failed = true;
            return false;
        }
        received += bytes;
        return true;
    };
    const auto body = snapshot_request(request, token, cancelled);
    if (failed || !body.empty() || received == 0)
        throw Failure(0);
}
std::int64_t BackendAccountClient::restore_snapshot(std::size_t size, const online::HttpBodySource &source,
                                                    std::int64_t revision, const std::string &token,
                                                    const online::CancellationCheck &cancelled)
{
    if (!source || size == 0 || size > 512U * 1024U * 1024U || revision < 0 ||
        revision == std::numeric_limits<std::int64_t>::max())
        throw Failure(400);
    online::HttpRequest request;
    request.method = online::HttpMethod::Put;
    request.url = "https://api.msime.app/v1/users/me/dictionary/snapshot?revision=" + std::to_string(revision);
    request.headers = {"Accept: application/json", "Content-Type: application/x-ndjson"};
    request.body_source = source;
    request.body_size = size;
    request.max_response_bytes = 65536;
    const auto result = object(snapshot_request(request, token, cancelled));
    const auto *reset = result.if_contains("reset"), *updated = result.if_contains("revision");
    if (result.size() != 2 || !reset || !reset->is_bool() || !reset->as_bool() || !updated || !updated->is_int64() ||
        updated->as_int64() <= revision)
        throw Failure(0);
    return updated->as_int64();
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
