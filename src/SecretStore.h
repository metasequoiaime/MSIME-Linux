#pragma once

#include <string>
#include <string_view>

namespace metasequoia::linux_ime
{
enum class SecretKind
{
    AiApiToken,
    TranslationApiToken,
};

enum class SecretStatus
{
    Found,
    NotFound,
    Unavailable,
};

struct SecretLookupResult
{
    SecretStatus status = SecretStatus::NotFound;
    std::string value;
    std::string diagnostic;
};

class SecretStore
{
  public:
    virtual ~SecretStore() = default;

    // These operations may block on the desktop Secret Service. Call them from
    // startup/background work, never from an active IBus main-loop callback.
    virtual SecretLookupResult lookup(SecretKind kind, std::string_view provider) const = 0;
    virtual bool store(SecretKind kind, std::string_view provider, std::string_view secret,
                       std::string *diagnostic = nullptr) = 0;
    virtual bool erase(SecretKind kind, std::string_view provider, std::string *diagnostic = nullptr) = 0;
};

class LibsecretSecretStore final : public SecretStore
{
  public:
    explicit LibsecretSecretStore(std::string collection = "default");

    SecretLookupResult lookup(SecretKind kind, std::string_view provider) const override;
    bool store(SecretKind kind, std::string_view provider, std::string_view secret,
               std::string *diagnostic = nullptr) override;
    bool erase(SecretKind kind, std::string_view provider, std::string *diagnostic = nullptr) override;

  private:
    std::string collection_;
};
} // namespace metasequoia::linux_ime
