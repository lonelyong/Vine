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
#include <vine/vsg/VsgRetentionStats.hpp>

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

    /** @brief Begins a frame (advance + handle events).
     *
     * Also opens the frame's commit token (see @ref FrameCommit): exactly one is minted here and the
     * one submitFrame() consumes it, so a second swapBuffers() without a new beginFrame() cannot
     * advance the deferral rings twice.
     */
    void beginFrame() override;

    /** @brief Ends a frame (viewer update). */
    void endFrame() override;

    /** @brief Sets the render target for the pass scope (a scope attribute).
     *
     * nullptr is the window: the shared swapchain graph built in initialize().
     * A non-null target is supported (see supportsRenderTargets) and the
     * backend owns its GPU attachments, which it creates on the target's first
     * bind and rebuilds whenever the target's size changes — the RenderTarget
     * itself stays a logical description. A pass whose off-screen target cannot
     * be drawn (no camera on the pass, an invalid target, or one with neither a
     * colour nor a depth attachment) draws nothing.
     *
     * @param target Render target, or nullptr for the default framebuffer.
     */
    void setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target) override;

    /** @brief Opens a pass scope for the pass the engine is about to execute.
     *
     * The announced pass is the identity of the GPU state this backend
     * retains for it (content view + scene bridge, full-screen-program
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
     * True between beginPass() and endPass(); a drawing call made while it is false is refused (see
     * refuseNoPassAnnounced).
     *
     * The scope flag is this backend's own state and nothing in the engine asks for it, so it is a
     * method of THIS class rather than of the RenderBackend interface: the protocol test asserts the
     * scope it opens and closes, and no backend that keeps no per-pass state has to answer for it.
     *
     * @return true while a pass scope is open.
     */
    bool isPassScopeOpen() const;

    /** @brief Releases every GPU resource this backend retains for a pass.
     *
     * Detaches and drops the pass' content view (window or off-screen), its
     * full-screen-program slot, its per-pass scene-bridge cache and the
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

    /** @brief Draws a full-screen pass through a fragment program, sampling the source's attachments.
     *
     * This is the ONLY full-screen draw: a copy of a sub-rectangle, a deferred lighting pass
     * and a host post-process are the same call with different programs and viewports (see
     * RenderBackend::drawScreenProgram for the contract). The backend
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
     * @param source  Target whose colour attachments are sampled (binding i = attachment i).
     * @param program Fragment-stage program to draw with (screenCopyProgram for a plain copy).
     * @param camera  Camera whose view transforms the pushed lights.
     */
    void drawScreenProgram(vine::graphics::RenderTarget*                       source,
                           vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                           vine::raw_ptr<const vine::graphics::Camera>        camera) override;

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

    /** @brief Frees GPU state (offscreen pass graphs + full-screen program slots) for a removed target.
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
     * @param why        Receives why the read did not happen (Ok when it did), or null to
     *                   ignore (see RenderBackend::readColorBuffer).
     * @return true when the pixels were read; false when the target was never
     *         built, the attachment is out of range or its format is not RGBA8.
     */
    bool readColorBuffer(const vine::graphics::RenderTarget* target, int attachment,
                         std::vector<std::uint8_t>& outPixels,
                         vine::graphics::ReadbackResult* why = nullptr) override;

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
     * @param why       Receives why the read did not happen (Ok when it did), or null to
     *                  ignore (see RenderBackend::readDepthBuffer).
     * @return true when the depth values were read; false when the target was
     *         never built, has no depth attachment or uses a packed format. A
     *         target that borrows its depth (RenderTarget::shareDepth) reports
     *         false: read the source target's depth instead.
     */
    bool readDepthBuffer(const vine::graphics::RenderTarget* target, std::vector<float>& outDepths,
                         vine::graphics::ReadbackResult* why = nullptr) override;

    /** @brief Renders the current frame from the render command stream.
     *
     * The retained vsg scene is reconciled against the commands (SceneBridge)
     * and (re)compiled when the set of drawables changed structurally.
     *
     * @param commands Render commands for this frame.
     * @param camera   Camera used for view/projection.
     */
    void render(const std::vector<vine::graphics::RenderCommand>& commands, vine::raw_ptr<const vine::graphics::Camera> camera) override;

    /** @brief Announces the targets the pass about to execute declares as its INPUTS.
     *
     * Called by RenderPass::execute() from the pass' own declaration before it draws. The list is
     * borrowed for the scope and read by the slot when it (re)builds its retained state — that is
     * how a pass that declares a shadow map gets it bound (see detail::resolveShadowInput, the one
     * rule both shadow consumers use).
     *
     * @param inputs Targets the pass declares (its own order decides which one is the shadow).
     */
    void setPassInputs(const std::vector<vine::raw_ptr<vine::graphics::RenderTarget>>& inputs) override;

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

    /** @brief Announces how this pass' target is cleared before it draws (see
     * RenderBackend::setClearPolicy).
     *
     * @param policy Colour of attachment 0 and whether depth is cleared too.
     */
    void setClearPolicy(const vine::graphics::ClearPolicy& policy) override;

    /** @brief Sets how the next render()'s content handles depth (see
     * RenderBackend::setDepthMode).
     *
     * @param mode Depth handling for the next render() content.
     */
    void setDepthMode(vine::graphics::DepthMode mode) override;

    /** @brief Presents the rendered frame. */
    void swapBuffers() override;

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

    /** @brief Sets the program content without its own program is shaded with.
     *
     * A content set is built once per program (the window's with this call, an off-screen target's
     * with its first slot), so a switch on a running session re-bakes the shading side: the window
     * sets are rebuilt for @p program and every content slot is dropped so the next frame's passes
     * build theirs again — a slot's set, its light wiring and its View features are all decided at
     * slot build (see resetContentShaderSlots). Attachments, pass graphs and depth history are
     * untouched. A null @p program means "no default content program": program-less content is reported and
     * skipped rather than shaded with a guess.
     *
     * @param program Program to shade program-less content with, or null for none.
     */
    void setDefaultContentProgram(vine::intrusive_ptr<const vine::graphics::ShaderProgram> program) override;

    // ---- VSG diagnostics interface ----

    // The frame is driven through the RenderBackend interface (beginFrame / per-pass calls / endFrame /
    // swapBuffers) and nothing else: this class used to offer a convenience frame() and a viewer()
    // accessor, neither of which had a caller in the repository, and frame() documented a call
    // sequence it did not perform.

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
     * Diagnostic: a replaced render pass / framebuffer, a dropped fullscreen-program node and a
     * dropped content-slot node are PARKED for a few frame advances and then released (see
     * VsgRetireRing — park() is the only way in, and its three users are named on the type). A ring
     * that never released would grow without bound, so a policy-changing check asserts this
     * advances — and, together with the validation-clean run, is what shows the
     * deferral is live rather than merely silent.
     *
     * @return Number of objects released by the ring so far.
     */
    [[nodiscard]] std::size_t retiredObjectCount() const noexcept;

    /** @brief Gets the session's retention picture: what is held back, and how the deferral behaves.
     *
     * One value, because the pieces are only meaningful together: a slot pool whose capacity climbs
     * while its reserved count does not is a leak, and a compile-context count that grows while the
     * content slots do not is retention vsg gives us no way to release (see VsgRetentionStats).
     * Read as a series, it is what answers "is the backend holding more than the scene needs?" --
     * the question that otherwise gets reassembled by hand from six counters.
     *
     * @return The session's retention counters (see VsgRetentionStats for each field's meaning).
     */
    [[nodiscard]] VsgRetentionStats retentionStats() const noexcept;

  private:

    /** @brief The pass-protocol rules this backend can be misused about (see reportPassMisuse).
     *
     * The rules are the HOST's to keep, so they cannot be made unrepresentable from this side — what
     * can be unified is where breaking one is reported: this is the complete list, each entry naming
     * the rule it breaks.
     */
    enum class PassMisuse
    {
        NestedScope,               ///< beginPass() while a scope was open: the open pass' request was dropped.
        UnpairedEnd,               ///< endPass() with no open scope: the announced request was already dropped.
        CallOutsideScope,          ///< A drawing call was made while no pass was announced (no beginPass before it).
        ReleasedTargetAnnouncement ///< A call still named a render target that was released while announced.
    };

    /** @brief Reports one misuse of the pass protocol (the ONE place that does).
     *
     * One severity, one category and one message per rule, so a caller learns which rule it broke and
     * how to fix it — and so a new rule is a new case here instead of another hand-written triple at
     * a refusal point. The rules that CAN be made unrepresentable are not reported but enforced where
     * they are decided: the frame's commit token (see @ref FrameCommit) and the refusal of a call
     * whose announced target is gone (see refuseDeadTargetAnnouncement).
     *
     * @param misuse Which rule was broken.
     * @param call   Entry point whose work was refused or skipped, for the rules that refuse a call,
     *               so the host knows which of render() / clear() / drawScreenProgram() it was;
     *               unused by the scope rules.
     */
    void reportPassMisuse(PassMisuse misuse, const char* call = nullptr);

    /** @brief Retires (detaches) the retained view of every pass that was not
     * announced this frame (disabled / unregistered).
     *
     * Runs once per submitted frame: a slot whose pass did not execute this frame would otherwise
     * keep drawing its last synced content, i.e. the pass would appear to ignore
     * RenderPass::setEnabled(). The slot keeps its data and pipelines, so
     * re-enabling the pass only re-attaches the view. Already-retired slots are
     * skipped, so a pass that stays disabled costs nothing per frame.
     */
    void retireInactivePassSlots();

    /** @brief Refuses a drawing call that no pass scope opened.
     *
     * The pass announced by beginPass() is the identity of everything this backend retains and the
     * scope is what makes "which call means what" independent of the call order, so a drawing call
     * with no pass behind it has nothing to belong to: it is skipped instead of being served with
     * state no pass announced (a stale target / order from whenever a scope last ran, or the
     * defaults). The state setters are deliberately NOT guarded here — they are inert on their own
     * (the next beginPass() starts from an empty request), while a draw that reached the device
     * would be content nobody asked for.
     *
     * The report is an EPISODE: one message per frame (the next beginFrame() re-arms it), so a host
     * looping on such a call is told once.
     *
     * @param call Name of the entry point refusing the call (one message per call name, so the host
     *             knows which of render() / clear() / drawScreenProgram() was skipped).
     * @return true when the caller must skip the call.
     */
    [[nodiscard]] bool refuseNoPassAnnounced(const char* call);

    /** @brief Refuses a call whose announced target is dead.
     *
     * A pass is borrowed for its scope (RenderBackend::beginPass), so
     * RenderBackend::releaseRenderTarget has to drop an announcement naming the released
     * target: the host releases it because its last owner is going away, and the class contract
     * forbids keeping such a pointer. A call that would still use it cannot be honoured — drawing
     * into the window instead would put the content somewhere the caller never asked for — so the
     * call is skipped and the caller is told once, with the fix (announce the target again, or
     * nullptr for the window).
     *
     * The report is an EPISODE, and the episode is the rest of this scope: it fires on the first
     * call that needed the dead announcement and re-arms when the caller announces a target again
     * (a new scope starts from an empty request), so a pass that keeps drawing without
     * re-announcing says so once.
     *
     * @param call Name of the entry point refusing the call (one message per call, so the host
     *             knows which of render() / clear() / drawScreen*() was skipped).
     * @return true when the caller must skip the call.
     */
    [[nodiscard]] bool refuseDeadTargetAnnouncement(const char* call);

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
     *    the scenes' retained nodes (one ring per content slot, see SceneBridge::retireNode),
     *    and the renderer-owned objects the frame assembly parked rather than stopping the
     *    device (a pass' replaced render pass / framebuffer, a dropped fullscreen-program
     *    node — see VsgRetireRing::park). All three run on one clock (@ref
     *    VsgDeferredRelease): the rings, and the per-draw slots the pool retired
     *    (VsgDrawBlockPool::advanceRetired).
     *
     * All three advances are keyed on the SAME event, which is why they share one entry point: a
     * frame has been committed. Advancing a ring twice in one frame would release objects
     * one frame too early, and settling variants before the submit would corrupt the frame
     * being recorded — so they are applied together or not at all.
     *
     * @param commit The token beginFrame() minted for this frame (see @ref FrameCommit): the
     *               advance is only legal after the submit, and the type says so.
     */
    void settleSubmittedFrame(FrameCommit commit);

    /** @brief Counts every cache's retained shares into the frame's ownership picture.
     *
     * A cache can only tell "the app has let go of this object" from "another cache still holds
     * it" by counting every retained entry that holds it (see OwnedShareCounts). Two moments need
     * the count and both need it FRESH: the frame's start, so every content slot's sync judges by
     * one picture (SceneBridge::syncRenderCommands is handed it), and the frame's end just before
     * the sweeps (releaseAbandonedContent()), so a slot dropped during the frame cannot leave it
     * over-counted. Both passes are O(entries now), never O(entries ever seen).
     *
     * @pre The session is initialized (the target tables are the session's).
     */
    void collectFrameShares();

    /** @brief Releases what the app has let go of, judged by counts collected for THIS moment.
     *
     * The frame's end point for the caches that need no per-slot pass of their own: every slot's
     * abandoned geometries (including the slots whose pass did not run at all, whose caches no
     * sync of theirs swept) and the material manager's abandoned materials. A material is also
     * held by the variant template of every slot that draws it, so that cache's own shares alone
     * never say "the app dropped it" — the session counts break that mutual wait (P11).
     *
     * The counts are COLLECTED HERE, immediately before the sweeps, and that is deliberate: a slot
     * dropped while the frame was open (an offscreen target rebuilt at a new size, a pass
     * retargeted, a target released) took its entries — and the shares they held — with it. A
     * picture taken at the frame's start still counts them, and that over-count reports "the app
     * let go" for an object the app still holds, so the sweep would release an entry the rule says
     * to keep. Fusing the two makes judging by a stale picture inexpressible rather than a rule to
     * remember.
     *
     * @pre The frame has been submitted (settleSubmittedFrame() has run), so nothing will draw
     *      the retained state this releases.
     */
    void releaseAbandonedContent();

    /** @brief Drops the per-pass request (scope attributes included).
     *
     * Called when a pass scope opens (so a scope never inherits the previous pass'
     * pending state) and by endPass() (so nothing a pass announced may outlive its
     * scope, and the next pass starts from an empty request).
     */
    void resetPassRequest();

    /** @brief Records and presents the frame (once, when swapBuffers is called).
     *
     * Refuses a call that has no frame open: the deferral rings advance on the committed-frame
     * clock, so a second submit of the same frame would release what they parked a frame too early,
     * while a command buffer the GPU may still execute names it (see @ref FrameCommit).
     */
    void submitFrame();

    /** @brief Takes the sub-viewport queued for the next draw call.
     *
     * Every draw path (the main scene, the full-screen program) reads the
     * same pending rectangle and clears it, so the consume is factored here.
     * A pass that never queued a viewport gets std::nullopt and the caller
     * substitutes the full target.
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
