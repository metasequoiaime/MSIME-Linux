#include "NativeResources.h"
#include "BackendAccountClient.h"
#include <gio/gio.h>
#include <algorithm>
#include <array>

namespace metasequoia::linux_ime::account
{
namespace
{
void check_cancelled(GCancellable *cancelled)
{
    if (cancelled && g_cancellable_is_cancelled(cancelled))
        throw Failure(0, true);
}
void check_path(const std::filesystem::path &path)
{
    const auto value = path.string();
    if (!path.is_absolute() || value.find_first_of(std::string("\0\n\r", 3)) != std::string::npos ||
        !g_utf8_validate(value.data(), value.size(), nullptr))
        throw Failure(400);
}
struct Child
{
    GSubprocess *process;
    bool reaped = false;
    ~Child()
    {
        if (!reaped)
        {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
        g_object_unref(process);
    }
};
} // namespace
NativeResources prepare_native_resources(const std::filesystem::path &tool_directory,
                                         const std::filesystem::path &source,
                                         const std::filesystem::path &resources_root, GCancellable *cancelled)
{
    check_cancelled(cancelled);
    for (const auto &path : {tool_directory, source, resources_root})
        check_path(path);
    const auto root = resources_root.lexically_normal();
    const auto script = tool_directory / "native_resources.py";
    const auto lock = tool_directory / "native-resource-lock.json";
    // Ignore Python environment overrides/user site packages. Keep the script's
    // directory on sys.path so its pinned public validator modules can be imported.
    auto *process = g_subprocess_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr,
        METASEQUOIA_NATIVE_RESOURCE_PYTHON, "-E", "-s", script.c_str(), "prepare", source.c_str(), root.c_str(),
        lock.c_str(), nullptr);
    if (!process)
        throw Failure(500);
    Child child{process};
    std::array<char, 8193> output{};
    gsize count = 0;
    if (!g_input_stream_read_all(g_subprocess_get_stdout_pipe(process), output.data(), output.size(), &count, cancelled,
                                 nullptr))
    {
        check_cancelled(cancelled);
        throw Failure(500);
    }
    if (count == output.size())
        throw Failure(500);
    if (!g_subprocess_wait(process, cancelled, nullptr))
    {
        check_cancelled(cancelled);
        throw Failure(500);
    }
    child.reaped = true;
    check_cancelled(cancelled);
    if (!g_subprocess_get_successful(process))
        throw Failure(500);
    std::string path(output.data(), count);
    if (path.empty() || path.back() != '\n')
        throw Failure(500);
    path.pop_back();
    const std::filesystem::path directory(path);
    const auto content_id = directory.filename().string();
    if (directory.parent_path() != root || content_id.size() != 64 ||
        !std::all_of(content_id.begin(), content_id.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw Failure(500);
    return {directory, content_id};
}
} // namespace metasequoia::linux_ime::account
