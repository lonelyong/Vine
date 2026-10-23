#pragma once
#include "graphics_global.hpp"

#include <optional>
#include <span>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>

#include "DepthMode.hpp"
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

/**
 * @brief One rendering stage: its declaration (a PassDesc, stored once) plus the execution facts a frame resolved.
 *
 * @details
 * - ONE FACT, ONE STORAGE. The pass IS its declaration: every accessor below reads or writes a field of declaration(), and a
 *   descriptor handed to `RenderEngine::addPass(const PassDesc&)` is adopted as-is (see setDesc). There is no second
 *   copy of the clear colour, the camera, the inputs or the layer mask to keep in step, so "the value form and the
 *   node form disagree" is not a defect this type can have.
 * - WHAT IS NOT HERE: the facts the FRAME produces - the transient target a compile resolved, the measured record
 *   time - belong to the engine's slot. A pass that declares `writes_image` still draws into the target the compile
 *   handed it (see renderTarget()).
 * - THE EXTENSION POINT IS describe(). A pass issues what it describes, and describe() needs nothing from a backend
 *   (the picture's shape is the camera's), so a whole frame of descriptions can be assembled before anything runs.
 *
 * @note
 * - Naming: this is an engine-level rendering STAGE (an OSG render stage, a VulkanSceneGraph view + target + clear
 *   policy), NOT a Vulkan render pass. Several passes targeting one framebuffer are recorded into one backend render
 *   pass; the backend owns that distinction.
 */
class VN_GRAPHICS_API RenderPass : public Object, public RefCounted<RenderPass>
{
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
     * @return The declaration (valid until the next setDesc or mutating setter).
     */
    [[nodiscard]] const PassDesc& declaration() const noexcept { return desc_; }

    /**
     * @brief Adopts @p desc as this pass' whole declaration.
     *
     * @details
     * - Everything the pass stated before is replaced - this is "I am this descriptor", not a merge - which is what
     *   makes a descriptor-declared pass and a hand-built one the same object (see RenderEngine::addPass).
     * - A stated depth view is applied to the declared target again (see setDepthView), so a re-stated declaration
     *   keeps its depth edge.
     *
     * @warning A layered depth view is kept as a declaration and applied to nothing: layered targets do not exist yet,
     *          and the engine is what reports the refusal before adopting.
     *
     * @param desc Declaration to adopt (moved in).
     */
    void setDesc(PassDesc desc);

    /**
     * @brief Gets the pass name.
     *
     * @return The name used by diagnostics and the frame plan (empty when never set).
     */
    [[nodiscard]] String name() const { return desc_.name; }

    /**
     * @brief Sets the pass name.
     *
     * @param name Name used by diagnostics and the frame plan.
     */
    void setName(const String& name) { desc_.name = name; }

    /**
     * @brief Gets the content scene this pass draws.
     *
     * @return The scene (null draws nothing, which is how a content-less pass states itself).
     */
    [[nodiscard]] raw_ptr<Scene> content() const noexcept { return desc_.content.get(); }

    /**
     * @brief Binds the content scene this pass draws (see RenderEngine::bindPassContent).
     *
     * @details
     * - The pass keeps the scene alive. An overlay pass (a gizmo, an FPS readout) owns the scene it draws, so this is
     *   also how such a pass is built.
     *
     * @param scene Scene to draw, or null to draw no content.
     */
    void setContent(intrusive_ptr<Scene> scene) { desc_.content = std::move(scene); }

    /**
     * @brief Gets the target this pass draws into.
     *
     * @return The target, or null for the window / the frame's own presented image.
     */
    [[nodiscard]] raw_ptr<RenderTarget> renderTarget() const noexcept { return desc_.writes.get(); }

    /**
     * @brief Sets the target this pass draws into.
     *
     * @details
     * - The pass keeps the target alive: it draws into it every frame, so it must not be able to outlive it.
     * - The compile also calls this with the transient target it resolved for a pass that declared
     *   `PassDesc::writes_image`, which is why the effective target is the one this returns.
     * - A stated depth view is applied to the new target (see setDepthView); a pass that stated none does not un-say
     *   what another pass set.
     *
     * @param target Target to draw into, or null for the window.
     */
    void setRenderTarget(intrusive_ptr<RenderTarget> target);

