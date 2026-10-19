#pragma once
#include "graphics_global.hpp"

#include <optional>
#include <span>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/Color.hpp>

#include "DepthMode.hpp"
#include "DepthView.hpp"
#include "FrameDescription.hpp"
#include "ImageRef.hpp"
#include "ImageSource.hpp"
#include "Node.hpp"
#include "PolygonMode.hpp"
#include "Sampler.hpp"
#include "PresentTarget.hpp"
#include "StateNode.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class RenderTarget;
class RenderBackend;
class Scene;
class ShaderProgram;

using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief A render pass describing one complete rendering stage.
 *
 * @details
 * - Binds a camera, render target, and clear state.
 * - Executing a pass collects render commands from a scene and dispatches them to a backend.
 * - Multiple passes can be chained for split-screen, post-processing, etc.
 *
 * @note
 * - Naming: this is an engine-level rendering STAGE (the equivalent of an OSG render stage / a VulkanSceneGraph view
 *   + target + clear policy), NOT a Vulkan render pass. Several passes targeting the same framebuffer are recorded
 *   into one backend render pass; the backend owns that distinction.
 */
class VN_GRAPHICS_API RenderPass : public Object, public RefCounted<RenderPass> {
    VN_OBJECT_META_DECL;

  public:
    RenderPass();
    ~RenderPass();

  public:
    /** @brief Gets the pass name. */
    String name() const;

    /** @brief Sets the pass name. */
    void setName(const String& name);

    /** @brief Gets the associated render target. */
    raw_ptr<RenderTarget> renderTarget() const;

    /** @brief Gets the depth image this pass draws against.
     *
     * @return The view stated by setDepthView, or an invalid view when the pass uses its target's own depth.
     */
    DepthView depthView() const;

    /** @brief States the depth image this pass draws against, when it is another pass' image (see DepthView).
     *
     * @details
     * - THE PASS DECLARES THE EDGE; the target stores the mechanism. The view is kept here and applied to the
     *   pass' current target (RenderTarget::shareDepth), and re-applied when a target is set later
     *   (setRenderTarget) - so a pass moved to another target keeps drawing against the same depth.
     * - Constraints are the target's (both off-screen and equally sized, the lender renders earlier, its depth is
     *   not promoted): they are the backend's to enforce, and it reports a plan that breaks them rather than
     *   guessing.
     * - An invalid view clears the declaration: the pass goes back to its target's own depth.
     *
     * @param view The depth image this pass draws against.
     */
    void setDepthView(DepthView view);

    /** @brief Sets the render target this pass renders into.
     *
     * @details
     * - The pass keeps a reference to the target (its output resource), so the target stays alive as long as the pass
     *   uses it.
     *
     * @param target Render target to render into, or null for the default framebuffer.
     */
    void setRenderTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Gets the camera used by this pass. */
    raw_ptr<Camera> camera() const;

    /** @brief Sets the camera used by this pass.
     *
     * @details
     * - The pass KEEPS the camera alive, like it keeps its render target: every frame it draws through this camera,
     *   so a caller that builds one for it — the pipeline builder derives the shadow pass' light camera on the fly —
     *   must not be able to leave the pass rendering through freed memory by dropping its own handle.
     * - That is not hypothetical: while this stored a borrowed pointer, the shadow pass drew through the freed light
     *   camera and its map came back empty (found by vsg_backend_selftest's deferred shadow phase). A host camera
     *   outliving its pass is unaffected.
     * - The parameter says so: it is an owning handle, not a `raw_ptr` , so a caller reading the signature can see
     *   the pass takes the camera over. Passing a borrowed camera is spelled `intrusive_ptr<Camera>(borrowed)` — the
     *   conversion is explicit, which is what makes it a decision rather than an accident (this parameter used to be
     *   a `raw_ptr` that the body retained, the one place in the SDK where the signature hid the retention).
     *
     * @param camera Camera to draw through and keep alive (may be null).
     */
    void setCamera(intrusive_ptr<Camera> camera);

