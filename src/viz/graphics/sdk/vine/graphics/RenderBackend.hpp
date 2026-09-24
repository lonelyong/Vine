#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vine/Color.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "RenderDiagnostic.hpp"
#include "RenderPass.hpp"
#include "ShaderProgram.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class RenderTarget;
class RenderPass;
class ShaderProgram;
struct RenderCommand;

/**
 * @brief Why a readback did or did not happen (see readColorBuffer / readDepthBuffer).
 *
 * A bool alone answered four different questions with one value, so a caller could not tell
 * "this backend cannot read back at all" (expected — gate the feature on it) from "there is
 * nothing to read YET" (retry after the target is rendered) from "the request was wrong" (a
 * caller bug) from "the copy was attempted and failed" (report it). The methods keep their bool
 * return, and a caller that needs the distinction passes a pointer to one of these: the same
 * information an enum return would give, without re-spelling every `if (!read(...))` in a
 * caller (this codebase's other reason-reporting entries use the same out-parameter shape, see
 * VsgTextureCache::makeImage).
 *
 * The diagnostics channel still carries the human-readable sentence; this is the machine answer.
 */
enum class ReadbackResult
{
    Ok,          ///< The pixels / depths were read.
    Unsupported, ///< This backend cannot read this back: no readback support, or a format / usage it does not decode.
    NotReady,    ///< There is nothing to read yet: unknown target, no built attachments, no usable size.
    Invalid,     ///< The request itself was wrong (no target, attachment index out of range).
    Failed,      ///< The copy was attempted and failed (no usable device, submission error).
};

/** @brief How a pass has its target cleared before it draws.
 *
 * A SCOPE ATTRIBUTE, announced between beginPass() and the pass' draw (setClearPolicy), so it reads
 * as what it is: the pass describes the clear its content expects, and every draw call of that pass
 * gets it. Nothing is cleared at the moment of the call — a backend that clears through its render
 * pass' load-op (which is what the load-op IS for) applies it when the pass records, and the name
 * no longer promises an action that may never happen.
 *
 * ONE COLOUR, because one colour is what a colour ATTACHMENT gets: @ref color is attachment 0's,
 * and every further attachment of an MRT target is left TRANSPARENT BLACK (0,0,0,0) until a
 * fragment writes it. That is the contract, not an omission: it lets a consumer tell "nothing was
 * drawn here" from the stored data itself — the deferred-lighting program reads a stored view
 * position of ~0 as background — so a backend may not clear every attachment uniformly without
 * auditing those consumers, and one that cannot honour this must report it on its diagnostics
 * channel instead of clearing differently.
 *
 * DEPTH IS A FLAG, NOT A VALUE: every backend clears depth to its own far plane (this SDK's
 * backends render reverse-Z, whose far plane is 0 — see Camera), so a value here would be a second
 * place to get the clip convention wrong. @ref depth is honoured for off-screen targets, whose
 * depth content survives frames through a depth-LOAD pass; the WINDOW is the exception — the
 * surface render pass belongs to the windowing system and clears depth whatever this says (a pass
 * that needs a previous frame's depth renders into an off-screen target and composites it).
 */
struct ClearPolicy
{
    /// Colour attachment 0 is cleared to (the only colour a single-attachment target reads).
    Color color{};
    /// Whether the depth buffer is cleared too (honoured off-screen; the window always clears depth).
    bool depth = true;
};

