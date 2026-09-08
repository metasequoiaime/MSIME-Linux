#include "DictionaryLease.h"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace metasequoia::linux_ime
{
namespace
{
struct Descriptor
{
    int value;
    ~Descriptor()
    {
        if (value >= 0)
            close(value);
    }
};
} // namespace
std::unique_ptr<DictionaryLease> DictionaryLease::acquire(const std::filesystem::path &directory, Mode mode)
{
    if (!directory.is_absolute() || directory.native().find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid dictionary coordination directory");
    std::filesystem::create_directories(directory);
    Descriptor root{open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    struct stat info
    {
    };
    if (root.value < 0 || fstat(root.value, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0022) != 0)
        throw std::runtime_error("Unsafe dictionary coordination directory");
    Descriptor lock{
        openat(root.value, "dictionary-sessions.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600)};
    if (lock.value < 0 || fstat(lock.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        info.st_nlink != 1 || (info.st_mode & 0077) != 0)
        throw std::runtime_error("Unsafe dictionary session lock");
    int result;
    do
    {
        result = flock(lock.value, (mode == Mode::Shared ? LOCK_SH : LOCK_EX) | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return {};
        throw std::runtime_error("Cannot acquire dictionary session lock");
    }
    auto lease = std::unique_ptr<DictionaryLease>(new DictionaryLease(lock.value));
    lock.value = -1;
    return lease;
}
DictionaryLease::~DictionaryLease()
{
    // close releases this open-file-description's flock. Do not unlink the stable
    // lock inode: new sessions must never acquire a different lock concurrently.
    close(descriptor_);
}
} // namespace metasequoia::linux_ime