    /** @brief Gets one attachment's clear colour (attachment 0 unless one is named).
     *
     * @details
     * - Attachment 0's default is an opaque dark grey (51, 51, 51, 255): the value the main WINDOW pass was
     *   written against, because that is the pass filling the surface a host looks at.
     * - Nothing in the engine names a colour for the off-screen targets its builders create, so an off-screen
     *   pass that wants one says so itself - otherwise attachment 0 of an off-screen target clears to that
     *   window grey.
     * - Attachments 1 and beyond are not that value at all: an attachment nobody states a colour for is
     *   cleared to transparent black by contract, so a consumer can tell "nothing drawn here" from data (see
     *   ClearPolicy).
     *
     * @param attachment Colour attachment index (0 = the first, the default).
     * @return The colour stated for that attachment, or transparent black for an unstated one.
     */
    Color clearColor(int attachment = 0) const;

    /** @brief Sets one attachment's clear colour (attachment 0 unless one is named).
     *
     * @details
     * - Attachment 0 and the extras are ONE fact with one authority: the pass' clear policy. The index is
     *   what a G-buffer needs (its normal image cleared to (0,0,1,0), say), and stating nothing for an
     *   attachment keeps transparent black - the value that means "nothing here" (see ClearPolicy).
     * - A negative index is ignored; indices above 0 grow the stated list with transparent-black entries so
     *   the index the caller names is the index the backend clears.
     *
     * @param color      Colour that attachment is cleared to (0..255 per channel).
     * @param attachment Colour attachment index (0 = the first, the default).
     */
    void setClearColor(const Color& color, int attachment = 0);

    /** @brief Returns whether the depth buffer is cleared.
     *
     * A REQUEST, not the state — hence "should" rather than the is/has prefix the other boolean
     * accessors use: a backend may not be able to honour it (the window surface always clears depth),
     * which is exactly what its documentation records. The state is only ever this pass' intent.
     *
     * @return True when this pass asks for a cleared depth buffer.
     */
    bool shouldClearDepth() const;

    /** @brief Sets whether the depth buffer is cleared before this pass.
     *
     * @details
     * - Controls the depth flag of the ClearPolicy announced to the backend when this pass clears (see
     *   setClearEnabled / ClearPolicy).
     * - Whether a false value actually preserves the previous depth depends on the target: it is honoured for
     *   off-screen targets (their depth survives via a depth-LOAD pass), but a pass rendering into the window / main
     *   surface always gets a cleared depth buffer (the surface render pass clears depth), so the flag is ignored
     *   there.
     *
     * @param clear True to clear the depth buffer (the default).
     */
    void setShouldClearDepth(bool clear);

    /** @brief Returns whether the colour/depth buffer is cleared before this pass.
     *
     * @note The main pass clears by default; top / HUD passes usually disable it so they draw over the previous
     *       frame's content.
     */
    bool isClearEnabled() const;

    /** @brief Sets whether the colour/depth buffer is cleared before this pass.
     *
     * @param enabled True to clear the buffers (the default).
     */
    void setClearEnabled(bool enabled);

    /** @brief Returns how this pass's content handles depth (the pass' own value).
     *
     * @return The depth mode (DepthMode::TestAndWrite by default). It is the pass' POLICY for content that authored
     *         none - see stateOverrides() and StateNode.hpp's resolveRenderState for the decision.
     */
    DepthMode depthMode() const;

    /** @brief Sets how this pass's content handles depth.
     *
     * @details
     * - TestAndWrite (the default) draws the content as depth-occluded opaque scene content; TestOnly tests against
     *   the target's current depth but does not write it (translucent content composited over already-written depth);
     *   Disabled draws on top of whatever is already in the target with no depth testing (HUD / overlay style).
     * - Independent of clearing and of lighting. A translucent pass drawn into a target whose depth an earlier pass
     *   wrote uses TestOnly and disables only the clear.
     * - It is a POLICY, not an override: a command that authored a depth state of its own keeps it (depth is
     *   something scene content routinely declares), so this decides for the content that did not.
     *
     * @param mode The depth mode.
     */
    void setDepthMode(DepthMode mode);