/**
 * @brief Abstract render backend interface.
 *
 * Defines the contract that concrete graphics backends (vsg/Vulkan, OpenGL,
 * null/test) must implement, and what the engine guarantees in return.
 * Supports both high-level pass execution and low-level command rendering.
 *
 * CALL ORDER. The engine drives a frame one pass at a time, in this order:
 *
 *   1. beginFrame();
 *   2. per enabled pass, in ascending pass order:
 *        beginPass(pass)          announces the pass (its identity) as active;
 *        setPassOrder(order)      where the pass stacks in the target;
 *        setRenderTarget / setViewport / setLights / setDepthMode / clear
 *                                 optional per-pass state (see beginPass);
 *        render() | drawScreenProgram();
 *        endPass();
 *   3. endFrame();
 *   4. swapBuffers();            once per frame — the only call that presents.
 *
 * Everything between the frame pair is optional: a pass may draw several
 * times, set no state, or a backend may leave a call unimplemented (that is
 * what the no-op defaults are for). Before the first beginFrame() the engine
 * runs a warm-up: every enabled, non-clearing pass executes once, so a backend
 * sees its whole pass set before it ever presents a frame and can place each
 * pass' retained state in its final position. beginFrame() may acquire the
 * next presentable image, so a driven frame must end in swapBuffers(); a
 * backend must not present from endFrame().
 *
 * BORROWED ARGUMENTS. Every pointer or reference argument (camera, commands,
 * lights, target, program) is borrowed for the duration of the call and never
 * owned; the pass announced by beginPass() is borrowed for its scope. The host
 * may destroy any of them as soon as the call returns — or, for a pass, as soon
 * as its scope closes or it is removed from the engine, which releasePass() /
 * releaseRenderTarget() announce. A backend that needs the data later must
 * copy or upload it and must never keep such a pointer.
 *
 * WHAT MAY BE RETAINED. Retained GPU state (a content view, a compiled
 * pipeline, a sampling slot, a texture cache) is expected, but it must be
 * keyed by the lifetime of what it serves (see beginPass), released by the
 * matching release* call, and must not grow with the frame count: a long-
 * running session has to reach a steady state. A backend must never require
 * the host to call a release* method for correctness — the host calls them
 * because a pass or target is gone.
 *
 * THREADING. The engine drives one pass at a time from one thread, so calls
 * into a backend are serialised, never concurrent and never reentrant. A
 * backend may therefore keep its state unsynchronised, but must not assume it
 * stays on the same thread across an initialize()/shutdown() pair. The
 * diagnostic sink is invoked synchronously from inside a backend call, so a
 * sink must record and return, never call back into the backend.
 *
 * FAILURE. A backend reports what it could not do instead of degrading
 * silently (see setDiagnosticSink) and never throws across this interface:
 * initialize() returns false, the readback methods return false, and a request
 * that cannot be served leaves the previous state intact. A false return never
 * means "partially applied". A false initialize() must not leave a half-built
 * session behind: the engine calls shutdown() only after a successful
 * initialize() (see RenderEngine::shutdown), so the backend owns the cleanup
 * of its own partial state.
 *
 * RenderBackend is reference-counted: factories return an intrusive_ptr and
 * RenderEngine keeps its own reference, so ownership and lifetime are
 * explicit.
 */
class VN_GRAPHICS_API RenderBackend : public Object, public RefCounted<RenderBackend> {
    VN_OBJECT_META_DECL;

  public:
    virtual ~RenderBackend() = default;

    /** @brief Initializes the backend: device, surface, pipelines, caches.
     *
     * Called once per session, after the host announced the native window
     * (setWindowHandle) and the default content program (setDefaultContentProgram). A backend
     * that is already initialized tears the previous session down first, so
     * this may be called again on a recreated surface.
     *
     * @return true when the backend is ready to render; false when a required
     *         resource could not be created. On false the backend owns the
     *         cleanup of whatever it built (the engine does not call
     *         shutdown() after a failed initialize()) and reports the reason
     *         on its diagnostics channel.
     */
    virtual bool initialize() = 0;

    /** @brief Releases everything the backend owns for the current session.
     *
     * Safe to call after a failed initialize(), before any initialize(), or
     * twice: when it returns, the backend must be in the state of a freshly
     * constructed one and initializable again. Every window / GPU object the
     * session created must be gone — the engine calls it both when the
     * renderer shuts down and when the rendering surface is recreated.
     */
    virtual void shutdown() = 0;

    /** @brief Begins a frame.
     *
     * May acquire the next presentable image of the surface, which the frame
     * must hand back through swapBuffers() (see the class contract).
     */
    virtual void beginFrame() = 0;

    /** @brief Ends the frame: flushes what the frame's passes queued.
     *
     * Must not present — presenting is swapBuffers().
     */
    virtual void endFrame() = 0;

    /** @brief Sets the active render target.
     *
     * Pass nullptr to render into the default (window) framebuffer. When the
     * backend reports supportsRenderTargets() and a valid off-screen target
     * is passed, the backend creates and manages the target's GPU
     * attachments (created lazily on first bind, rebuilt when the target is
     * resized) and renders into it until another target is set. The backend
     * owns the attachments; the RenderTarget stays a logical description.
     *
     * @param target Render target, or nullptr for the default framebuffer.
     */
    virtual void setRenderTarget(RenderTarget* target) = 0;

