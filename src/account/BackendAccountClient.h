#pragma once

#include "online/HttpTransport.h"

#include <map>
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
    void logout(const std::string &token, bool all = false, const online::CancellationCheck &cancelled = {});
    void delete_account(const std::string &token, const online::CancellationCheck &cancelled = {});

  private:
    std::string request(online::HttpMethod method, const char *path, const std::string &body, const std::string &token,
                        const online::CancellationCheck &cancelled);
    online::HttpTransport &transport_;
};
} // namespace metasequoia::linux_ime::account