    /** @brief What this pass says about the state of the content it draws.
     *
     * @details
     * - The pass side of the state priority table: its depth policy (depthMode()) and its rasterisation override
     *   (polygonModeOverride()), in one value.
     * - Announced to the backend before the pass draws, so a backend can resolve each command's FINAL state in one
     *   place - `resolveRenderState(command.authored_state, pass.stateOverrides())` , see StateNode.hpp.
     *
     * @return The pass' state for its content.
     */
    PassRenderState stateOverrides() const;

    /** @brief Returns whether this pass is drawn by the engine this frame. */
    bool isEnabled() const;

    /** @brief Sets whether this pass is drawn by the engine.
     *
     * @details
     * - The engine skips disabled passes, which lets a registered pass (e.g. a HUD overlay) be toggled on and off
     *   without removing it. The default is true.
     *
     * @param enabled True to draw the pass (the default).
     */
    void setEnabled(bool enabled);

    /** @brief Restricts this pass to a sub-rectangle of the render target.
     *
     * @details
     * - Used for sub-viewports such as an axis gizmo in a screen corner, a picture-in-picture preview, or a second
     *   view drawn into part of the target.
     * - One rule decides what a pass draws into, and EVERY pass follows it: this rectangle, or the whole target when
     *   none is set, clamped into the target - in DEVICE pixels with a top-left origin (what a host's
     *   devicePixelRatio() is for).
     * - What the pass CLEARS is a different question and is not affected: a clear covers the whole target while a
     *   draw stays inside this rectangle, which is what makes a preview in a corner work. A content pass used to be
     *   the exception - one that cleared its target filled it whatever it announced, so this rectangle was silently
     *   dropped - and the two kinds of pass honour it alike now.
     * - The rectangle is a fact of the PASS (PassDescription::viewport), not of one drawing call: every draw the pass
     *   issues is placed inside it. The base pass describes it once and draws once, so it covers the whole pass; a
     *   subclass that draws more than once describes each draw itself.
     * - The rectangle does NOT decide the shape the picture is projected with: a camera carries the shape of its own
     *   picture (see Camera), and with it the frustum the content was culled against. For a pass drawing through a
     *   camera that does not have this rectangle's shape the engine reports the mismatch - the drawing is a STRETCHED
     *   picture, not a differently framed one - so a sub-viewport is set together with its camera's shape
     *   (`camera->setAspect(w / h)`).
     *
     * @param viewport Draw rectangle in device pixels (top-left origin).
     */
    void setViewport(const Viewport& viewport);

    /** @brief Restricts this pass to a sub-rectangle of the render target.
     *
     * @details
     * - Convenience for setViewport(const Viewport&).
     *
     * @param x      Viewport origin x in device pixels.
     * @param y      Viewport origin y in device pixels (top-left origin).
     * @param width  Viewport width in device pixels.
     * @param height Viewport height in device pixels.
     */
    void setViewport(int x, int y, int width, int height);

    /** @brief Returns whether a sub-viewport is configured for this pass. */
    bool hasViewport() const;

    /** @brief Gets the configured draw viewport.
     *
     * Only meaningful when hasViewport() is true; otherwise the pass draws the whole target.
     *
     * @return The draw rectangle in device pixels.
     */
    Viewport viewport() const;

    /** @brief Clears any configured sub-viewport (pass renders to the full surface). */
    void clearViewport();

    /** @brief Returns which layers of the content this pass draws (see Node::setLayers).
     *
     * @return The mask (kAllLayers by default, which draws every layer).
     */
    std::uint32_t layerMask() const;

    /** @brief Sets which layers of the content this pass draws.
     *
     * @details
     * - The content selection of one pass: the collection leaves out every node whose resolved layers do not
     *   intersect this mask, so one scene can feed several passes that each draw a different part of it (a shadow
     *   pass drawing casters, a main pass drawing everything) - without splitting the scene in two and without a
     *   second walk per pass that only the mask differs in (the mask is part of the collected list's key, see
     *   Scene::collectRenderCommandsShared).
     * - It is a declaration about CONTENT, not about the pass: an overlay pass drawing its own HUD scene states its
     *   own mask if it wants one, and a pass with no content is unaffected.
     *
     * @param mask Layer mask (kAllLayers draws every layer; 0 draws no content at all).
     */
    void setLayerMask(std::uint32_t mask);