    /** @brief Returns whether off-screen render targets are supported.
     *
     * Backends that support them honour non-null RenderTargets in
     * setRenderTarget() by creating and binding GPU attachments. Backends
     * that do not support them only accept nullptr (the default framebuffer)
     * and should ignore off-screen targets.
     *
     * The engine ASKS this once per frame (see RenderEngine::frame), so declining is a state the host
     * is told about — a pass staged through a target the backend cannot draw would otherwise be
     * recorded where the pipeline never put it, which is a wrong picture rather than a slow one.
     *
     * @return true when setRenderTarget() accepts off-screen targets.
     */
    virtual bool supportsRenderTargets()
    {
        return false;
    }

    /** @brief Opens a pass scope: announces the pass about to be executed.
     *
     * The engine calls this once per enabled registered pass, immediately
     * before that pass's per-pass state (setRenderTarget / setViewport /
     * setLights / setDepthMode / clear / setPassOrder) and its draw call
     * (render / drawScreenProgram); endPass() follows
     * once the pass ran.
     *
     * WHAT THE SCOPE MEANS. The scope makes the pass explicit, so "which call
     * means what" no longer depends on the call order:
     *   * scope attributes — setRenderTarget, setPassOrder, setDepthMode and
     *     the clear() marker — describe the pass, so every draw call of the
     *     scope sees them (a pass that draws twice keeps its stacking order,
     *     depth policy and target for both) and they are dropped at endPass()
     *     instead of leaking into the next pass;
     *   * per-draw-call attributes — setViewport and setLights — are consumed
     *     by the draw call that follows them.
     * The scope is the ONLY way a pass runs. Between the frame pair a host may
     * announce state with no scope open — those calls are inert (the next
     * beginPass() starts from an empty request) — but a DRAWING call (render /
     * clear / drawScreenProgram) with no pass announced has nothing to belong
     * to, so it is refused and reported (PassProtocolViolation) instead of
     * drawing with state that pass never announced.
     *
     * The pass is the pass's identity to the backend: a backend that retains
     * per-pass GPU state (a content view, a compiled pipeline, a sampling
     * slot) keys that state by this object, so two passes never alias each
     * other even when they share a camera and an order. Announcing the pass
     * also marks it active for the current frame: a backend may retire the
     * retained state of any pass it was not asked to draw this frame, which
     * is what makes disabling a pass (RenderPass::setEnabled) or changing its
     * camera / render target / depth mode take effect instead of leaving
     * stale content on screen.
     *
     * The default no-op keeps backends without retained per-pass state working
     * unchanged.
     *
     * @param pass The pass about to execute (borrowed for the scope).
     */
    virtual void beginPass(raw_ptr<const RenderPass> pass)
    {
        (void)pass;
    }

    /** @brief Closes the pass scope opened by beginPass().
     *
     * Called after the pass's draw call. Per-pass state queued by this scope
     * that no draw call consumed (a pass that set a render target but then
     * drew nothing) is discarded here, so it can never leak into the next
     * pass. A backend must not retain any per-pass state beyond this call
     * except the GPU resources it owns for the pass itself.
     *
     * Call it exactly once per beginPass(): a nested beginPass() or an endPass()
     * with no open scope means the protocol is out of step, so the state the
     * caller expected to apply did not — a backend should report that on its
     * diagnostics channel (DiagnosticCategory::PassProtocolViolation) rather
     * than silently drawing something else.
     */
    virtual void endPass() {}

