#pragma once
#include "PreparedSnapshot.h"
#include <metasequoia/dictionary_state.h>
namespace metasequoia::linux_ime::account
{
// Captures one consistent Engine journal transaction as a frozen version-1 cloud
// snapshot. Caller holds the installation lease; this neither publishes nor uploads.
std::unique_ptr<PreparedSnapshot> export_native_snapshot(const RuntimePaths &paths,
                                                         const online::CancellationCheck &cancelled = {});
// Validates the frozen transport and builds an exclusive Engine generation. The
// caller supplies a verified resource bundle/content ID. This does not activate
// it: host writers and input sessions must be coordinated before publication.
RuntimePaths stage_native_snapshot(const PreparedSnapshot &snapshot, const std::filesystem::path &resources,
                                   const std::filesystem::path &generation, const std::string &content_id,
                                   const online::CancellationCheck &cancelled = {});
} // namespace metasequoia::linux_ime::account
