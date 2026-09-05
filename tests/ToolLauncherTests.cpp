#include "ToolLauncher.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;
using metasequoia::linux_ime::running_program_directory;
using metasequoia::linux_ime::tool_path;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

fs::path make_root()
{
    const fs::path root = fs::temp_directory_path() / fs::path("metasequoia-launcher-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_executable(const fs::path &path)
{
    std::ofstream(path) << "#!/bin/sh\n";
    fs::permissions(path, fs::perms::owner_all);
}

void resolves_a_sibling_executable()
{
    const fs::path root = make_root();
    write_executable(root / "metasequoia-ime-tools");

    require(tool_path(root, "metasequoia-ime-tools") == (root / "metasequoia-ime-tools").string(),
            "a sibling executable was not resolved to an absolute path");
    fs::remove_all(root);
}

// Falling back to the bare name keeps PATH working for anyone who does have the
// tools installed somewhere PATH covers.
void falls_back_to_the_bare_name()
{
    const fs::path root = make_root();

    require(tool_path(root, "metasequoia-ime-tools") == "metasequoia-ime-tools",
            "a missing sibling was not left for PATH to resolve");
    require(tool_path(fs::path{}, "metasequoia-ime-tools") == "metasequoia-ime-tools",
            "an unknown program directory was not handled");
    fs::remove_all(root);
}

void ignores_a_sibling_that_is_not_executable()
{
    const fs::path root = make_root();
    std::ofstream(root / "metasequoia-ime-tools") << "not a program\n";
    fs::permissions(root / "metasequoia-ime-tools", fs::perms::owner_read | fs::perms::owner_write);

    require(tool_path(root, "metasequoia-ime-tools") == "metasequoia-ime-tools",
            "a non-executable file was treated as a tool");
    fs::remove_all(root);
}

void ignores_a_directory_with_the_right_name()
{
    const fs::path root = make_root();
    fs::create_directories(root / "metasequoia-ime-tools");

    require(tool_path(root, "metasequoia-ime-tools") == "metasequoia-ime-tools", "a directory was treated as a tool");
    fs::remove_all(root);
}

void finds_the_directory_of_the_running_program()
{
    const fs::path directory = running_program_directory();
    require(!directory.empty(), "the running program directory was not resolved");
    require(fs::is_directory(directory), "the running program directory is not a directory");
}
} // namespace

int main()
{
    resolves_a_sibling_executable();
    falls_back_to_the_bare_name();
    ignores_a_sibling_that_is_not_executable();
    ignores_a_directory_with_the_right_name();
    finds_the_directory_of_the_running_program();
    return 0;
}
