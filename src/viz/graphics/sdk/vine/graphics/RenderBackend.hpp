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
#include "ShaderPreset.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class MaterialManager;
class RenderTarget;
class RenderPass;
class ShaderProgram;
struct RenderCommand;

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
 *        render() | drawScreenTexture() | drawScreenProgram();
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
class V_GRAPHICS_API RenderBackend : public Object, public RefCounted<RenderBackend> {
    V_OBJECT_META_DECL;

  public:
    virtual ~RenderBackend() = default;

    /** @brief Initializes the backend: device, surface, pipelines, caches.
     *
     * Called once per session, after the host announced the native window
     * (setWindowHandle) and the shading preset (setShaderPreset). A backend
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
     * (render / drawScreenTexture / drawScreenProgram); endPass() follows
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
     * A backend that also accepts the direct-drive style (no scope at all, as
     * the device self-test uses) keeps the queued request until the caller
     * overwrites it.
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
     * The default no-op keeps backends without retained per-pass state (and
     * direct backend drivers that skip the pass protocol) working unchanged.
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

    /** @brief Reports whether a beginPass() scope is currently open.
     *
     * The engine drives one pass at a time, so the per-pass calls (see
     * beginPass) belong to the scope between beginPass() and endPass(). A
     * backend that accepts the direct-drive style (queued state and a draw call
     * without any scope, which the device self-test uses) treats an open scope
     * as "this request belongs to that pass" and no scope as "the request is
     * the caller's to manage". Exposed so a host or test can assert the
     * protocol state instead of inferring it.
     *
     * @return true while a beginPass() scope is open.
     */
    virtual bool isPassScopeOpen() const
    {
        return false;
    }

    /** @brief Draws a full-screen textured pass sampling a target's colour
     * attachment.
     *
     * Samples the colour attachment @p attachment of @p source (a target
     * written earlier in the same frame, e.g. an off-screen render-to-texture
     * pass) through a full-screen textured triangle drawn into the CURRENT
     * target — the one set by the most recent setRenderTarget() (nullptr = the
     * default framebuffer) — respecting any sub-viewport configured via
     * setViewport(). This is the low-level primitive behind a screen/composite
     * pass; the target to sample stays a logical RenderTarget and the backend
     * resolves it to its own GPU texture. A multi-attachment target (MRT /
     * G-buffer) exposes each colour attachment as an independent sampleable
     * texture, so a consumer selects which one to read by index. The default
     * no-op lets backends without texture-input support ignore the call.
     *
     * @param source     Target whose colour texture to sample, or nullptr.
     * @param attachment Colour attachment index in [0, source->colorCount()).
     */
    virtual void drawScreenTexture(vine::graphics::RenderTarget* source, int attachment)
    {
        (void)source;
        (void)attachment;
    }

    /** @brief Draws a full-screen textured pass sampling a target's first
     * colour attachment.
     *
     * Convenience for the common single-texture case: samples colour
     * attachment 0 of @p source (see drawScreenTexture(RenderTarget*, int)).
     *
     * @param source Target whose colour texture to sample, or nullptr.
     */
    virtual void drawScreenTexture(vine::graphics::RenderTarget* source)
    {
        drawScreenTexture(source, 0);
    }

    /** @brief Draws a full-screen pass through a user fragment program,
     * sampling every colour attachment of an MRT source.
     *
     * Draws a full-screen triangle (the backend supplies the vertex stage)
     * whose fragment shader is @p program's fragment stage, written into the
     * CURRENT target (see setRenderTarget, nullptr = the default framebuffer)
     * within the sub-viewport set by setViewport(). Each colour attachment of
     * @p source is bound as a sampled texture at descriptor binding 0..N-1, so
     * a G-buffer producer's textures (albedo / normal / position) reach the
     * pass in one draw. Lights set by the most recent setLights() and the
     * pass camera are forwarded as per-frame push-constant parameters (the
     * lights pre-transformed to the camera's view space). The default no-op
     * lets backends without texture-input support ignore the call.
     *
     * @param source  MRT target whose colour attachments are sampled.
     * @param program User program supplying the fragment stage (vertex stage,
     *                if any, is ignored — the backend provides the fullscreen
     *                vertex shader).
     * @param camera  Camera whose view transforms the pushed lights; also the
     *                key for the retained fullscreen slot.
     */
    virtual void drawScreenProgram(vine::graphics::RenderTarget*  source,
                                   vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                   vine::raw_ptr<const vine::graphics::Camera> camera)
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

