#pragma once
#include "SnapshotEnvelope.h"
#include <memory>
namespace metasequoia::linux_ime::account
{
// Owns an unlinked private copy. The source is never reread after preparation.
// Envelope integrity does not establish record validity or authorize activation.
class PreparedSnapshot
{
  public:
    static std::unique_ptr<PreparedSnapshot> open(const std::string &source,
                                                  const online::CancellationCheck &cancelled = {});
    // Producer and sink are synchronous; downloaded bytes are never published by name.
    static std::unique_ptr<PreparedSnapshot> receive(
        const std::function<void(const online::HttpResponseSink &)> &producer,
        const online::CancellationCheck &cancelled = {});
    ~PreparedSnapshot();
    void inspect_records(const SnapshotRecordVisitor &visitor, const online::CancellationCheck &cancelled = {}) const;
    PreparedSnapshot(const PreparedSnapshot &) = delete;
    PreparedSnapshot &operator=(const PreparedSnapshot &) = delete;
    const SnapshotEnvelope &envelope() const
    {
        return envelope_;
    }
    std::size_t size() const
    {
        return size_;
    }
    // Independent reads permit validation and upload without sharing a cursor.
    std::size_t read(std::size_t offset, char *buffer, std::size_t capacity,
                     const online::CancellationCheck &cancelled = {}) const;

  private:
    explicit PreparedSnapshot(int descriptor) : descriptor_(descriptor)
    {
    }
    int descriptor_;
    std::size_t size_ = 0;
    SnapshotEnvelope envelope_;
};
} // namespace metasequoia::linux_ime::account
