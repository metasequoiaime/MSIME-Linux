#pragma once
#include "online/HttpTransport.h"
#include <array>
#include <cstdint>
#include <istream>
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
SnapshotEnvelope inspect_snapshot_envelope(std::istream &input, const online::CancellationCheck &cancelled = {});
} // namespace metasequoia::linux_ime::account
