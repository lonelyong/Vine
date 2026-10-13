#pragma once

#include <cstdint>
#include <optional>

#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Counters about one frame's RECORDING: the pass scopes, the draws, the skipped components.
 *
 * @details
 * - This is the SDK's vocabulary for "what shape was this frame", so a host, a HUD and a gate can ask the same question of any backend (see RenderEngine::frameReport) and read the SAME numbers the backend's own evidence uses, instead of counting a second time from outside.
 * - Per frame: an answer describes the frame that just ended, never a running total (RetentionStats is where the numbers that only grow live).
 * - A backend that keeps no counters answers "no counters" rather than zeros - an empty frame and an unmeasured one are different answers.
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
 * @details
 * - Cumulative for the backend's lifetime, unlike FrameCounters: these are the numbers behind "the frame path never idles the device" and "the retained set does not grow with the frame count".
 */
struct VN_GRAPHICS_API RetentionStats
{
    std::uint64_t released_nodes{0}; ///< Objects released so far.
    std::uint64_t device_waits{0};   ///< Counted device idles (destructive paths only).
};

/**
 * @brief Where the frame clock is: submitted versus provably complete.
 *
 * @details
 * - SUBMITTED IS NOT COMPLETED, and a host that wants to know whether the GPU is keeping up needs both: `submitted` counts frames handed to the queue, `completed` counts frames whose GPU work is provably done, and the difference is how many frames are still in flight.
 * - A backend's proof of completion is its own (this one reads the command slot being recycled - see backend/FrameTimeline.hpp); the SDK's vocabulary is only the two watermarks, so a host, a HUD or a gate asks any backend the same question.
 * - Cumulative for the backend's lifetime, like RetentionStats. There is no frame-scoped half here, because a watermark that could go backwards would be a lie.
 */
struct VN_GRAPHICS_API FrameProgress
{
    std::uint64_t submitted{0}; ///< Frames handed to the queue so far.
    std::uint64_t completed{0}; ///< Frames whose GPU work is provably done (monotonic).

    /**
     * @brief Gets how many frames are submitted and not yet provably complete.
     *
     * @return Frames in flight, never negative (a backend may report completion first).
     */
    [[nodiscard]] std::uint64_t inFlight() const noexcept
    {
        return submitted > completed ? submitted - completed : 0;
    }
};

/**
 * @brief What a backend can say about its frames, as ONE answer (see RenderFrameExecutor::frameReport).
 *
 * @details
 * - ONE QUESTION, ONE VALUE. The three parts used to be three calls with three out-parameters and three bools, so a host asking "how is it going" made three calls and kept three "did it answer" flags beside them - and a caller that stopped after the second could not tell whether the third had been written. They are one value because they describe the same thing from three distances: the frame that just ENDED (FrameCounters), what is RETAINED and what it cost to let go (RetentionStats), and where the clock is (FrameProgress).
 * - ABSENT IS NOT ZERO - that is why every part is optional. "This backend keeps no such number" and "it keeps it, and the number is 0" are different answers: an unmeasured frame must not read like an empty one, and a clock that is not kept must not read as "nothing in flight". Zeros would be a claim; a disengaged part is the silence, and it is distinguishable at the type level rather than by a flag beside it.
 * - The whole answer is asked once (RenderEngine::frameReport hands it over unchanged), so a host reads the same numbers the backend's own evidence uses instead of counting a second time from outside.
 */
struct VN_GRAPHICS_API FrameReport
{
    std::optional<FrameCounters>  counters;  ///< The frame that just ended; disengaged when none are kept.
    std::optional<RetentionStats> retention; ///< Cumulative retention; disengaged when none are kept.
    std::optional<FrameProgress>  progress;  ///< Where the frame clock is; disengaged when none is kept.
};

VN_GRAPHICS_NS_END
