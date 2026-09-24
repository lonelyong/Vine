/**
 * @brief The instrumenter behind AllocationGate's COUNTING half: this binary replaces the global allocation
 * functions and tells the gate, so a phase can assert a NUMBER instead of a byte reading.
 *
 * WHY IT LIVES IN THE TEST TREE AND NOT IN THE LIBRARY. Replacing the global allocation functions changes
 * the allocator of whoever links the code, so a backend that shipped a replacement would be reaching into
 * its host's process (and the host may have its own reasons to instrument allocation). What the library
 * owns is the RULE and the counter (`core::AllocationGate`), and what a harness owns is being the
 * instrumenter. This file is that harness for `test_vsg`, and it announces itself so the gate's
 * `countsAvailable()` is true — which is what keeps "the window saw no allocation" distinguishable from
 * "nothing was counting" (the two are the same number and only one of them is evidence).
 *
 * WHY THE HEAP READING WAS NOT ENOUGH (the defect this closes). `end()` used to mean "the process' allocated
 * bytes grew", which is blind to churn: a frame that allocates and frees per pass leaves the byte count
 * where it was. On glibc the phases did read a heap number, and on Windows (`malloc.h`/`mallinfo2` do not
 * exist) the reading is `supported() == false`, so those phases asserted nothing there while reporting
 * success. Counting allocations sees both: a churn is a count, and the count needs no C library.
 *
 * WHAT IT COUNTS AND WHAT IT DOES NOT. Every allocation this BINARY makes, including the test harness's own
 * and any thread's - so a window must contain only the thing it gates (see AllocationGate's file note).
 * Allocations made inside a DLL (Qt's, the plugins') go through that module's allocator and are NOT counted;
 * the phases that use this gate gate code compiled into this binary, which is where the backend lives.
 *
 * WHY THE SIZE ARGUMENT IS ONLY A DIAGNOSTIC. The rule the gate asserts is "a steady frame does not ask for
 * memory" - one allocation of one byte breaks it exactly as much as a megabyte - so the verdict is the
 * COUNT. The bytes are recorded because "how much" is the next question a reader asks, and `delete` is
 * counted so the two numbers can be compared when a window is being understood.
 */
#include <cstddef>
#include <cstdlib>
#include <new>

#include <vine/vsg/core/AllocationGate.hpp>

#if defined(_MSC_VER)
#    include <malloc.h>
#endif

namespace
{

using vn::vsg::core::AllocationGate;

/// @brief Allocates @p bytes with an alignment the allocation functions must honour.
///
/// Two deallocators, because the platforms do not agree: MSVC's `_aligned_malloc` memory must be released
/// with `_aligned_free` (its `free` would corrupt the heap), while `posix_memalign` memory is ordinary
/// `malloc` memory and is released with `free`.
void* allocateAligned(std::size_t bytes, std::size_t alignment)
{
#if defined(_MSC_VER)
    return _aligned_malloc(bytes, alignment);
#else
    void* block = nullptr;
    // `posix_memalign` (not `std::aligned_alloc`): the latter requires the size to be a multiple of the
    // alignment, which the language's aligned allocation functions do not guarantee - they are given
    // whatever the caller asked for.
    if (::posix_memalign(&block, alignment, bytes) != 0)
    {
        return nullptr;
    }
    return block;
#endif
}

void releaseAligned(void* block) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(block);
#else
    ::free(block);
#endif
}

/// @brief Announces the instrumentation once, before `main()`.
[[maybe_unused]] const bool kAnnounced = [] {
    AllocationGate::announceCountedAllocations();
    return true;
}();

}  // namespace

// --- the replaceable allocation functions -----------------------------------------------------------
//
// Every one of them counts and then forwards to the C allocator; the throwing forms raise `bad_alloc` for a
// null block (the contract of the replaceable forms, and the reason they cannot simply return null).

void* operator new(std::size_t bytes)
{
    AllocationGate::noteAllocation(bytes);
    if (void* block = std::malloc(bytes == 0U ? 1U : bytes))
    {
        return block;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes)
{
    return ::operator new(bytes);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    AllocationGate::noteAllocation(bytes);
    return std::malloc(bytes == 0U ? 1U : bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept
{
    return ::operator new(bytes, tag);
}

void* operator new(std::size_t bytes, std::align_val_t alignment)
{
    AllocationGate::noteAllocation(bytes);
    if (void* block = allocateAligned(bytes == 0U ? 1U : bytes, static_cast<std::size_t>(alignment)))
    {
        return block;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
    return ::operator new(bytes, alignment);
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    AllocationGate::noteAllocation(bytes);
    return allocateAligned(bytes == 0U ? 1U : bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
    return ::operator new(bytes, alignment, tag);
}

void operator delete(void* block) noexcept
{
    AllocationGate::noteDeallocation();
    std::free(block);
}

void operator delete[](void* block) noexcept
{
    ::operator delete(block);
}

void operator delete(void* block, std::size_t) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block, std::size_t) noexcept
{
    ::operator delete(block);
}

void operator delete(void* block, const std::nothrow_t&) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block, const std::nothrow_t&) noexcept
{
    ::operator delete(block);
}

void operator delete(void* block, std::align_val_t) noexcept
{
    AllocationGate::noteDeallocation();
    releaseAligned(block);
}

void operator delete[](void* block, std::align_val_t alignment) noexcept
{
    ::operator delete(block, alignment);
}

void operator delete(void* block, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(block, alignment);
}

void operator delete[](void* block, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(block, alignment);
}

void operator delete(void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    ::operator delete(block, alignment);
}

void operator delete[](void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    ::operator delete(block, alignment);
}