    /**
     * @brief Gets the depth image this pass draws against.
     *
     * @return The view stated by setDepthView, or an invalid view when the pass uses its target's own depth.
     */
    [[nodiscard]] DepthView depthView() const { return desc_.depth_view.value_or(DepthView{}); }

    /**
     * @brief States the depth image this pass draws against, when it is another pass' image (see DepthView).
     *
     * @details
     * - THE PASS DECLARES THE EDGE; the target stores the mechanism. The view is applied to the pass' current target
     *   (RenderTarget::shareDepth) and re-applied when a target is set later, so a pass moved to another target keeps
     *   drawing against the same depth.
     * - Constraints are the target's (both off-screen and equally sized, the lender renders earlier, its depth is not
     *   promoted); the backend reports a plan that breaks them rather than guessing.
     * - An invalid view clears the declaration: the pass goes back to its target's own depth.
     *
     * @warning A view whose layer is not 0 is kept as a declaration but applied to nothing: layered targets are not
     *          built yet (the engine reports the refusal).
     *
     * @param view The depth image this pass draws against.
     */
    void setDepthView(DepthView view);

    /**
     * @brief Gets the camera this pass draws through.
     *
     * @return The camera (null draws nothing).
     */
    [[nodiscard]] raw_ptr<Camera> camera() const noexcept { return desc_.camera.get(); }

    /**
     * @brief Sets the camera this pass draws through.
     *
     * @details
     * - The pass KEEPS the camera alive: it draws through it every frame, and a caller that derives one for the pass
     *   (the pipeline builder derives the shadow pass' light camera) must not be able to leave it drawing through
     *   freed memory. The owning parameter says so at the signature; pass a borrowed one explicitly as
     *   `intrusive_ptr<Camera>(borrowed)`.
     *
     * @param camera Camera to draw through and keep alive (may be null).
     */
    void setCamera(intrusive_ptr<Camera> camera) { desc_.camera = std::move(camera); }

    /**
     * @brief Gets one attachment's clear colour (attachment 0 unless one is named).
     *
     * @details
     * - Attachment 0 defaulting to the #333333 grey the main WINDOW pass was written against is the value stated
     *   in the space it is DISPLAYED in and decoded once (see kDefaultClearColor); nothing in the engine names a
     *   colour for the off-screen targets its builders create, so an off-screen pass that wants one states it.
     * - Attachments 1 and beyond are not that value: an unstated attachment is transparent black, so a consumer can
     *   tell "nothing drawn here" from data (see ClearPolicy).
     *
     * @param attachment Colour attachment index (0 = the first, the default).
     * @return The LINEAR colour stated for that attachment, or transparent black for an unstated one past the first.
     */
    [[nodiscard]] Colorf clearColor(int attachment = 0) const;

    /**
     * @brief Sets one attachment's clear colour (attachment 0 unless one is named).
     *
     * @details
     * - Attachment 0 and the extras are ONE fact with one authority: the pass' clear policy, i.e. two fields of the
     *   declaration. Stating nothing for an attachment keeps transparent black - the value that means "nothing here".
     * - A negative index is ignored; indices above 0 grow the stated list with transparent-black entries so the index
     *   the caller names is the index the backend clears.
     *
     * @param color      Colour that attachment is cleared to, in LINEAR light (a host holding an encoded colour - a
     *                   palette, a PNG, a `QColor` - states it as `Color(...).toLinear()`).
     * @param attachment Colour attachment index (0 = the first, the default).
     */
    void setClearColor(const Colorf& color, int attachment = 0);

    /**
     * @brief Returns whether the depth buffer is cleared.
     *
     * @details
     * - A REQUEST, not the state - hence "should" rather than the is/has prefix: a backend may not be able to honour
     *   it (the window surface always clears depth), which is what its documentation records.
     *
     * @return True when this pass asks for a cleared depth buffer (the default for a Scene pass, never for a Screen
     *         pass).
     */
    [[nodiscard]] bool shouldClearDepth() const noexcept;

