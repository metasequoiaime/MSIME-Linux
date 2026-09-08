#pragma once
#include <filesystem>
#include <string>

typedef struct _GCancellable GCancellable;
namespace metasequoia::linux_ime::account
{
struct NativeResources
{
    std::filesystem::path directory;
    std::string content_id;
};
// Background-only bridge to the installed pinned verifier. All paths are absolute;
// tool_directory contains native_resources.py and native-resource-lock.json. The
// caller chooses the trusted installation, never a tool supplied by a snapshot.
// Cancellation terminates/reaps the helper. No user journal or active marker is changed.
NativeResources prepare_native_resources(const std::filesystem::path &tool_directory,
                                         const std::filesystem::path &source,
                                         const std::filesystem::path &resources_root,
                                         GCancellable *cancelled = nullptr);
} // namespace metasequoia::linux_ime::account
