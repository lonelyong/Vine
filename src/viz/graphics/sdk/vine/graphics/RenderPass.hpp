#pragma once
#include "graphics_global.hpp"

#include <optional>
#include <span>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "DepthPolicy.hpp"
#include "DepthRange.hpp"
#include "DepthView.hpp"
#include "FrameDescription.hpp"
#include "ImageRef.hpp"
#include "ImageSource.hpp"
#include "PassDesc.hpp"
#include "PresentTarget.hpp"
#include "Sampler.hpp"
#include "StateResolution.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class RenderBackend;
class RenderTarget;
class Scene;
class ShaderProgram;

using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

namespace detail
{

struct PassNode;

} // namespace detail

/**
 * @brief One rendering stage: its declaration (a PassDesc, stored once) plus the execution facts a frame resolved.
 *
 * @details
 * - ONE FACT, ONE STORAGE. The pass IS its declaration: every accessor below reads or writes a field of declaration(), and a
 *   descriptor stated through `RenderEngine::applyPlan` is adopted as-is (see setDesc). There is no second
 *   copy of the clear colour, the camera, the inputs or the layer mask to keep in step, so "the value form and the
 *   node form disagree" is not a defect this type can have.
 * - WHAT IS NOT HERE: the facts the FRAME produces - the transient target a compile resolved, the measured record
 *   time - belong to the engine's slot. A pass that declares a transient target still draws into the target the compile
 *   handed it (see renderTarget()).
 * - THE EXTENSION POINT IS describe(). A pass issues what it describes, and describe() needs nothing from a backend
 *   (the picture's shape is the camera's), so a whole frame of descriptions can be assembled before anything runs.
 *
 * @note
 * - Naming: this is an engine-level rendering STAGE (an OSG render stage, a VulkanSceneGraph view + target + clear
 *   policy), NOT a Vulkan render pass. Several passes targeting one framebuffer are recorded into one backend render
 *   pass; the backend owns that distinction.
 */
class VN_GRAPHICS_API RenderPass : public Object, public RefCounted<RenderPass> {
    VN_OBJECT_META_DECL;

  public:
    /// @brief Constructs a Scene pass: the ordinary stage that draws content through a camera.
    RenderPass();
    ~RenderPass() override;

  public:
    /**
     * @brief Gets the declaration this pass IS.
     *
     * @details
     * - The single storage of everything the pass states: kind, camera, content, target, inputs, clear policy, state,
     *   layer mask and the per-frame hook.
     *
     * @return The declaration (valid until the next adoption of a declaration).
     */
    [[nodiscard]] const PassDesc& declaration() const noexcept
    {
        return desc_;
    }

    /**
     * @brief Gets the pass name.
     *
     * @return The name used by diagnostics and the frame plan (empty when never set).
     */
    [[nodiscard]] String name() const
    {
        return desc_.name;
    }

    /**
     * @brief Gets the content scene this pass draws.
     *
     * @return The scene (null draws nothing, which is how a content-less pass states itself).
     */
    [[nodiscard]] raw_ptr<Scene> content() const noexcept
    {
        return desc_.content.get();
    }

    /**
     * @brief Gets the target this pass draws into.
     *
     * @details
     * - The EFFECTIVE target: an explicit target as declared, the target the compile resolved for a transient
     *   declaration, or null for the window (see Target / resolvedTarget).
     *
     * @return The target, or null for the window / the frame's own presented image.
     */
    [[nodiscard]] raw_ptr<RenderTarget> renderTarget() const noexcept
    {
        return resolved_target_ != nullptr ? resolved_target_.get() : desc_.target.target.get();
    }

    /**
     * @brief Gets the depth image this pass draws against.
     *
     * @return The view stated by setDepthView, or an invalid view when the pass uses its target's own depth.
     */
    [[nodiscard]] DepthView depthView() const
    {
        return desc_.depth_view.value_or(DepthView{});
    }

    /**
     * @brief Gets the camera this pass draws through.
     *
     * @return The camera (null draws nothing).
     */
    [[nodiscard]] raw_ptr<Camera> camera() const noexcept
    {
        return desc_.camera.get();
    }

    /**
     * @brief Gets one attachment's clear colour (attachment 0 unless one is named).
     *
     * @details
     * - Attachment 0 defaulting to the #333333 grey the main WINDOW pass was written against is the value stated
     *   in the space it is DISPLAYED in and decoded once (see kDefaultClearColor); nothing in the engine names a
     *   colour for the off-screen targets its builders create, so an off-full-screen pass that wants one states it.
     * - Attachments 1 and beyond are not that value: an unstated attachment is transparent black, so a consumer can
     *   tell "nothing drawn here" from data (see ClearPolicy).
     *
     * @param attachment Colour attachment index (0 = the first, the default).
     * @return The LINEAR colour stated for that attachment, or transparent black for an unstated one past the first.
     */
    [[nodiscard]] Colorf clearColor(int attachment = 0) const;

