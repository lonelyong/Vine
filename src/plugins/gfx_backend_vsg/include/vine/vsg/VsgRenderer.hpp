#pragma once

// The renderer's definition: the class, its private helper declarations and the two state
// objects it drives (VsgRendererState.hpp). It used to be TWO headers with the class hidden
// behind a PImpl, which bought nothing here — the host only ever sees the graphics SDK
// interface (vine::graphics::RenderBackend), and the plugin's own translation units were the
// only consumers of the renderer header — while costing a helper-placement rule on every
// refactor (a helper needing a state type could not be declared in the "public" header) and a
// pointer indirection on every state access.
//
// No d-pointer is left, and nothing here is named after one: the state is held BY VALUE and
// split by LIFETIME,
//
//   * VsgRendererPersistent — outlives every window session (the material manager contract),
//   * VsgRendererState — one window session: everything that references a vsg::Window /
//     vsg::Device, replaced wholesale on shutdown()/initialize() so a session-scoped resource
//     can never be left behind by a manual teardown list.
//
// (The reasoning the deleted public header carried is in .ai/design/vsg-pass-lifecycle.md §44,
// the state split in §47 and the concepts' extraction in §48.)

#include <vine/vsg/vsg_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec4.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

/**
 * @brief VSG render backend implementing vine::graphics::RenderBackend.
 *
 * VsgRenderer is the VulkanSceneGraph implementation of the graphics
 * abstraction layer. It drives a vsg::Viewer running on a vsg::Window and
 * reconciles a retained vsg scene against the per-frame render command
 * stream (SceneBridge), so runtime scene edits are reflected without
 * re-initializing the backend.
 */
class V_VSG_API VsgRenderer : public vine::graphics::RenderBackend {
  public:
    /** @brief Constructs a renderer.
     *
     * The engine owns the pipeline and drives content per pass, so the
     * renderer binds neither a Vine scene nor a camera: content slots are
     * created lazily from the per-pass render() calls the engine drives.
     */
    VsgRenderer();
    ~VsgRenderer() override;

    // ---- RenderBackend interface ----

    /** @brief Initializes the window, viewer, camera and pipeline.
     *
     * @return true when initialization succeeded.
     */
    bool initialize() override;

    /** @brief Closes the window and releases the viewer. */
    void shutdown() override;

    /** @brief Begins a frame (advance + handle events). */
    void beginFrame() override;

    /** @brief Ends a frame (viewer update). */
    void endFrame() override;

