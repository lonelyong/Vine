#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include "CameraSnapshot.hpp"
#include "PresentTarget.hpp"
#include "SceneSnapshotRef.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The world ONE answered frame saw: what a pointer query must aim with (see FrameResult).
 *
 * @details
 * - TWO VALUES, ONE MOMENT: the publication the picture was drawn from and the camera value it was taken with. Both
 *   were taken when the frame was BUILT, so a pick, an anchor or a fit aimed with them asks about the picture the user
 *   is looking at - never about content or a camera that moved while the frame was being answered (`I7`, design N17).
 * - AN EMPTY SNAPSHOT IS A FACT, not an error: the presenting pass draws no content (a screen pass, an overlay-only
 *   picture), so a query has no rows to hit and says so rather than aiming at whatever the scene holds now.
 * - WHAT IS NOT HERE YET: poses. The prototype/instance layer is what would produce them (`graphics-frame-pipeline.md`
 *   F10 / F12); until it exists a query that needs posed geometry reads it from the live content, and this value does
 *   not pretend to know it.
 */
struct VN_GRAPHICS_API FrameQuery {
    /// The publication the picture was drawn from (empty when the presenting pass draws no content).
    SceneSnapshotRef snapshot{};
    /// The camera that picture was taken with; `present` is false when nothing presents this engine's area.
    CameraSnapshot view{};
};

/**
 * @brief What ONE answered frame was: its picture, the world it saw, and what it cost (see RenderEngine::frameResult).
 *
 * @details
 * - ONE VALUE PER ANSWERED FRAME, and it describes THAT frame: a host reading it while the next frame is being
 *   answered gets the last answer, complete - never this frame's id beside the last frame's outcome (the same rule
 *   `RenderEngine::presentRecord` states for its one word).
 * - WHY IT EXISTS. "Which publication did the picture come from", "what did the frame consist of", "how many
 *   diagnostics did it produce" used to be three separate questions with three different lifetimes (a snapshot only
 *   the host had, `frameTable()`'s compile-time facts, a cumulative diagnostic counter). A query that must agree with
 *   what the user sees needs them as ONE value, because half of it going stale is exactly the bug the frame pipeline
 *   design calls "aiming at the wrong picture".
 * - THE COUNTS describe the frame that ran, not a running total: `passes` are the enabled passes its compiled value
 *   carried, `draws` the drawing calls those passes issued, and `diagnostics` what this engine reported through its
 *   one route while the frame was built and answered.
 * - `frame` is 0 until a frame has been answered, which is what `answered()` reports: an empty value says "nothing has
 *   been answered yet" rather than presenting a zero-filled result as fact.
 */
struct VN_GRAPHICS_API FrameResult {
    /// The frame this answer belongs to (see RenderEngine::frameNumber); 0 when no frame has been answered yet.
    std::uint64_t frame{ 0 };
    /// What became of the frame's picture (see PresentOutcome).
    PresentOutcome present{ PresentOutcome::Skipped };
    /// The world the picture saw: the publication it drew and the camera it was taken with.
    FrameQuery query{};
    /// Enabled passes the frame ran.
    std::uint32_t passes{ 0 };
    /// Drawing calls those passes issued (a pass that issued none counts 0 here, as it does in the frame table).
    std::uint32_t draws{ 0 };
    /// Diagnostics this engine reported while the frame was built and answered.
    std::uint64_t diagnostics{ 0 };

    /**
     * @brief Gets whether a frame has been answered at all.
     *
     * @return true when @ref frame is not 0.
     */
    [[nodiscard]] bool answered() const noexcept
    {
        return frame != 0;
    }

    /**
     * @brief Gets whether the frame's picture reached its target.
     *
     * @return true when @ref present is #PresentOutcome::Presented (see PresentOutcome: a device that presents
     *         nothing answers `Skipped`, and `Lost` is the one answer a host must act on).
     */
    [[nodiscard]] bool presents() const noexcept
    {
        return present == PresentOutcome::Presented;
    }
};

VN_GRAPHICS_NS_END