    /**
     * @brief Sets whether the depth buffer is cleared before this pass.
     *
     * @details
     * - Off-screen targets honour a false value (their depth survives through a depth-LOAD pass); the window always
     *   gets a cleared depth buffer, so the flag is ignored there.
     *
     * @param clear True to clear the depth buffer.
     */
    void setShouldClearDepth(bool clear) { desc_.clear_depth = clear; }

    /**
     * @brief Returns whether the colour/depth buffer is cleared before this pass.
     *
     * @return True when the pass clears (the default for a Scene pass, never for a Screen pass: a composite draws over
     *         what is already there).
     */
    [[nodiscard]] bool isClearEnabled() const noexcept;

    /**
     * @brief Sets whether the colour/depth buffer is cleared before this pass.
     *
     * @param enabled True to clear the buffers.
     */
    void setClearEnabled(bool enabled) { desc_.clear_enabled = enabled; }

    /**
     * @brief Returns how this pass's content handles depth (the pass' own policy).
     *
     * @return The depth mode (DepthMode::TestAndWrite by default), which is what content that authored no depth state
     *         of its own gets (see resolveRenderState).
     */
    [[nodiscard]] DepthMode depthMode() const noexcept { return desc_.depth_mode; }

    /**
     * @brief Sets how this pass's content handles depth.
     *
     * @details
     * - TestAndWrite draws depth-occluded opaque content; TestOnly tests but does not write (translucent content over
     *   already-written depth); Disabled draws on top with no depth test (HUD / overlay).
     * - A POLICY, not an override: a command that authored its own depth state keeps it.
     *
     * @param mode The depth mode.
     */
    void setDepthMode(DepthMode mode) noexcept { desc_.depth_mode = mode; }

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
    [[nodiscard]] bool isEnabled() const noexcept { return desc_.enabled; }

    /**
     * @brief Sets whether this pass is drawn by the engine.
     *
     * @details
     * - A disabled pass keeps its declaration, costs nothing, and still counts as a declaration for the wiring checks,
     *   which is what lets a registered pass (a HUD overlay) be toggled without removing it.
     *
     * @param enabled True to draw the pass.
     */
    void setEnabled(bool enabled) noexcept { desc_.enabled = enabled; }

    /**
     * @brief Restricts this pass to a sub-rectangle of its target.
     *
     * @details
     * - Used for an axis gizmo in a corner, a picture-in-picture preview, or a second view drawn into part of the
     *   target. The rectangle is in DEVICE pixels with a top-left origin, and every draw the pass issues is placed
     *   inside it.
     * - What the pass CLEARS is a different question: a clear covers the whole target, a draw stays inside this
     *   rectangle, which is what makes a corner preview work.
     * - The rectangle does NOT decide the shape the picture is projected with: that is the camera's (see Camera). The
     *   engine reports a pass whose rectangle and camera shape disagree, because the result is a stretched picture.
     *
     * @param viewport Draw rectangle in device pixels (top-left origin).
     */
    void setViewport(const Viewport& viewport) { desc_.viewport = viewport; }

    /**
     * @brief Restricts this pass to a sub-rectangle of its target (see setViewport(const Viewport&)).
     *
     * @param x      Viewport origin x in device pixels.
     * @param y      Viewport origin y in device pixels (top-left origin).
     * @param width  Viewport width in device pixels.
     * @param height Viewport height in device pixels.
     */
    void setViewport(int x, int y, int width, int height) { setViewport(Viewport{ x, y, width, height }); }

    /**
     * @brief Returns whether a sub-viewport is configured for this pass.
     *
     * @return True when the pass draws into a stated rectangle.
     */
    [[nodiscard]] bool hasViewport() const noexcept { return desc_.viewport.has_value(); }

    /**
     * @brief Gets the configured draw viewport.
     *
     * @return The draw rectangle in device pixels; only meaningful when hasViewport() is true.
     */
    [[nodiscard]] Viewport viewport() const noexcept { return desc_.viewport.value_or(Viewport{}); }

    /// @brief Clears any configured sub-viewport: the pass draws the whole target again.
    void clearViewport() noexcept { desc_.viewport.reset(); }

