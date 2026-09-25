#pragma once

#include <cstdint>

#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The numbers a phase gates on, in two aggregates that must agree where they overlap.
 *
 * WHY NUMBERS AT ALL. "It rendered" is not a claim a phase can check in a headless run - the pixels
 * are read back for that - and "it did not crash" is even weaker. What a backend regression actually
 * looks like is a COUNTER that moved when it should not have (a recorded pass that recorded no draw, a
 * schedule that skipped a pass component) or one that did not move when it should have (a bootstrap
 * that never cleared). Counters are what turn "the rule holds" into something a phase can fail on.
 *
 * WHY THE OTHER NUMBERS ARE NOT HERE. How many rows the content tables built, how many streams were
 * uploaded or aliased, how many pipelines were compiled - each is answered by the layer that does the
 * work (`ContentStore::builds()`, `StreamUploads::uploads()`, `VariantPool::created()`), and those are
 * what a phase reads. A copy here would be a second spelling of a number this object cannot see: the
 * fields that tried (`data_nodes_built`, `streams_refreshed`, `offscreen_builds`, ...) were written by
 * nobody and deleted in §11.16cr of the backend design log.
 *
 * WHY TWO AGGREGATES AND A CROSS-CHECK. The frame-side counters and the retention-side numbers are
 * written at different places (the executor records draws; the retirement queue knows what it
 * released), so a bug can make one of them lie. Where they describe the same event - device idles,
 * released objects - they are asserted equal against the queue's own account, which is how a number
 * that drifted from its source is caught instead of believed.
 *
 * This is instrumentation, not a layer: nothing here decides anything, and everything here is called
 * from the places that do.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief Counters about one frame's RECORDING: the pass scopes, the draws, the skipped components. */
struct FrameCounters
{
    std::uint64_t passes{0};             ///< Pass scopes recorded.
    std::uint64_t draws{0};              ///< Draw calls recorded.
    std::uint64_t invalid_schedules{0};  ///< Cyclic pass-dependency components skipped (see FrameGraph).
};

/** @brief Counters about what is retained and what it cost to let go. */
struct RetentionStats
{
    std::uint64_t released_nodes{0};  ///< Objects released so far.
    std::uint64_t device_waits{0};    ///< Counted device idles (destructive paths only).
};

/**
 * @brief The backend's instrumentation (see the file note for what it is and is not).
 */
class Observe
{
  public:
    /** @brief Gets the frame counters for updating. */
    [[nodiscard]] FrameCounters& counters() noexcept;

    /** @brief Gets the retention numbers for updating. */
    [[nodiscard]] RetentionStats& retention() noexcept;

    /** @brief Gets the frame counters for reading. */
    [[nodiscard]] const FrameCounters& counters() const noexcept;

    /** @brief Gets the retention numbers for reading. */
    [[nodiscard]] const RetentionStats& retention() const noexcept;

    /** @brief Checks the overlapping numbers against the queue that produced them.
     *
     * The two aggregates describe release and idle from two sides; where they overlap they must be the
     * same number, and this is the assertion that says so.
     *
     * @param queue Retirement queue whose own account is compared against retention().
     * @return true when the overlapping numbers agree.
     */
    [[nodiscard]] bool agreesWith(const RetirementQueue& queue) const noexcept;


  private:
    FrameCounters  counters_;
    RetentionStats retention_;
};

}  // namespace core

VN_VSG_NS_END
