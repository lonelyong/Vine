#include <vine/vsg/core/AllocationGate.hpp>

#if defined(__GLIBC__)
#    include <malloc.h>
#endif

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#    define VINE_HAVE_MALLINFO2 1
#endif

namespace
{

/// @brief Reads the process' heap usage, or reports that this platform cannot.
struct HeapUsage
{
    bool        supported{false};
    std::size_t bytes{0};
};

/// @brief The platform's heap-usage reading (see the header: unsupported is reported, never faked).
HeapUsage heapUsage() noexcept
{
#if defined(VINE_HAVE_MALLINFO2)
    const struct mallinfo2 info = mallinfo2();
    return HeapUsage{true, static_cast<std::size_t>(info.uordblks)};
#elif defined(__GLIBC__)
    // glibc < 2.33: the old counters are `int`, so they saturate on a heap past 2 GB. Still worth
    // having on those systems, and the saturation is why mallinfo2 is preferred where it exists.
    const struct mallinfo info = mallinfo();
    return HeapUsage{true, static_cast<std::size_t>(info.uordblks)};
#else
    return HeapUsage{};
#endif
}

}  // namespace

V_VSG_NS_BEGIN

namespace core
{

bool AllocationGate::supported() noexcept
{
    return heapUsage().supported;
}

std::size_t AllocationGate::bytesInUse() noexcept
{
    return heapUsage().bytes;
}

void AllocationGate::begin() noexcept
{
    if (open_)
    {
        // Already measuring: the first open owns the window, so a nested one cannot shorten it.
        return;
    }
    opened_at_ = bytesInUse();
    open_      = true;
}

std::ptrdiff_t AllocationGate::end() noexcept
{
    if (!open_)
    {
        return 0;
    }
    open_ = false;
    ++windows_;

    if (!supported())
    {
        last_growth_ = 0;
        return last_growth_;
    }

    const auto now = static_cast<std::ptrdiff_t>(bytesInUse());
    const auto at  = static_cast<std::ptrdiff_t>(opened_at_);
    last_growth_   = now - at;
    return last_growth_;
}

bool AllocationGate::open() const noexcept
{
    return open_;
}

std::size_t AllocationGate::windows() const noexcept
{
    return windows_;
}

std::ptrdiff_t AllocationGate::lastGrowth() const noexcept
{
    return last_growth_;
}

}  // namespace core

V_VSG_NS_END