    /**
     * @brief Maps this pass' draws into a band of the depth buffer instead of the whole of it.
     *
     * @details
     * - The band is what makes a semantic layer sit where the view wants it rather than where the geometry is:
     *   annotations over solids, an overlay over a scene, one layer of a stacked picture in front of another.
     * - The mapping is order-preserving, so near/far inside the band and against default-mapped content are
     *   unchanged - but a remapped depth is NO LONGER A DISTANCE. A pass that states a band is declaring its
     *   target's depth to be a layer value, so nothing may read that depth back as geometry afterwards.
     * - A pass cannot compose a band with an enclosing one: there is no enclosing pass, and two passes writing
     *   one target with different bands leave a pixel's value with no single meaning (see DepthRange).
     *
     * @param range Depth-buffer interval the pass' draws are mapped into; {0, 1} is the whole buffer.
     */
    void setDepthRange(const DepthRange& range) { desc_.depth_range = range; }

    /**
     * @brief Gets the depth band this pass maps its draws into.
     *
     * @return The band, or nullopt when the pass maps into the whole depth buffer.
     */
    [[nodiscard]] std::optional<DepthRange> depthRange() const noexcept { return desc_.depth_range; }

    /// @brief Clears any configured depth band: the pass maps into the whole depth buffer again.
    void clearDepthRange() noexcept { desc_.depth_range.reset(); }

    /** @brief States the device-pixel size of the surface this pass draws into when it has no target.
     *
     * @details
     * - A pass that writes an explicit RenderTarget already knows its size; a WINDOW pass does not. The engine
     *   (or PassChain, which already converts the surface size with the pixel ratio) states it here, and
     *   collection uses it as the viewport for view-dependent features (AutoTransform screen-space scaling).
     * - It does NOT change the backend's viewport: a pass without setViewport() still draws the full target.
     *
     * @param width  Draw width in DEVICE pixels (<= 0 clears the size).
     * @param height Draw height in DEVICE pixels (<= 0 clears the size).
     */
    void setDrawSize(int width, int height) noexcept
    {
        draw_size_ = (width > 0 && height > 0) ? Viewport{ 0, 0, width, height } : Viewport{};
    }

    /** @brief Gets the draw size stated by setDrawSize(). */
    [[nodiscard]] Viewport drawSize() const noexcept { return draw_size_; }

    /**
     * @brief Returns which layers of the content this pass draws (see Node::setLayers).
     *
     * @return The mask (kAllLayers by default, which draws every layer).
     */
    [[nodiscard]] std::uint32_t layerMask() const noexcept { return desc_.layer_mask; }

    /**
     * @brief Sets which layers of the content this pass draws.
     *
     * @details
     * - The content selection of one pass: nodes whose resolved layers do not intersect the mask are left out of the
     *   collection, so one scene can feed several passes that each draw a different part of it - and the mask is part
     *   of the collected list's key, so no second walk is needed (see Scene::collectRenderCommands).
     * - A mask of 0 selects nothing - legal, and reported once as a frame-plan problem, because a content pass that
     *   draws nothing is nearly always a mistake.
     *
     * @param mask Layer mask (kAllLayers draws every layer; 0 draws no content at all).
     */
    void setLayerMask(std::uint32_t mask) noexcept { desc_.layer_mask = mask; }

    /**
     * @brief Sets the name this pass publishes its output under.
     *
     * @details
     * - Later passes can then consume it by name without holding a pointer to the producer. The engine clears the
     *   registry at the start of every frame, so the producer must run earlier in the same frame.
     *
     * @param name Output slot name, or empty to publish nothing.
     */
    void setOutputName(const String& name) { desc_.output_name = name; }

    /**
     * @brief Gets the output slot name this pass publishes under.
     *
     * @return The output name (empty when publishing is disabled).
     */
    [[nodiscard]] String outputName() const { return desc_.output_name; }

    /**
     * @brief Declares the image this pass produces (the fine-grained output declaration).
     *
     * @details
     * - The object-typed counterpart of setOutputName: producer and consumers point at the SAME ImageRef, so a hand-off
     *   cannot be mistyped and the engine can see the wiring (it reports an image two passes declare).
     * - The image has to be BOUND to a target (ImageRef::bind); an unbound identity is reported instead of passing
     *   unnoticed, and a promise about a target this pass does not draw into is reported too.
     *
     * @param image Image this pass produces (null clears it).
     */
    void setOutput(intrusive_ptr<ImageRef> image) { desc_.output = std::move(image); }