    /** @brief Draws a full-screen pass through a fragment program, sampling the source's colour attachments.
     *
     * The ONE full-screen draw: a plain copy, a deferred lighting pass and a host post-process are
     * all this call with different programs (see BuiltinShaders::screenCopyProgram /
     * deferredLightProgram, and ScreenPass::setProgram for the contract the fragment stage
     * compiles against).
     *
     * Draws a full-screen triangle whose fragment stage is @p program's, written into the CURRENT
     * target (see setRenderTarget, nullptr = the default framebuffer) within the sub-viewport set by
     * setViewport(). Each colour attachment of @p source is bound as a sampled texture at
     * descriptor binding 0..N-1 (so binding i reads attachment i), so a G-buffer producer's
     * textures (albedo / normal / position) reach the pass in one draw. Lights set by the most
     * recent setLights() and the pass camera are forwarded as per-frame push-constant parameters
     * (the lights pre-transformed to the camera's view space). The default no-op lets backends
     * without texture-input support ignore the call.
     *
     * @param source  Target whose colour attachments are sampled.
     * @param program Fragment-stage program to draw with (vertex stage, if any, is ignored — the
     *                backend provides the fullscreen vertex stage, BuiltinShaders::fullscreenVertexProgram).
     * @param camera  Camera whose view transforms the pushed lights; also the key for the retained
     *                fullscreen slot. Without one the draw is refused: there is no view to build.
     *
     * A program that cannot be prepared (no fragment stage, a stage that fails to compile, a binding
     * the source cannot provide) is reported and draws NOTHING — never a substituted picture.
     */
    virtual void drawScreenProgram(vn::graphics::RenderTarget*  source,
                                   vn::raw_ptr<const vn::graphics::ShaderProgram> program,
                                   vn::raw_ptr<const vn::graphics::Camera> camera)
    {
        (void)source;
        (void)program;
        (void)camera;
    }

    /** @brief Notifies the backend of the order of the pass about to render.
     *
     * The engine calls this right before each registered pass executes, with
     * the order the caller passed to addPass() — the explicit pipeline order
     * that already drives pass execution. A backend that keeps several
     * retained content slots under one target uses it as the slot's STACKING
     * position (ascending), so the draw order inside the target always equals
     * the user-set pipeline order regardless of when each slot was created
     * (a pre-frame warm-up pass may create a higher-order slot before a
     * lower-order one has run). The slot's IDENTITY, in contrast, is the pass
     * announced by beginPass(), so two passes never share a slot. The value is
     * consumed by the following render() call. The default no-op lets backends
     * without per-slot ordering ignore it.
     *
     * @param order The current pass's explicit pipeline order.
     */
    virtual void setPassOrder(int order)
    {
        (void)order;
    }

    /** @brief Releases the backend GPU resources a pass owns.
     *
     * Called by the engine just before a removed pass is dropped. The backend
     * owns everything it retained for that pass (its content view / compiled
     * pipelines / per-pass scene-bridge cache, any sampling slot it drew
     * through, and the window layer it presented through), so it must free
     * that state here to keep a closed resource loop; the RenderPass object
     * itself stays a logical description owned by the caller. It is separate
     * from releaseRenderTarget(): a pass may own a render target (freed by
     * both), while the retained per-pass GPU state is only known to the
     * backend and is keyed by the pass announced via beginPass().
     *
     * The default no-op lets backends that keep no per-pass GPU state ignore
     * the call.
     *
     * @param pass The pass being removed (borrowed for the call).
     */
    virtual void releasePass(raw_ptr<const RenderPass> pass)
    {
        (void)pass;
    }

    /** @brief Releases backend GPU resources for a removed render target.
     *
     * Called by the engine before a target's owning pass/slot is destroyed.
     * The backend owns the target's GPU attachments (images / views / render
     * passes / framebuffers / a per-target scene bridge) plus any sampling
     * (PiP) state, so it must free them here to keep a closed resource loop.
     * The target object itself stays a logical description owned by the
     * caller. The default no-op lets backends without off-screen targets
     * ignore the call.
     *
     * The call also announces that the caller may destroy @p target now, so a
     * backend holding an announcement of it — the setRenderTarget() of the scope
     * being executed (see beginPass) — must DROP that announcement rather than keep
     * the pointer. A call that would still have used it cannot be
     * honoured: it must be skipped and reported (with the reason) instead of
     * being silently redirected to the default framebuffer, which would draw
     * the content where the caller never asked for it.
     *
     * @param target The render target being removed, or nullptr.
     */
    virtual void releaseRenderTarget(vn::graphics::RenderTarget* target)
    {
        (void)target;
    }

