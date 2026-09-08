#pragma once
#include "online/HttpTransport.h"
#include <string>
namespace metasequoia::linux_ime::account
{
// Atomically publishes a complete new file. Existing files are never replaced.
void save_dictionary_export(const std::string &path, const std::string &text,
                            const online::CancellationCheck &cancelled = {});
} // namespace metasequoia::linux_ime::account
