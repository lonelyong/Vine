#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <vsg/app/CommandGraph.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/backend/DeviceClock.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief A render session with NO WINDOW: one device, one command graph, one submission per frame.
 *
 * @details
 * - WHY IT EXISTS. A frame that names no surface (RenderFrameExecutor::executeFrame(frame, nullptr)) still has work
 *   to submit: its passes draw into off-screen targets of the device, and that work has to reach the queue even
 *   though nothing is acquired and nothing is presented. A windowed session cannot express that - its frame opens by
 *   acquiring the swapchain's next image - so the submit half is split out here (see
 *   `.ai/design/device-is-not-a-window.md` §6).
 * - WHAT IT OWNS. A viewer with no window, one record-and-submit task over a windowless command graph
 *   (`vsg::CommandGraph(device, family)`, the framework's own spelling of "a submission that neither acquires nor
 *   presents"), and the slot learning every session does. The DEVICE is borrowed from the caller, like the device
 *   clock and the park queue are.
 * - WHAT IT DOES NOT DO. It never presents: `framesPresented()` is always 0, and the only call that ends a frame is
 *   commitFrame(). It also has no surface to keep, so initialize() is idempotent rather than following the windowed
 *   session's keep/move/rebuild table (Session::wouldKeep does not apply here).
 * - COMPLETION IS EVIDENCE, NOT A GUESS, exactly as in the windowed session: committing frame F proves that every
 *   frame up to F - slots is finished, because the framework waits a slot's fence before re-recording it. The
 *   session registers itself on the DEVICE's clock (core::DeviceClock::addWindow) and reports that one frame's
 *   proof, so an off-screen frame counts towards the device watermark the same way a window's frame does - without
 *   it the device's minimum would be pinned by work nobody reports.
 *
 * ONE THREAD, LIKE EVERYTHING ELSE HERE: the backend's frame path is serialised, never concurrent and never
 * reentrant (see RenderDevice's threading note), so this type keeps no synchronisation.
 */
VN_VSG_NS_BEGIN

namespace api
{

/** @brief What an off-screen session needs to come up: the device to submit on, and its graphics queue family. */
struct OffscreenSessionOptions
{
    /// The device to render on, carried as `void*` because the value is a `vsg::Device*` and the session takes its
    /// own reference to it. Never null: an off-screen session borrows a device, it does not create one (a Vulkan
    /// device is built with the surface it presents to, which is what the windowed session's window provides).
    void* device{nullptr};
    /// The device's graphics queue family: the family the command graph records on. A negative value refuses the
    /// session, because a graph without a family can neither allocate a command buffer nor reach a queue.
    int queue_family{-1};
    /// The DEVICE's frame clock, when this session is one of several frame sources on one device (see
    /// core::DeviceClock). Null = this session owns its own; a borrowed one must outlive the session.
    core::DeviceClock* clock{nullptr};
    /// The DEVICE's park queue, which stands in for the session's own when @ref clock is borrowed. Null = own.
    core::RetirementQueue* retirement{nullptr};
};

/**
 * @brief One device's off-screen frame session: submit and completion evidence, with no window anywhere (see the
 *        file note).
 */
class OffscreenSession
{
  public:
    OffscreenSession();
    ~OffscreenSession();

    OffscreenSession(const OffscreenSession&)            = delete;
    OffscreenSession& operator=(const OffscreenSession&) = delete;

    /** @brief Brings the session up: a windowless viewer, one command graph, one record-and-submit task.
     *
     * @details
     * - Idempotent by design: an off-screen session has no surface to keep or move, so a second initialize() on a live
     *   one is a no-op rather than the windowed session's rebuild.
     *
     * @param options     The device and its graphics queue family (see OffscreenSessionOptions).
     * @param diagnostics Where the reason goes when the session cannot come up (it never throws).
     * @return true when the session is ready to run frames; false when it is not (the reason is reported).
     */
    [[nodiscard]] bool initialize(const OffscreenSessionOptions& options, vn::graphics::Diagnostics& diagnostics);

    /** @brief Tears the session down: safe before any initialize(), after a failed one, and twice. */
    void shutdown() noexcept;

    /** @brief Stops the device and counts the stop: the destructive teardown's legal wait.
     *
     * @details
     * - The off-screen counterpart of the windowed session's counted wait: a whole-session teardown drops
     *   objects a submitted command buffer may still name, so it must idle the device first.
     *
     * @note
     * - Safe before initialize(), after a failed one, and twice; a session that never submitted does not idle.
     */
    void waitDeviceIdle() noexcept;

    /** @brief Gets whether the session is up. */
    [[nodiscard]] bool initialized() const noexcept;

    /** @brief Opens a frame: mints its token and advances the viewer's frame stamp. NOTHING IS ACQUIRED.
     *
     * @details
     * - There is no image to acquire: the frame's work draws into targets that are not the presentation's, so the
     *   only frame state this step owns is the device clock's token (and the slot probe, which the first frames fold
     *   in the same way the windowed session does).
     *
     * @return The frame's token, or an empty token when the session is not up.
     */
    [[nodiscard]] core::FrameToken beginFrame();

    /** @brief Commits the open frame: submits it and reports its completion evidence. It NEVER presents.
     *
     * @details
     * - The submission is the viewer's own sequence (reset every graph, one submit per task) with the driver's
     *   VkResult KEPT: a submission that did not happen reports SubmissionFailed and answers false, its frame is
     *   abandoned on the timeline, and no completion is claimed.
     * - On success the session reports this frame's proof to the device clock (see the file note) and advances the
     *   park queue, which is the frame's last step for the same reason it is in the windowed session: a park made
     *   after that advance would be released a frame early.
     *
     * @return true when the frame was submitted; false when there was no open frame (reported) or the submission did
     *         not happen (reported, with the reason).
     */
    [[nodiscard]] bool commitFrame();

    /** @brief Gets how many frames may be in flight, as probed at session start (0 = could not learn). */
    [[nodiscard]] std::uint32_t slots() const noexcept;

    /** @brief Gets how many frames have been presented: always 0, because an off-screen session presents nothing.
     *
     * @details
     * - The method exists so a caller can read the same counter off either session without asking which kind it has;
     *   the honest answer for this one is zero, and a nonzero one would mean the backend presented a frame it said it
     *   would not.
     */
    [[nodiscard]] std::uint64_t framesPresented() const noexcept;

    /** @brief Gets how many commits reported that their submission did not happen (see @ref commitFrame). */
    [[nodiscard]] std::uint64_t lostFrames() const noexcept;

    /** @brief Gets the open frame's time stamp: seconds since this session came up (sampled by beginFrame()). */
    [[nodiscard]] float frameSeconds() const noexcept;

    /** @brief Gets how many counted device idles this session has taken (shared with the device when one is borrowed). */
    [[nodiscard]] std::size_t deviceWaits() const noexcept;

    /** @brief Gets the graphics queue family the session's command graph records on. */
    [[nodiscard]] int queueFamily() const noexcept;

    /** @brief Gets the frame timeline: this session's own, or the DEVICE's when it was given one. */
    [[nodiscard]] core::FrameTimeline& timeline() noexcept;

    /** @brief Gets the retirement queue: this session's own, or the DEVICE's when it was given one. */
    [[nodiscard]] core::RetirementQueue& retirement() noexcept;

    /** @brief Creates the command graph ONE frame records into: windowless, on the device and its family.
     *
     * @details
     * - One graph per frame, not one retained graph: the plan's execution order decides where each pass graph sits,
     *   and a graph that survived the frame would keep the previous frame's order and collect the new one behind it
     *   (the windowed session creates its own per frame for the same reason).
     *
     * @return The graph, or null when the session is not up.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::CommandGraph> makeFrameGraph() const;

    /** @brief Hands the session the command graph THIS frame records, and compiles what it holds.
     *
     * @details
     * - The existing task is kept and given the graph: a task owns the fences a submission in flight still names, so
     *   re-assigning the whole task list would destroy objects the queue is using. Only a viewer with no task at all
     *   is built by the viewer (which this session is not: it comes up with one).
     * - The compile pass follows immediately, because a graph the viewer has never seen has nodes whose
     *   implementations (descriptor sets above all) are created by exactly that pass.
     *
     * @param graphs The graphs the next commitFrame() submits.
     * @return true when the graphs were handed over and compiled; false when the session is not up or the compile
     *         pass failed (reported).
     */
    bool assignFrameGraphs(const ::vsg::CommandGraphs& graphs);

  private:
    /** @brief Folds one frame's slot observation in, and sets the park window up once the count is known. */
    void probeSlots();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace api

VN_VSG_NS_END
