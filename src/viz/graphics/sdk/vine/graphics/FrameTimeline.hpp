#pragma once

#include <cstdint>
#include <vector>

#include <vine/graphics/graphics_global.hpp>

/**
 * @brief The frame clock: one timeline for every deferred action in the backend.
 *
 * @details
 * - WHY ONE TIMELINE. Three different things in a backend are deferred to a later frame: a replaced GPU object is
 *   parked until the command buffer that could still name it has been re-recorded, a pass that was not announced this
 *   frame has its view detached, and the load-op variant a bootstrap left behind is swapped back. Each of them, given
 *   its own countdown, can drift a frame away from the others - and a frame of drift is exactly the width of the bug
 *   class this design exists to remove ("destroyed an object a submitted command buffer still named", "cleared a
 *   target that was still being read"). So they all read one counter.
 * - SUBMITTED IS NOT COMPLETED, and conflating them is the last silent failure available here. `submittedFrame()`
 *   counts frames handed to the queue; `completedFrame()` counts frames whose GPU work is provably done. The proof
 *   is the BACKEND's to obtain, and the evidence it obtains is a REUSE: the command-buffer slot that could still
 *   name an object is waited on before it is recorded again (a slot's fence is waited before the slot is recorded
 *   into), so "the slot that could still name this object has been reused" IS the completion evidence. An executor
 *   reports it by moving the watermark - `completeUpTo()` where one slot list exists, `DeviceClock::windowCompleted()`
 *   where a device serves several surfaces - and an executor that stops waiting on its slots' fences loses the
 *   evidence and must fall back to a counted device wait instead of assuming.
 * - THE SLOT COUNT BELONGS TO THE SURFACE, not to this type (the window's own buffer count),
 *   so it is probed once at session start and passed in here - `retirePoint()` is why the number is needed: a parked
 *   object must outlive the slot that recorded it, hence slots + 1.
 * - SEVERAL FRAMES MAY BE OPEN AT ONCE (one per driver). A device that hands out more than one frame driver opens one
 *   frame per driver, and the numbers are MINTED AT OPENING (not derived from the submission count) so two open frames
 *   never share a number. `submitted` / `abandoned` close the token they name; the submission watermark is the highest
 *   number actually handed to a queue, which is what a park is dated against.
 */
VN_GRAPHICS_NS_BEGIN

/** @brief Identity of one frame between executeFrame() and present(). */
struct FrameToken {
    std::uint64_t frame{ 0 }; // 1-based frame number; 0 means "no frame".

    /** @brief Whether this token names a frame. */
    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return frame != 0;
    }
};

/**
 * @brief The frame clock (see the file note for what it is shared by, and for the completion rule).
 */
class VN_GRAPHICS_API FrameTimeline {
  public:
    /**
     * @brief Opens a frame and mints its token.
     *
     * @details
     * - EVERY CALL MINTS A NEW NUMBER, so two drivers that open a frame at the same time get two tokens rather than
     *   one. Numbers are strictly increasing, never reused, and independent of how many frames were submitted.
     * - A caller that opens a frame and never submits or abandons it keeps it OPEN, which is exactly what holds the
     *   completion watermark back (see DeviceClock): a frame that is still being built may still name parked objects.
     *
     * @return The token of the frame now open. It stays open until `submitted` or `abandoned` closes it.
     */
    [[nodiscard]] FrameToken begin() noexcept;

    /**
     * @brief Marks the frame @p token names as submitted: the highest such number becomes the submitted watermark.
     *
     * @details
     * - ONE TOKEN PER OPEN FRAME (see `begin`): committing one driver's frame leaves another driver's open frame open,
     *   and only a token this timeline opened is accepted - a stale or foreign one is ignored, so the timeline never
     *   moves backwards on behalf of a stale caller.
     *
     * @param token Token returned by begin() (a stale one is ignored).
     */
    void submitted(FrameToken token) noexcept;

    /** @brief Marks the open frame as OVER without having been submitted.
     *
     * @details
     * - The frame is consumed exactly like a submitted one - it cannot be retried (its swapchain image was acquired
     *   and its graph was recorded), so begin() has to be able to open the next one - but nothing was handed to the
     *   queue, so the submitted watermark does NOT move. That keeps the watermark a count of SUBMISSIONS rather than
     *   a count of frames, and it is the honest dating for anything parked during such a frame: nothing in flight
     *   names it, so it is dated against the last frame that really was submitted.
     *
     * @param token Token returned by begin() (a stale one is ignored, like in submitted()).
     */
    void abandoned(FrameToken token) noexcept;

    /** @brief Advances the completion watermark (the slot that recorded @p frame was recycled).
     *
     * @details
     * - Monotonic: a lower value than the current watermark is ignored, because completion is a fact about the past
     *   that a later report cannot un-do.
     *
     * @param frame Highest frame whose GPU work is provably done.
     */
    void completeUpTo(std::uint64_t frame) noexcept;

    /** @brief Gets the most recently opened frame that is still open, or an empty token when none is. */
    [[nodiscard]] FrameToken current() const noexcept;

    /** @brief Gets how many frames have been submitted. */
    [[nodiscard]] std::uint64_t submittedFrame() const noexcept;

    /** @brief Gets how many frames are provably complete. */
    [[nodiscard]] std::uint64_t completedFrame() const noexcept;

    /** @brief Gets whether at least one frame is open (between begin() and submitted()/abandoned()). */
    [[nodiscard]] bool hasOpenFrame() const noexcept;

    /** @brief Gets whether @p token names a frame that is still open. */
    [[nodiscard]] bool isOpenFrame(FrameToken token) const noexcept;

    /** @brief Computes the frame at which an object parked now may be released.
     *
     * One more than the slot count: the slot that could still reference the object has been recycled
     * (and therefore had its fence waited) by then.
     *
     * @param submitted_frame Frame the object was parked during (see submittedFrame()).
     * @param slots Number of command-buffer slots in flight (probed from the viewer at session start).
     * @return The first completed frame at which the object is safe to destroy.
     */
    [[nodiscard]] static std::uint64_t retirePoint(std::uint64_t submitted_frame, std::uint32_t slots) noexcept;


  private:
    std::uint64_t              submitted_{ 0 }; // Frames submitted (the highest frame number handed to a queue).
    std::uint64_t              completed_{ 0 }; // Frames provably complete (monotonic).
    std::uint64_t              minted_{ 0 };    // The highest number begin() ever handed out.
    std::vector<std::uint64_t> open_{};         // The numbers opened and not yet submitted/abandoned.
};

VN_GRAPHICS_NS_END
