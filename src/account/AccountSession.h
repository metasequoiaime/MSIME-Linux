#pragma once

#include "AccountCredentialStore.h"
#include <functional>
#include <mutex>

namespace metasequoia::linux_ime::account
{
struct SessionSnapshot
{
    std::uint64_t generation = 0;
    std::optional<User> user;
};

struct PreferencesReview
{
    std::uint64_t generation = 0;
    std::string user_id;
    Preferences remote;
    PreferencesSchema schema;
};

// All methods can block: call from a background worker. One instance owns one
// desktop session. Generation binds operations and UI results to an account.
class AccountSession
{
  public:
    using Clock = std::function<std::int64_t()>;
    AccountSession(BackendAccountClient &client, AccountCredentialStore &store, Clock clock = {});
    SessionSnapshot restore();
    SessionSnapshot snapshot() const;
    SessionSnapshot login(std::uint64_t generation, const std::string &challenge, const std::string &credential,
                          const online::CancellationCheck &cancelled = {});
    Challenge begin_link(std::uint64_t generation, const std::string &provider, const std::string &target,
                         const online::CancellationCheck &cancelled = {});
    SessionSnapshot link(std::uint64_t generation, const std::string &challenge, const std::string &credential,
                         const online::CancellationCheck &cancelled = {});
    std::string access_token(std::uint64_t generation, const online::CancellationCheck &cancelled = {});
    Profile profile(std::uint64_t generation, const online::CancellationCheck &cancelled = {});
    Profile rename(std::uint64_t generation, const std::string &name, const online::CancellationCheck &cancelled = {});
    PreferencesReview review_preferences(std::uint64_t generation, const online::CancellationCheck &cancelled = {});
    Preferences upload_preferences(const PreferencesReview &review, const std::map<std::string, PreferenceValue> &local,
                                   const online::CancellationCheck &cancelled = {});
    // Holds the account lock through a background persistence callback. The callback
    // must not call this session or access GTK, and must check local-file conflicts.
    void apply_preferences(const PreferencesReview &review, const std::function<void()> &apply,
                           const online::CancellationCheck &cancelled = {});
    ClipboardSnapshot clipboard(std::uint64_t generation, const std::string &query = {},
                                const online::CancellationCheck &cancelled = {});
    ClipboardSnapshot set_clipboard_enabled(std::uint64_t generation, bool enabled,
                                            const online::CancellationCheck &cancelled = {});
    ClipboardSnapshot add_clipboard(std::uint64_t generation, const std::string &text,
                                    const online::CancellationCheck &cancelled = {});
    ClipboardSnapshot delete_clipboard(std::uint64_t generation, const std::string &id,
                                       const online::CancellationCheck &cancelled = {});
    void logout(std::uint64_t generation, bool all = false, const online::CancellationCheck &cancelled = {});
    void delete_account(std::uint64_t generation, const online::CancellationCheck &cancelled = {});

  private:
    SessionSnapshot current() const;
    void require_generation(std::uint64_t generation) const;
    std::string authorized(std::uint64_t generation, const online::CancellationCheck &cancelled);
    SavedSession saved(Tokens tokens) const;
    void discard();
    std::string reviewed_token(const PreferencesReview &review, const online::CancellationCheck &cancelled);
    ClipboardSnapshot clipboard_operation(std::uint64_t generation, const online::CancellationCheck &cancelled,
                                          const std::function<ClipboardSnapshot(const std::string &)> &operation);
    Profile read_profile(const std::string &token, const online::CancellationCheck &cancelled);
    BackendAccountClient &client_;
    AccountCredentialStore &store_;
    Clock clock_;
    mutable std::mutex mutex_;
    bool restored_ = false;
    std::uint64_t generation_ = 0;
    std::optional<SavedSession> session_;
};
} // namespace metasequoia::linux_ime::account
