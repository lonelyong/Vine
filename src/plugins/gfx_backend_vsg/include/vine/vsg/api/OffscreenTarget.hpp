#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/ImageView.h>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/core/ClearPlan.hpp>
#include <vine/vsg/core/DepthProbe.hpp>
#include <vine/vsg/core/PixelProbe.hpp>
#include <vine/vsg/vsg_global.hpp>

namespace vsg
{
class Device;
}

/**
 * @brief An off-screen colour target and the readback path that turns it into pixels a phase can assert.
 *
 * WHY A PHASE NEEDS ONE. "It rendered" and "validation was clean" are both satisfiable by a program that
 * draws nothing, so the only evidence this backend accepts for its picture is the pixels themselves. A
 * swapchain cannot be read after it is presented, and reading it before is a race with the presentation
 * engine; an off-screen target has neither problem, so the evidence path is a target of our own.
 *
 * THE TARGET IS ONE IMAGE, ONE RENDER PASS, ONE FRAMEBUFFER. The pass clears the colour attachment,
 * whatever the caller adds as children draws into it, and leaves it in SHADER_READ_ONLY - the layout a later
 * pass samples it in, which is what makes "an input of the next pass" work without a barrier: an input target
 * is a picture somebody else reads. Committing to that final layout is also what makes the readback a single
 * copy with one pair of transitions (the copy needs TRANSFER_SRC, and the attachments go back to being
 * sampleable afterwards, because a capture may run between two passes). The pass DISCARDS the previous
 * contents every time (initial layout is UNDEFINED), which is exactly right for a phase that clears and draws.
 *
 * THE COPY IS PART OF THE COMMAND GRAPH, not a separate submission: `capture()` returns the node the caller
 * appends AFTER the render graph, so the copy is recorded in the same command buffer, on the same queue, in
 * order. After the frame is submitted and the device is idle, `probe()` reads the mapped destination buffer.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief An off-screen colour target plus its copy-back path. */
class OffscreenTarget
{
  public:
    /** @brief The target's shape and what the pass clears it to. */
    struct Layout
    {
        std::uint32_t width{256};          ///< Image width in pixels.
        std::uint32_t height{256};         ///< Image height in pixels.
        float         clear_color[4]{ 0.0F, 0.0F, 0.0F, 1.0F };  ///< Clear value (RGBA, linear).
    };

    /** @brief The target's shape in the engine's terms: several colour attachments, optional depth.
     *
     * The shape is the core's (@ref core::TargetShape), not a list of API formats: a target the backend draws
     * into has to be expressible in the SDK's vocabulary, and the conversion to the API's enums is the API
     * layer's job (the same mapping the pipeline factory uses).
     */
    struct TargetLayout
    {
        std::uint32_t                                          width{256};   ///< Extent in pixels.
        std::uint32_t                                          height{256};
        std::vector<vine::graphics::RenderTarget::ColorFormat> color_formats{ vine::graphics::RenderTarget::ColorFormat::RGBA8 };
        std::optional<vine::graphics::RenderTarget::DepthFormat> depth_format;  ///< Absent for colour only.
        bool           depth_sampleable{false};  ///< The host asked for a depth a shader may sample (promotion).
        core::ClearPolicy clear;        ///< What the pass clears (bootstrap applies).
    };

  public:
    /** @brief Creates the image, the pass, the framebuffer and the copy-back commands.
     *
     * @param device The device everything belongs to (not owned).
     * @param layout Image extent and clear value.
     * @return The target, or null when the image, the pass or the readback buffer could not be created.
     */
    static std::unique_ptr<OffscreenTarget> create(::vsg::ref_ptr<::vsg::Device> device, const Layout& layout);

    /** @brief Creates a target with the given shape (several colour attachments and/or a depth attachment).
     *
     * The pass' clear decisions come from the core's plan (`core::planClearValues`) with the bootstrap rule on:
     * a freshly built image is UNDEFINED, so every attachment clears - attachment 0 with the policy's colour,
     * the extras with transparent black, and a depth with the reverse-Z far plane.
     *
     * @param device The device everything belongs to (not owned).
     * @param layout The shape and what the pass clears.
     * @param depth_source When given, this target USES that target's depth image instead of owning one - the
     *        classic shared-depth case, where an earlier pass wrote the scene's depth and this pass draws
     *        against it. Such a pass LOADs the depth (clearing it would undo what the lender wrote) and never
     *        promises it to a shader (the policy is the lender's, see `core::depthPlan`). The source must
     *        outlive the borrower and the depth formats must match.
     * @return The target, or null when any image, the pass or a readback buffer could not be created, or when
     *         the borrowed depth does not fit (no source depth, mismatched formats, a LOAD plan without one).
     */
    static std::unique_ptr<OffscreenTarget> create(::vsg::ref_ptr<::vsg::Device> device, const TargetLayout& layout,
                                                   const OffscreenTarget* depth_source = nullptr);