    /** @brief Sets the name this pass publishes its output under.
     *
     * @details
     * - When a pass renders into a non-null RenderTarget and declares an output name, the engine registers that
     *   target in its named-output registry after the pass runs, so later passes can sample it by name without
     *   holding a pointer to the producer. Leave empty to publish nothing.
     *
     * @param name Output slot name, or empty to disable publishing.
     */
    void setOutputName(const String& name);

    /** @brief Gets the output slot name this pass publishes under.
     *
     * @return The output name (empty when publishing is disabled).
     */
    String outputName() const;

    /** @brief Declares the image this pass produces (design §14).
     *
     * @details
     * - The object-typed counterpart of setOutputName: the producer and every consumer point at the SAME ImageRef, so
     *   a hand-off cannot be mistyped, and the engine can SEE the wiring — it reports an image that two passes
     *   declare as their output instead of letting the second one silently win at run time.
     * - A promise is about the content a consumer gets, so the engine checks that the target named here is the one
     *   this pass RENDERS into (see setRenderTarget): promising what the pass never writes is a claim about nothing,
     *   and it is reported.
     * - The image has to be BOUND to a target (ImageRef::bind): an unbound identity has no address, so no consumer
     *   can resolve it and the check above has nothing to compare — declaring one is reported (once per image, per
     *   episode) instead of passing unnoticed.
     * - A pass may declare an output image, an output target, an output name, or several of them. The object-typed
     *   declarations drive the runtime; a name is resolved by the engine's named-output registry, which a pass
     *   without object declarations falls back on (design §14.3). See setOutputName for what each declaration is for.
     *
     * @param image Image this pass produces (null clears it).
     */
    void setOutput(intrusive_ptr<ImageRef> image);

    /** @brief Gets the output image this pass produces.
     *
     * @return The declared output image, or null when none was declared.
     */
    raw_ptr<ImageRef> output() const;

    /** @brief Declares a WHOLE target this pass hands to consumers (the coarse counterpart of setOutput(ImageRef),
     * design §14).
     *
     * @details
     * - "Any image of this target is mine to hand out": a pass rendering into an MRT target fills ALL of its
     *   attachments in one render scope, so promising them one by one would mean repeating today's attachment count
     *   at every producer — and getting it wrong when the target grows an attachment.
     * - Shape-agnostic on purpose: the promise covers whatever the target has when the engine validates it. One of
     *   the three output declarations; see setOutputName for how they relate.
     * - The target is NOT where the content comes from (that is setRenderTarget); it is what this pass promises about
     *   it. Declaring both one image and a whole target is redundant, not wrong.
     *
     * @param target Target whose images this pass produces (null clears the declaration).
     */
    void setOutputTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Gets the whole target this pass promises to consumers.
     *
     * @return The declared output target, or null when none was declared.
     */
    raw_ptr<RenderTarget> outputTarget() const;

    /** @brief Adds an input texture slot this pass consumes by name.
     *
     * @details
     * - A consumer declares "I want the texture published as X" without holding a pointer to the producer. The engine
     *   resolves each declared name against its named-output registry before execute() and hands the matching targets
     *   to resolveInputTextures().
     * - The producer must run EARLIER in the same frame (a lower pass order, or an earlier registration at the same
     *   order): the registry is cleared at the start of every frame.
     * - Several names may be declared as alternatives — the first that resolves is used (a chain that falls back).
     *   When NONE of them resolves, the pass draws nothing; the engine reports that on the host's diagnostic channel,
     *   because a wiring mistake used to be silent.
     *
     * @param name Name of a published output to consume.
     */
    void addInputName(const String& name);

    /** @brief Gets the declared input texture slot names.
     *
     * @return The input names, in the order they were added.
     */
    const std::vector<String>& inputNames() const;

    /** @brief Clears all declared input texture slots. */
    void clearInputNames();