    /**
     * @brief Returns whether this pass asks for a CLEARED DEPTH attachment.
     *
     * @details
     * - A REQUEST, not the state - hence "should" rather than the is/has prefix: what a target can express is the
     *   target's fact, and a backend that cannot honour the request reports it (see DiagnosticCategory).
     * - The DEPTH half of the pass' clear; the colour half is isClearEnabled, and the two are independent, so a HUD
     *   overlay can clear its own depth region and keep the picture underneath.
     * - Unstated, the KIND's policy answers for a pass that CLEARS: a Scene pass clears both attachments, a Fullscreen
     *   pass neither. A pass that clears no colour does not imply depth - only an explicit `clear_depth = true` asks
     *   for a depth-only clear (see PassDesc::clear_depth).
     *
     * @return True when this pass asks for a cleared depth buffer.
     */
    [[nodiscard]] bool shouldClearDepth() const noexcept;

    /**
     * @brief Returns whether this pass asks for CLEARED COLOUR attachments.
     *
     * @details
     * - The COLOUR half of the pass' clear; the depth half is shouldClearDepth, `clearPolicy()` states both, and a
     *   pass that asks for neither announces no clear at all.
     *
     * @return True when the pass clears its colour attachments (the default for a Scene pass, never for a Fullscreen
     *         pass: a composite draws over what is already there).
     */
    [[nodiscard]] bool isClearEnabled() const noexcept;

    /**
     * @brief Returns how this pass's content handles depth (the pass' own policy).
     *
     * @return The depth policy (DepthPolicy::TestAndWrite by default), which is what content that authored no depth
     *         state of its own gets (see resolveRenderState).
     */
    [[nodiscard]] DepthPolicy depthPolicy() const noexcept
    {
        return desc_.depth_policy;
    }

    /**
     * @brief What this pass says about the state of the content it draws.
     *
     * @details
     * - The pass side of the state priority table: its depth policy and its rasterisation override, in one value,
     *   announced to the backend so it can resolve each command's final state in one place.
     *
     * @return The pass' state for its content.
     */
    [[nodiscard]] PassRenderState stateOverrides() const noexcept;

    /**
     * @brief Returns whether this pass is drawn by the engine this frame.
     *
     * @return True when enabled (the default).
     */
    [[nodiscard]] bool isEnabled() const noexcept
    {
        return desc_.enabled;
    }

    /**
     * @brief Returns whether a sub-viewport is configured for this pass.
     *
     * @return True when the pass draws into a stated rectangle.
     */
    [[nodiscard]] bool hasViewport() const noexcept
    {
        return desc_.viewport.has_value();
    }

    /**
     * @brief Gets the configured draw viewport.
     *
     * @return The draw rectangle in device pixels; only meaningful when hasViewport() is true.
     */
    [[nodiscard]] Viewport viewport() const noexcept
    {
        return desc_.viewport.value_or(Viewport{});
    }

    /**
     * @brief Gets the depth band this pass maps its draws into.
     *
     * @return The band, or nullopt when the pass maps into the whole depth buffer.
     */
    [[nodiscard]] std::optional<DepthRange> depthRange() const noexcept
    {
        return desc_.depth_range;
    }

    /** @brief Gets the device-pixel size of the surface this pass draws into when it has no target. */
    [[nodiscard]] Viewport drawSize() const noexcept
    {
        return draw_size_;
    }

    /**
     * @brief Returns which layers of the content this pass draws (see Node::setLayers).
     *
     * @return The mask (kAllLayers by default, which draws every layer).
     */
    [[nodiscard]] std::uint32_t layerMask() const noexcept
    {
        return desc_.layer_mask;
    }

    /**
     * @brief Gets the output slot name this pass publishes under.
     *
     * @return The output name (empty when publishing is disabled).
     */
    [[nodiscard]] String outputName() const
    {
        return desc_.output.name;
    }

    /**
     * @brief Gets the output image this pass produces.
     *
     * @return The declared output image, or null when none was declared.
     */
    [[nodiscard]] raw_ptr<ImageRef> output() const noexcept
    {
        return desc_.output.image.get();
    }

    /**
     * @brief Gets the whole target this pass promises to consumers.
     *
     * @return The target the pass draws into when the whole-target claim is stated, or null.
     */
    [[nodiscard]] raw_ptr<RenderTarget> outputTarget() const noexcept
    {
        return desc_.output.whole_target ? renderTarget() : nullptr;
    }

    /**
     * @brief Gets the declared input names.
     *
     * @return The names, in declaration order.
     */
    [[nodiscard]] const std::vector<String>& inputNames() const noexcept
    {
        return desc_.input_names;
    }