    /** @brief Reads back a colour attachment of an off-screen render target.
     *
     * Synchronously copies colour attachment @p attachment of @p target (a
     * target this backend rendered into) into @p outPixels as tightly packed
     * RGBA8 — width * height * 4 bytes, row-major. This is the controlled
     * readback entry point: a logical RenderTarget holds no GPU pixels, so the
     * transfer belongs to the backend that owns the attachments. The default
     * implementation reports the operation as unsupported; a backend that
     * implements readback overrides it (staging buffer, image-to-buffer copy,
     * queue/fence synchronisation and format conversion).
     *
     * A false return is one of several causes — the backend does not support
     * readback, the target was never rendered into / built, the target cannot
     * answer, or the transfer failed. Pass @p why to tell them apart
     * programmatically (ReadbackResult); the backend also reports the reason on
     * its diagnostics channel (see the class contract), and @p outPixels is left
     * untouched either way.
     *
     * @param target     Off-screen target whose colour attachment to read. Borrowed, and CONST: reading
     *                   pixels changes nothing about the target, and a host holding one as const (a
     *                   reader of a target it did not create) can still ask for its pixels.
     * @param attachment Colour attachment index in [0, target->colorCount()).
     * @param outPixels  Receives the packed RGBA8 pixels on success.
     * @param why        Receives why the read did not happen (Ok when it did), or null
     *                   to ignore. A backend that does not implement readback leaves it
     *                   at Unsupported.
     * @return true when the pixels were read; false when the read could not be
     *         performed (see @p why and the diagnostics channel).
     */
    virtual bool readColorBuffer(const vn::graphics::RenderTarget* target, int attachment,
                                 std::vector<std::uint8_t>& outPixels, ReadbackResult* why = nullptr)
    {
        (void)target;
        (void)attachment;
        (void)outPixels;
        if (why != nullptr) {
            *why = ReadbackResult::Unsupported;
        }
        return false;
    }

    /** @brief Reads back the depth attachment of an off-screen render target.
     *
     * Synchronously copies @p target's depth buffer into @p outDepths as
     * width * height floats in [0, 1], row-major. Ownership and support model
     * match readColorBuffer(): the default implementation reports the
     * operation as unsupported. A target whose depth is borrowed
     * (RenderTarget::shareDepth) is read through the SOURCE target, not the
     * borrower, which reports the request as unsupported.
     *
     * @param target    Off-screen target whose depth buffer to read (borrowed, and const like
     *                  readColorBuffer's).
     * @param outDepths Receives the depth values on success.
     * @param why       Receives why the read did not happen (Ok when it did), or null
     *                  to ignore; see readColorBuffer().
     * @return true when the depth values were read; false when the read could
     *         not be performed (see @p why and the diagnostics channel).
     */
    virtual bool readDepthBuffer(const vn::graphics::RenderTarget* target,
                                 std::vector<float>& outDepths, ReadbackResult* why = nullptr)
    {
        (void)target;
        (void)outDepths;
        if (why != nullptr) {
            *why = ReadbackResult::Unsupported;
        }
        return false;
    }

    /** @brief Renders a list of commands.
     *
     * Draws the current pass' content. The commands are this frame's
     * transients and are borrowed (see the class contract): a backend must not
     * keep a reference to them, or to @p camera, past the call. The backend
     * reconciles its retained scene against them, so the call is incremental —
     * a frame whose commands did not change structurally must neither
     * re-upload geometry nor recompile pipelines.
     *
     * @param commands Render commands to draw.
     * @param camera   Camera used for view/projection.
     */
    virtual void render(const std::vector<RenderCommand>& commands, const Camera* camera) = 0;

    /** @brief Sets the light sources for the NEXT drawing call of this pass scope.
     *
     * Called by RenderPass::execute() from the pass's content scene before
     * render(), so every pass lights whatever scene it renders. Backends that
     * support scene lights replace any view-level default light (e.g. a
     * headlight) with the given lights; an empty list restores the backend
     * default. Lights are borrowed for the duration of the call.
     *
     * ONE announcement serves ONE drawing call: a pass that draws more than once in one scope (a
     * custom pass placing several pictures-in-picture, say) announces again before each draw, and a
     * draw with no fresh announcement uses the backend default (for lights) / the whole surface (for
     * the viewport). beginPass() resets both.
     *
     * @param lights Lights of the content scene, or empty for the backend
     *               default.
     */
    virtual void setLights(const std::vector<raw_ptr<const Light>>& lights)
    {
        (void)lights;
    }