    /** @brief Declares one produced image this pass consumes (design §14).
     *
     * @details
     * - The object-typed counterpart of addInputName. A consumer samples one ATTACHMENT of the image's target
     *   (ImageRef::attachment), which is what an MRT pass hands out: the identity is "this target, attachment N", not
     *   just "this target".
     * - The image has to be BOUND to a target (ImageRef::bind), and to the same object the producer declared: an
     *   unbound identity has no address, so nothing can fill it — declaring one is reported (once per image, per
     *   episode) instead of leaving the pass drawing nothing.
     *
     * @param image Image this pass consumes (null is ignored).
     */
    void addInput(intrusive_ptr<ImageRef> image);

    /** @brief Declares one imported texture this pass consumes.
     *
     * @details
     * - The counterpart of addInput(ImageRef) for a STATIC/imported image: a texture has no producer in the frame,
     *   so it does not participate in the one-producer wiring rules; it is sampled where the pass' program declares
     *   a sampler for it (see the pass input set's binding order).
     * - A Scene pass (content) binds imported textures in declaration order with the other inputs. A full-screen
     *   pass binds them at the sampler bindings its program declares beyond the source's attachments, in input
     *   declaration order; a program/pass binding mismatch is reported by the backend.
     * - It is a full input in the pass' input list, in declaration order; a pass may mix produced images, whole
     *   targets and imported textures.
     *
     * @param texture Texture this pass consumes (null is ignored).
     */
    void addInput(intrusive_ptr<Texture> texture);

    /** @brief Declares one imported texture read with an explicit sampler state.
     *
     * @details
     * - The state belongs to THIS input, not to the texture: the same texture can be declared by another pass (or
     *   another declaration here) with different filter/wrap settings, and the backend binds one sampler per input.
     * - The declaration without a state keeps the backend's input sampler, so an existing pass is unchanged.
     *
     * @param texture Texture this pass consumes (null is ignored).
     * @param sampler How this pass reads the texture (see Sampler).
     */
    void addInput(intrusive_ptr<Texture> texture, const Sampler& sampler);

    /** @brief Declares one input from the declaration value (one of the two additions above).
     *
     * @param input Input declaration; an empty declaration is ignored.
     */
    void addInput(ImageInput input);

    /** @brief Gets the images this pass consumes, in declaration order.
     *
     * @return The input declarations (produced images or imported textures), as added.
     */
    const std::vector<ImageInput>& inputs() const;

    /** @brief Declares a WHOLE target this pass consumes (the coarse counterpart of addInput).
     *
     * @details
     * - "I read this target's images": a fullscreen program receives every colour attachment of its source (plus its
     *   depth while that one is sampleable) and its shader picks by binding — so the unit it really consumes is the
     *   target, not one attachment.
     * - Shape-agnostic like the promise side: a target that gains an attachment later needs no change here.
     *
     * @param target Target whose images this pass consumes (null is ignored).
     */
    void addInputTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Gets the whole targets this pass consumes.
     *
     * @return The input targets, in the order they were added.
     */
    const std::vector<intrusive_ptr<RenderTarget>>& inputTargets() const;

    /** @brief Clears all declared input images AND input targets. */
    void clearInputs();

    /** @brief Receives the engine-resolved inputs.
     *
     * @details
     * - Called by the engine just before execute() once per frame: one entry per declaration, in declaration order —
     *   the images of inputs() first, then the targets of inputTargets(); a pass that declared neither falls back to
     *   its inputNames(), resolved against the engine's named-output registry (the sugar layer).
     * - An entry stays null when nothing produced it this frame (the engine reports that once per episode), so the
     *   base pass ignores the inputs and a subclass such as ScreenPass treats a leading null as "nothing to sample".
     * - The sources are BORROWED: whatever produced a target and whatever owns a texture keep them alive while the
     *   frame runs. An imported texture is an input like any other; a pass that needs a produced source (a screen
     *   pass) picks it by kind.
     *
     * @param inputs Resolved input sources, in declaration order (names last).
     */
    virtual void resolveInputTextures(std::span<const ImageSource> inputs)
    {
        (void)inputs;
    }

    /** @brief Gets the pass-level program override (null when unset). */
    raw_ptr<ShaderProgram> programOverride() const;

