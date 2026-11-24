#pragma once

#include <cstdint>
#include <vector>

#include <vine/graphics/graphics_global.hpp>

/**
 * @brief The frame clock: the number one frame body carries, for every deferred action in the backend.
 *
 * @details
 * - WHAT IT IS, AND WHAT IT IS NOT. It mints ONE NUMBER PER FRAME BODY, strictly increasing and never reused, and it
 *   remembers which numbers are still open (opened and not yet submitted or abandoned). It says NOTHING about what the
 *   GPU has finished - that is a fact about the QUEUE, and it lives in `DeviceClock`'s submission sequence. Keeping
 *   the two apart is the whole point (`G1`): a frame number is minted when a body STARTS, while completion belongs to
 *   the submission that body becomes, and a device may have several bodies open before either submits.
 * - WHY ONE TIMELINE. Three different things in a backend are deferred to a later frame: a replaced GPU object is
 *   parked until the command buffer that could still name it has been re-recorded, a pass that was not announced this
 *   frame has its view detached, and the load-op variant a bootstrap left behind is swapped back. Each of them, given
 *   its own countdown, can drift from the others - and a frame of drift is exactly the width of the bug class this
 *   design exists to remove ("destroyed an object a submitted command buffer still named", "cleared a target that was
 *   still being read"). So they all read one clock.
 * - SEVERAL FRAMES MAY BE OPEN AT ONCE (one per driver). A device that hands out more than one frame driver opens one
 *   frame per driver, and the numbers are MINTED AT OPENING so two open frames never share a number.
 *   `submitted` / `abandoned` close the token they name.
 * - WHY A TOKEN IS NOT A RECEIPT: a frame that is abandoned was recorded and thrown away; its number is spent and
 *   never comes back, which is what keeps a stale token from closing another driver's frame.
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
     * @brief Spends the frame @p token names: it was handed to a queue and its number is never handed out again.
     *
     * @details
     * - ONE TOKEN PER OPEN FRAME (see `begin`): committing one driver's frame leaves another driver's open frame open,
     *   and only a token this timeline opened is accepted - a stale or foreign one is ignored.
     * - THIS DOES NOT RECORD COMPLETION (see the file note): a frame that reached the queue becomes a SUBMISSION, and
     *   the queue's evidence is `DeviceClock::noteSubmission` / `noteCompletion`.
     *
     * @param token Token returned by begin() (a stale one is ignored).
     */
    void submitted(FrameToken token) noexcept;

    /** @brief Marks the open frame as OVER without having been submitted.
     *
     * @details
     * - The frame is consumed (it cannot be retried: its swapchain image was acquired and its graph was recorded), so
     *   begin() can open the next one, but nothing was handed to the queue - which is why it mints no submission.
     *
     * @param token Token returned by begin() (a stale one is ignored, like in submitted()).
     */
    void abandoned(FrameToken token) noexcept;

    /** @brief Gets the most recently opened frame that is still open, or an empty token when none is. */
    [[nodiscard]] FrameToken current() const noexcept;

    /** @brief Gets whether at least one frame is open (between begin() and submitted()/abandoned()). */
    [[nodiscard]] bool hasOpenFrame() const noexcept;

    /** @brief Gets whether @p token names a frame that is still open. */
    [[nodiscard]] bool isOpenFrame(FrameToken token) const noexcept;


  private:
    std::uint64_t              minted_{ 0 }; // The highest number begin() ever handed out.
    std::vector<std::uint64_t> open_{};      // The numbers opened and not yet submitted/abandoned.
};

VN_GRAPHICS_NS_END
