#pragma once
#include "online/HttpTransport.h"
#include <metasequoia/dictionary_state.h>
namespace metasequoia::linux_ime::account
{
// Fingerprint of one consistent Engine journal snapshot, including overlays,
// positions and frequency counts. Compare again under the publication lease;
// also compare the active generation independently. This is not a cloud revision
// or a resource-bundle integrity check. Never logs native input records.
std::string native_dictionary_revision(const RuntimePaths &paths, const online::CancellationCheck &cancelled = {});
} // namespace metasequoia::linux_ime::account