    /** @brief Forces every command this pass renders to use one program.
     *
     * @details
     * - By default each geometry renders with its own effective program (leaf / StateNode resolution). Setting an
     *   override replaces the program of every collected command, so the same content scene can be re-rendered with a
     *   different shader (e.g. a wireframe or alternate-shading pass over the same scene).
     * - The pass is its own retained slot in the backend (keyed by the pass, see FrameDescription::Pass), so an
     *   override and the per-geometry pass never collapse into one slot; use setViewport / setClearEnabled to control
     *   where each one lands.
     * - The pass keeps a reference.
     *
     * @param program Program applied to all content, or null for per-geometry programs (the default).
     */
    void setProgramOverride(intrusive_ptr<ShaderProgram> program);

    /** @brief Gets the pass-level polygon-mode override (`nullopt` when unset). */
    std::optional<PolygonMode> polygonModeOverride() const;

    /** @brief Draws everything this pass renders as filled, outlined or point primitives.
     *
     * @details
     * - The RASTERISATION counterpart of setProgramOverride, and named the same way for the same reason: both are
     *   PASS-LEVEL OVERRIDES, applied to every command this pass renders whatever the content itself declared. The
     *   names carry the distinction on purpose - a pass-level setter that BEATS the content says "Override"
     *   (setProgramOverride, setPolygonModeOverride), while the one the content can beat is a plain default
     *   (setDepthMode, which yields to a command that authored its own depth - see DepthMode).
     * - WHAT IT IS FOR: a viewer's "wireframe" switch, or a debug pass that wants the edges - "show me how this is
     *   built" over content that asked for surfaces. Overriding the authored state is the point: a switch that left
     *   authored wireframe subtrees alone could not show anything as surfaces, and a host that wants the authored
     *   state to win simply does not set this (the StateNode's own mode is what applies then).
     * - Unset is NOT the same as Fill: unset leaves every command's own effective polygon mode alone, so a subtree
     *   that asked for Line stays outlined. That is what makes "back to shaded" mean "stop overriding" rather than
     *   "force everything to fill".
     * - It is rasterisation state, not wiring: a change does NOT bump the wiring revision (nothing about what this
     *   pass declares has changed), and the backend picks it up per frame from the pass' state (stateOverrides(),
     *   announced through PassDescription::state) - the same place its depth policy goes, so a pass states its state
     *   ONCE and a backend resolves a command without asking anybody else (see StateNode.hpp).
     *
     * @param mode Rasterisation mode applied to all content of this pass.
     */
    void setPolygonModeOverride(PolygonMode mode);

    /** @brief Removes the polygon-mode override, leaving each command's own state in effect. */
    void clearPolygonModeOverride();
    /** @brief Describes what this pass issues this frame: its target, its state and its drawing calls.
     *
     * @details
     * - THE EXTENSION POINT: what a pass does is data now, and a subclass that draws something else fills in a
     *   different description (ScreenPass draws a full-screen program instead of collected geometry). The description
     *   has no call order to get wrong - which state belongs to the pass and which to one drawing call is the shape
     *   of the value (see PassDescription / DrawDescription) - and it is what a backend is handed: the engine puts it
     *   in the frame it runs (see RenderEngine::frameDescription), and execute() hands over the same shape for a pass
     *   a host drives itself.
     * - ONE CALL, because nothing here needs the backend: the picture's shape is the CAMERA's (see Camera), the
     *   commands are collected and culled with it, and a description can therefore be built without announcing
     *   anything or asking anybody - which is what makes a whole frame's worth of these assemblable up front.
     * - A pass that issues NOTHING this frame answers false (a scene pass with no scene, a screen pass with no
     *   resolved source): nothing is announced at all, not even a target, which is what keeps "this pass is not
     *   running" distinguishable from "this pass has no picture". An empty PassDescription::draws is that second
     *   answer - the pass announces its target and state and draws nothing (no camera, no program).
     *
     * @param scene Content scene to draw (a scene pass with none issues nothing at all; a content-agnostic pass
     *              ignores it).
     * @param out   Receives the description; untouched when this returns false.
     * @return true when this pass issues anything at all this frame.
     */
    virtual bool describe(raw_ptr<Scene> scene, PassDescription& out);

