#pragma once

#include <cstdint>

#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Counters about one frame's RECORDING: the pass scopes, the draws, the skipped components.
 *
 * This is the SDK's vocabulary for "what shape was this frame", so a host, a HUD and a gate can ask
 * the same question of any backend (see RenderEngine::frameCounters) and read the SAME numbers the
 * backend's own evidence uses, instead of counting a second time from outside.
 *
 * Per frame: an answer describes the frame that just ended, never a running total (RetentionStats is
 * where the numbers that only grow live). A backend that keeps no counters answers
 * "no counters" rather than zeros - an empty frame and an unmeasured one are different answers.
 */
struct VN_GRAPHICS_API FrameCounters
{
    std::uint64_t passes{0};            ///< Pass scopes recorded.
    std::uint64_t draws{0};             ///< Draw calls recorded.
    std::uint64_t invalid_schedules{0}; ///< Cyclic pass-dependency components skipped (see FrameGraph).
};

/**
 * @brief Counters about what is retained, and what it cost to let go.
 *
 * Cumulative for the backend's lifetime, unlike FrameCounters: these are the numbers behind "the frame
 * path never idles the device" and "the retained set does not grow with the frame count".
 */
struct VN_GRAPHICS_API RetentionStats
{
    std::uint64_t released_nodes{0}; ///< Objects released so far.
    std::uint64_t device_waits{0};   ///< Counted device idles (destructive paths only).
};

/**
 * @brief Where the frame clock is: submitted versus provably complete.
 *
 * SUBMITTED IS NOT COMPLETED, and a host that wants to know whether the GPU is keeping up needs both:
 * `submitted` counts frames handed to the queue, `completed` counts frames whose GPU work is provably
 * done, and the difference is how many frames are still in flight. A backend's proof of completion is
 * its own (this one reads the command slot being recycled - see backend/FrameTimeline.hpp); the SDK's
 * vocabulary is only the two watermarks, so a host, a HUD or a gate asks any backend the same question.
 *
 * Cumulative for the backend's lifetime, like RetentionStats. There is no frame-scoped half here,
 * because a watermark that could go backwards would be a lie.
 */
struct VN_GRAPHICS_API FrameProgress
{
    std::uint64_t submitted{0}; ///< Frames handed to the queue so far.
    std::uint64_t completed{0}; ///< Frames whose GPU work is provably done (monotonic).

    /** @brief Gets how many frames are submitted and not yet provably complete.
     *
     * @return Frames in flight, never negative (a backend may report completion first).
     */
    [[nodiscard]] std::uint64_t inFlight() const noexcept
    {
        return submitted > completed ? submitted - completed : 0;
    }
};

VN_GRAPHICS_NS_END
