#include "DictionaryExport.h"
#include "BackendAccountClient.h"
#include <filesystem>
#include <vector>
#include <cerrno>
#include <unistd.h>
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
    std::size_t offset = 0;
    while (offset < text.size())
    {
        check();
        const auto count = write(temporary.descriptor, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw Failure(0);
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(temporary.descriptor) != 0)
        throw Failure(0);
    check();
    // link() supplies an atomic no-replace commit, including against symlinks.
    if (link(temporary.path.data(), destination.c_str()) != 0)
        throw Failure(errno == EEXIST ? 409 : 0);
}
} // namespace metasequoia::linux_ime::account
