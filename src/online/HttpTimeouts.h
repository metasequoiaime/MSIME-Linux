#pragma once

#include <chrono>

namespace metasequoia::linux_ime::online
{
// Network deadlines a provider applies to every request it builds. The defaults mirror the HttpRequest defaults so a
// provider constructed without configured values behaves exactly as before.
struct HttpTimeouts
{
    std::chrono::milliseconds connect{2500};
    std::chrono::milliseconds total{8000};
};
} // namespace metasequoia::linux_ime::online