    /** @brief Sets the resolved input targets for the NEXT drawing call of this pass scope.
     *
     * The pass' declared inputs (RenderPass::addInput / addInputTarget / addInputName), resolved
     * for THIS frame — null where nothing produced one — in the pass' declaration order. This is
     * how an effect that reads what an earlier pass wrote reaches the shading: a shadow map, a
     * screen-space occlusion buffer, a baked image. The pass states WHAT it reads and the backend
     * binds each entry where its shader's ABI says it goes; the order is the pass' own.
     *
     * Called by the engine between beginPass() and the pass' execute(), after the declared inputs
     * were resolved, so every draw call of that pass sees the same list (a pass is one draw of the
     * scene, and its inputs are a property of the pass, not of one drawable).
     *
     * An input a backend cannot consume is not an error it may paper over: the pass declared it, so
     * silently ignoring it means the picture differs from the one the pass asked for. A backend
     * either binds it or reports (see the diagnostic sink).
     *
     * Default: nothing — a backend that consumes no pass inputs needs no code for them, and the
     * engine's call is a no-op for it.
     *
     * @param inputs Resolved input targets, in the pass' declaration order.
     */
    virtual void setPassInputs(const std::vector<raw_ptr<RenderTarget>>& inputs)
    {
        (void)inputs;
    }

    /** @brief Sets how the upcoming render()'s content handles the target's
     * current depth.
     *
     * TestAndWrite depth-tests and writes (opaque scene content); TestOnly
     * depth-tests without writing (translucent content over already-written
     * depth); Disabled draws on top with no depth (HUD overlays). This is the
     * content slot's depth style and is independent of clearing and of
     * lighting: the backend chooses the light source from the content scene,
     * not from this setting. The default no-op lets backends without a
     * per-slot depth state ignore it.
     *
     * @param mode The depth handling for the next render() content.
     */
    virtual void setDepthMode(DepthMode mode)
    {
        (void)mode;
    }

    /** @brief Announces how this pass' target is cleared before it draws.
     *
     * The scope attribute for clearing (see ClearPolicy for what a policy means and which parts of
     * it a target can honour): call it between beginPass() and the pass' draw, before the draw that
     * should see the cleared target. Applies to the target selected with setRenderTarget(); null
     * (the default) means the window / main surface.
     *
     * NOT CALLING IT LEAVES THE TARGET UNCLEARED. That is the whole difference from the three
     * pass-side clear setters on RenderPass (clearColor / shouldClearDepth / clearEnabled): those
     * describe a policy, this announces it, and a pass that never announces one draws over whatever
     * the target already holds.
     *
     * The default no-op lets a backend that cannot clear ignore it (its diagnostics channel is where
     * it says so — a silently different clear is a picture the pass did not ask for).
     *
     * @param policy How to clear: the colour of attachment 0 and whether depth is cleared too.
     */
    virtual void setClearPolicy(const ClearPolicy& policy)
    {
        (void)policy;
    }

    /** @brief Presents the rendered frame.
     *
     * The only call that presents, and the last call of a driven frame.
     */
    virtual void swapBuffers() = 0;

