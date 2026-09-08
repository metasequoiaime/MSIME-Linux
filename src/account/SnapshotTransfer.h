#pragma once
#include "AccountSession.h"
#include "PreparedSnapshot.h"
namespace metasequoia::linux_ime::account
{
std::unique_ptr<PreparedSnapshot> download_validated_snapshot(AccountSession &session, std::uint64_t generation,
                                                              const online::CancellationCheck &cancelled = {});
// Background-only preparation performs no cloud mutations. The UI must show both
// envelopes and the account before invoking restore after explicit confirmation.
class SnapshotRestoreReview
{
  public:
    static std::unique_ptr<SnapshotRestoreReview> prepare(std::shared_ptr<AccountSession> session,
                                                          std::uint64_t generation, const std::string &file,
                                                          const online::CancellationCheck &cancelled = {});
    const SnapshotEnvelope &source() const
    {
        return source_->envelope();
    }
    const SnapshotEnvelope &target() const
    {
        return target_;
    }
    const User &user() const
    {
        return user_;
    }
    std::int64_t restore(const online::CancellationCheck &cancelled = {});

  private:
    SnapshotRestoreReview() = default;
    std::shared_ptr<AccountSession> session_;
    std::uint64_t generation_ = 0;
    std::unique_ptr<PreparedSnapshot> source_;
    SnapshotEnvelope target_;
    User user_;
};
} // namespace metasequoia::linux_ime::account
