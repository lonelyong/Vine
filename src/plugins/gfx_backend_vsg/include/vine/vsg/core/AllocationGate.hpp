#pragma once

#include <cstddef>
#include <cstdint>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief "Did the heap grow inside this window?" - the steady-state gate, answered in-process.
 *
 * WHAT THE RULE IS. A frame whose content did not change must not allocate: no plan storage, no
 * geometry re-upload, no cache entry, no container growth. Everything the backend does per frame is
 * supposed to be reuse of what an earlier frame built, and "supposed to" is worth nothing without a
 * check - a `std::vector` that quietly grows, or a plan rebuilt into a fresh allocation every frame,
 * costs the same as a correct implementation until the profiler says otherwise.
 *
 * WHY NOT LD_PRELOAD. The first plan was to run the binary under `scripts/alloc_trace.c` with
 * LD_PRELOAD and count the trace lines in a window. That file is explicitly an INVESTIGATION instrument
 * ("not a gate", its header says), and it interposes `mmap`, so it sees large mappings - not the small
 * allocations a frame's mistake actually consists of. Interposing the allocator is also where the
 * fragile parts live: the interposer has to resolve its real functions through the very allocator it is
 * wrapping. What the gate needs is one bit - did allocated bytes grow - and glibc already answers that
 * in-process, with no interposition, no extra process and no build step; `alloc_trace.c` keeps its
 * original job (explaining WHERE memory went) and the gate does not depend on it.
 *
 * WHAT IT MEASURES, honestly: the PROCESS, not the backend. In a window that spans unrelated threads or
 * the test harness's own work, their allocations are counted too. That is why the window a phase opens
 * must contain only the thing it is gating - the frame loop, where the backend is the only code running.
 *
 * WHY THE HEAP READING ALONE IS NOT ENOUGH, and what the second half is for. "Did allocated bytes grow"
 * is blind to CHURN: a frame that allocates and frees the same block (a vector rebuilt per pass, a
 * `std::function` parked and released) leaves the byte count exactly where it was, so a gate that only
 * reads the heap passes with a frame that allocates on every pass. Measured on the platform this gate was
 * built for (glibc `mallinfo2`), the small allocations a frame's mistake consists of are all of that kind.
 * The heap reading is also unavailable on a platform whose C library does not report it - Windows - where
 * the gate degraded to "not measured" for as long as the reading was the only half (a phase that read
 * `gate.end() == 0` on a platform with no reading was asserting nothing).
 *
 * So the gate has a COUNTING half, and the honesty about it is in the type: nothing in the LIBRARY may
 * replace the global allocation functions, because a backend that did would change the host's allocator.
 * A harness that wants the stronger gate replaces them itself and says so with
 * @ref announceCountedAllocations - which is what keeps "counted zero allocations" distinguishable from
 * "nothing was counting", the failure mode this whole file exists to avoid. This repository ships one such
 * harness for its own tests (`tests/test_vsg/AllocationCounter.cpp`), so the steady-state phases assert a
 * COUNT and not only a byte reading on every platform they run on.
 *
 * WHERE IT IS UNAVAILABLE: on a platform whose C library does not report heap usage (see the
 * implementation), `supported()` is false and a window reports nothing rather than reporting zero,
 * which would be a gate that passes without checking anything. The counted half is independent of that,
 * and `countsAvailable()` says whether anything is counting.
 */
V_VSG_NS_BEGIN

namespace core
{

/**
 * @brief The heap-growth gate (see the file note for what it measures and what it deliberately is not).
 */
class AllocationGate
{
  public:
    /** @brief Gets whether this platform can report heap usage at all.
     *
     * @return true when bytesInUse()/end() mean something.
     */
    [[nodiscard]] static bool supported() noexcept;

    /** @brief Gets the bytes the process currently has allocated.
     *
     * @return Bytes in use, or 0 when unsupported (check supported() first).
     */
    [[nodiscard]] static std::size_t bytesInUse() noexcept;

    /** @brief Gets whether anything in this process is COUNTING allocations (see the file note).
     *
     * @return true when @ref allocations() means something because an instrumenter announced itself.
     */
    [[nodiscard]] static bool countsAvailable() noexcept;

    /** @brief Announces that the global allocation functions are counting (see the file note).
     *
     * Called once, by the harness that replaced them. It exists so a phase can tell the two worlds apart:
     * "the window saw no allocation" and "no allocation was counted" are the same number and two very
     * different claims, and a gate that cannot tell them apart is the kind that passes without checking.
     */
    static void announceCountedAllocations() noexcept;

    /** @brief Records one allocation of @p bytes (called by an instrumented global `operator new`).
     *
     * Part of the gate's contract with its instrumenter: counted here so the count has ONE owner (a
     * harness that kept its own counter would be a second truth about the same window). It must not
     * allocate, so it is a plain atomic increment.
     *
     * @param bytes Bytes the allocation asked for; recorded for the diagnostics, not for the verdict.
     */
    static void noteAllocation(std::size_t bytes) noexcept;

    /** @brief Records one deallocation (called by an instrumented global `operator delete`). */
    static void noteDeallocation() noexcept;

    /** @brief Gets how many allocations have been counted in this process (0 when nothing counts them). */
    [[nodiscard]] static std::uint64_t allocationCount() noexcept;

  public:
    /** @brief Opens a window. Nested or repeated opens inside an open window are refused: the first
     *  open is the one `end()` measures against, and a second open would silently shorten the window.
     */
    void begin() noexcept;

    /** @brief Closes the window.
     *
     * @return Bytes the heap grew by since begin() (negative when it shrank), or 0 when the window was
     *         not open or the platform is unsupported.
     */
    [[nodiscard]] std::ptrdiff_t end() noexcept;

    /** @brief Gets whether a window is open. */
    [[nodiscard]] bool open() const noexcept;

    /** @brief Gets how many windows this gate has closed. */
    [[nodiscard]] std::size_t windows() const noexcept;

    /** @brief Gets the growth reported by the last closed window. */
    [[nodiscard]] std::ptrdiff_t lastGrowth() const noexcept;

    /** @brief Gets how many allocations the last closed window counted.
     *
     * Zero when nothing is counting (see countsAvailable()); a phase asserts that availability FIRST, so
     * this number is never read as a verdict it cannot support.
     */
    [[nodiscard]] std::uint64_t allocations() const noexcept;

    /** @brief Gets the bytes the last closed window's allocations asked for. */
    [[nodiscard]] std::size_t bytes() const noexcept;


  private:
    std::size_t    opened_at_{0};    ///< Bytes in use when the window was opened.
    bool           open_{false};     ///< Whether a window is currently open.
    std::size_t    windows_{0};      ///< Windows closed so far.
    std::ptrdiff_t last_growth_{0};  ///< Growth the last closed window measured.
    std::uint64_t  opened_count_{0}; ///< Counter when the window was opened.
    std::size_t    opened_bytes_{0}; ///< Bytes counted when the window was opened.
    std::uint64_t  allocations_{0};  ///< Allocations the last closed window counted.
    std::size_t    bytes_{0};        ///< Bytes the last closed window's allocations asked for.
};

}  // namespace core

V_VSG_NS_END
