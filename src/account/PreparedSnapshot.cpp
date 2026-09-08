#include "PreparedSnapshot.h"
#include "BackendAccountClient.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <glib.h>
#include <streambuf>
#include <sys/stat.h>
#include <unistd.h>
namespace metasequoia::linux_ime::account
{
namespace
{
constexpr std::size_t maximum_size = 512U * 1024U * 1024U;
void check_cancelled(const online::CancellationCheck &cancelled)
{
    if (cancelled && cancelled())
        throw Failure(0, true);
}
struct Descriptor
{
    int value;
    ~Descriptor()
    {
        if (value >= 0)
            ::close(value);
    }
};
class SnapshotBuffer final : public std::streambuf
{
  public:
    SnapshotBuffer(const PreparedSnapshot &snapshot, const online::CancellationCheck &cancelled)
        : snapshot_(snapshot), cancelled_(cancelled)
    {
    }

  protected:
    int_type underflow() override
    {
        const auto count = snapshot_.read(offset_, buffer_.data(), buffer_.size(), cancelled_);
        if (!count)
            return traits_type::eof();
        offset_ += count;
        setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
        return traits_type::to_int_type(*gptr());
    }

  private:
    const PreparedSnapshot &snapshot_;
    const online::CancellationCheck &cancelled_;
    std::size_t offset_ = 0;
    std::array<char, 16384> buffer_{};
};
} // namespace
PreparedSnapshot::~PreparedSnapshot()
{
    ::close(descriptor_);
}
std::size_t PreparedSnapshot::read(std::size_t offset, char *buffer, std::size_t capacity,
                                   const online::CancellationCheck &cancelled) const
{
    check_cancelled(cancelled);
    if (offset > size_ || (!buffer && capacity))
        throw Failure(400);
    capacity = std::min(capacity, size_ - offset);
    if (!capacity)
        return 0;
    for (;;)
    {
        check_cancelled(cancelled);
        const auto count = ::pread(descriptor_, buffer, capacity, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw Failure(0);
        return static_cast<std::size_t>(count);
    }
}
std::unique_ptr<PreparedSnapshot> PreparedSnapshot::open(const std::string &source,
                                                         const online::CancellationCheck &cancelled)
{
    check_cancelled(cancelled);
    if (source.empty() || source.find('\0') != std::string::npos)
        throw Failure(400);
    Descriptor input{::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat info
    {
    };
    if (input.value < 0 || ::fstat(input.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > maximum_size)
        throw Failure(400);
    gchar *path = g_build_filename(g_get_tmp_dir(), "msime-snapshot-XXXXXX", nullptr);
    const int descriptor = g_mkstemp_full(path, O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0)
    {
        g_free(path);
        throw Failure(0);
    }
    // Remove the name immediately. Cleanup cannot leave a portable dictionary behind.
    const auto removed = ::unlink(path);
    g_free(path);
    if (removed != 0)
    {
        ::close(descriptor);
        throw Failure(0);
    }
    Descriptor owned{descriptor};
    std::unique_ptr<PreparedSnapshot> result(new PreparedSnapshot(descriptor));
    owned.value = -1;
    std::array<char, 16384> buffer{};
    for (;;)
    {
        check_cancelled(cancelled);
        const auto count = ::read(input.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            throw Failure(0);
        if (count == 0)
            break;
        if (static_cast<std::size_t>(count) > maximum_size - result->size_)
            throw Failure(400);
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(count))
        {
            check_cancelled(cancelled);
            const auto amount = ::write(descriptor, buffer.data() + written, count - written);
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0)
                throw Failure(0);
            written += static_cast<std::size_t>(amount);
        }
        result->size_ += static_cast<std::size_t>(count);
    }
    SnapshotBuffer stream_buffer(*result, cancelled);
    std::istream frozen(&stream_buffer);
    // Preserve cancellation/I/O failures raised by the stream buffer.
    frozen.exceptions(std::ios::badbit);
    result->envelope_ = inspect_snapshot_envelope(frozen, cancelled);
    return result;
}
} // namespace metasequoia::linux_ime::account
