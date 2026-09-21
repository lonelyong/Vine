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
 * WHERE IT IS UNAVAILABLE: on a platform whose C library does not report heap usage (see the
 * implementation), `supported()` is false and a window reports nothing rather than reporting zero,
 * which would be a gate that passes without checking anything.
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


  private:
    std::size_t    opened_at_{0};    ///< Bytes in use when the window was opened.
    bool           open_{false};     ///< Whether a window is currently open.
    std::size_t    windows_{0};      ///< Windows closed so far.
    std::ptrdiff_t last_growth_{0};  ///< Growth the last closed window measured.
};

}  // namespace core

V_VSG_NS_END
