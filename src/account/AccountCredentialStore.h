#pragma once

#include "BackendAccountClient.h"
#include "SecretStore.h"
#include <cstdint>
#include <optional>

namespace metasequoia::linux_ime::account
{
struct SavedSession
{
    Tokens tokens;
    std::int64_t expires_at = 0;
};

// Blocking Secret Service operations; the session coordinator serializes callers.
// Missing records return nullopt. Unavailable/corrupt records throw without erasing.
class AccountCredentialStore
{
  public:
    explicit AccountCredentialStore(SecretStore &store) : store_(store)
    {
    }
    std::optional<SavedSession> load() const;
    void save(const SavedSession &session);
    void clear();

  private:
    SecretStore &store_;
};
} // namespace metasequoia::linux_ime::account
