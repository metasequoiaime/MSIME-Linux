#include "ToolLauncher.h"

#include <system_error>

namespace metasequoia::linux_ime
{
std::filesystem::path running_program_directory()
{
    std::error_code code;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", code);
    if (code || self.empty())
    {
        return {};
    }
    return self.parent_path();
}

std::string tool_path(const std::filesystem::path &directory, const std::string &program)
{
    if (directory.empty())
    {
        return program;
    }
    const std::filesystem::path candidate = directory / program;
    std::error_code code;
    if (!std::filesystem::is_regular_file(candidate, code) || code)
    {
        return program;
    }
    if ((std::filesystem::status(candidate, code).permissions() & std::filesystem::perms::owner_exec) ==
        std::filesystem::perms::none)
    {
        return program;
    }
    return candidate.string();
}

std::string tool_path(const std::string &program)
{
    return tool_path(running_program_directory(), program);
}
} // namespace metasequoia::linux_ime
