#include "../src/ClipboardHistory.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

using namespace metasequoia::linux_ime;

namespace
{
bool g_fail_fsync = false;
}

// Defining fsync inside the test executable interposes it ahead of the libc symbol for every caller linked into this
// binary, which is the only way to reach the fsync failure branch of the atomic write without a genuinely broken disk.
// With the switch off the call goes straight to the kernel, so every other write in the process still syncs normally.
extern "C" int fsync(int descriptor)
{
    if (g_fail_fsync)
    {
        errno = EIO;
        return -1;
    }
    return static_cast<int>(syscall(SYS_fsync, descriptor));
}

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A password copied from a manager must not reach the store. The manager marks
// it on the clipboard; nothing else tells the two apart.
void skips_content_a_password_manager_marked()
{
    require(ClipboardHistory::marked_sensitive({"UTF8_STRING", "text/plain", "x-kde-passwordManagerHint"}),
            "the KDE password manager hint was not recognised");
    require(ClipboardHistory::marked_sensitive({"org.nspasteboard.ConcealedType"}),
            "the concealed type marker was not recognised");
}

void keeps_ordinary_clipboard_content()
{
    require(!ClipboardHistory::marked_sensitive({"UTF8_STRING", "text/plain;charset=utf-8", "TARGETS"}),
            "ordinary clipboard targets were treated as sensitive");
    require(!ClipboardHistory::marked_sensitive({}), "an empty target list was treated as sensitive");
    // A prefix or suffix is a different target, not the marker.
    require(!ClipboardHistory::marked_sensitive({"x-kde-passwordManagerHintExtra"}),
            "a longer target name was treated as the marker");
    require(!ClipboardHistory::marked_sensitive({"application/x-kde-passwordManagerHint"}),
            "a namespaced lookalike was treated as the marker");
}

// /proc/self/fd is the only place a descriptor that was never closed becomes visible. Where it is not mounted the
// caller falls back to the observable half of the failure path, so the helper reports "cannot tell" rather than "none".
std::optional<std::size_t> open_temporary_history_descriptors()
{
    const std::filesystem::path descriptors("/proc/self/fd");
    std::error_code status_error;
    if (!std::filesystem::is_directory(descriptors, status_error) || status_error)
    {
        return std::nullopt;
    }
    std::error_code iteration_error;
    std::filesystem::directory_iterator entry(descriptors, iteration_error);
    if (iteration_error)
    {
        return std::nullopt;
    }
    const std::filesystem::directory_iterator end;
    std::size_t open_descriptors = 0;
    for (; entry != end; entry.increment(iteration_error))
    {
        if (iteration_error)
        {
            return std::nullopt;
        }
        std::error_code link_error;
        const std::filesystem::path target = std::filesystem::read_symlink(entry->path(), link_error);
        if (link_error)
        {
            continue;
        }
        if (target.string().find("clipboard_history.json.tmp") != std::string::npos)
        {
            ++open_descriptors;
        }
    }
    return open_descriptors;
}

std::size_t count_temporary_files(const std::filesystem::path &directory)
{
    std::error_code iteration_error;
    std::size_t temporary_files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory, iteration_error))
    {
        if (entry.path().filename().string().find("clipboard_history.json.tmp") != std::string::npos)
        {
            ++temporary_files;
        }
    }
    return temporary_files;
}

// dup() hands back the lowest free slot, so a descriptor the atomic write forgot to close shifts the answer upward even
// on a host where /proc is not mounted. A negative result means the probe itself could not run.
int lowest_free_descriptor()
{
    const int probe = dup(STDIN_FILENO);
    if (probe >= 0)
    {
        close(probe);
    }
    return probe;
}