    /**
     * @brief Gets the output image this pass produces.
     *
     * @return The declared output image, or null when none was declared.
     */
    [[nodiscard]] raw_ptr<ImageRef> output() const noexcept { return desc_.output.get(); }

    /**
     * @brief Declares a WHOLE target this pass hands to consumers (the coarse counterpart of setOutput).
     *
     * @details
     * - "Any image of this target is mine to hand out": a pass rendering into an MRT target fills all of its
     *   attachments in one render scope, so promising them one by one would mean repeating the attachment count at
     *   every producer.
     * - Shape-agnostic on purpose: the promise covers whatever the target has when the engine validates it.
     *
     * @param target Target whose images this pass produces (null clears the declaration).
     */
    void setOutputTarget(intrusive_ptr<RenderTarget> target) { desc_.output_target = std::move(target); }

    /**
     * @brief Gets the whole target this pass promises to consumers.
     *
     * @return The declared output target, or null when none was declared.
     */
    [[nodiscard]] raw_ptr<RenderTarget> outputTarget() const noexcept { return desc_.output_target.get(); }

    /**
     * @brief Adds an input slot this pass consumes BY NAME.
     *
     * @details
     * - Sugar over the object-typed declarations: the engine resolves each name against its named-output registry
     *   before the pass runs, and names are only consulted when the pass declared no object-typed input at all - one
     *   wire stated twice must not be resolved twice.
     * - Several names may be declared as alternatives: the first that resolves is used, and when none resolves the
     *   pass draws nothing and the engine reports it.
     *
     * @param name Name of a published output to consume (empty is ignored).
     */
    void addInputName(const String& name)
    {
        if (!name.empty()) {
            desc_.input_names.push_back(name);
        }
    }

    /**
     * @brief Gets the declared input names.
     *
     * @return The names, in declaration order.
     */
    [[nodiscard]] const std::vector<String>& inputNames() const noexcept { return desc_.input_names; }

    /**
     * @brief Declares one produced image this pass consumes.
     *
     * @details
     * - A consumer samples one ATTACHMENT of the image's target (ImageRef::attachment), which is what an MRT pass
     *   hands out. The image has to be bound to a target, or nothing can fill it and the engine reports that.
     *
     * @param image Image this pass consumes (null is ignored).
     */
    void addInput(intrusive_ptr<ImageRef> image);

    /**
     * @brief Declares one imported texture this pass consumes.
     *
     * @details
     * - A static image has no producer in the frame, so it takes no part in the one-producer wiring rules; it is
     *   sampled where the pass' program declares a sampler for it.
     *
     * @param texture Texture this pass consumes (null is ignored).
     */
    void addInput(intrusive_ptr<Texture> texture);

    /**
     * @brief Declares one imported texture read with an explicit sampler state.
     *
     * @details
     * - The state belongs to THIS input, not to the texture: the same texture declared by another pass may be read
     *   with different filter/wrap settings.
     *
     * @param texture Texture this pass consumes (null is ignored).
     * @param sampler How this pass reads the texture (see Sampler).
     */
    void addInput(intrusive_ptr<Texture> texture, const Sampler& sampler);

    /**
     * @brief Declares one input from the declaration value (one of the two forms above).
     *
     * @param input Input declaration; an empty declaration is ignored.
     */
    void addInput(ImageInput input);

    /**
     * @brief Gets the images this pass consumes, in declaration order.
     *
     * @return The input declarations (produced images or imported textures), as added.
     */
    [[nodiscard]] const std::vector<ImageInput>& inputs() const noexcept { return desc_.inputs; }