    /** @brief Selects the DEFAULT program for content: what a drawable naming none of its own gets.
     *
     * A default, not an override: a drawable's own program still wins, and so does a pass' program
     * for the passes that take one. There is no shading-model enum and no fallback: the host names a
     * program (the engine starts with forwardProgram(), see RenderEngine), or content without its own
     * program is NOT drawn and the reason is reported.
     *
     * May be called before initialize(), where it is a session decision baked as the session starts,
     * or on a RUNNING session, where the backend re-bakes the shading side so the next frame draws
     * with @p program. A live switch rebuilds content shading only: the targets keep their
     * attachments, their pass graphs and their depth history, so a host that offers a shading toggle
     * gets a differently shaded picture rather than a restarted session.
     *
     * @param program Program to shade program-less content with, or null for "none" (such content is
     *                then reported and skipped).
     */
    virtual void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program)
    {
        (void)program;
    }

    /** @brief Binds a host native window the backend may render into.
     *
     * Called by RenderEngine before initialize() when the host provides an
     * existing native window (e.g. a Qt QWindow handle). The backend attaches
     * to it instead of creating its own window. Default no-op.
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr.
     */
    virtual void setWindowHandle(void* native_handle)
    {
        (void)native_handle;
    }

    /** @brief Announces a change of the rendering surface size.
     *
     * The surface itself owns its size, so the authority order is surface > announcement > default:
     * a backend whose surface belongs to a window system (an embedded host window, a swapchain)
     * FOLLOWS THAT SURFACE and uses this call to re-derive what hangs off the size, whereas a
     * backend that owns its surface (headless, or it created the window itself) applies the
     * announcement. Either way the announced numbers are what the engine keeps in its frame context
     * for the passes that lay themselves out on the surface size (see RenderEngine::resize), so the
     * two sides never disagree about it.
     *
     * @param width  Announced surface width in pixels.
     * @param height Announced surface height in pixels.
     */
    virtual void resize(int width, int height)
    {
        (void)width;
        (void)height;
    }

    /** @brief Restricts the NEXT drawing call of this pass scope to a sub-rectangle of the surface.
     *
     * Called by RenderPass::execute() before drawing a pass that owns a
     * sub-viewport (e.g. an axis gizmo in a screen corner). Backends should
     * combine the viewport and scissor to that rectangle. The default no-op
     * keeps drawing to the full surface, which is correct for backends that
     * do not support sub-viewports yet.
     *
     * ONE announcement serves ONE drawing call (see setLights): the base RenderPass announces its
     * declared viewport once and draws once, while a pass that draws several times announces the
     * viewport of each draw — which is what lets one scope place several pictures-in-picture.
     * beginPass() resets the announcement.
     *
     * @param x      Viewport origin x in device pixels.
     * @param y      Viewport origin y in device pixels (top-left origin).
     * @param width  Viewport width in device pixels.
     * @param height Viewport height in device pixels.
     */
    virtual void setViewport(int x, int y, int width, int height)
    {
        (void)x;
        (void)y;
        (void)width;
        (void)height;
    }

    /** @brief Gets the native handle the backend is currently attached to.
     *
     * Returns the native window handle (HWND on Windows) the backend bound
     * its render surface to during initialize(), or nullptr when the backend
     * is not attached to a host surface (standalone window or not yet
     * initialized). It answers "which surface am I on" for a host that has to notice the windowing
     * system recreating that surface underneath the backend: re-announcing through
     * setWindowHandle() is how the backend is moved to the new one, and this is how a host decides
     * whether that is necessary.
     *
     * @return The attached native handle, or nullptr.
     */
    virtual void* nativeHandle() const
    {
        return nullptr;
    }

    /** @brief Installs the sink that receives backend diagnostics.
     *
     * A backend reports what it could not serve (a rejected geometry, a
     * dropped channel, a shader that fell back to the built-in one, an
     * off-screen target it could not build) instead of degrading silently, so
     * a host can surface it — log, overlay, telemetry — without the backend
     * having to link a logging framework or write to stderr. Installing no
     * sink keeps the backend's own stderr tracing and still counts every
     * diagnostic (diagnosticCount()).
     *
     * The sink is invoked synchronously from inside the backend call that
     * reported, so it must only record and return — calling back into the
     * backend from a sink is unsupported.
     *
     * @param sink Callback invoked for every diagnostic, or empty to clear.
     */
    virtual void setDiagnosticSink(DiagnosticSink sink);

    /** @brief Gets the installed diagnostic sink (empty when unset).
     *
     * @return The sink a previous setDiagnosticSink() installed.
     */
    const DiagnosticSink& diagnosticSink() const { return diagnostic_sink_; }

    /** @brief Gets how many diagnostics this backend has reported.
     *
     * Counted whether or not a sink is installed, so a host can gate on
     * "did anything unexpected happen" without listening.
     *
     * @return Total number of reported diagnostics.
     */
    std::size_t diagnosticCount() const { return diagnostic_count_; }

    /** @brief Gets how many diagnostics of @p category were reported.
     *
     * @param category Category to count.
     * @return Number of reported diagnostics in that category.
     */
    std::size_t diagnosticCount(DiagnosticCategory category) const;

  protected:
    /** @brief Reports one diagnostic to the installed sink and counts it.
     *
     * Backends call this instead of failing silently. The backend keeps its
     * own out-of-band tracing (e.g. stderr) in addition to the sink, so an
     * unmodified host sees today's behaviour plus a programmatic channel.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, with the numbers involved.
     */
    void reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category,
                          const String& message);

    RenderBackend() = default;

  private:
    DiagnosticSink diagnostic_sink_;
    std::size_t    diagnostic_count_ = 0;
    // Per-category counts, indexed by DiagnosticCategory (sized by its Count).
    std::array<std::size_t, static_cast<std::size_t>(DiagnosticCategory::Count)>
        diagnostic_counts_{};
};

VN_GRAPHICS_NS_END
