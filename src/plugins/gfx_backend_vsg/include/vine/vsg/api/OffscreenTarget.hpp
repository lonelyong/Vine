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
#include <vine/vsg/core/Readback.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/core/TargetPlan.hpp>
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
     * @return The target, or null when the image, the pass or the readback buffer could not be created, or
     *         when the extent is 0 (there is no image of no size - the lifecycle plan calls that state
     *         `Repair(SizeUnknown)` and no target is built for it, see core::planTarget).
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

    /** @brief Gets the facts the lifecycle plan is decided from (`core::planTarget`).
     *
     * ONE SPELLING FOR ONE FACT. `built` is not "the attachments exist": a target that exists but has never
     * been written into cannot be LOADed, so asking the plan about it has to come back
     * `Repair(Bootstrap)` - the fact that answers that question is @ref written, not the presence of images.
     * The shape carries the device formats (what a pipeline's compatibility half needs), so a caller can hand
     * these facts to the plan AND to the compiler's target table (`TargetFacts::current`) without assembling
     * anything by hand - and the two cannot drift apart about what this target is.
     *
     * @return The instance: the description the current attachments were built for, the generation, whether
     *         they hold something loadable, and whether a submission into them was lost.
     */
    [[nodiscard]] core::TargetInstance instance() const noexcept;

    /** @brief Records that a submission into this target did not complete, so its contents cannot be trusted.
     *
     * The situation it is for: the frame was recorded and submitted, the submission failed (or the session
     * lost the device), and whether anything was written is now unknowable. The attachments still exist and
     * are still loadable objects - what changed is the FACT about their contents, which is why this is a flag
     * and not a teardown.
     *
     * What the plan does with it: `instance()` reports it and `core::planTarget` answers
     * `Repair(Bootstrap)`, so the next frame's first writer CLEARS instead of loading. Clearing is what
     * repairs the fact, so the frame that does it clears the flag again (see @ref passGraph) - and only a
     * frame that bootstraps does: a caller that ignores the plan's answer cannot silently mark an unknown
     * image as loadable.
     *
     * The caller that saw the failure is the one that knows about it; a target does not watch submissions,
     * so this is the seam a session reports through.
     */
    void invalidateAttachments() noexcept;

    /** @brief Gets the node that copies attachment 0 into host-visible memory.
     *
     * Append it to the command graph AFTER the render graph: it has to record after the pass, and it must not
     * be a child of the render graph (a copy inside the pass would run before the attachment is written).
     *
     * Asking for the node is also what makes a later @ref probe meaningful: the copy is the only thing that
     * writes the host-visible buffer, so a target whose copy was never handed out answers a probe with
     * `NotCaptured` instead of with whatever the allocation happened to hold (@ref readbackResult). Like
     * @ref written, the flag records the RECORDING side: whether the submission completed is the caller's
     * business (probe after the device is idle).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> capture() const noexcept;

    /** @brief Gets the node that copies one colour attachment into host-visible memory.
     *
     * @param attachment Colour attachment index (0 when there is only one).
     * @return The copy commands, or null when there is no such attachment, or when its format cannot be
     *         read back (`core::colorReadbackOf`: this backend packs RGBA8 only, so no buffer and no copy
     *         are built for a float attachment - the pass still renders).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> capture(std::uint32_t attachment) const;

    /** @brief Reads the last submitted frame's pixels.
     *
     * The caller must have submitted a frame that included @ref capture and waited for the device (a device
     * idle, or a fence that covers the submission). Reading without that is a race, and the probe would
     * report pixels that may be from the previous frame.
     *
     * @return A probe over the copied pixels (tightly packed RGBA8 rows), or an invalid probe when the
     *         readback was refused (see @ref readbackResult for why).
     */
    [[nodiscard]] core::PixelProbe probe() const;

    /** @brief Reads one colour attachment's pixels from the last submitted frame (see @ref capture).
     *
     * @param attachment Colour attachment index.
     * @return A probe over the copied pixels, or an invalid probe when the readback was refused (see
     *         @ref readbackResult for why).
     */
    [[nodiscard]] core::PixelProbe probe(std::uint32_t attachment) const;

    /** @brief Gets whether a readback of @p request can be served, and why not when it cannot.
     *
     * One table (`core::readbackOf`) answers for every readback entry point, so @ref probe, @ref depthProbe
     * and this call cannot disagree - and the answer is available WITHOUT touching the device, which is what
     * makes "this will not work, and here is why" possible before anything runs.
     *
     * The pixels are not part of the result: this is the classification (see core::ReadbackRefusal for what
     * each category lets a caller do), and a caller that gets `ok` reads them with @ref probe or
     * @ref depthProbe.
     *
     * @param request Which attachment to ask about.
     * @return Whether it is servable, and the refusal category when it is not.
     */
    [[nodiscard]] core::ReadbackResult readbackResult(const core::ReadbackRequest& request) const noexcept;

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

    /** @brief Gets the DEPTH attachment's view, for a pass that samples it (see core::CompiledInput).
     *
     * A shader may sample it only while the depth is sampleable (`core::depthPlan`: the host asked for it and
     * no pass preserves it), which is the same fact the plan's input table carries - and the layout a
     * sampleable depth-only target leaves behind is the one a sampler reads (see core::depthFinalLayout).
     *
     * @return The depth view, or null when the target has no depth attachment.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::ImageView> depthView() const noexcept;

    /** @brief Gets the node that copies this target's picture back for a probe.
     *
     * The READBACK seam: a target with colour attachments copies attachment 0 (see @ref capture), and a
     * DEPTH-ONLY one (a shadow map) copies its depth instead - "what this target can be asked about" is one
     * question, and the executor appends the answer for every target of the frame.
     *
     * @return The copy commands, or null when this target has nothing readable.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> readback() const noexcept;

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
     *         (a combined depth/stencil format is refused rather than converted as if it were plain depth -
     *         see core::depthReadbackOf).
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> captureDepth() const;

    /** @brief Reads the depth attachment from the last submitted frame (see @ref captureDepth).
     *
     * The numbers are the FORMAT's: D32 / D32F arrive as the float the attachment holds, D16 as its
     * unsigned value divided by 65535 (the conversion the format defines). A borrower reads the LENDER's
     * image - that is what sharing the depth means, and this probe is where it becomes measurable.
     *
     * @return A probe over the depth values, or an invalid probe when the readback was refused (see
     *         @ref readbackResult for why).
     */
    [[nodiscard]] core::DepthProbe depthProbe() const;

    /** @brief Gets the target's width in pixels. */
    [[nodiscard]] std::uint32_t width() const noexcept;

    /** @brief Gets the target's height in pixels. */
    [[nodiscard]] std::uint32_t height() const noexcept;

    /** @brief Gets the attachments' generation: it changes when a resize replaces them (see @ref resize). */
    [[nodiscard]] std::uint64_t generation() const noexcept;

    /** @brief What a plan-driven extent change did (see @ref resize). */
    struct Resized
    {
        core::TargetDecision decision{};      ///< The plan's own answer (`planTarget`).
        bool                 replaced{false}; ///< Attachments were replaced (the extent really changed).
        bool                 refused{false};  ///< A depth lease blocked it (see the declaration of resize).
        bool                 parked{false};   ///< What was replaced went to the retirement queue.
        std::uint64_t        generation{0};   ///< The generation in force when the call returned.
    };

    /** @brief Makes the target serve a new extent, the way the plan says (`core::planTarget`).
     *
     * WHAT IS REPLACED, AND WHAT IS NOT. An extent is not part of a pipeline's identity and not part of
     * render-pass compatibility (`core::TargetShape`), so a resize replaces EXACTLY the objects whose size is in
     * their description - the images, their views, the copy-back buffers and nodes, the framebuffer and the
     * graph built around it - and KEEPS the render pass and its load-op variants: a pipeline compiled for this
     * target before the resize is still the pipeline for it afterwards. A resize that rebuilt the pass would
     * hand that pipeline a render pass it was not compiled against, which is the failure the compatibility half
     * of the key exists to prevent.
     *
     * THE OLD OBJECTS ARE PARKED, NEVER FREED. The frame that had them may still be in flight - a resize happens
     * between frames, and submissions overlap - so they go to @p retirement, which releases them once the
     * timeline is past the slot that could still name them. When parking is unavailable (a caller that never
     * learned how many frames are in flight) the release runs immediately under a COUNTED device wait: an object
     * with no safe window is destroyed only when the evidence says nothing can be using it.
     *
     * WHAT IT REFUSES. A lease in either direction stops it, because a leased depth has one owner and every
     * borrower's framebuffer names the LENDER's image. A target that LENDS its depth (another target loads it)
     * refuses: the borrowers' framebuffers name the image a resize would replace and this target does not know
     * who they are, so the caller rebuilds the borrower first - the rule the reference reads as "the borrow
     * points at another image". A target that BORROWS its depth refuses as well: its new framebuffer would
     * name the lender's image at the lender's extent, and a framebuffer's attachments must be at least as
     * large as the framebuffer itself (VUID-VkFramebufferCreateInfo-pAttachments-00861), so the caller
     * resizes the lender and builds this target against it again. `refused` says so; nothing is replaced and
     * nothing is parked.
     *
     * WHAT THE TARGET COUNTS AS AFTERWARDS. The new attachments have never been drawn into, so a resize makes
     * the target behave like one that was just created: the next pass in clears (see @ref written), and the
     * generation moved, which is how a caller holding a compiled frame tells that the images it named are
     * gone. The render pass, its variants and every pipeline compiled against them are untouched.
     *
     * @param width     Wanted width in pixels (0 = not known yet: the plan repairs and nothing changes).
     * @param height    Wanted height in pixels.
     * @param timeline  The frame timeline the park is dated against (the caller's own clock).
     * @param retirement Where the replaced objects go (the caller owns it, like its device waits).
     * @return What happened: the plan's decision, whether objects were replaced, whether they were parked, and
     *         the generation in force afterwards. A wanted extent that could not be honoured is
     *         `replaced == false`: `refused` says a lease blocked it, and an action of ResizeInPlace (or
     *         Rebuild) without a refusal says the build itself failed, leaving the target serving what it had.
     */
    [[nodiscard]] Resized resize(std::uint32_t width, std::uint32_t height, const core::FrameTimeline& timeline,
                                 core::RetirementQueue& retirement);

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

    /** @brief Gets the layout this target's depth is in between passes (see core::depthFinalLayout).
     *
     * The OWNER of the image answers: a borrower's depth is wherever its lender leaves it, which is what makes
     * "sample the depth a shadow pass wrote" and "depth-test against a borrowed depth" two different starting
     * layouts rather than an assumption.
     *
     * @return The steady layout of the depth this target writes (or borrows).
     */
    [[nodiscard]] core::ImageLayout depthSteadyLayout() const noexcept;

    /** @brief Everything that depends on the extent: what a resize replaces and what it must keep alive until
     *         the retirement queue releases it. */
    struct Attachments
    {
        /// @brief One colour attachment and the memory its pixels are copied back into.
        struct Color
        {
            ::vsg::ref_ptr<::vsg::Image>                         image;
            ::vsg::ref_ptr<::vsg::ImageView>                     view;
            ::vsg::ref_ptr<::vsg::Commands>                      capture;  ///< Empty when the format is unreadable.
            ::vsg::ref_ptr<::vsg::Buffer>                        destination;
            ::vsg::ref_ptr<::vsg::DeviceMemory>                  destination_memory;
            ::vsg::ref_ptr<::vsg::MappedData<::vsg::ubyteArray>> mapped;
            /// Whether the copy node was handed out for recording (see capture). A fresh set starts
            /// UNCLEARED here: its buffers hold whatever the allocation held, which is not a picture.
            bool                                                 captured{false};
        };

        std::vector<Color>                                   colors;
        ::vsg::ref_ptr<::vsg::Image>                         depth_image;   ///< Empty when the depth is borrowed.
        ::vsg::ref_ptr<::vsg::ImageView>                     depth_view;    ///< Empty when the depth is borrowed.
        ::vsg::ref_ptr<::vsg::Commands>                      depth_capture;  ///< Empty when the format is unreadable.
        ::vsg::ref_ptr<::vsg::Buffer>                        depth_destination;
        ::vsg::ref_ptr<::vsg::DeviceMemory>                  depth_destination_memory;
        ::vsg::ref_ptr<::vsg::MappedData<::vsg::ubyteArray>> depth_mapped;
        bool                                                 depth_captured{false};  ///< See Color::captured.
        ::vsg::ref_ptr<::vsg::Framebuffer>                   framebuffer;
        ::vsg::ref_ptr<::vsg::RenderGraph>                   render_graph;
    };

    /** @brief Builds everything whose description contains the extent (images, views, copy-back, framebuffer).
     *
     * A pure "make the objects for this width and height" step, writing into @p out and touching no field of
     * this object: `create` and `resize` both need exactly this set, and a resize has to build its replacement
     * BEFORE it can part with what it has (a build that fails leaves the target serving its old extent). The
     * extent is a PARAMETER rather than this object's width()/height() for exactly that reason: while a resize
     * builds the replacement, the target still serves the extent it had.
     *
     * @param width  Extent to build for, in pixels.
     * @param height Extent to build for, in pixels.
     * @param out    Receives the objects; the caller moves them into place (or parks them, when it is done
     *               with a previous set).
     * @return true when every object was created.
     */
    [[nodiscard]] bool buildAttachments(std::uint32_t width, std::uint32_t height, Attachments& out) const;
};

V_VSG_NS_END
