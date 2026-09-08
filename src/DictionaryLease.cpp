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
int lock_file(int fd, int flags)
{
    int result;
    do
    {
        result = flock(fd, flags);
    } while (result != 0 && errno == EINTR);
    return result;
}
bool try_lock(int fd, int flags)
{
    if (lock_file(fd, flags | LOCK_NB) == 0)
        return true;
    if (errno == EWOULDBLOCK || errno == EAGAIN)
        return false;
    throw std::runtime_error("Cannot acquire dictionary session lock");
}
void checked_lock(int fd, int flags)
{
    if (lock_file(fd, flags) != 0)
        throw std::runtime_error("Cannot restore dictionary session lock");
}
int open_lock(int root, const char *name)
{
    Descriptor file{openat(root, name, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600)};
    struct stat info
    {
    };
    if (file.value < 0 || fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        info.st_nlink != 1 || (info.st_mode & 0077) != 0)
        throw std::runtime_error("Unsafe dictionary session lock");
    const int result = file.value;
    file.value = -1;
    return result;
}
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
    Descriptor gate{open_lock(root.value, "dictionary-publication.lock")};
    if (!try_lock(gate.value, mode == Mode::Shared ? LOCK_SH : LOCK_EX))
        return {};
    Descriptor lock{open_lock(root.value, "dictionary-sessions.lock")};
    if (!try_lock(lock.value, mode == Mode::Shared ? LOCK_SH : LOCK_EX))
        return {};
    if (mode == Mode::Shared)
        checked_lock(gate.value, LOCK_UN);
    auto lease = std::unique_ptr<DictionaryLease>(new DictionaryLease(lock.value, gate.value, mode));
    lock.value = gate.value = -1;
    return lease;
}
bool DictionaryLease::exclusively(const std::function<void()> &operation)
{
    if (mode_ != Mode::Shared || publishing_ || !operation)
        throw std::logic_error("Invalid dictionary publication operation");
    if (!try_lock(gate_, LOCK_EX))
        return false;
    publishing_ = true;
    // All publishers and new readers pass the gate, so nobody can take an
    // exclusive session lock during conversion or restoration of our sharing.
    auto restore = [&] {
        checked_lock(descriptor_, LOCK_SH);
        checked_lock(gate_, LOCK_UN);
        publishing_ = false;
    };
    try
    {
        checked_lock(descriptor_, LOCK_UN);
        if (!try_lock(descriptor_, LOCK_EX))
        {
            restore();
            return false;
        }
        operation();
    }
    catch (...)
    {
        restore();
        throw;
    }
    restore();
    return true;
}

DictionaryLease::~DictionaryLease()
{
    // close releases this open-file-description's flock. Do not unlink the stable
    // lock inode: new sessions must never acquire a different lock concurrently.
    close(descriptor_);
    close(gate_);
}
} // namespace metasequoia::linux_ime
