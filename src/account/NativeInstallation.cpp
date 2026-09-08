#include "NativeInstallation.h"
#include "contracts/assets/assets.h"
#include <random>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace metasequoia::linux_ime::account
{
namespace
{
constexpr const char *marker = "active-dictionary";
const std::string prefix = "msime-active-dictionary-v1\n";
struct Descriptor
{
    int value;
    ~Descriptor()
    {
        if (value >= 0)
            close(value);
    }
};
void require(bool value)
{
    if (!value)
        throw std::runtime_error("Invalid or unavailable native dictionary installation");
}
void identifier(const std::string &value)
{
    require(!value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
               ch == '-';
    }));
}
int directory(const std::filesystem::path &path)
{
    Descriptor fd{open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info
    {
    };
    require(fd.value >= 0 && fstat(fd.value, &info) == 0 && info.st_uid == geteuid() && (info.st_mode & 0022) == 0);
    const int result = fd.value;
    fd.value = -1;
    return result;
}
std::optional<std::string> read_file(int parent, const char *name, bool private_file)
{
    Descriptor fd{openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (fd.value < 0 && errno == ENOENT)
        return {};
    struct stat info
    {
    };
    require(fd.value >= 0 && fstat(fd.value, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
            info.st_nlink == 1 && (!private_file || (info.st_mode & 0077) == 0) && info.st_size > 0 &&
            info.st_size <= 512);
    std::string value;
    char buffer[513];
    for (;;)
    {
        const auto count = read(fd.value, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        require(count >= 0);
        if (!count)
            break;
        value.append(buffer, static_cast<std::size_t>(count));
        require(value.size() <= 512);
    }
    require(value.size() == static_cast<std::size_t>(info.st_size));
    return value;
}
void require_data_file(int parent, const char *name)
{
    Descriptor file{openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    struct stat info
    {
    };
    require(file.value >= 0 && fstat(file.value, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
            info.st_nlink == 1 && info.st_size > 0);
}
std::pair<std::string, std::string> decode(const std::string &value)
{
    require(value.compare(0, prefix.size(), prefix) == 0);
    const auto end = value.find('\n', prefix.size());
    require(end != std::string::npos && value.back() == '\n');
    auto first = value.substr(prefix.size(), end - prefix.size());
    auto second = value.substr(end + 1, value.size() - end - 2);
    identifier(first);
    identifier(second);
    return {first, second};
}
} // namespace
NativeInstallation::NativeInstallation(std::filesystem::path root, RuntimePaths legacy)
    : root_(std::move(root)), legacy_(std::move(legacy))
{
    require(root_.is_absolute() && root_.native().find('\0') == std::string::npos);
    legacy_.validate();
    std::filesystem::create_directories(root_);
    Descriptor owned{directory(root_)};
    for (const auto *name : {"resources", "generations"})
    {
        const auto path = root_ / name;
        std::filesystem::create_directory(path);
        Descriptor child{directory(path)};
    }
}
std::filesystem::path NativeInstallation::resources(const std::string &content_id) const
{
    identifier(content_id);
    return root_ / "resources" / content_id;
}
std::filesystem::path NativeInstallation::generation(const std::string &name) const
{
    identifier(name);
    return root_ / "generations" / name;
}
RuntimePaths NativeInstallation::prepared_paths(const std::string &name, const std::string &content_id) const
{
    const auto folder = generation(name);
    RuntimePaths paths{resources(content_id), folder / "user", folder / "cache",
                       folder / "user" / "dictionaries" / content_id};
    paths.validate();
    for (const auto &path : {root_ / "resources", root_ / "generations", paths.resources, folder, paths.user_data,
                             paths.cache, paths.user_data / "dictionaries", paths.dictionaries})
    {
        Descriptor checked{directory(path)};
    }
    Descriptor dictionaries{directory(paths.dictionaries)};
    const auto ready = read_file(dictionaries.value, ".ready", false);
    require(ready && *ready == content_id + "\n");
    require_data_file(dictionaries.value, assets::main_dictionary);
    require_data_file(dictionaries.value, assets::english_dictionary);
    Descriptor user{directory(paths.user_data)};
    require_data_file(user.value, assets::user_journal);
    return paths;
}
ActiveDictionary NativeInstallation::active() const
{
    Descriptor root{directory(root_)};
    const auto value = read_file(root.value, marker, true);
    if (!value)
        return {legacy_, {}, {}, {}};
    const auto names = decode(*value);
    return {prepared_paths(names.first, names.second), names.first, names.second, *value};
}
NativePublication NativeInstallation::publish(const std::string &name, const std::string &content_id,
                                              const std::string &expected_token) const
{
    (void)prepared_paths(name, content_id);
    Descriptor root{directory(root_)};
    const auto current = read_file(root.value, marker, true);
    if (current)
        (void)decode(*current);
    if (current.value_or("") != expected_token)
        return {};
    const std::string contents = prefix + name + "\n" + content_id + "\n";
    std::random_device random;
    const std::string temporary = ".active-" + std::to_string(random()) + "-" + std::to_string(random()) + "-" +
                                  std::to_string(random()) + "-" + std::to_string(random());
    Descriptor output{
        openat(root.value, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    require(output.value >= 0);
    struct Cleanup
    {
        int root;
        const std::string &name;
        ~Cleanup()
        {
            unlinkat(root, name.c_str(), 0);
        }
    } cleanup{root.value, temporary};
    std::size_t offset = 0;
    while (offset < contents.size())
    {
        const auto count = write(output.value, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        require(count > 0);
        offset += static_cast<std::size_t>(count);
    }
    require(fsync(output.value) == 0);
    require(renameat(root.value, temporary.c_str(), root.value, marker) == 0);
    return {true, fsync(root.value) == 0};
}
} // namespace metasequoia::linux_ime::account
