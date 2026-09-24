#include <vine/vsg/core/AllocationGate.hpp>

#include <atomic>

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

namespace
{

/// @brief The counting half's state (see the header: the instrumenter that feeds it is the harness').
///
/// Function-local statics, so a global `operator new` may call into them during static initialisation - the
/// allocation that happens before `main()` is as real as any other, and a counter that only becomes usable
/// once some TU's dynamic initialiser ran would silently lose it.
struct Counters
{
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::size_t>   bytes{0};
    std::atomic<bool>          announced{false};
};

Counters& counters() noexcept
{
    static Counters state;
    return state;
}

}  // namespace

VN_VSG_NS_BEGIN

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

bool AllocationGate::countsAvailable() noexcept
{
    return counters().announced.load(std::memory_order_relaxed);
}

void AllocationGate::announceCountedAllocations() noexcept
{
    counters().announced.store(true, std::memory_order_relaxed);
}

void AllocationGate::noteAllocation(std::size_t bytes) noexcept
{
    Counters& state = counters();
    state.count.fetch_add(1U, std::memory_order_relaxed);
    state.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void AllocationGate::noteDeallocation() noexcept
{
    // The count is of ALLOCATIONS, not of live blocks: the rule a frame has to keep is "it does not ask for
    // memory", and a frame that allocates and frees in the same pass breaks it just as much as one that
    // leaks. (That is the whole reason this half exists next to the byte reading - see the header.)
}

std::uint64_t AllocationGate::allocationCount() noexcept
{
    return counters().count.load(std::memory_order_relaxed);
}

void AllocationGate::begin() noexcept
{
    if (open_)
    {
        // Already measuring: the first open owns the window, so a nested one cannot shorten it.
        return;
    }
    opened_at_    = bytesInUse();
    opened_count_ = allocationCount();
    opened_bytes_ = counters().bytes.load(std::memory_order_relaxed);
    open_         = true;
}

std::ptrdiff_t AllocationGate::end() noexcept
{
    if (!open_)
    {
        return 0;
    }
    open_ = false;
    ++windows_;

    // The counted half first: it is the one a phase can always read (the byte reading depends on the
    // platform's C library, this one on whether a harness instrumented the allocator - see the header).
    allocations_ = allocationCount() - opened_count_;
    bytes_       = counters().bytes.load(std::memory_order_relaxed) - opened_bytes_;

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

std::uint64_t AllocationGate::allocations() const noexcept
{
    return allocations_;
}

std::size_t AllocationGate::bytes() const noexcept
{
    return bytes_;
}

}  // namespace core

VN_VSG_NS_END
