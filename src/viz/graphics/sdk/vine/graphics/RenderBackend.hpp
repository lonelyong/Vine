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
 * Defines the contract that concrete graphics backends (OpenGL, Vulkan, etc.)
 * must implement. Supports both high-level pass execution and low-level
 * command rendering.
 *
 * RenderBackend is reference-counted: factories return an intrusive_ptr and
 * RenderEngine keeps its own reference, so ownership and lifetime are
 * explicit.
 */
class V_GRAPHICS_API RenderBackend : public Object, public RefCounted<RenderBackend> {
    V_OBJECT_META_DECL;

  public:
    virtual ~RenderBackend() = default;

    /** @brief Initializes the backend. */
    virtual bool initialize() = 0;

    /** @brief Releases backend resources. */
    virtual void shutdown() = 0;

    /** @brief Begins a frame. */
    virtual void beginFrame() = 0;

    /** @brief Ends a frame. */
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
     */
    virtual void endPass() {}

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
     * operation as unsupported.
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
     * @param commands Render commands to draw.
     * @param camera   Camera used for view/projection.
     */
    virtual void render(const std::vector<RenderCommand>& commands, const Camera* camera) = 0;

    /** @brief Sets the light sources for the upcoming render() pass.
     *
     * Called by RenderPass::execute() from the pass's content scene before
     * render(), so every pass lights whatever scene it renders. Backends that
     * support scene lights replace any view-level default light (e.g. a
     * headlight) with the given lights; an empty list restores the backend
     * default. Lights are borrowed for the duration of the call.
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
     * @param backgroundColor Clear color.
     * @param clearDepth      Whether to also clear the depth buffer. Ignored
     *                        for the window target (always cleared).
     */
    virtual void clear(const Color& backgroundColor, bool clearDepth = true) = 0;

    /** @brief Swaps buffers (double buffering). */
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

    /** @brief Handles a change of the rendering surface size.
     *
     * @param width  New surface width in pixels.
     * @param height New surface height in pixels.
     */
    virtual void resize(int width, int height)
    {
        (void)width;
        (void)height;
    }

    /** @brief Restricts subsequent drawing to a sub-rectangle of the surface.
     *
     * Called by RenderPass::execute() before drawing a pass that owns a
     * sub-viewport (e.g. an axis gizmo in a screen corner). Backends should
     * combine the viewport and scissor to that rectangle. The default no-op
     * keeps drawing to the full surface, which is correct for backends that
     * do not support sub-viewports yet.
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