    /** @brief Executes this pass on its own: describes it, then hands the backend the one-pass frame that says so.
     *
     * @details
     * - SUGAR, NOT AN EXTENSION POINT. This method is deliberately NOT virtual, because there is only one way a pass
     *   becomes a picture: `describe()` fills a value, and a frame is that value (see
     *   RenderFrameExecutor::executeFrame). A subclass that draws something else overrides `describe()`; a subclass
     *   that must change content BEFORE the picture uses the declaration hook (`PassDesc::before_draw`), which the
     *   compile runs in run order. While this was virtual, an override could draw something the engine's compile
     *   never saw - two truths for one pass.
     * - WHAT IT IS FOR: a caller that drives ONE pass itself - a host drawing into its own device, a test. It builds
     *   `singlePassFrame(this, 0, {}, description)` and calls the executor's single entry (`executeFrame`), which is
     *   the same shape the engine hands over for a whole frame, so the two spellings cannot drift. A pass that issues
     *   nothing (describe answers false) issues nothing here either: no frame is handed over at all.
     * - The TARGET is the third thing such a caller says, because a frame's "default framebuffer" has to resolve to
     *   one (see RenderFrameExecutor::executeFrame): the target the frame is presented through, or nullptr for a pass
     *   whose work is off-screen. The EXECUTOR presents through it at the frame's tail, so the caller does not.
     *
     * @param scene      Content scene to draw (a content-agnostic pass ignores it).
     * @param backend    Device to render with (null does nothing).
     * @param present_to Target the pass' default framebuffer resolves to, or nullptr for off-screen work.
     */
    void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend,
                 raw_ptr<PresentTarget> present_to = nullptr);

  protected:
    /** @brief Fills this pass' own facts (target, viewport, clear, state) into @p out.
     *
     * @details
     * - The ONE mapping from the pass' fields to a description, so a subclass states what is DIFFERENT about it
     *   (ScreenPass draws a full-screen program instead of collected geometry) without restating what is the same.
     * - The subclass' own "issues nothing at all" gate lives in its describe, not here: what counts as nothing is a
     *   property of the kind of pass.
     *
     * @param out Receives the facts.
     */
    void fillPassFacts(PassDescription& out);

  private:
    /** @brief The clear this pass announces, or nothing when it clears nothing (the ONE rule).
     *
     * @return The policy, or nullopt when this pass does not clear.
     */
    [[nodiscard]] std::optional<ClearPolicy> clearPolicy() const;

    String name_;
    String output_name_;
    std::vector<String> input_names_;
    intrusive_ptr<ImageRef> output_image_;                 // fine output declaration (design §14)
    intrusive_ptr<RenderTarget> output_target_;            // coarse output declaration (design §14)
    std::vector<ImageInput> input_images_;    // fine input declarations: produced images or imported textures
    std::vector<intrusive_ptr<RenderTarget>> input_targets_;   // coarse input declarations (design §14)
    ShaderProgramPtr program_override_;   // null = per-geometry programs
    std::optional<PolygonMode> polygon_mode_override_;  // unset = each command's own effective mode
    intrusive_ptr<RenderTarget> render_target_;
    intrusive_ptr<Camera> camera_;
    DepthView depth_view_;   // the depth image this pass draws against (invalid = its target's own)
    Color clear_color_{ 51, 51, 51, 255 };
    // Colours for attachments 1..N-1, in attachment order (an absent entry is transparent black; see
    // setClearColor(int) and ClearPolicy).
    std::vector<Color> extra_clear_colors_;
    bool clear_depth_ = true;
    bool clear_enabled_ = true;
    DepthMode depth_mode_ = DepthMode::TestAndWrite;
    bool enabled_ = true;
    bool has_viewport_ = false;
    Viewport viewport_;
    std::uint32_t layer_mask_ = kAllLayers;
};

using RenderPassPtr = intrusive_ptr<RenderPass>;

VN_GRAPHICS_NS_END