    ~OffscreenTarget();

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

  public:
    /** @brief Gets the render graph to add content to (the caller's children are drawn after the clear). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> renderGraph() const noexcept;

    /** @brief Gets a render graph over this target's attachments that clears or loads per @p policy.
     *
     * WHY A GRAPH PER PASS. A pass scope IS one render pass in the recording, and what a pass does to its
     * attachments belongs to the pass, not to the target: a later pass LOADS what an earlier one wrote (that
     * is what "two passes over one target" means), and the one recorded later is the one that lands. The
     * render pass OBJECT is shared by every pass whose load-op variant is the same, the framebuffer is shared
     * by all of them, and the graph, its render area and its clear VALUES are per pass.
     *
     * That is also what makes "the executor records in the schedule's order, not in the host's call order" a
     * claim a PIXEL can check: the last pass recorded owns the final colour, and the earlier pass' picture is
     * still underneath it.
     *
     * The three inputs are the plan's: what the pass asked to clear, whether it is the target's FIRST writer
     * (a fresh image cannot be loaded - it must clear), and whether a later pass reads the depth this one
     * writes (never cleared, not even by the bootstrap rule). A pass that is neither the first writer nor asks
     * for a clear LOADS, which is a variant this target builds the first time a frame needs it.
     *
     * @param policy          What this pass asks the target to clear (attachment 0's colour, and the depth).
     * @param bootstrap       Whether this pass is the first writer of freshly built attachments.
     * @param depth_preserved Whether a later pass reads the depth this one writes.
     * @return The graph this pass records into (its only child is the target's content view - see
     *         `addContent`), or null when the target has no attachments or the variant could not be built.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> passGraph(const core::ClearPolicy& policy, bool bootstrap = true,
                                                              bool depth_preserved = false);

    /** @brief Gets how many load-op variants of this target's render pass have been built.
     *
     * One per distinct set of load/store operations and layouts a pass has asked for, so a phase can read
     * "this frame's two passes were one clear and one load" off the target (see core::LoadOpVariantKey).
     */
    [[nodiscard]] std::size_t passVariantCount() const noexcept;

    /** @brief Gets whether anything has been recorded into this target's attachments yet.
     *
     * THE PLAN'S BOOTSTRAP FACT, and the reason it has to come from the target: an image nobody has written
     * is in the UNDEFINED layout, so a pass that LOADs it reads whatever the driver left in the memory. The
     * compiler already has the rule (`core::planTarget`'s `RepairReason::Bootstrap`: "nothing usable yet - the
     * first pass in has to clear"), but it can only apply it if the facts it is handed say so: a target's
     * `TargetFacts::current.built` is FALSE until this says true, i.e. until the first pass graph has been
     * built for it. A host that hard-codes `built = true` for a target it has just created makes the first
     * pass load undefined contents - which is exactly the hole this accessor closes (measured: the
     * validation layer reports `VUID-vkCmdDraw-None-09600` on a DEPTH attachment - "expected
     * DEPTH_STENCIL_ATTACHMENT_OPTIMAL, current layout UNDEFINED" - as soon as a pass that declares no clear
     * is not forced to clear).
     *
     * It becomes true when a pass' graph is built (@ref passGraph), not when a frame is submitted: the fact is
     * "something has been recorded into it", and a frame that is recorded and then dropped describes the same
     * target as one that was not.
     */
    [[nodiscard]] bool written() const noexcept;

    /** @brief Gets the node that copies attachment 0 into host-visible memory.
     *
     * Append it to the command graph AFTER the render graph: it has to record after the pass, and it must not
     * be a child of the render graph (a copy inside the pass would run before the attachment is written).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> capture() const noexcept;

    /** @brief Gets the node that copies one colour attachment into host-visible memory.
     *
     * @param attachment Colour attachment index (0 when there is only one).
     * @return The copy commands, or null when there is no such attachment.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> capture(std::uint32_t attachment) const;

    /** @brief Reads the last submitted frame's pixels.
     *
     * The caller must have submitted a frame that included @ref capture and waited for the device (a device
     * idle, or a fence that covers the submission). Reading without that is a race, and the probe would
     * report pixels that may be from the previous frame.
     *
     * @return A probe over the copied pixels (tightly packed RGBA8 rows).
     */
    [[nodiscard]] core::PixelProbe probe() const;

