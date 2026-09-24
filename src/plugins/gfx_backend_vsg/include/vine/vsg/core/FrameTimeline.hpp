#pragma once

#include <cstdint>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame clock: one timeline for every deferred action in the backend.
 *
 * WHY ONE TIMELINE. Three different things in this backend are deferred to a later frame: a replaced
 * GPU object is parked until the command buffer that could still name it has been re-recorded, a pass
 * that was not announced this frame has its view detached, and the load-op variant a bootstrap left
 * behind is swapped back. Each of them, given its own countdown, can drift a frame away from the
 * others - and a frame of drift is exactly the width of the bug class this design exists to remove
 * ("destroyed an object a submitted command buffer still named", "cleared a target that was still
 * being read"). So they all read one counter.
 *
 * SUBMITTED IS NOT COMPLETED, and conflating them is the last silent failure available here.
 * `submittedFrame()` counts frames handed to the queue; `completedFrame()` counts frames whose GPU
 * work is provably done. In this backend the proof does not come from a timeline semaphore - it comes
 * from the command-buffer slot being recycled: vsg waits that slot's fence before it re-records it
 * (`vsg 1.1.16 - src/vsg/app/RecordAndSubmitTask.cpp - RecordAndSubmitTask::start()`), so "the slot
 * that could still name this object has been reused" IS the completion evidence. The executor
 * reports that by calling completeUpTo(); an executor that stops waiting on slot fences loses the
 * evidence and must fall back to a counted device wait instead of assuming.
 *
 * The slot count is a property of the viewer (`numBuffers`, 3 in this vsg) and is not part of the
 * public API, so it is probed once at session start and passed in here - `retirePoint()` is why the
 * number is needed: a parked object must outlive the slot that recorded it, hence slots + 1.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief Identity of one frame between beginFrame() and swapBuffers(). */
struct FrameToken
{
    std::uint64_t frame{0};  ///< 1-based frame number; 0 means "no frame".

    /** @brief Whether this token names a frame. */
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return frame != 0; }
};

/**
 * @brief The frame clock (see the file note for what it is shared by, and for the completion rule).
 */
class FrameTimeline
{
  public:
    /** @brief Opens a frame and mints its token.
     *
     * Called from beginFrame(). Opening a frame while one is already open is a caller bug rather than
     * a condition to absorb, so the open frame is kept and its token returned again - the caller gets
     * a usable token either way, and the protocol layer is where the mismatch is reported.
     *
     * @return The token of the frame now open.
     */
    [[nodiscard]] FrameToken begin() noexcept;

    /** @brief Marks the open frame as submitted.
     *
     * Called from swapBuffers() after the queue submission returned. A token that does not name the
     * open frame is ignored: the timeline never moves backwards on behalf of a stale caller.
     *
     * @param token Token returned by begin().
     */
    void submitted(FrameToken token) noexcept;

    /** @brief Marks the open frame as OVER without having been submitted.
     *
     * The frame is consumed exactly like a submitted one - it cannot be retried (its swapchain image was
     * acquired and its graph was recorded), so begin() has to be able to open the next one - but nothing
     * was handed to the queue, so the submitted watermark does NOT move. That keeps the watermark a count
     * of SUBMISSIONS rather than a count of frames, and it is the honest dating for anything parked during
     * such a frame: nothing in flight names it, so it is dated against the last frame that really was
     * submitted.
     *
     * @param token Token returned by begin() (a stale one is ignored, like in submitted()).
     */
    void abandoned(FrameToken token) noexcept;

    /** @brief Advances the completion watermark (the slot that recorded @p frame was recycled).
     *
     * Monotonic: a lower value than the current watermark is ignored, because completion is a fact
     * about the past that a later report cannot un-do.
     *
     * @param frame Highest frame whose GPU work is provably done.
     */
    void completeUpTo(std::uint64_t frame) noexcept;

    /** @brief Gets the token of the open frame, or an empty token when no frame is open. */
    [[nodiscard]] FrameToken current() const noexcept;

    /** @brief Gets how many frames have been submitted. */
    [[nodiscard]] std::uint64_t submittedFrame() const noexcept;

    /** @brief Gets how many frames are provably complete. */
    [[nodiscard]] std::uint64_t completedFrame() const noexcept;

    /** @brief Gets whether a frame is open (between begin() and swapBuffers()). */
    [[nodiscard]] bool hasOpenFrame() const noexcept;

    /** @brief Gets whether a token names the open frame. */
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
    std::uint64_t submitted_{0};   ///< Frames submitted.
    std::uint64_t completed_{0};   ///< Frames provably complete (monotonic).
    FrameToken    open_{};         ///< The frame between begin() and submitted().
};

}  // namespace core

VN_VSG_NS_END