    /**
     * @brief Gets the images this pass consumes, in declaration order.
     *
     * @return The input declarations (produced images or imported textures), as added.
     */
    [[nodiscard]] const std::vector<ImageInput>& inputs() const noexcept
    {
        return desc_.inputs;
    }

    /**
     * @brief Gets the whole targets this pass consumes.
     *
     * @return The input targets, in declaration order.
     */
    [[nodiscard]] const std::vector<intrusive_ptr<RenderTarget>>& inputTargets() const noexcept
    {
        return desc_.input_targets;
    }

    /**
     * @brief Receives the engine-resolved inputs.
     *
     * @details
     * - Called by the engine once per frame before the pass is described: one entry per declaration, in declaration
     *   order - the images of inputs() first, then the targets of inputTargets(); a pass that declared neither falls
     *   back to its inputNames() resolved against the engine's named-output registry.
     * - An entry stays empty when nothing produced it this frame (reported once per episode).
     * - The sources are BORROWED: whatever produced a target and whatever owns a texture keep them alive.
     *
     * @param inputs Resolved input sources, in declaration order (names last).
     */
    virtual void resolveInputTextures(std::span<const ImageSource> inputs)
    {
        (void)inputs;
    }

    /**
     * @brief Gets the pass-level program override.
     *
     * @return The program every command is rewritten to, or null for per-geometry programs.
     */
    [[nodiscard]] raw_ptr<ShaderProgram> programOverride() const noexcept
    {
        return desc_.program_override.get();
    }

    /**
     * @brief Gets the pass-level polygon-mode override.
     *
     * @return The mode, or nullopt when each command keeps its own.
     */
    [[nodiscard]] std::optional<PolygonMode> polygonModeOverride() const noexcept
    {
        return desc_.polygon_mode;
    }

    /**
     * @brief Gets the pass-level point-size override.
     *
     * @return The size in framebuffer pixels, or nullopt when each command keeps its own.
     */
    [[nodiscard]] std::optional<float> pointSizeOverride() const noexcept
    {
        return desc_.point_size;
    }

    /**
     * @brief Gets the pass-level line-width override.
     *
     * @return The width in framebuffer pixels, or nullopt when each command keeps its own.
     */
    [[nodiscard]] std::optional<float> lineWidthOverride() const noexcept
    {
        return desc_.line_width;
    }

    /**
     * @brief Describes what this pass issues this frame: its target, its state and its draw packets.
     *
     * @details
     * - THE EXTENSION POINT: what a pass does is data, and a subclass that draws something else fills in a different
     *   description (FullscreenPass draws a full-screen program instead of collected geometry).
     * - ONE CALL, because nothing here needs a backend: the picture's shape is the camera's, and the VIEW it states
     *   is collected by the frame's expansion step (see DrawListSource / `expandPass`), so a whole frame's worth of
     *   requests can be assembled before anything runs.
     * - A pass that issues NOTHING this frame answers false (a scene pass with no scene, a full-screen pass with no
     *   resolved source): nothing is announced at all, which keeps "this pass is not running" distinguishable from
     *   "this pass has no picture" (an empty PassDraw::packets).
     *
     * @param scene Content scene to draw (a scene pass with none issues nothing; a content-agnostic pass ignores it).
     * @param out   Receives the description; untouched when this returns false.
     * @return true when this pass issues anything at all this frame.
     */
    virtual bool describe(raw_ptr<Scene> scene, PassDraw& out);