    /** @brief Reads one colour attachment's pixels from the last submitted frame (see @ref capture).
     *
     * @param attachment Colour attachment index.
     * @return A probe over the copied pixels, or an invalid probe for an index that does not exist.
     */
    [[nodiscard]] core::PixelProbe probe(std::uint32_t attachment) const;

    /** @brief Gets how many colour attachments this target has. */
    [[nodiscard]] std::uint32_t colorAttachmentCount() const noexcept;

    /** @brief Gets one colour attachment's image view, for a pass that samples this target as an input.
     *
     * Why the VIEW and not the identity: the sampled images of a pass' input are GPU objects, and they belong
     * to this target. The layer that owns the target answers for them, and the content layer binds what it is
     * offered (see ContentPass::record). The view is left in SHADER_READ_ONLY by this target's pass, so a
     * descriptor built from it names the layout the image really is in.
     *
     * @param attachment Colour attachment index (0 when there is only one).
     * @return The view, or null when there is no such attachment.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::ImageView> colorView(std::uint32_t attachment) const noexcept;

    /** @brief Gets the shape the render pass, the framebuffer and every pipeline were built against.
     *
     * The compatibility half of a pipeline's identity, in the engine's terms: attachment formats, depth
     * format, sample count and subpass. It is read by whatever builds a pipeline key for a pass, so the key
     * names the shape the target REALLY has rather than one a plan happened to carry (the plan holds no
     * vector - see the design's §11.16d - so the shape is answered here, by the object that owns it).
     *
     * @return The shape (already stored: this is the one the render pass was created from).
     */
    [[nodiscard]] core::TargetShape shape() const noexcept;

    /** @brief Gets whether this target has a depth attachment. */
    [[nodiscard]] bool hasDepth() const noexcept;

    /** @brief Gets this target's depth plan, derived from its facts by the core (`core::depthPlan`).
     *
     * What it answers, and why it is derived rather than stored:
     *
     *   * a BORROWED depth is never sampleable and never preserved by the borrower - the policy is the
     *     lender's;
     *   * a promotion (the host asked for a sampleable depth) survives only while no pass preserves the
     *     depth, and another target loading this one's depth is exactly such a pass - so lending revokes
     *     the promotion, and the revocation disappears when the borrower does.
     *
     * @return The plan: whether there is a depth, whether a shader may sample it, whether it is borrowed, and
     *         whether a pass depends on what an earlier pass wrote.
     */
    [[nodiscard]] core::DepthPlan depth() const noexcept;

    /** @brief Gets the target whose depth image this target uses, or null when it owns its own. */
    [[nodiscard]] const OffscreenTarget* depthSource() const noexcept;

    /** @brief Gets the node that copies the DEPTH attachment into host-visible memory.
     *
     * Append it after the passes that write the depth. The copy needs the attachment in the transfer layout, so
     * it transitions it there and back: a shared depth must still be usable as an attachment by the pass that
     * comes after this node, and leaving it in the transfer layout would break exactly that.
     *
     * @return The copy commands, or null when there is no depth attachment or its format cannot be read
     *         (a combined depth/stencil format is refused rather than converted as if it were plain depth).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> captureDepth() const;

    /** @brief Reads the depth attachment from the last submitted frame (see @ref captureDepth).
     *
     * @return A probe over the depth values, or an invalid probe when the depth is absent, unreadable or was
     *         not captured.
     */
    [[nodiscard]] core::DepthProbe depthProbe() const;

    /** @brief Gets the target's width in pixels. */
    [[nodiscard]] std::uint32_t width() const noexcept;

    /** @brief Gets the target's height in pixels. */
    [[nodiscard]] std::uint32_t height() const noexcept;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    OffscreenTarget();

    /** @brief Gets the render pass for @p key, building and remembering it when this target has not served
     *         that load-op variant before (see LoadOpVariantKey).
     *
     * @param key The variant a pass' clear plan resolves to.
     * @return The render pass, or null when the API refused the description.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderPass> renderPassFor(const core::LoadOpVariantKey& key);
};

V_VSG_NS_END
