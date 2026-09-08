#include "account/DictionaryExport.h"
#include "account/BackendAccountClient.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
using namespace metasequoia::linux_ime::account;
namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
std::string read(const std::filesystem::path &path)
{
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
template <class F> void fails(F f, long status)
{
    try
    {
        f();
    }
    catch (const Failure &e)
    {
        require(e.status() == status, "wrong export failure");
        return;
    }
    throw std::runtime_error("export unexpectedly succeeded");
}
} // namespace
int main()
{
    char pattern[] = "/tmp/msime-export-test-XXXXXX";
    auto *directory = mkdtemp(pattern);
    require(directory, "temp directory failed");
    const std::filesystem::path root(directory);
    const auto destination = root / "中文词库.tsv";
    const std::string text = "测试\ttest\t10\n";
    save_dictionary_export(destination.string(), text);
    require(read(destination) == text, "export content changed");
    struct stat info
    {
    };
    require(stat(destination.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "export permissions wrong");
    fails([&] { save_dictionary_export(destination.string(), "replace"); }, 409);
    require(read(destination) == text, "existing export overwritten");
    const auto alias = root / "alias.tsv";
    std::filesystem::create_symlink(destination, alias);
    fails([&] { save_dictionary_export(alias.string(), "replace"); }, 409);
    require(read(destination) == text, "symlink target overwritten");
    int calls = 0;
    fails([&] { save_dictionary_export((root / "cancelled.tsv").string(), text, [&] { return ++calls >= 3; }); }, 0);
    require(!std::filesystem::exists(root / "cancelled.tsv"), "cancelled export published");
    save_dictionary_export((root / "empty.tsv").string(), "");
    require(std::filesystem::file_size(root / "empty.tsv") == 0, "empty export failed");
    const std::string streamed(70000, 'x');
    std::size_t supplied = 0;
    save_dictionary_export((root / "stream.ndjson").string(), streamed.size(),
                           [&](std::size_t offset, char *data, std::size_t capacity) {
                               require(offset == supplied, "stream offset changed");
                               const auto size = std::min<std::size_t>(capacity, 997);
                               std::memcpy(data, streamed.data() + offset, size);
                               supplied += size;
                               return size;
                           });
    require(read(root / "stream.ndjson") == streamed, "streamed export changed");
    fails(
        [&] {
            save_dictionary_export((root / "short.ndjson").string(), 10,
                                   [](std::size_t, char *, std::size_t) { return std::size_t(0); });
        },
        400);
    fails(
        [&] {
            save_dictionary_export((root / "long.ndjson").string(), 10,
                                   [](std::size_t, char *, std::size_t capacity) { return capacity + 1; });
        },
        400);
    fails(
        [&] {
            save_dictionary_export((root / "failed.ndjson").string(), 10,
                                   [](std::size_t, char *, std::size_t) -> std::size_t { throw Failure(0); });
        },
        0);
    for (const char *name : {"short.ndjson", "long.ndjson", "failed.ndjson"})
        require(!std::filesystem::exists(root / name), "failed stream published file");
    bool won[2] = {false, false};
    auto writer = [&](int index) {
        try
        {
            save_dictionary_export((root / "race.tsv").string(), std::to_string(index));
            won[index] = true;
        }
        catch (const Failure &e)
        {
            require(e.status() == 409, "race failed unexpectedly");
        }
    };
    std::thread first(writer, 0), second(writer, 1);
    first.join();
    second.join();
    require(won[0] != won[1], "concurrent exports both overwrote target");
    for (const auto &entry : std::filesystem::directory_iterator(root))
        require(entry.path().filename().string().find(".msime-export-") != 0, "temporary export remained");
    std::filesystem::remove_all(root);
    std::cout << "Atomic dictionary export, cancellation and no-overwrite tests passed\n";
}
