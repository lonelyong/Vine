#pragma once
#include "graphics_global.hpp"

#include "FrameDescription.hpp"
#include "FrameStats.hpp"
#include "RenderSurfaceTarget.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The frame half of a device: how ONE frame comes out (see RenderDevice::executor).
 *
 * WHY IT IS ITS OWN CONTRACT. A device says what a GPU has (queues, resources, capabilities) and how a window is
 * presented (RenderDevice::createSurfaceTarget); running a frame is a third question with a third answer, and the
 * three change for different reasons: a device can hand out another window without the frame path changing, and a
 * frame path can be re-implemented without touching what the device has. Today one object answers all three (see
 * RenderBackend, which is the aggregate), and a device that splits them hands this face out itself.
 *
 * ONE WIRE: THE FRAME IS A VALUE. A frame reaches an executor in ONE call - executeFrame(FrameDescription,
 * present_to) - with every pass of it in the order it runs, each entry carrying the pass' identity, its order, its
 * resolved inputs and everything the pass issues (see FrameDescription / PassDescription / DrawDescription). There
 * is no pass scope, no incremental state setter and no call order to keep: what a frame ran and what it drew is the
 * SHAPE of the value, so the sequence rules this layer used to document cannot be violated - they cannot be
 * expressed. The engine compiles the frame and hands that value over (see RenderEngine::frameDescription); a host
 * that drives one pass itself builds the same shape (RenderPass::execute -> singlePassFrame).
 *
 * A FRAME IS A PARAMETER, NOT AN INTERVAL, and it NAMES ITS TARGET. executeFrame() does the frame's whole body: it
 * acquires the next presentable image of @p present_to when the frame has one (that is what "this is a frame" means
 * to a windowed device), records the value, and leaves it ready to be presented. The row of a frame that means
 * "the default framebuffer" (a pass naming no target) resolves to @p present_to, which is why the parameter is
 * there: one frame draws into ONE surface, and an area that has none passes none (see below). There is no
 * beginFrame() to pair it with, and no endFrame() to close it: "this frame is done" is the call returning.
 *
 * A NULL TARGET IS "THIS FRAME PRESENTS NOWHERE" - off-screen, which is a first-class state rather than a mode
 * (see Capabilities::presentation): the default-framebuffer row has nothing to resolve to, nothing is acquired and
 * nothing is presented, while the passes that name a render target still run. A device that presents nothing
 * serves only such frames, and it is the answer a display-less run gets instead of an error.
 *
 * BORROWED ARGUMENTS. Every pointer or reference INSIDE a FrameDescription (camera, commands, lights, target,
 * program) is borrowed for the duration of executeFrame() and never owned: the producer keeps them alive while the
 * call runs, and the engine reuses its own storage for them (a pass' resolved input vector, a scene's command
 * list) as soon as the call returns. A backend that needs the data later must copy or upload it and must never
 * keep such a pointer - which includes one that retains the whole frame (a render-graph builder, an offline
 * renderer): it copies what it keeps, at the moment it is called.
 *
 * WHAT MAY BE RETAINED. Retained GPU state (a content view, a compiled pipeline, a sampling slot, a texture
 * cache) is expected, but it must be keyed by the lifetime of what it serves (a pass' identity, the target's
 * object), released by the matching release* call (see RenderDevice), and must not grow with the frame count: a
 * long-running session has to reach a steady state. An executor must never require the host to call a release*
 * method for correctness - the host calls them because a pass or target is gone.
 *
 * THREADING. The engine drives one frame at a time from one thread, so calls into an executor are serialised,
 * never concurrent and never reentrant. An executor may therefore keep its state unsynchronised, but must not
 * assume it stays on the same thread across an initialize()/shutdown() pair (see RenderDevice).
 */
class VN_GRAPHICS_API RenderFrameExecutor
{
  public:
    virtual ~RenderFrameExecutor() = default;

    /** @brief Executes a whole frame from its value (see FrameDescription), resolving its default framebuffer to
     *         @p present_to.
     *
     * THE FRAME'S BODY, and the one frame entry this contract has: the engine has already compiled the frame
     * (which pass runs when, what its inputs resolved to, what each pass issues), so this is "run THIS", not
     * "here is one call at a time, in an order you have to know". Nothing is announced to the executor before or
     * after this call that is about the frame - the acquisition of the next presentable image happens inside it
     * for the target the frame names, and presenting that image is the target's own call (see
     * RenderSurfaceTarget::present), which the frame's driver makes when the frame's tail is reached.
     *
     * WHAT IT MUST DO, in the order a frame means: take the value's passes as they come (an entry with a null
     * pass is nothing to run; an entry whose description is empty is a pass that ran and issued nothing - the
     * executor keys per-pass retained state by its identity, so that is a fact it must not confuse with "the pass
     * was not in the frame"), and make the frame's content reach the target each pass names. How is the executor's
     * business: the vsg backend walks the value into its own collection stage and its session, a retained
     * renderer builds a graph from it, an offline renderer traverses it - the value says what ran, never how.
     *
     * The value is valid for the duration of this call (see the class contract), and an executor that keeps any
     * of it beyond the call copies what it keeps.
     *
     * @param frame      The frame to execute, in run order (see FrameDescription).
     * @param present_to Target the frame's default framebuffer resolves to, or nullptr for a frame that names no
     *                   target of the host's: nothing is resolved, nothing is acquired and nothing is presented on
     *                   the host's behalf (an off-screen frame, see the class contract). A device whose frame path
     *                   owns a surface of its own keeps drawing into that one - it IS its surface.
     */
    virtual void executeFrame(const FrameDescription& frame, RenderSurfaceTarget* present_to = nullptr) = 0;

    /** @brief Gets this executor's own report about its frames (see FrameReport).
     *
     * The executor is the only place that knows how a frame actually went (how many pass scopes ran, how many
     * draws they issued, what is retained and what letting go cost, and where the frame clock is), so a host,
     * a HUD and a gate read ITS numbers instead of counting a second time from outside - and the report is
     * ONE answer, so "how is it going" is one question rather than three with three written-or-not flags.
     *
     * What the executor does not keep, it leaves DISENGAGED rather than zero: "no counters" and "a frame that
     * counted nothing" are different answers (see FrameReport).
     *
     * @return The report, with the parts this executor keeps engaged.
     */
    [[nodiscard]] virtual FrameReport frameReport() const noexcept
    {
        return {};
    }
};

VN_GRAPHICS_NS_END
