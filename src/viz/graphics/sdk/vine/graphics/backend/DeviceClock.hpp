#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/graphics/backend/BackendHalfGuard.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/graphics_global.hpp>

/**
 * @brief The completion evidence of a DEVICE: one frame clock, and the watermark every window's evidence agrees on.
 *
 * @details
 * - WHY IT EXISTS. A device hands out several windows (one swapchain each), and every window has its own fences: the
 *   framework waits a slot's fence before it re-records that slot, which is proof that the work THAT window submitted
 *   up to some frame is finished - and proof of nothing about another window's. A resource is shared, though: a buffer
 *   or a pipeline the host replaced may still be named by a submission from ANY window, so "may this be destroyed" is
 *   a DEVICE question, not a window's.
 * - THE RULE. Every window reports its own evidence (`windowCompleted`: "all work I submitted up to frame F is done",
 *   which is the frame its recycled slot proves). The device watermark is the MINIMUM over the live windows: for
 *   X = min, every window has proven its own work up to at least X, so every submission up to X - from any window -
 *   is finished. A window proves nothing about work it never did, which is vacuously done.
 * - FRAME NUMBERS ARE THE DEVICE'S. One `FrameTimeline` mints them, so a token and a watermark mean the same thing
 *   whichever window asked. Frames are opened one at a time (a frame is executed and presented before the next
 *   begins), so one open-frame slot is enough - see FrameTimeline::begin().
 * - A WINDOW THAT JOINS starts at the current watermark: it existed for no earlier frame, so it constrains nothing.
 *   A window that leaves stops constraining it (the min can only rise), which is why windows are removed rather than
 *   left at their last evidence forever.
 * - This is the device-level equivalent of what Filament (`Driver::destroyDeferred`), Unreal (`FRHIResource` deferred
 *   delete) and wgpu (submission index) do: ONE submission watermark per device, not one per window. See
 *   `.ai/design/graphics-window-device-viewer.md` §11.
 *
 * NOT thread-safe: the device is driven from one thread (see RenderDevice's threading note).
 */
VN_GRAPHICSBACKEND_NS_BEGIN

/**
 * @brief One device's frame clock and the completion watermark its windows agree on (see the file note).
 */
class VN_GRAPHICS_API DeviceClock
{
  public:
    /** @brief Identity of one window on this device. */
    using WindowId = std::uint32_t;

    /** @brief Registers a window: from now on its evidence constrains the watermark.
     *
     * @details
     * - It starts AT the current watermark, because it existed for no earlier frame - a window that has not drawn
     *   yet must not hold the device's evidence at zero.
     *
     * @return The window's id, for the calls below.
     */
    [[nodiscard]] WindowId addWindow() noexcept;

    /** @brief Unregisters a window: its evidence stops constraining the watermark.
     *
     * @param window Id returned by @ref addWindow (an unknown or already removed id changes nothing).
     */
    void removeWindow(WindowId window) noexcept;

    /** @brief Reports the in-flight slot count a window learned, or 0 when it could not learn one.
     *
     * @details
     * - Until a window knows its count it proves nothing (its evidence would be a guess), and the device watermark
     *   stays where it is - which is exactly what parking depends on: a park that cannot be dated is not made (see
     *   core::RetirementQueue).
     *
     * @param window Window that learned the count.
     * @param slots  Slots in flight, or 0 when unknown.
     */
    void setWindowSlots(WindowId window, std::uint32_t slots) noexcept;

    /** @brief Reports one window's evidence: all work IT submitted up to @p frame is complete.
     *
     * @details
     * - Monotonic per window (an older report is ignored) and folded into the device watermark as a minimum, so a
     *   window that is behind holds the watermark back until it catches up.
     *
     * @param window Window whose slot was recycled.
     * @param frame  Highest frame that window's recycled slot proves finished.
     */
    void windowCompleted(WindowId window, std::uint64_t frame) noexcept;

    /** @brief Opens a frame on the device's clock (see FrameTimeline::begin). */
    [[nodiscard]] FrameToken begin() noexcept;

    /** @brief Marks the open frame as submitted (see FrameTimeline::submitted).
     *
     * @param token Token the device's clock returned from @ref begin.
     */
    void submitted(FrameToken token) noexcept;

    /** @brief Marks the open frame as over without a submission (see FrameTimeline::abandoned).
     *
     * @param token Token the device's clock returned from @ref begin.
     */
    void abandoned(FrameToken token) noexcept;

    /** @brief Gets the device's frame clock (what a park is dated against). */
    [[nodiscard]] FrameTimeline& timeline() noexcept;

    /** @brief Gets the device's frame clock (const). */
    [[nodiscard]] const FrameTimeline& timeline() const noexcept;

    /** @brief Gets the frame every live window has proven finished (see the file note). */
    [[nodiscard]] std::uint64_t completedFrame() const noexcept;

    /** @brief Gets the widest in-flight count any live window learned (the park window a device queue must use).
     *
     * @details
     * - The widest, not the narrowest: a park made during window A's frame is released against the device watermark,
     *   and the resource it names may be in flight in the window with more slots. 0 means no window knows yet, which
     *   is what keeps parking off (see core::RetirementQueue::parkingAvailable).
     */
    [[nodiscard]] std::uint32_t widestSlots() const noexcept;

    /** @brief Gets how many windows are registered. */
    [[nodiscard]] std::size_t windows() const noexcept;

  private:
    /// One window's contribution to the device watermark (see the file note).
    struct Window
    {
        std::uint64_t completed{0};  ///< Highest frame THIS window proved finished.
        std::uint32_t slots{0};      ///< In-flight count it learned, or 0 while unknown.
        bool          live{true};    ///< false once removed: it stops constraining the watermark.
    };

    /// Recomputes the device watermark from the live windows, and moves the timeline (monotonic).
    void updateWatermark() noexcept;

    FrameTimeline      timeline_;
    std::vector<Window> windows_;
    std::uint64_t      completed_{0};  ///< The device watermark (see completedFrame).
};

VN_GRAPHICSBACKEND_NS_END
