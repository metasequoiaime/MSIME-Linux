#pragma once
#include "online/HttpTransport.h"
#include <string>
namespace metasequoia::linux_ime::account
{
void save_dictionary_export(const std::string &path, std::size_t size, const online::HttpBodySource &source,
                            const online::CancellationCheck &cancelled = {});
// Atomically publishes a complete new file. Existing files are never replaced.
void save_dictionary_export(const std::string &path, const std::string &text,
                            const online::CancellationCheck &cancelled = {});
} // namespace metasequoia::linux_ime::account
