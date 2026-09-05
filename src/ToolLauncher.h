#pragma once

#include <filesystem>
#include <string>

namespace metasequoia::linux_ime
{
// The desktop tools live next to whichever executable wants to start one, both
// in the build tree and after scripts/install.sh puts them in the user prefix.
// Resolving them through PATH breaks the current-user install, because neither
// the GNOME session nor the systemd user manager carries ~/.local/bin in PATH,
// so every button that opens another tool silently does nothing.
//
// This lives here rather than in one of the GUIs because the same mistake was
// made twice: once in the floating toolbar and once in the settings window.
std::filesystem::path running_program_directory();

// Returns an absolute path to program when it sits next to the running
// executable, and program unchanged otherwise so PATH can still resolve it.
std::string tool_path(const std::filesystem::path &directory, const std::string &program);

std::string tool_path(const std::string &program);
} // namespace metasequoia::linux_ime
