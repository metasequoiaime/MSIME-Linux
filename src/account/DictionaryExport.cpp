#include "DictionaryExport.h"
#include "BackendAccountClient.h"
#include <filesystem>
#include <vector>
#include <cerrno>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cstring>
namespace metasequoia::linux_ime::account
{
namespace
{
struct Temporary
{
    int descriptor = -1;
    std::vector<char> path;
    ~Temporary()
    {
        if (descriptor >= 0)
            close(descriptor);
        if (!path.empty())
            unlink(path.data());
    }
};
} // namespace
void save_dictionary_export(const std::string &destination, const std::string &text,
                            const online::CancellationCheck &cancelled)
{
    save_dictionary_export(
        destination, text.size(),
        [&](std::size_t offset, char *data, std::size_t capacity) {
            const auto count = std::min(capacity, text.size() - offset);
            std::memcpy(data, text.data() + offset, count);
            return count;
        },
        cancelled);
}
void save_dictionary_export(const std::string &destination, std::size_t size, const online::HttpBodySource &source,
                            const online::CancellationCheck &cancelled)
{
    if (!source || size > 512U * 1024U * 1024U)
        throw Failure(400);
    const auto check = [&] {
        if (cancelled && cancelled())
            throw Failure(0, true);
    };
    check();
    if (destination.empty() || destination.find('\0') != std::string::npos)
        throw Failure(400);
    const std::filesystem::path target(destination);
    if (target.filename().empty())
        throw Failure(400);
    auto parent = target.parent_path();
    if (parent.empty())
        parent = ".";
    const auto pattern = (parent / ".msime-export-XXXXXX").string();
    Temporary temporary;
    temporary.path.assign(pattern.begin(), pattern.end());
    temporary.path.push_back('\0');
    temporary.descriptor = mkstemp(temporary.path.data());
    if (temporary.descriptor < 0)
    {
        temporary.path.clear();
        throw Failure(0);
    }
    std::array<char, 16384> buffer{};
    std::size_t offset = 0;
    while (offset < size)
    {
        check();
        const auto capacity = std::min(buffer.size(), size - offset);
        const auto count = source(offset, buffer.data(), capacity);
        if (!count || count > capacity)
            throw Failure(400);
        std::size_t written = 0;
        while (written < count)
        {
            check();
            const auto amount = write(temporary.descriptor, buffer.data() + written, count - written);
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0)
                throw Failure(0);
            written += static_cast<std::size_t>(amount);
        }
        offset += count;
    }
    if (fsync(temporary.descriptor) != 0)
        throw Failure(0);
    check();
    // link() supplies an atomic no-replace commit, including against symlinks.
    if (link(temporary.path.data(), destination.c_str()) != 0)
        throw Failure(errno == EEXIST ? 409 : 0);
}
} // namespace metasequoia::linux_ime::account
