#include "account/PreparedSnapshot.h"
#include "account/BackendAccountClient.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace metasequoia::linux_ime::account;
namespace fs = std::filesystem;
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
std::string fixture()
{
    const std::string body =
        "{\"type\":\"header\",\"format\":\"msime-dictionary-snapshot\",\"version\":1,\"revision\":7}\n";
    auto *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    auto result = body + "{\"type\":\"footer\",\"records\":1,\"sha256\":\"" + hash + "\"}\n";
    g_free(hash);
    return result;
}
void write(const fs::path &path, const std::string &data)
{
    std::ofstream out(path);
    out << data;
    require(bool(out), "write failed");
}
std::size_t descriptors()
{
    return std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{});
}
std::string read(const PreparedSnapshot &snapshot)
{
    std::string result;
    char buffer[7];
    for (;;)
    {
        const auto count = snapshot.read(result.size(), buffer, sizeof(buffer));
        if (!count)
            return result;
        result.append(buffer, count);
    }
}
template <class F> void rejected(F action, bool cancellation = false)
{
    const auto before = descriptors();
    bool caught = false;
    try
    {
        action();
    }
    catch (const Failure &error)
    {
        caught = error.cancelled() == cancellation;
    }
    require(caught, "invalid snapshot not rejected with expected reason");
    require(descriptors() == before, "descriptor leaked on failure");
}
} // namespace
int main()
{
    gchar *root = g_dir_make_tmp("msime-prepared-test-XXXXXX", nullptr);
    require(root, "temporary directory failed");
    const fs::path directory(root);
    g_free(root);
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            fs::remove_all(path);
        }
    } cleanup{directory};
    const auto source = directory / "词库.ndjson";
    const auto original = fixture();
    write(source, original);
    const auto before = descriptors();
    {
        auto snapshot = PreparedSnapshot::open(source.string());
        require(snapshot->envelope().revision == 7 && snapshot->size() == original.size(), "wrong metadata");
        write(source, "replaced after review");
        require(read(*snapshot) == original, "prepared snapshot reread changed source");
        fs::remove(source);
        require(read(*snapshot) == original, "deleting source invalidated prepared snapshot");
        require(snapshot->read(snapshot->size(), nullptr, 0) == 0, "EOF read failed");
        rejected([&] { snapshot->read(snapshot->size() + 1, nullptr, 0); });
        rejected(
            [&] {
                char value;
                snapshot->read(0, &value, 1, [] { return true; });
            },
            true);
    }
    require(descriptors() == before, "prepared descriptor not closed");
    {
        auto received = PreparedSnapshot::receive([&](const metasequoia::linux_ime::online::HttpResponseSink &sink) {
            require(sink(original.data(), 17), "first chunk rejected");
            require(sink(original.data() + 17, original.size() - 17), "second chunk rejected");
        });
        require(read(*received) == original, "downloaded content changed");
    }
    require(descriptors() == before, "received descriptor leaked");
    rejected([&] { PreparedSnapshot::receive({}); });
    rejected([&] { PreparedSnapshot::receive([](const auto &sink) { sink("x", 512U * 1024U * 1024U + 1); }); });
    rejected([&] { PreparedSnapshot::receive([](const auto &sink) { sink(nullptr, 1); }); });
    rejected([&] {
        PreparedSnapshot::receive([&](const auto &sink) {
            sink(original.data(), 17);
            throw Failure(503);
        });
    });
    rejected([&] { PreparedSnapshot::receive([&](const auto &sink) { sink(original.data(), 17); }); });
    rejected([&] { PreparedSnapshot::open(source.string()); });
    write(source, original);
    const auto link = directory / "link";
    fs::create_symlink(source, link);
    rejected([&] { PreparedSnapshot::open(link.string()); });
    rejected([&] { PreparedSnapshot::open(directory.string()); });
    const auto pipe = directory / "pipe";
    require(::mkfifo(pipe.c_str(), 0600) == 0, "FIFO failed");
    rejected([&] { PreparedSnapshot::open(pipe.string()); });
    rejected([&] { PreparedSnapshot::open(source.string() + std::string("\0suffix", 7)); });
    rejected(
        [&] {
            int calls = 0;
            PreparedSnapshot::open(source.string(), [&] { return ++calls >= 3; });
        },
        true);
    rejected(
        [&] {
            int calls = 0;
            PreparedSnapshot::open(source.string(), [&] { return ++calls >= 6; });
        },
        true);
    write(source, "corrupt");
    rejected([&] { PreparedSnapshot::open(source.string()); });
    require(::truncate(source.c_str(), 512LL * 1024 * 1024 + 1) == 0, "sparse oversized fixture failed");
    rejected([&] { PreparedSnapshot::open(source.string()); });
    std::cout << "Private snapshot freezing, independent reads, rejection and cleanup passed\n";
}
