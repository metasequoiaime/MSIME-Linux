#pragma once
#include "online/HttpTransport.h"
#include <array>
#include <cstdint>
#include <istream>
#include <functional>
namespace metasequoia::linux_ime::account
{
struct SnapshotEnvelope
{
    std::int64_t revision = 0;
    std::size_t records = 0;
    std::array<std::size_t, 4> counts{};
    std::string sha256;
};
// Checks transport framing and checksum, not record semantics or consistency.
// The caller must freeze the input and validate records before native staging.
// Visitors may stage records only: checksum verification finishes after callbacks.
using SnapshotRecordVisitor = std::function<void(const std::string &, std::int64_t)>;
SnapshotEnvelope inspect_snapshot_envelope(std::istream &input, const online::CancellationCheck &cancelled = {},
                                           const SnapshotRecordVisitor &visitor = {});
} // namespace metasequoia::linux_ime::account
