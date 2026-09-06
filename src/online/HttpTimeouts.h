#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>

namespace metasequoia::linux_ime::online
{
// Network deadlines a provider applies to every request it builds. The defaults mirror the HttpRequest defaults so a
// provider constructed without configured values behaves exactly as before.
struct HttpTimeouts
{
    std::chrono::milliseconds connect{2500};
    std::chrono::milliseconds total{8000};
};

// The deadlines a provider reads on every request rather than copying once when it is constructed. connect-timeout-ms
// and total-timeout-ms are settings the user can change while the engine is running, and re-applying a captured copy
// would mean replacing the providers, which means joining a worker that may be inside a request of up to `total` -- a
// wait the IBus main loop cannot take. Reading them per request is what lets a live settings reload change them
// instead of leaving them to the next start.
//
// Worker threads read while the main loop writes, so the pair is packed into a single atomic word: read as two
// independent values a request could take the connect deadline of one configuration together with the total deadline
// of another, which is a combination the user never chose. Milliseconds are stored as 32-bit counts, three orders of
// magnitude above the largest value SettingsStore accepts.
class HttpTimeoutsHandle
{
  public:
    HttpTimeoutsHandle() = default;
    explicit HttpTimeoutsHandle(HttpTimeouts timeouts) : packed_(pack(timeouts))
    {
    }

    HttpTimeouts get() const
    {
        return unpack(packed_.load(std::memory_order_relaxed));
    }

    void set(HttpTimeouts timeouts)
    {
        packed_.store(pack(timeouts), std::memory_order_relaxed);
    }

    // Deadlines that never change, for the callers that have no settings to re-apply: tests, and any provider built
    // outside the engine. Const, so the shape of the pointer says the values are fixed.
    static std::shared_ptr<const HttpTimeoutsHandle> fixed(HttpTimeouts timeouts)
    {
        return std::make_shared<const HttpTimeoutsHandle>(timeouts);
    }

  private:
    // A deadline this class was handed must come back out as the same deadline or not at all, so a value outside the
    // 32-bit range saturates rather than wrapping into a far shorter one.
    static std::uint32_t to_milliseconds(std::chrono::milliseconds value)
    {
        constexpr std::int64_t maximum = std::numeric_limits<std::uint32_t>::max();
        const std::int64_t count = value.count();
        return static_cast<std::uint32_t>(count < 0 ? 0 : (count > maximum ? maximum : count));
    }

    static std::uint64_t pack(HttpTimeouts timeouts)
    {
        return (static_cast<std::uint64_t>(to_milliseconds(timeouts.connect)) << 32) |
               static_cast<std::uint64_t>(to_milliseconds(timeouts.total));
    }

    static HttpTimeouts unpack(std::uint64_t packed)
    {
        return {std::chrono::milliseconds(static_cast<std::uint32_t>(packed >> 32)),
                std::chrono::milliseconds(static_cast<std::uint32_t>(packed & 0xffffffffU))};
    }

    std::atomic<std::uint64_t> packed_{pack(HttpTimeouts{})};
};

// The handle a provider falls back to when it is built without one, so a request never has to test for a null handle.
inline std::shared_ptr<const HttpTimeoutsHandle> default_http_timeouts()
{
    return HttpTimeoutsHandle::fixed(HttpTimeouts{});
}
} // namespace metasequoia::linux_ime::online
