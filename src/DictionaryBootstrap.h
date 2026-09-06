#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::linux_ime
{
// A packaged install leaves the dictionaries in a system directory the engine never looks at, so these helpers seed the
// per-user directory from the system copy the package provides. The engine resolves that directory from
// METASEQUOIA_IME_DATA_DIR first and only then from the XDG locations, so this mirrors metasequoia::data_directory()
// rather than deriving a second answer that can disagree with it.
std::filesystem::path user_data_directory();

std::vector<std::filesystem::path> system_data_directories();

// Copies any database, Japanese sentence-model payload or helpcode file that the user directory is missing. Returns the
// number of files copied; existing user data is never overwritten, so learned frequencies and staged upgrades are left
// alone.
//
// Every candidate is attempted even when an earlier one fails, because one unreadable file used to abandon the rest of
// the payloads, the helpcodes and every later system directory. `errors`, when given, is cleared on entry and left
// holding one message per failure, so an empty vector on return means every attempt succeeded. The count is not a
// success flag on its own: zero copied is the normal result once the user directory is already complete, and a non-zero
// count says nothing about what could not be placed alongside it.
std::size_t seed_user_data(const std::filesystem::path &user_directory,
                           const std::vector<std::filesystem::path> &system_directories,
                           std::vector<std::string> *errors);

std::size_t seed_user_data(std::vector<std::string> *errors);

// The same run, for callers that report a single line: the failures above joined with "; ", empty when there were none.
// Only these two carry a default argument, so a call that passes no out-parameter is never ambiguous.
std::size_t seed_user_data(const std::filesystem::path &user_directory,
                           const std::vector<std::filesystem::path> &system_directories, std::string *error = nullptr);

std::size_t seed_user_data(std::string *error = nullptr);

// Describes why the main dictionary cannot be used, or an empty string when it
// can. The engine opens a database it cannot find by creating an empty one, so
// without this a broken installation just produces no candidates and says
// nothing about why.
std::string describe_unusable_dictionary(const std::filesystem::path &user_directory);

std::string describe_unusable_dictionary();
} // namespace metasequoia::linux_ime
