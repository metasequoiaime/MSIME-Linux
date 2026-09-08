#pragma once
#include "AccountSession.h"
#include "SnapshotEnvelope.h"
#include <filesystem>
#include <memory>
#include <vector>

typedef struct _GCancellable GCancellable;
typedef struct _GDBusConnection GDBusConnection;
namespace metasequoia::linux_ime::account
{
struct NativeRestoreLocations
{
    std::filesystem::path root, tools;
    std::vector<std::filesystem::path> sources;
};
NativeRestoreLocations installed_native_restore_locations();
struct NativeRestoreResult
{
    bool durable;
};
// Background preparation freezes the cloud snapshot and stages a complete local
// generation. The UI must show source/account/scope before calling publish().
class NativeRestoreReview
{
  public:
    static std::unique_ptr<NativeRestoreReview> prepare(std::shared_ptr<AccountSession> session,
                                                        std::uint64_t generation, NativeRestoreLocations locations,
                                                        GCancellable *cancelled);
    ~NativeRestoreReview();
    const User &user() const
    {
        return user_;
    }
    const SnapshotEnvelope &source() const
    {
        return source_;
    }
    NativeRestoreResult publish(GCancellable *cancelled);
    NativeRestoreReview(const NativeRestoreReview &) = delete;
    NativeRestoreReview &operator=(const NativeRestoreReview &) = delete;

  private:
    NativeRestoreReview() = default;
    std::shared_ptr<AccountSession> session_;
    std::shared_ptr<GDBusConnection> connection_;
    NativeRestoreLocations locations_;
    User user_;
    SnapshotEnvelope source_;
    std::uint64_t account_generation_ = 0;
    std::string owner_, token_, revision_, generation_, content_;
    bool staged_ = false, attempted_ = false;
};
} // namespace metasequoia::linux_ime::account
