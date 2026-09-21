#pragma once

#include <cstdint>

#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The numbers a phase gates on, in two aggregates that must agree where they overlap.
 *
 * WHY NUMBERS AT ALL. "It rendered" is not a claim a phase can check in a headless run - the pixels
 * are read back for that - and "it did not crash" is even weaker. What a backend regression actually
 * looks like is a COUNTER that moved when it should not have (a resize that rebuilt every program
 * slot, a steady frame that re-uploaded geometry, a frame path that idled the device) or one that did
 * not move when it should have (a bootstrap that never cleared). Counters are what turn "the rule
 * holds" into something a phase can fail on.
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
V_VSG_NS_BEGIN

namespace core
{

/** @brief Counters about one frame's work: what was built, what was refreshed, what was drawn. */
struct FrameCounters
{
    std::uint64_t frames{0};                ///< Frames submitted.
    std::uint64_t passes{0};                ///< Pass scopes recorded.
    std::uint64_t draws{0};                 ///< Draw calls recorded.
    std::uint64_t data_nodes_built{0};      ///< Geometry data nodes (re)built.
    std::uint64_t streams_refreshed{0};     ///< Geometry streams refreshed in place.
    std::uint64_t offscreen_builds{0};      ///< Off-screen targets built from nothing.
    std::uint64_t offscreen_resizes{0};     ///< Off-screen targets resized in place.
    std::uint64_t window_builds{0};         ///< Window sessions built (a move does not count).
    std::uint64_t program_slot_builds{0};   ///< Full-screen program slots built.
};

/** @brief Counters about what is retained and what it cost to let go. */
struct RetentionStats
{
    std::uint64_t parked_nodes{0};       ///< Objects waiting in the retirement queue.
    std::uint64_t released_nodes{0};     ///< Objects released so far.
    std::uint64_t device_waits{0};       ///< Counted device idles (destructive paths only).
    std::uint64_t compile_contexts{0};   ///< Live compile registrations (a leak shows up here).
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

V_VSG_NS_END
