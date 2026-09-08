#pragma once
#include "PreparedSnapshot.h"
namespace metasequoia::linux_ime::account
{
// Validates portable record fields and relationships in a disposable SQLite index.
// Native Engine normalization/staging and idle-session activation remain mandatory.
void validate_snapshot_contents(const PreparedSnapshot &snapshot, const online::CancellationCheck &cancelled = {});
} // namespace metasequoia::linux_ime::account
