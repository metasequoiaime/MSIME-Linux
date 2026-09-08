#pragma once

#include "online/HttpTransport.h"

#include <map>
#include <optional>
#include <cstdint>
#include <variant>
#include <stdexcept>
#include <string>
#include <vector>

namespace metasequoia::linux_ime::account
{
// Fixed-origin account transport. UI/session code owns consent and Secret Service persistence.
struct User
{
    std::string id;
    std::string display_name;
    std::string created_at;
};
struct Tokens
{
    std::string access_token;
    std::string refresh_token;
    int expires_in = 0;
    User user;
};
struct Challenge
{
    std::string id;
    int expires_in = 0;
    std::string nonce;
    std::string authorization_url;
};
struct Identity
{
    std::string provider;
    std::string subject;
};
using PreferenceValue = std::variant<bool, std::int64_t, double, std::string>;
struct Preferences
{
    std::int64_t revision = 0;
    std::map<std::string, PreferenceValue> settings;
};
struct PreferenceField
{
    std::string type;
    std::size_t maximum_length = 1024;
};
struct PreferencesSchema
{
    std::size_t maximum_bytes = 0;
    std::map<std::string, PreferenceField> fields;
};
struct ClipboardItem
{
    std::string id, text, updated_at;
};
struct ClipboardSnapshot
{
    bool enabled = false;
    std::vector<ClipboardItem> items;
};
struct Profile
{
    User user;
    std::vector<Identity> identities;
};
class Failure final : public std::runtime_error
{
  public:
    explicit Failure(long status, bool cancelled = false);
    long status() const noexcept
    {
        return status_;
    }
    bool cancelled() const noexcept
    {
        return cancelled_;
    }

  private:
    long status_;
    bool cancelled_;
};
struct DictionaryEntry
{
    std::string id, kind, code, word;
    std::int64_t weight = 10, revision = 0;
    std::string updated_at;
};
struct DictionaryPage
{
    std::vector<DictionaryEntry> entries;
    bool has_more = false;
    int offset = 0;
    std::int64_t revision = 0;
};
struct DictionaryCatalogOptions
{
    std::string scheme = "pinyin", profile = "xiaohe";
};
struct DictionaryImportResult
{
    int imported = 0;
    std::int64_t revision = 0;
};
struct DictionaryChange
{
    std::int64_t revision = 0;
    std::optional<DictionaryEntry> previous, replacement;
};
class BackendAccountClient
{
  public:
    explicit BackendAccountClient(online::HttpTransport &transport) : transport_(transport)
    {
    }
    // Shared strict decoder for network responses and Secret Service records.
    static Tokens decode_tokens(const std::string &text);
    std::map<std::string, bool> providers(const online::CancellationCheck &cancelled = {});
    Challenge challenge(const std::string &provider, const std::string &target = {}, const std::string &link_token = {},
                        const online::CancellationCheck &cancelled = {});
    Tokens login(const std::string &challenge_id, const std::string &credential, const std::string &link_token = {},
                 const online::CancellationCheck &cancelled = {});
    Tokens refresh(const std::string &refresh_token, const online::CancellationCheck &cancelled = {});
    Profile profile(const std::string &token, const online::CancellationCheck &cancelled = {});
    void rename(const std::string &name, const std::string &token, const online::CancellationCheck &cancelled = {});
    Preferences preferences(const std::string &token, const online::CancellationCheck &cancelled = {});
    PreferencesSchema preferences_schema(const std::string &token, const online::CancellationCheck &cancelled = {});
    Preferences put_preferences(const Preferences &value, const PreferencesSchema &schema, const std::string &token,
                                const online::CancellationCheck &cancelled = {});
    ClipboardSnapshot clipboard(const std::string &token, const std::string &query = {},
                                const online::CancellationCheck &cancelled = {});
    void set_clipboard_enabled(bool enabled, const std::string &token, const online::CancellationCheck &cancelled = {});
    ClipboardItem add_clipboard(const std::string &text, const std::string &token,
                                const online::CancellationCheck &cancelled = {});
    void delete_clipboard(const std::string &id, const std::string &token,
                          const online::CancellationCheck &cancelled = {});
    DictionaryPage dictionary(const std::string &kind, const std::string &query, int offset, int limit,
                              const std::string &token, const online::CancellationCheck &cancelled = {});
    DictionaryPage dictionary_catalog(const std::string &kind, const std::string &query, int offset, int limit,
                                      const std::string &token, const online::CancellationCheck &cancelled = {},
                                      DictionaryCatalogOptions options = {});
    DictionaryChange manage_dictionary(const std::string &kind, std::int64_t revision, const DictionaryEntry &previous,
                                       const std::optional<DictionaryEntry> &replacement, const std::string &token,
                                       const online::CancellationCheck &cancelled = {});
    // Empty id creates an entry (revision must be zero). An existing id requires
    // its exact entry revision; null replacement deletes. No conflict retries.
    DictionaryChange edit_dictionary(const std::string &kind, const std::string &id, std::int64_t revision,
                                     const std::optional<DictionaryEntry> &replacement, const std::string &token,
                                     const online::CancellationCheck &cancelled = {});
    DictionaryImportResult import_dictionary(const std::string &kind, const std::string &text,
                                             const std::string &format, const std::string &token,
                                             const online::CancellationCheck &cancelled = {});
    DictionaryImportResult import_han_dictionary(const std::string &text, std::int64_t weight, const std::string &token,
                                                 const online::CancellationCheck &cancelled = {});
    std::string export_dictionary(const std::string &kind, const std::string &format, const std::string &token,
                                  const online::CancellationCheck &cancelled = {});
    // Download bytes remain provisional until envelope and content validation complete.
    void download_snapshot(const online::HttpResponseSink &sink, const std::string &token,
                           const online::CancellationCheck &cancelled = {});
    // The source must be the frozen, validated snapshot approved by the user.
    std::int64_t restore_snapshot(std::size_t size, const online::HttpBodySource &source, std::int64_t revision,
                                  const std::string &token, const online::CancellationCheck &cancelled = {});
    void logout(const std::string &token, bool all = false, const online::CancellationCheck &cancelled = {});
    void delete_account(const std::string &token, const online::CancellationCheck &cancelled = {});

  private:
    std::string request(online::HttpMethod method, const char *path, const std::string &body, const std::string &token,
                        const online::CancellationCheck &cancelled, std::size_t response_limit = 1024 * 1024,
                        std::size_t request_limit = 65536);
    std::string snapshot_request(online::HttpRequest &request, const std::string &token,
                                 const online::CancellationCheck &cancelled);
    online::HttpTransport &transport_;
};
} // namespace metasequoia::linux_ime::account