    /**
     * @brief Executes this pass on its own: describes it, then hands the backend the one-pass frame that says so.
     *
     * @details
     * - SUGAR, NOT AN EXTENSION POINT (deliberately not virtual): there is one way a pass becomes a picture -
     *   describe() fills a value and a frame IS that value. While this was virtual, an override could draw something
     *   the engine's compile never saw.
     * - It builds `singlePassFrame(this, 0, {}, description)` and calls the executor's single entry, the same shape
     *   the engine hands over for a whole frame, so the two spellings cannot drift.
     * - The EXECUTOR presents through @p present_to at the frame's tail, so the caller does not present afterwards.
     *
     * @param scene      Content scene to draw (a content-agnostic pass ignores it).
     * @param backend    Device to render with (null does nothing).
     * @param present_to Target the pass' default framebuffer resolves to, or nullptr for off-screen work.
     */
    void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend, raw_ptr<PresentTarget> present_to = nullptr);

  protected:
    /**
     * @brief Constructs a pass of @p kind (the kind decides the defaults the declaration leaves unstated).
     *
     * @param kind How this pass executes.
     */
    explicit RenderPass(DrawKind kind);

    /**
     * @brief Fills this pass' own facts (target, viewport, clear, state) into @p out.
     *
     * @details
     * - The ONE mapping from the pass' declaration to a description, so a subclass states what is DIFFERENT about it
     *   without restating what is the same.
     *
     * @param out Receives the facts; its draw list is cleared.
     */
    void fillPassFacts(PassDraw& out);

    /// The declaration this pass IS: one storage for every fact the pass states.
    ///
    /// Protected because a subclass that states something of its own (FullscreenPass names the program it draws through)
    /// writes it here instead of keeping a second copy of a fact the base already owns.
    PassDesc desc_;

  private:
    friend struct detail::PassNode;

    /**
     * @brief The attachment policy this pass announces, or nothing when it states neither a clear nor an op.
     *
     * @details
     * - THE ONE RULE for both spellings: the group flags answer every attachment, and a stated per-attachment op
     *   overrides its own attachment. A pass that only states ops ("load both, discard one") still announces a policy,
     *   because that statement is what a backend must receive.
     *
     * @return The policy, or nullopt when this pass clears nothing and states no per-attachment op.
     */
    [[nodiscard]] std::optional<ClearPolicy> clearPolicy() const;

    /**
     * @brief Adopts @p desc as this pass' whole declaration (see detail::PassNode).
     *
     * @details
     * - Everything the pass stated before is replaced - this is "I am this descriptor", not a merge.
     * - A resolved transient belonged to the declaration that asked for it, so it is dropped.
     * - A STATED depth view is applied to the declared target; an absent (or invalid) one is not a statement about
     *   the target, so re-stating a declaration never un-says another pass' edge.
     *
     * @param desc Declaration to adopt (moved in).
     */
    void adoptDesc(PassDesc desc);

    /**
     * @brief Takes the target the frame compile resolved for this pass' transient declaration (execution state).
     *
     * @param target The target a transient declaration resolved to, or null to drop the resolution.
     */
    void resolveTarget(intrusive_ptr<RenderTarget> target) noexcept
    {
        resolved_target_ = std::move(target);
    }

    /**
     * @brief States the device-pixel size of the surface this pass draws into when it has no target (execution state).
     *
     * @details
     * - A pass that writes an explicit RenderTarget already knows its size; a WINDOW pass does not. The engine
     *   states it once per frame, and collection uses it as the viewport for view-dependent features (AutoTransform
     *   screen-space scaling).
     * - It does NOT change the backend's viewport: a pass without a stated viewport still draws the full target.
     *
     * @param width  Draw width in DEVICE pixels (<= 0 clears the size).
     * @param height Draw height in DEVICE pixels (<= 0 clears the size).
     */
    void setDrawSize(int width, int height) noexcept
    {
        draw_size_ = (width > 0 && height > 0) ? Viewport{ 0, 0, width, height } : Viewport{};
    }

    /// Device-pixel size of the surface for a pass that has no target (see setDrawSize).
    Viewport draw_size_{};

    /// The target a transient declaration resolved to this frame, or null when the declaration is not transient.
    ///
    /// EXECUTION STATE beside the declaration: set by the frame compile (see resolveTarget), cleared when the
    /// declaration changes, so renderTarget() answers the effect while declaration().target keeps the request.
    intrusive_ptr<RenderTarget> resolved_target_{};
};

using RenderPassPtr = intrusive_ptr<RenderPass>;

namespace detail
{

/**
 * @brief The engine's write door onto a pass node: adoption and execution facts, not authoring.
 *
 * @details
 * - TWO CALLERS, ONE SHAPE: the frame compile adopts the declarations it was handed (`RenderEngine::applyPlan` and a
 *   transient resolution), and nothing else may write a node. A host's edit is a declaration it hands to the plan -
 *   there is no setter on `RenderPass` for it to reach (see E-04).
 * - These are NOT authoring entries: `adopt` replaces the whole declaration (what the plan diff decided), it does not
 *   merge a field; `resolveTarget` and `setDrawSize` state facts of the FRAME (which target a transient resolved to,
 *   how big the surface is), which no declaration carries.
 */
struct VN_GRAPHICS_API PassNode {
    /**
     * @brief Makes @p desc the pass' whole declaration.
     *
     * @param pass Pass to adopt it (a node the engine owns).
     * @param desc Declaration to adopt (moved in).
     */
    static void adopt(RenderPass& pass, PassDesc desc);

    /**
     * @brief States the target a transient declaration resolved to this frame.
     *
     * @param pass   Pass whose transient was resolved.
     * @param target The resolved target, or null to drop the resolution.
     */
    static void resolveTarget(RenderPass& pass, intrusive_ptr<RenderTarget> target) noexcept;

    /**
     * @brief States the device-pixel surface size a window pass draws into.
     *
     * @param pass   Pass to state it on.
     * @param width  Width in device pixels (<= 0 clears it).
     * @param height Height in device pixels (<= 0 clears it).
     */
    static void setDrawSize(RenderPass& pass, int width, int height) noexcept;
};

} // namespace detail

VN_GRAPHICS_NS_END