    /** @brief Releases backend GPU state for a removed pass' window content.
     *
     * Legacy counterpart of releasePass() for the historical (pass camera,
     * pass order) content-slot key. The engine still calls it for backends
     * that only implement this narrower contract; new backends should key
     * their per-pass state by the pass announced via beginPass() and release
     * it in releasePass(). The default no-op lets backends that keep no
     * per-camera GPU state ignore the call.
     *
     * @param camera The removed pass's camera (the content-slot key), or
     *               nullptr.
     * @param order  The removed pass's explicit pipeline order (the
     *               content-slot key within that camera).
     */
    virtual void releaseWindowLayer(raw_ptr<const Camera> camera, int order = 0)
    {
        (void)camera;
        (void)order;
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
     * backend that queued it (a direct driver's setRenderTarget announcement,
     * which survives frames — see beginPass) must DROP that announcement rather
     * than keep the pointer. A call that would still have used it cannot be
     * honoured: it must be skipped and reported (with the reason) instead of
     * being silently redirected to the default framebuffer, which would draw
     * the content where the caller never asked for it.
     *
     * @param target The render target being removed, or nullptr.
     */
    virtual void releaseRenderTarget(vine::graphics::RenderTarget* target)
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
     * queue/fence synchronisation and format conversion). Backends without
     * readback support return false and leave @p outPixels untouched, so a
     * caller can always distinguish a successful read from unsupported.
     *
     * @param target     Off-screen target whose colour attachment to read.
     * @param attachment Colour attachment index in [0, target->colorCount()).
     * @param outPixels  Receives the packed RGBA8 pixels on success.
     * @return true when the pixels were read; false when unsupported or the
     *         read failed.
     */
    virtual bool readColorBuffer(vine::graphics::RenderTarget* target, int attachment,
                                 std::vector<std::uint8_t>& outPixels)
    {
        (void)target;
        (void)attachment;
        (void)outPixels;
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
     * @param target    Off-screen target whose depth buffer to read.
     * @param outDepths Receives the depth values on success.
     * @return true when the depth values were read; false when unsupported or
     *         the read failed.
     */
    virtual bool readDepthBuffer(vine::graphics::RenderTarget* target,
                                 std::vector<float>& outDepths)
    {
        (void)target;
        (void)outDepths;
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

    /** @brief Clears the colour buffer, and optionally the depth buffer, of
     * the current render target.
     *
     * Applies to the target selected with setRenderTarget(); null (the
     * default) means the window / main surface. For the WINDOW target the
     * depth buffer is always cleared regardless of @p clearDepth: the
     * windowing system owns the surface render pass and fixes its depth
     * load-op to CLEAR, so clearDepth=false cannot be honoured there and is
     * treated as true (only the clear colour takes effect).
     *
     * clearDepth=false IS honoured for off-screen render targets: their depth
     * content is preserved across frames through a depth-LOAD pass. Passes
     * that need a previous frame's depth (accumulation, or incremental writes
     * that depth-test against existing content) should render into an
     * off-screen target and composite it into the window.
     *
     * MULTI-ATTACHMENT (MRT) TARGETS: the clear color applies to the FIRST
     * colour attachment; every further attachment is left TRANSPARENT BLACK
     * (0,0,0,0) until a fragment writes it. That is deliberate, not an
     * oversight: it lets a consumer tell "nothing was drawn here" from the
     * stored data itself — the deferred-lighting program treats a stored view
     * position of ~0 as background — so the rule must not be changed to a
     * uniform clear without auditing those consumers. A backend that cannot
     * honour it must say so on its diagnostics channel rather than silently
     * clearing differently.
     *
     * @param backgroundColor Clear color.
     * @param clearDepth      Whether to also clear the depth buffer. Ignored
     *                        for the window target (always cleared).
     */
    virtual void clear(const Color& backgroundColor, bool clearDepth = true) = 0;

    /** @brief Presents the rendered frame.
     *
     * The only call that presents, and the last call of a driven frame.
     */
    virtual void swapBuffers() = 0;

    /** @brief Gets the backend's material manager.
     *
     * Returns nullptr when the backend has no material manager, e.g. before
     * initialize() or when the backend does not support materials. The
     * returned manager stays valid for the backend's lifetime.
     *
     * @return The backend material manager, or nullptr.
     */
    virtual MaterialManager* materialManager()
    {
        return nullptr;
    }

    /** @brief Selects the shading-model preset for scene geometry.
     *
     * Must be called before initialize(); the backend maps the preset onto its
     * shader/material pipeline (vsg: Phong vs flat ShaderSet). Presets without
     * a backend implementation yet (Pbr / ShadowedPhong) fall back to
     * StandardPhong. Default no-op.
     *
     * @param preset Shading-model preset.
     */
    virtual void setShaderPreset(ShaderPreset preset)
    {
        (void)preset;
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
     * initialized). Host code can compare this against the current window
     * context handle to detect when the windowing system recreated the native
     * surface underneath the backend.
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

V_GRAPHICS_NS_END
