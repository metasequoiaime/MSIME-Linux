#include "SecretStore.h"

#include <libsecret/secret.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace metasequoia::linux_ime
{
namespace
{
constexpr std::size_t kMaximumProviderBytes = 128;
constexpr std::size_t kMaximumSecretBytes = 4096;
constexpr const char *kUnavailableDiagnostic = "Credential service is unavailable; the affected provider was disabled.";
constexpr const char *kInvalidDiagnostic = "Credential settings were invalid; the affected provider was disabled.";

const SecretSchema kCredentialSchema = {
    "org.metasequoiaime.OnlineCredential",
    SECRET_SCHEMA_NONE,
    {{"kind", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {"provider", SECRET_SCHEMA_ATTRIBUTE_STRING},
     {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr};

const char *kind_name(SecretKind kind)
{
    switch (kind)
    {
    case SecretKind::AiApiToken:
        return "ai-api-token";
    case SecretKind::TranslationApiToken:
        return "translation-api-token";
    case SecretKind::VoiceApiToken:
        return "voice-api-token";
    }
    return nullptr;
}

const char *kind_label(SecretKind kind)
{
    switch (kind)
    {
    case SecretKind::AiApiToken:
        return "Metasequoia IME AI API token";
    case SecretKind::TranslationApiToken:
        return "Metasequoia IME translation API token";
    case SecretKind::VoiceApiToken:
        return "Metasequoia IME voice API token";
    }
    return nullptr;
}

bool valid_provider(std::string_view provider)
{
    return !provider.empty() && provider.size() <= kMaximumProviderBytes &&
           std::all_of(provider.begin(), provider.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
           });
}

bool valid_secret(std::string_view secret)
{
    return !secret.empty() && secret.size() <= kMaximumSecretBytes &&
           secret.find('\0') == std::string_view::npos &&
           g_utf8_validate(secret.data(), static_cast<gssize>(secret.size()), nullptr);
}

void set_diagnostic(std::string *destination, const char *message)
{
    if (destination != nullptr)
    {
        *destination = message;
    }
}
} // namespace

LibsecretSecretStore::LibsecretSecretStore(std::string collection) : collection_(std::move(collection))
{
    if (collection_ != "default" && collection_ != "session")
    {
        collection_ = "default";
    }
}

SecretLookupResult LibsecretSecretStore::lookup(SecretKind kind, std::string_view provider) const
{
    const char *kind_value = kind_name(kind);
    if (kind_value == nullptr || !valid_provider(provider))
    {
        return {SecretStatus::Unavailable, {}, kInvalidDiagnostic};
    }

    const std::string provider_value(provider);
    GError *error = nullptr;
    gchar *password = secret_password_lookup_sync(&kCredentialSchema, nullptr, &error, "kind", kind_value,
                                                  "provider", provider_value.c_str(), nullptr);
    if (error != nullptr)
    {
        g_clear_error(&error);
        return {SecretStatus::Unavailable, {}, kUnavailableDiagnostic};
    }
    if (password == nullptr)
    {
        return {SecretStatus::NotFound, {}, {}};
    }

    std::string value(password);
    secret_password_free(password);
    if (!valid_secret(value))
    {
        return {SecretStatus::Unavailable, {}, kInvalidDiagnostic};
    }
    return {SecretStatus::Found, std::move(value), {}};
}

bool LibsecretSecretStore::store(SecretKind kind, std::string_view provider, std::string_view secret,
                                 std::string *diagnostic)
{
    set_diagnostic(diagnostic, "");
    const char *kind_value = kind_name(kind);
    const char *label = kind_label(kind);
    if (kind_value == nullptr || label == nullptr || !valid_provider(provider) || !valid_secret(secret))
    {
        set_diagnostic(diagnostic, kInvalidDiagnostic);
        return false;
    }

    const std::string provider_value(provider);
    const std::string secret_value(secret);
    GError *error = nullptr;
    const gboolean stored = secret_password_store_sync(
        &kCredentialSchema, collection_.c_str(), label, secret_value.c_str(), nullptr, &error, "kind", kind_value,
        "provider", provider_value.c_str(), nullptr);
    if (!stored || error != nullptr)
    {
        g_clear_error(&error);
        set_diagnostic(diagnostic, kUnavailableDiagnostic);
        return false;
    }
    return true;
}

bool LibsecretSecretStore::erase(SecretKind kind, std::string_view provider, std::string *diagnostic)
{
    set_diagnostic(diagnostic, "");
    const char *kind_value = kind_name(kind);
    if (kind_value == nullptr || !valid_provider(provider))
    {
        set_diagnostic(diagnostic, kInvalidDiagnostic);
        return false;
    }

    const std::string provider_value(provider);
    GError *error = nullptr;
    const gboolean erased = secret_password_clear_sync(&kCredentialSchema, nullptr, &error, "kind", kind_value,
                                                       "provider", provider_value.c_str(), nullptr);
    if (!erased || error != nullptr)
    {
        g_clear_error(&error);
        set_diagnostic(diagnostic, kUnavailableDiagnostic);
        return false;
    }
    return true;
}
} // namespace metasequoia::linux_ime