    /** @brief Sets the render target.
     *
     * Off-screen targets are not yet supported; only the default framebuffer
     * (nullptr) is valid.
     *
     * @param target Render target, or nullptr for the default framebuffer.
     */
    void setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target) override;

    /** @brief Opens a pass scope for the pass the engine is about to execute.
     *
     * The announced pass is the identity of the GPU state this backend
     * retains for it (content view + scene bridge, PiP / fullscreen-program
     * slot), so two passes never alias each other even when they share a
     * camera and a pass order, and the state follows the pass when its camera
     * / render target / program changes. It also marks the pass active for
     * this frame: submitFrame() retires the retained state of every pass that
     * was NOT announced this frame, which is what makes disabling a pass
     * (RenderPass::setEnabled) stop drawing it instead of leaving its last
     * synced content on screen.
     *
     * @param pass The pass about to execute (borrowed for the scope).
     */
    void beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass) override;

    /** @brief Closes the pass scope opened by beginPass().
     *
     * Discards any per-pass state queued by the scope that no draw call
     * consumed (a pass that set a render target / viewport but drew nothing),
     * so it cannot leak into the next pass.
     */
    void endPass() override;

    /** @brief Reports whether a beginPass() scope is open.
     *
     * True between beginPass() and endPass(); the direct-drive style (queued
     * state without a scope) reports false.
     *
     * @return true while a pass scope is open.
     */
    bool isPassScopeOpen() const override;

    /** @brief Releases every GPU resource this backend retains for a pass.
     *
     * Detaches and drops the pass' content view (window or off-screen), its
     * PiP / fullscreen-program slot, its per-pass scene-bridge cache and the
     * compiled pipelines it owns, after waiting for the device to go idle.
     * Called by the engine when the pass is removed.
     *
     * @param pass The removed pass (borrowed for the call), or null.
     */
    void releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass) override;

    /** @brief Returns whether off-screen render targets are supported.
     *
     * @return true.
     */
    bool supportsRenderTargets() override;

    /** @brief Draws a full-screen textured pass sampling a target's colour
     * attachment.
     *
     * Samples colour attachment @p attachment of @p source (an off-screen
     * target this backend rendered earlier in the same frame) through a
     * full-screen textured triangle drawn into the CURRENT target (the one
     * set by setRenderTarget(), nullptr = the window) within the sub-viewport
     * set by setViewport() (picture-in-picture). A multi-attachment target
     * (MRT / G-buffer) exposes each colour attachment as an independent
     * sampleable texture. EXPERIMENTAL.
     *
     * @param source     Off-screen target whose colour texture to sample.
     * @param attachment Colour attachment index to sample (0 = first).
     */
    void drawScreenTexture(vine::graphics::RenderTarget* source, int attachment) override;

    /** @brief Draws a full-screen pass through a user fragment program,
     * sampling every colour attachment of an MRT source (deferred lighting).
     *
     * See RenderBackend::drawScreenProgram for the contract. The backend
     * compiles the program's fragment stage, binds each source colour
     * attachment as a sampled texture (binding 0..N-1) and pushes view-space
     * light parameters each frame.
     *
     * The source's DEPTH is bound as an extra sampled texture only when it is
     * actually sampleable: the source must declare depth promotion
     * (RenderTarget::setDepthPromotion) AND no pass of it may preserve depth —
     * a depth-LOAD pass overrides promotion (§28), leaving the image in the
     * attachment layout so it cannot be sampled.
     *
     * @param source  MRT target whose colour attachments are sampled.
     * @param program User program supplying the fragment stage.
     * @param camera  Camera whose view transforms the pushed lights.
     */
    void drawScreenProgram(vine::graphics::RenderTarget*                       source,
                           vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                           vine::raw_ptr<const vine::graphics::Camera>        camera) override;

    /** @brief Stops drawing and frees GPU state for a removed pass' window
     * content slot.
     *
     * Legacy (pass camera, pass order) key used by backends that predate
     * releasePass(); this backend keys every slot by the pass announced in
     * beginPass(), so the call only cleans up state created by a direct
     * driver that never opened a pass scope.
     *
     * @param camera The removed pass's camera (the legacy content-slot key),
     *               or null.
     * @param order  The removed pass's explicit pipeline order (the legacy
     *               content-slot key within that camera).
     */
    void releaseWindowLayer(raw_ptr<const vine::graphics::Camera> camera, int order) override;

    /** @brief Notifies the renderer of the order of the pass about to render.
     *
     * The engine announces each pass's explicit pipeline order (addPass())
     * right before it executes; the value is consumed by the following draw
     * call and used as the STACKING position of that pass' retained slot
     * (ascending) within the target. The slot's identity is the pass announced
     * by beginPass(), so the order only decides where the pass draws relative
     * to the target's other slots.
     *
     * @param order The current pass's explicit pipeline order.
     */
    void setPassOrder(int order) override;

    /** @brief Frees GPU state (offscreen graph + PiP slot) for a removed target.
     *
     * @param target Render target being removed, or null.
     */
    void releaseRenderTarget(vine::graphics::RenderTarget* target) override;

    /** @brief Reads back a colour attachment of an off-screen render target.
     *
     * Synchronously copies colour attachment @p attachment of @p target into
     * @p outPixels as packed RGBA8 (width * height * 4 bytes, row-major) — the
     * device-side readback that lets a harness assert PIXELS, so a rendering
     * regression is caught by the picture instead of by the absence of
     * validation errors. RGBA8 attachments are read directly; a float
     * attachment (RGBA16F / RGBA32F) is reported as unsupported rather than
     * being silently mis-packed, and the window target has no off-screen image
     * to read.
     *
     * @param target     Off-screen target this backend rendered into.
     * @param attachment Colour attachment index in [0, target->colorCount()).
     * @param outPixels  Receives the packed RGBA8 pixels on success.
     * @return true when the pixels were read; false when the target was never
     *         built, the attachment is out of range or its format is not RGBA8.
     */
    bool readColorBuffer(vine::graphics::RenderTarget* target, int attachment,
                         std::vector<std::uint8_t>& outPixels) override;

    /** @brief Reads back the depth attachment of an off-screen render target.
     *
     * Synchronously copies @p target's depth buffer into @p outDepths as
     * width * height values in [0, 1], row-major — the direct measurement the
     * depth-order pixel assertion otherwise has to infer from colours. Only the
     * unambiguous depth formats are read: D32_SFLOAT (the stored texel is the
     * value) and D16_UNORM (divided by 65535). A packed D24_UNORM_S8_UINT target
     * is reported as unsupported instead of guessing which 24 of its 32 bits
     * hold the depth; a target without a depth attachment has nothing to read.
     *
     * @param target    Off-screen target this backend rendered into.
     * @param outDepths Receives the depth values on success.
     * @return true when the depth values were read; false when the target was
     *         never built, has no depth attachment or uses a packed format. A
     *         target that borrows its depth (RenderTarget::shareDepth) reports
     *         false: read the source target's depth instead.
     */
    bool readDepthBuffer(vine::graphics::RenderTarget* target, std::vector<float>& outDepths) override;

    /** @brief Renders the current frame from the render command stream.
     *
     * The retained vsg scene is reconciled against the commands (SceneBridge)
     * and (re)compiled when the set of drawables changed structurally.
     *
     * @param commands Render commands for this frame.
     * @param camera   Camera used for view/projection.
     */
    void render(const std::vector<vine::graphics::RenderCommand>& commands, vine::raw_ptr<const vine::graphics::Camera> camera) override;

    /** @brief Sets the light sources for the upcoming render() pass.
     *
     * Called by RenderPass::execute() from the pass's content scene before
     * render(). The lights replace the view's default light (the headlight on
     * the main view / the ambient fill on off-screen views); an empty list
     * keeps the view's default. Lights are borrowed for the duration of the
     * call and translated to vsg light nodes immediately.
     *
     * @param lights Lights of the content scene, or empty for the default.
     */
    void setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights) override;

    /** @brief Sets the clear color and depth-clear state. */
    void clear(const vine::Color& backgroundColor, bool clearDepth) override;

    /** @brief Sets how the next render()'s content handles depth (see
     * RenderBackend::setDepthMode).
     *
     * @param mode Depth handling for the next render() content.
     */
    void setDepthMode(vine::graphics::DepthMode mode) override;

    /** @brief Presents the rendered frame. */
    void swapBuffers() override;

    /** @brief Gets the backend's material manager.
     *
     * The manager is created with the renderer and stays valid for the
     * renderer's lifetime.
     *
     * @return The VSG material manager.
     */
    vine::raw_ptr<vine::graphics::MaterialManager> materialManager() override;

    /** @brief Binds a host native window; when present, the renderer renders
     * into that window's native surface instead of creating its own window.
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr.
     */
    void setWindowHandle(void* native_handle) override;

    /** @brief Rebuilds the swapchain for the new surface size.
     *
     * @param width  New surface width in pixels.
     * @param height New surface height in pixels.
     */
    void resize(int width, int height) override;

    /** @brief Restricts the next render() to a sub-viewport of the surface.
     *
     * Called by RenderPass::execute() before a pass that owns a sub-viewport
     * (e.g. an axis gizmo in a screen corner). The rectangle is consumed by
     * the following render() call, which draws into its own RenderGraph. A
     * pass without a sub-viewport renders the full surface.
     *
     * @param x      Viewport origin x in device pixels.
     * @param y      Viewport origin y in device pixels (top-left origin).
     * @param width  Viewport width in device pixels.
     * @param height Viewport height in device pixels.
     */
    void setViewport(int x, int y, int width, int height) override;

    /** @brief Gets the native handle the renderer attached its window to.
     *
     * Returns the host surface handle (HWND on Windows) used when the window
     * was created, or nullptr when not attached to a host surface.
     *
     * @return The attached native handle, or nullptr.
     */
    void* nativeHandle() const override;

    /** @brief Installs the sink that receives backend diagnostics.
     *
     * Every request this backend cannot serve — a rejected geometry, a dropped
     * channel, a user shader that fell back to the built-in one, an off-screen
     * target that could not be built, a pass whose content could not be
     * prepared — is reported instead of degrading silently. The sink is stored
     * and forwarded to every content slot's SceneBridge, including slots
     * created later, so a host installed sink sees the whole backend. Without a
     * sink the renderer keeps its stderr tracing and still counts diagnostics
     * (diagnosticCount()).
     *
     * @param sink Callback invoked for every diagnostic, or empty to clear.
     */
    void setDiagnosticSink(vine::graphics::DiagnosticSink sink) override;

    /** @brief Selects the shading-model preset for scene geometry.
     *
     * Forwarded by the engine before initialize(); maps onto vsg's Phong or
     * flat ShaderSet. Reserved presets (Pbr / ShadowedPhong) fall back to
     * Phong until implemented.
     *
     * @param preset Shading-model preset.
     */
    void setShaderPreset(vine::graphics::ShaderPreset preset) override;

    // ---- VSG convenience interface ----

    /** @brief Convenience: runs one full frame loop (begin/render/end/swap).
     *
     * Equivalent to calling beginFrame(), render(), endFrame() and
     * swapBuffers() in sequence.
     */
    void frame();

    /** @brief Gets the underlying vsg viewer. */
    ::vsg::ref_ptr<::vsg::Viewer> viewer() const;

    /** @brief Gets how many off-screen target graphs this backend has built.
     *
     * Diagnostic: counts every successful off-screen build (a fresh attachment
     * set or a rebuild). A target built once and then driven with a stable size
     * and clear policy must keep this flat — a count that grows with the frame
     * count is the signature of a rebuild loop (a size or depth-policy mismatch
     * re-entering the build path every frame, which tears the graph down,
     * waits for the device and recompiles the whole graph).
     *
     * @return Number of off-screen target builds so far.
     */
    [[nodiscard]] std::size_t offscreenBuildCount() const noexcept;

    /** @brief Gets how many retained slot views are currently retired.
     *
     * A slot whose pass did not execute in the last submitted frame has its
     * view detached from the render graph (its data and pipelines are kept, so
     * re-enabling the pass only re-attaches). This counts those detached views:
     * it is 0 while every registered pass draws, and rises as passes are
     * disabled / unregistered. Diagnostic for "why is nothing drawing?" and the
     * regression check of the retirement path.
     *
     * @return Number of slot views currently detached.
     */
    [[nodiscard]] std::size_t detachedSlotCount() const noexcept;

    /** @brief Gets how many fullscreen-program slots this backend has built.
     *
     * Diagnostic: counts every successful build / rebuild of a retained
     * fullscreen-program slot (drawScreenProgram). A slot is rebuilt when its
     * sampled source, destination size or program changes — and, since the
     * slot's identity includes the program's CONTENT revision, also when the
     * program object is edited in place (ShaderProgram::replaceStages /
     * setStage). This makes a hot-reload observable, which a pointer-only
     * identity could not: it kept drawing the old SPIR-V. A steady scene must
     * keep this flat.
     *
     * @return Number of fullscreen-program slot builds so far.
     */
    [[nodiscard]] std::size_t programSlotBuildCount() const noexcept;

    /** @brief Gets how many device-wide idles this backend has taken.
     *
     * Diagnostic with an invariant behind it: NO frame-assembly path may stop the
     * device any more — a replaced render pass / framebuffer, a dropped
     * fullscreen-program node, a slot being torn down, a target being rebuilt and
     * a bridge dropping its state wrappers all PARK their objects in the retire
     * ring instead. So this stays 0, and a check that changes a pass' clear and
     * depth policy, a pass' activity, its depth MODE and a target's attachment
     * shape every frame asserts that it does (VsgRetireRing::waitForIdle is the counted
     * entry point a future teardown that cannot park would have to use).
     *
     * @return Number of device-wide idles taken so far.
     */
    [[nodiscard]] std::size_t deviceWaitCount() const noexcept;

    /** @brief Gets how many parked objects the retire ring has released.
     *
     * Diagnostic: a replaced render pass / framebuffer or a dropped
     * fullscreen-program node is parked for a few frame advances and then
     * released (VsgRendererState::retireObject / advanceRetireRing). A ring that never
     * released would grow without bound, so a policy-changing check asserts this
     * advances — and, together with the validation-clean run, is what shows the
     * deferral is live rather than merely silent.
     *
     * @return Number of objects released by the ring so far.
     */
    [[nodiscard]] std::size_t retiredObjectCount() const noexcept;

  private:

    /** @brief Retires (detaches) the retained view of every pass that was not
     * announced this frame (disabled / unregistered).
     *
     * Runs once per submitted frame once the pass protocol has been used at
     * all: a slot whose pass did not execute this frame would otherwise keep
     * drawing its last synced content, i.e. the pass would appear to ignore
     * RenderPass::setEnabled(). The slot keeps its data and pipelines, so
     * re-enabling the pass only re-attaches the view. Already-retired slots are
     * skipped, so a pass that stays disabled costs nothing per frame.
     */
    void retireInactivePassSlots();

    /** @brief Releases the off-screen targets the host dropped without announcing it.
     *
     * The target table owns every entry it holds (VsgRenderTargetEntry::owner), so once the host's last
     * reference is gone nothing can ever look that entry up again — the same rule the
     * geometry and material caches follow. Releasing them here keeps their attachments,
     * render graph and compiled pipelines from living until the session ends. Engine targets
     * are normally released by releaseRenderTarget(); this is the safety net for a host that
     * drops the RenderTarget object itself.
     *
     * Safe to call at any frame boundary: it sweeps what the engine's own
     * releaseRenderTarget() would have, and calling it again is a no-op when nothing is
     * abandoned.
     */
    void releaseAbandonedTargets();

    /** @brief Reports which device this session runs on — once, on the first submit.
     *
     * "The gate passed" is only meaningful together with the driver it passed on: a software
     * rasteriser and a real GPU exercise different paths. The first submit is where the
     * window's device and swapchain exist (vsg creates them lazily, on first use).
     *
     * Reports once per session (the flag is checked and set here), so a later call is a no-op
     * and cannot duplicate the line.
     */
    void reportSessionDevice();

    /** @brief What has to happen once a frame HAS been submitted, in this order.
     *
     * 1. A pass that preserved depth on a target whose depth image was in a transitional
     *    layout (UNDEFINED, or still PROMOTED to SHADER_READ_ONLY) recorded the variant that
     *    consumes that layout, for this frame only. Swapping it earlier would make the pass'
     *    very first frame record a LOAD against an image that has not made that transition
     *    yet. Swapping the graph's render pass (not the framebuffer) is legal because the
     *    variants differ only in the depth load-op and the depth attachment's initial layout,
     *    i.e. they are render-pass compatible.
     * 2. The objects parked kRetireRingDepth frames ago can go: one frame has been submitted,
     *    so every command-buffer slot that could still reference them has been re-recorded —
     *    the scenes' retained nodes (one ring per content slot, see SceneBridge::retireNode)
     *    and the renderer-owned objects the frame assembly parked rather than stopping the
     *    device (a pass' replaced render pass / framebuffer, a dropped fullscreen-program
     *    node — see VsgRendererState::retireObject). Both are a @ref VsgRetireRing.
     *
     * Both steps are keyed on the SAME event, which is why they share one entry point: a
     * frame has been presented. Advancing a ring twice in one frame would release objects
     * one frame too early, and settling variants before the submit would corrupt the frame
     * being recorded — so the pair is applied together or not at all.
     *
     * @pre The frame has been submitted (recordAndSubmit() + present()).
     */
    void settleSubmittedFrame();

    /** @brief Resets the queued per-pass state (target / viewport / lights /
     * depth policy / pass order / presenting marker).
     *
     * Called when a pass scope opens (so a scope never inherits the previous
     * pass' pending state) and when it closes (so state queued by a pass that
     * drew nothing cannot leak into the next pass).
     */
    /** @brief Drops the per-pass request (scope attributes included).
     *
     * Called by endPass(): nothing a pass announced may outlive its scope, so
     * the next pass (or a direct driver) starts from an empty request.
     */
    void resetPassRequest();

    /** @brief Records and presents the frame (once, when swapBuffers is called). */
    void submitFrame();

    /** @brief Consumes the sub-viewport queued by setViewport() for one pass.
     *
     * Every draw path (main scene, PiP screen, fullscreen program) reads the
     * same pending rectangle and clears it, so the consume is factored here.
     * A pass that never queued a viewport gets std::nullopt and the caller
     * substitutes the full target.
     *
     * @return The queued rectangle, or std::nullopt when the pass queued none
     *         (it then draws the full target).
     */
    /** @brief Takes the sub-viewport queued for the next draw call.
     *
     * @return The queued viewport, or empty when the caller announced none.
     */
    [[nodiscard]] std::optional<vine::graphics::Viewport> takeRequestViewport();

  private:
    /** @brief Delivers one report to the SDK's channel (the route's downstream).
     *
     * A member rather than a lambda in the constructor: `reportDiagnostic` is a PROTECTED
     * member of the base, and a lambda's closure type is not a member of this class, so it
     * cannot call it through `this`.
     *
     * @param diagnostic Report to hand to the host's sink and to the SDK's counters.
     */
    void deliverToSdkChannel(const vine::graphics::RenderDiagnostic& diagnostic);

    // The diagnostic route (cross-session: the host installs its sink once), then the
    // renderer-lifetime services and the window session (see VsgRendererState.hpp: the split
    // is by LIFETIME, and shutdown() replaces the session in one assignment).
    VsgDiagnostics        diagnostics;
    VsgRendererPersistent persistent;
    VsgRendererState      state;
};

V_VSG_NS_END
