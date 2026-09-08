#include "SnapshotTransfer.h"
#include "SnapshotContents.h"
namespace metasequoia::linux_ime::account
{
namespace
{
User checked_user(AccountSession &session, std::uint64_t generation)
{
    const auto current = session.snapshot();
    if (current.generation != generation || !current.user)
        throw Failure(0, true);
    return *current.user;
}
} // namespace
std::unique_ptr<PreparedSnapshot> download_validated_snapshot(AccountSession &session, std::uint64_t generation,
                                                              const online::CancellationCheck &cancelled)
{
    checked_user(session, generation);
    auto snapshot = PreparedSnapshot::receive(
        [&](const online::HttpResponseSink &sink) { session.download_snapshot(generation, sink, cancelled); },
        cancelled);
    validate_snapshot_contents(*snapshot, cancelled);
    checked_user(session, generation);
    return snapshot;
}
std::unique_ptr<SnapshotRestoreReview> SnapshotRestoreReview::prepare(std::shared_ptr<AccountSession> session,
                                                                      std::uint64_t generation, const std::string &file,
                                                                      const online::CancellationCheck &cancelled)
{
    if (!session)
        throw Failure(400);
    auto user = checked_user(*session, generation);
    auto source = PreparedSnapshot::open(file, cancelled);
    validate_snapshot_contents(*source, cancelled);
    auto remote = download_validated_snapshot(*session, generation, cancelled);
    std::unique_ptr<SnapshotRestoreReview> review(new SnapshotRestoreReview);
    review->session_ = std::move(session);
    review->generation_ = generation;
    review->source_ = std::move(source);
    review->target_ = remote->envelope();
    review->user_ = std::move(user);
    return review;
}
std::int64_t SnapshotRestoreReview::restore(const online::CancellationCheck &cancelled)
{
    return session_->restore_snapshot(
        generation_, source_->size(),
        [&](std::size_t offset, char *data, std::size_t size) { return source_->read(offset, data, size, cancelled); },
        target_.revision, cancelled);
}
} // namespace metasequoia::linux_ime::account