    /**
     * @brief Declares a WHOLE target this pass consumes (the coarse counterpart of addInput).
     *
     * @details
     * - "I read this target's images": a full-screen program receives every colour attachment of its source (plus its
     *   depth while that one is sampleable) and its shader picks by binding, so the unit it consumes is the target.
     *
     * @param target Target whose images this pass consumes (null is ignored).
     */
    void addInputTarget(intrusive_ptr<RenderTarget> target)
    {
        if (target != nullptr) {
            desc_.input_targets.push_back(std::move(target));
        }
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
     * @brief Clears every declared input: images, targets and names.
     *
     * @details
     * - One call, because "this pass now consumes nothing" is one statement. Clearing only part of the list would
     *   leave the resolve order (images, then targets, then names) reading a stale half.
     */
    void clearInputs();

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
    virtual void resolveInputTextures(std::span<const ImageSource> inputs) { (void)inputs; }

    /**
     * @brief Gets the pass-level program override.
     *
     * @return The program every command is rewritten to, or null for per-geometry programs.
     */
    [[nodiscard]] raw_ptr<ShaderProgram> programOverride() const noexcept { return desc_.program_override.get(); }

    /**
     * @brief Forces every command this pass renders to use one program.
     *
     * @details
     * - A pass-level OVERRIDE, applied to every command whatever the content declared, so the same scene can be
     *   re-rendered with another shader (an unlit or normals view).
     * - IT TRAVELS AS PASS STATE, not as a rewrite of the commands: the pass states it once
     *   (`PassDraw::program_override`) and the backend resolves it per command exactly like the pass' state
     *   (`PassDraw::state`). The collected command list stays the shared, immutable list - rewriting it cost a whole
     *   extra command list per frame per overriding pass.
     * - The pass keeps a reference.
     *
     * @param program Program applied to all content, or null for per-geometry programs (the default).
     */
    void setProgramOverride(intrusive_ptr<ShaderProgram> program) { desc_.program_override = std::move(program); }

    /**
     * @brief Gets the pass-level polygon-mode override.
     *
     * @return The mode, or nullopt when each command keeps its own.
     */
    [[nodiscard]] std::optional<PolygonMode> polygonModeOverride() const noexcept { return desc_.polygon_mode; }

    /**
     * @brief Draws everything this pass renders as filled, outlined or point primitives.
     *
     * @details
     * - The RASTERISATION counterpart of setProgramOverride: a pass-level override that beats the content, named
     *   "Override" for exactly that reason.
     * - Unset is NOT the same as Fill: unset leaves every command's own effective mode alone.
     * - It is state, not wiring: the backend reads it per frame from stateOverrides(), where the pass states its
     *   state once for every command it draws.
     *
     * @param mode Rasterisation mode applied to all content of this pass.
     */
    void setPolygonModeOverride(PolygonMode mode) noexcept { desc_.polygon_mode = mode; }

    /// @brief Removes the polygon-mode override, leaving each command's own state in effect.
    void clearPolygonModeOverride() noexcept { desc_.polygon_mode.reset(); }

    /**
     * @brief Describes what this pass issues this frame: its target, its state and its drawing calls.
     *
     * @details
     * - THE EXTENSION POINT: what a pass does is data, and a subclass that draws something else fills in a different
     *   description (ScreenPass draws a full-screen program instead of collected geometry).
     * - ONE CALL, because nothing here needs a backend: the picture's shape is the camera's and the commands are
     *   collected with it, so a whole frame's worth of descriptions can be assembled before anything runs.
     * - A pass that issues NOTHING this frame answers false (a scene pass with no scene, a screen pass with no
     *   resolved source): nothing is announced at all, which keeps "this pass is not running" distinguishable from
     *   "this pass has no picture" (an empty PassDraw::draws).
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
    explicit RenderPass(PassKind kind);

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
    /// Protected because a subclass that states something of its own (ScreenPass names the program it draws through)
    /// writes it here instead of keeping a second copy of a fact the base already owns.
    PassDesc desc_;

  private:
    /**
     * @brief The clear this pass announces, or nothing when it clears nothing (the ONE rule).
     *
     * @return The policy, or nullopt when this pass does not clear.
     */
    [[nodiscard]] std::optional<ClearPolicy> clearPolicy() const;

    /// Device-pixel size of the surface for a pass that has no target (see setDrawSize).
    Viewport draw_size_{};
};

using RenderPassPtr = intrusive_ptr<RenderPass>;

VN_GRAPHICS_NS_END