// Turning the feature off has to erase the stored clipboard text before the setting is committed, so that a clear the
// filesystem refuses leaves the config exactly as the caller found it. A store path that is itself a directory makes
// rename() fail for every user including root, which is how the clear is made to fail without relying on permission
// bits a container running as root would ignore.
void keeps_the_setting_untouched_when_disabling_cannot_clear_the_history(const std::filesystem::path &root)
{
    const auto data_directory = root / "unclearable-data";
    std::filesystem::create_directories(data_directory / "clipboard_history.json");
    ClipboardHistory history(data_directory);
    std::string error;
    require(history.set_enabled(true, &error), "Clipboard history could not be enabled for the failed clear case.");
    require(history.enabled(), "Enabling clipboard history did not reach the settings file.");
    error.clear();
    require(!history.set_enabled(false, &error),
            "Disabling reported success even though the stored history could not be cleared.");
    require(!error.empty(), "A failed clear produced no error message.");
    require(history.enabled(),
            "The disabled setting was persisted even though the stored history could not be cleared.");
}

// A write whose fsync fails must still close the descriptor it opened. The caller retries on every clipboard change, so
// a descriptor left open per attempt exhausts the table of a long-running tools process.
void closes_the_temporary_file_when_the_sync_fails(const std::filesystem::path &root)
{
    const auto data_directory = root / "sync-failure-data";
    std::filesystem::create_directories(data_directory);
    ClipboardHistory history(data_directory);
    std::string error;
    require(history.set_enabled(true, &error), "Clipboard history could not be enabled for the sync failure case.");
    require(history.add("payload", &error), "The clipboard item stored before the sync failure was rejected.");

    // One failing write before the measurement settles whatever the settings and JSON layers open lazily, so the two
    // descriptor probes around the retries below only account for what the atomic write itself left behind.
    g_fail_fsync = true;
    require(!history.add("probe-warmup", &error), "A write whose fsync failed was reported as success.");
    require(!error.empty(), "A failed atomic replacement produced no error message.");
    const int free_descriptor_before = lowest_free_descriptor();
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        require(!history.add("probe-" + std::to_string(attempt), &error),
                "A write whose fsync failed was reported as success.");
    }
    const int free_descriptor_after = lowest_free_descriptor();
    g_fail_fsync = false;

    if (free_descriptor_before >= 0 && free_descriptor_after >= 0)
    {
        require(free_descriptor_after == free_descriptor_before,
                "Clipboard history writes that failed to sync leaked their file descriptors.");
    }
    const auto leaked = open_temporary_history_descriptors();
    require(!leaked.has_value() || *leaked == 0,
            "A clipboard history temporary file was still open after its write failed.");
    require(count_temporary_files(data_directory) == 0,
            "A clipboard history temporary file was left behind after its write failed.");
    const auto items = history.load(&error);
    require(items.size() == 1 && items.front() == "payload",
            "A clipboard history write that failed to sync still changed the stored history.");
}
} // namespace

int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() / "metasequoia-clipboard-history-tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);

        ClipboardHistory history;
        std::string error;
        require(history.set_enabled(true, &error), "Clipboard history could not be enabled.");
        require(history.add("first\r", &error), "The first clipboard item was not stored.");
        require(history.add("second", &error), "The second clipboard item was not stored.");
        require(!history.add("second", &error), "A duplicate clipboard item was stored twice.");
        auto items = history.load(&error);
        require(items.size() == 2 && items[0] == "second" && items[1] == "first",
                "Clipboard history ordering or normalization was incorrect.");

        const std::string long_text(5000, 'x');
        require(history.add(long_text, &error), "A long clipboard item was not accepted.");
        items = history.load(&error);
        require(items.front().size() == ClipboardHistory::kMaxChars,
                "Clipboard history did not apply its maximum size.");
        require(history.remove("second", &error), "A clipboard item could not be removed.");
        require(history.clear(&error), "Clipboard history could not be cleared.");
        require(history.load(&error).empty(), "Clipboard history remained after clearing.");
        require(history.set_enabled(false, &error), "Clipboard history could not be disabled.");
        require(!history.add("disabled", &error), "Disabled clipboard history accepted a new item.");

        skips_content_a_password_manager_marked();
        keeps_ordinary_clipboard_content();
        keeps_the_setting_untouched_when_disabling_cannot_clear_the_history(root);
        closes_the_temporary_file_when_the_sync_fails(root);

        std::filesystem::remove_all(root);
        std::cout << "Clipboard history tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
