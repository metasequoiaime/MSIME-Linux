#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::linux_ime
{
// The engine resolves its data directory to the per-user XDG location only, so a
// packaged install leaves the dictionaries where it will never look. These helpers
// seed the user directory from the system copy the package provides.
std::filesystem::path user_data_directory();

std::vector<std::filesystem::path> system_data_directories();

// Copies any dictionary or helpcode file that the user directory is missing.
// Returns the number of files copied; existing user data is never overwritten,
// so learned frequencies and staged upgrades are left alone.
std::size_t seed_user_data(const std::filesystem::path &user_directory,
                           const std::vector<std::filesystem::path> &system_directories, std::string *error = nullptr);

std::size_t seed_user_data(std::string *error = nullptr);
} // namespace metasequoia::linux_ime
