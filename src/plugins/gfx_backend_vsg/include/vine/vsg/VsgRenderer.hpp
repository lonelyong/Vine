#pragma once

// The renderer's definition: the class, its session state and its private helpers in
// one plugin-internal header. It used to be TWO headers with the class hidden behind a
// PImpl, which bought nothing here — the host only ever sees the graphics SDK interface
// (vine::graphics::RenderBackend), and the plugin's own translation units were the only
// consumers of the renderer header — while costing a helper-placement rule on every
// refactor (a helper needing a state type could not be declared in the "public" header)
// and a pointer indirection on every state access.
//
// The state is still split in two, but BY VALUE and for a lifetime reason rather than a
// compilation one:
//
//   * Persistent — outlives every window session (the material manager contract), and
//   * Impl — one window session: everything that references a vsg::Window / vsg::Device,
//     replaced wholesale on shutdown()/initialize() so a session-scoped resource can
//     never be left behind by a manual teardown list.
//
// (The reasoning the deleted public header carried is in .ai/design/vsg-pass-lifecycle.md
// §44.)

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
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include <vine/vsg/VsgPipelineFactory.hpp>


#include <vine/vsg/vsg_global.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/raw_ptr.hpp>
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
     * shape every frame asserts that it does (Impl::waitForIdle is the counted
     * entry point a future teardown that cannot park would have to use).
     *
     * @return Number of device-wide idles taken so far.
     */
    [[nodiscard]] std::size_t deviceWaitCount() const noexcept;

    /** @brief Gets how many parked objects the retire ring has released.
     *
     * Diagnostic: a replaced render pass / framebuffer or a dropped
     * fullscreen-program node is parked for a few frame advances and then
     * released (Impl::retireObject / advanceRetireRing). A ring that never
     * released would grow without bound, so a policy-changing check asserts this
     * advances — and, together with the validation-clean run, is what shows the
     * deferral is live rather than merely silent.
     *
     * @return Number of objects released by the ring so far.
     */
    [[nodiscard]] std::size_t retiredObjectCount() const noexcept;

  private:
    /** @brief Identity of one retained backend slot.
     *
     * Primary identity: @ref owner, the pass that draws the slot (announced
     * via beginPass). Two passes therefore never alias each other even when
     * they share a camera and a pass order, and the retained state follows the
     * pass when its camera / render target / program changes.
     *
     * A direct backend driver that skips the pass protocol (no beginPass, as
     * the device self-test does) keeps the historical identity instead, carried
     * by @ref scope / @ref index: camera + pass order for a content slot,
     * sampled target + attachment for a picture-in-picture slot, sampled target
     * for a fullscreen-program slot. The two identity schemes never mix in one
     * slot map because a pass-scoped key leaves scope / index at their
     * defaults.
     */
    struct SlotKey
    {
        const vine::graphics::RenderPass* owner = nullptr; ///< Pass that owns the slot, or null (direct driver).
        const void*                       scope = nullptr; ///< Fallback identity: camera / sampled target.
        int                               index = 0;       ///< Fallback identity: pass order / attachment index.

        bool operator<(const SlotKey& o) const noexcept
        {
            if (owner != o.owner) return owner < o.owner;
            if (scope != o.scope) return scope < o.scope;
            return index < o.index;
        }

        /** @brief Key of the slot owned by a pass. */
        static SlotKey ownerPass(const vine::graphics::RenderPass* pass) noexcept
        {
            return SlotKey{ pass, nullptr, 0 };
        }

        /** @brief Fallback key of a content slot: (camera, pass order). */
        static SlotKey cameraOrder(const vine::graphics::Camera* camera, int order) noexcept
        {
            return SlotKey{ nullptr, camera, order };
        }

        /** @brief Fallback key of a PiP screen slot: (sampled target, attachment). */
        static SlotKey sampledTarget(const vine::graphics::RenderTarget* source, int attachment) noexcept
        {
            return SlotKey{ nullptr, source, attachment };
        }

        /** @brief Fallback key of a fullscreen-program slot: its sampled target. */
        static SlotKey sampledTarget(const vine::graphics::RenderTarget* source) noexcept
        {
            return SlotKey{ nullptr, source, 0 };
        }
    };
    /** @brief Builds (or rebuilds) an off-screen target's GPU attachments and
     * empty render graph, sized to the target.
     *
     * Called from render() when an off-screen target is first rendered or was
     * resized. The graph is created without Views; content-slot Views are
     * appended by setupContentSlot() as passes render into the target (C6.4:
     * the same multi-slot mechanism the window target uses, so one RT can
     * bake several content groups with different programs / depth policy). A
     * rebuild first releases the previous graph and every content slot
     * compiled against it. EXPERIMENTAL: needs on-device validation.
     *
     * @param target Off-screen target to (re)build.
     */
    void buildOffscreenTarget(vine::graphics::RenderTarget* target);

    /** @brief Keeps the command graph's off-screen render graphs in a
     * dependency-valid record order.
     *
     * A consumer's graph is ordered after every target it samples (its PiP
     * screen / fullscreen-program sources), so a same-frame producer chain
     * (A -> B -> window) samples the CURRENT frame; the window swapchain graph
     * stays the last child. Called whenever an off-screen graph is (re)built
     * or a sampling slot is newly attached / dropped — creation order alone cannot
     * guarantee the dependency order (a producer rebuilt after its consumers
     * existed, or a consumer wired to a producer built later would otherwise
     * record the consumer first and sample stale / undefined content).
     */
    void reconcileOffscreenOrder();

    /** @brief Detaches and drops every retained slot a pass owns under one
     * target (its content view, PiP screen slot and fullscreen-program slot).
     *
     * Used when a pass' retained state must be discarded: the pass is removed
     * (releasePass), it was not active this frame (retireInactivePassSlots) or it
     * moved to another target (retargetPass). The owning graph's child list is
     * swept first, then the device is waited on so no in-flight command buffer
     * still references the dropped view / pipelines.
     *
     * @param target Target key whose slots to inspect (nullptr = the window).
     * @param pass   Pass whose slots to drop.
     */
    void erasePassSlotsFromTarget(vine::graphics::RenderTarget* target,
                                  const vine::graphics::RenderPass* pass);

    /** @brief Drops the fullscreen-program slots that BIND @p target's depth.
     *
     * Called when a pass of @p target revokes its depth promotion: a program slot
     * built before that point was built while the promotion stood, so its
     * descriptor set names the promoted layout, and recording it in the same frame
     * would name a layout the image is no longer in. The slot is dropped (and
     * reported) for that frame; the owner's next drawScreenProgram call rebuilds
     * it, without the depth binding — or refuses the program when it needs it.
     *
     * @param target Target whose promotion was just revoked.
     */
    void dropDepthSamplingProgramSlots(vine::graphics::RenderTarget* target);

    /** @brief Decides whether a target's shared depth can be borrowed.
     *
     * RenderTarget::shareDepth points a target at another target's depth image so
     * forward content can test against a G-buffer's depth. That is only possible
     * when the source's image is usable as THIS framebuffer's depth attachment as
     * it stands; every other case is reported (once per episode) and the target
     * builds its own depth instead of attaching an unusable image.
     *
     * @param target Target requesting the borrow.
     * @param w      Width the framebuffer is being built with.
     * @param h      Height the framebuffer is being built with.
     * @return true when the target must attach its source's depth image.
     */
    bool resolveDepthBorrow(vine::graphics::RenderTarget& target, uint32_t w, uint32_t h);

    /** @brief What an overlay draw (PiP / fullscreen program) records into.
     *
     * Every overlay draw resolves the same destination: the target the pass is
     * bound to (setRenderTarget; nullptr = the window), the graph its view must
     * record into, and the surface size its rectangle is expressed in. The two
     * overlay kinds differ in WHAT they draw and in how they fit a rectangle —
     * never in where they draw, which is why the rules live in one place (see
     * resolveOverlayDestination).
     */
    struct OverlayDestination {
        vine::graphics::RenderTarget*      target = nullptr; ///< Destination target (nullptr = the window).
        ::vsg::ref_ptr<::vsg::RenderGraph> graph;            ///< Graph the view records into; null = refuse to draw.
        int                                surf_w = 0;        ///< Surface width the rectangle is clamped to.
        int                                surf_h = 0;        ///< Surface height the rectangle is clamped to.
    };

    /** @brief Resolves and prepares the destination of an overlay draw.
     *
     * Rejects the source == destination feedback loop (sampling the very
     * attachments the pass writes), refuses a destination without a usable
     * colour attachment, (re)builds an off-screen destination whose size changed,
     * and hands back the graph the draw records into. Also re-targets the pass:
     * a pass that drew into another target before drops the slot it left there,
     * so it stops compositing into it.
     *
     * @param source Sampled target (also what the feedback loop is checked against).
     * @param key    Slot key of the draw (identifies the graph of an off-screen pass).
     * @param what   Draw name for the diagnostics ("drawScreenTexture" / "drawScreenProgram").
     * @return The destination; @c graph is null when the draw must not record.
     */
    OverlayDestination resolveOverlayDestination(vine::graphics::RenderTarget* source, const SlotKey& key,
                                                 const char* what);

    /** @brief Places an overlay view in its destination's graph by its explicit order.
     *
     * The view is already compiled (against the destination's render pass), so this
     * only changes the RECORD order — the stacking position among the target's other
     * slot views (see placeViewByOrder). A view that (re)starts recording under an
     * off-screen destination also puts that pass' graph back into the command graph,
     * which is why the off-screen order is reconciled here.
     *
     * @param dest  Resolved destination of the draw.
     * @param view  The slot's retained view (compiled).
     * @param order The pass' explicit pipeline order.
     */
    void placeOverlayView(const OverlayDestination& dest, const ::vsg::ref_ptr<::vsg::View>& view, int order);

    /** @brief Builds the View an overlay drawable records through and installs it in its slot.
     *
     * Shared by the PiP screen triangle and the fullscreen program: wrap @p content in
     * its own View (own camera + the sub-rect viewport), compile it against the
     * destination's render pass, then hand it to the slot and position it by the slot's
     * explicit order. A compile failure reports (when it was the compile) and returns
     * false, and the caller drops its slot so the next frame retries — the half-compiled
     * view is never recorded.
     *
     * @tparam Slot    Screen / program slot type (both carry camera / view / order / ready).
     * @param dest     Resolved destination of the draw.
     * @param slot     Slot to install into (its @c order positions the view).
     * @param content  The drawable the view wraps.
     * @param x        Viewport origin x in device pixels.
     * @param y        Viewport origin y in device pixels.
     * @param w        Viewport width in device pixels.
     * @param h        Viewport height in device pixels.
     * @param front    Insert the view as the graph's FIRST child until it is ordered.
     * @param what     Draw name for the compile-failure diagnostic.
     * @return true when the slot now holds a compiled, placed view.
     */
    template <class Slot>
    bool installOverlayView(const OverlayDestination& dest, Slot& slot, const ::vsg::ref_ptr<::vsg::Node>& content, int x,
                            int y, int w, int h, bool front, const char* what);

    /** @brief Restricts a pass to @p target by dropping its slots elsewhere.
     *
     * A pass owns exactly one retained slot per target; when a pass renders
     * into a different target than before (its render target changed at run
     * time) the slot it left behind would otherwise keep drawing its content
     * forever. @p target may be nullptr (the window target), so callers pass
     * the target the pass is (re)drawing into.
     *
     * @param pass   Pass being re-targeted.
     * @param target Target the pass now renders into (nullptr = window).
     */
    void retargetPass(const vine::graphics::RenderPass* pass,
                      vine::graphics::RenderTarget*     target);

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
     * The target table owns every entry it holds (Target::owner), so once the host's last
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

    /** @brief Compiles the views this frame's slot syncs queued, before anything is recorded.
     *
     * D22 incremental compile, ON by default (see incrementalCompileViews for why vsg's own
     * compile path is not wired up in this renderer). VINE_VSG_DISABLE_INCREMENTAL_COMPILE
     * forces the full-graph compile as an A/B escape hatch, and any incremental failure falls
     * back to it automatically. A failed compile is reported; the frame still submits, because
     * an acquired swapchain image has to be presented.
     *
     * @pre Nothing of this frame has been recorded yet — the whole point is that the new
     *      subtrees carry their compiled pipelines when the record happens.
     * @post The pending queue is empty (a frame that never reaches here keeps it for the next
     *       submit, since nothing was presented in between).
     */
    void compilePendingViews();

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
     *    node — see Impl::retireObject).
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

    /** @brief Builds (on first use) the retained vsg view for a content slot.
     *
     * A content slot is a View of a TARGET's render graph — the window target
     * (@p target == nullptr) and every off-screen target share this one
     * mechanism. The slot belongs to the pass that draws it (see SlotKey), and
     * its view is stacked by that pass' explicit @p order — the order the
     * caller gave addPass() — so a slot created early (pre-frame warm-up) still
     * lands at its pipeline position. @p depth_mode is the content's depth
     * handling (explicit per pass, independent of clearing); @p presenting
     * marks the full-target pass that cleared the target (its default light
     * seeds the window headlight when there is no scene light). Lights come
     * from the content scene each frame.
     *
     * @param key        Slot identity (the owning pass, or the legacy fallback key).
     * @param target     Output target key the slot lives under (nullptr = window).
     * @param camera     Vine camera identifying the slot (borrowed, read-only).
     * @param order      The pass's explicit pipeline order (stacking position).
     * @param depth_mode Depth handling for the slot's content.
     * @param presenting True when this slot is the full-target pass that cleared.
     */
    void setupContentSlot(const SlotKey& key,
                          vine::graphics::RenderTarget* target,
                          vine::raw_ptr<const vine::graphics::Camera> camera,
                          int order,
                          vine::graphics::DepthMode depth_mode,
                          bool presenting);

    /** @brief One content-slot draw request.
     *
     * Groups what one render() call contributes to a content slot, so the draw
     * path takes one self-describing argument instead of a long positional
     * parameter list (four adjacent ints in particular were easy to swap by
     * mistake). Both collection pointers are borrowed for the call.
     */
    struct ContentSlotRequest
    {
        vine::graphics::RenderTarget*                  target   = nullptr; ///< Target key (nullptr = window).
        vine::raw_ptr<const vine::graphics::Camera>    camera   = nullptr; ///< Camera identifying the slot.
        const std::vector<vine::graphics::RenderCommand>* commands = nullptr; ///< Commands to reconcile (borrowed).
        const std::vector<const vine::graphics::Light*>* lights  = nullptr; ///< Content lights (borrowed, may be empty).
        vine::graphics::DepthMode                      depth_mode = vine::graphics::DepthMode::TestAndWrite; ///< The pass' depth policy.
        bool                                           presenting = false; ///< True for the full-target pass that cleared.
        bool                                           clear_depth = false; ///< The pass' own depth-clear request (a mixed target clears it per pass).
        int                                            order      = 0;     ///< The pass' explicit pipeline order (stacking).
        std::optional<vine::graphics::Viewport>        viewport;  ///< Sub-viewport, or nullopt for the full target.
    };

    /** @brief Renders one content slot (a View of a target's render graph).
     *
     * The slot is owned by the pass that draws it (@ref SlotKey) and its view
     * is stacked by that pass' explicit pipeline order, so several passes
     * sharing one camera and one order stay separate content. The request's
     * depth policy is the explicit content depth handling (independent of
     * clearing; it fills the depth state of commands that did not author one),
     * and its presenting flag marks the full-target pass that cleared the
     * target (such content fills the whole target and seeds the window
     * headlight when there is no scene light). Lights come from the content
     * scene each frame.
     *
     * @param request Draw request (see ContentSlotRequest).
     */
    void renderContentSlot(const ContentSlotRequest& request);

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

    /** @brief Moves a slot View into its target graph's children at the
     * position matching its explicit stacking order.
     *
     * Every slot view under a target's render graph carries an explicit order
     * — content slots by their owning pass, fullscreen-program views
     * and PiP / present screen views by the pass order announced via
     * setPassOrder(). The children stay sorted ascending by that order, so a
     * fullscreen lighting / present view can be stacked between two content
     * slots (e.g. an opaque depth pass below it and a forward transparent pass
     * above it) instead of always drawing first or last. Any child that maps
     * to no known slot sorts as INT_MAX (drawn last).
     *
     * @param graph  The pass' render graph that receives the view — the
     *               window's swapchain graph, or an off-screen pass' own graph
     *               (see passGraph).
     * @param target Output target the graph belongs to (nullptr = the window),
     *               used to resolve each child's explicit stacking order.
     * @param view   The View to position (removed from any current position
     *               first).
     * @param order  The view's explicit stacking order.
     */
    void placeViewByOrder(::vsg::ref_ptr<::vsg::RenderGraph> graph,
                          vine::graphics::RenderTarget* target,
                          const ::vsg::ref_ptr<::vsg::View>& view,
                          int order);

    /** @brief Gets the render graph a pass records its views into.
     *
     * The window session has exactly ONE graph — the swapchain graph, whose
     * render pass vsg bakes to the surface — so every window pass shares it.
     * An off-screen target has one graph PER PASS instead (§28): one render
     * pass bakes ONE pair of attachment load-ops, so a pass that clears and a
     * pass that preserves cannot share a render pass. A pass' graph is created
     * on demand from that pass' own clear request (the open pass scope), which
     * is what lets both requests be honoured without a ClearAttachments hack.
     *
     * A target's pass graphs record in the passes' explicit pipeline order
     * (setPassOrder), i.e. the position the pass' content would have occupied as
     * a View of a single target-wide render pass (see reconcileOffscreenOrder).
     *
     * @param target Off-screen target, or nullptr for the window session.
     * @param key    Slot key of the pass that owns the graph.
     * @return The pass' graph, or null when the target has no attachments yet.
     */
    ::vsg::ref_ptr<::vsg::RenderGraph> passGraph(vine::graphics::RenderTarget* target, const SlotKey& key);

    /** @brief Incrementally compiles only the content-slot views that gained
     * new/rebuild subtrees this frame (D22).
     *
     * vsg compiles Vulkan objects per viewID and can only create a graphics
     * pipeline when the compiling context carries the owning target's render
     * pass (window swapchain or off-screen framebuffer). vsg's own
     * CompileManager pool is built once from the views present at first
     * Viewer::compile() — in Vine that runs on an EMPTY window graph, so the
     * pool's contexts are empty and compileManager->compile(view) silently
     * compiles nothing (and record then hits unbuilt pipelines). This method
     * instead registers each queued view's (window/framebuffer render pass +
     * view) context into the pool on first sight via the public
     * CompileManager::add() API, then compiles that view through its own
     * context only — mirroring what Viewer::compile() does for the whole
     * graph, scoped to the views that actually changed.
     *
     * @return true when every queued view was compiled incrementally, false
     *         when any step failed and the caller should fall back to a full
     *         Viewer::compile().
     */
    bool incrementalCompileViews();

    struct Persistent {
        CameraBridge                        cameraBridge;
        VsgMaterialManager                  materialManager;
        vine::graphics::ShaderPreset        shader_preset{ vine::graphics::ShaderPreset::StandardPhong };
        void*                               bound_handle = nullptr;
    };

    struct Impl {
        ::vsg::ref_ptr<::vsg::Window>       window;
        ::vsg::ref_ptr<::vsg::Viewer>       viewer;
        ::vsg::ref_ptr<::vsg::CommandGraph> command_graph;
        // Window-target shader sets shared by its content slots' bridges: one per
        // DepthMode (TestAndWrite / TestOnly / Disabled) so each slot bakes the
        // right depth test/write state. Per-geometry pipelines are compiled per
        // view (vsg compiles per viewID), so every content slot carries its own
        // SceneBridge; off-screen targets bake their own per-size sets (see Target).
        ::vsg::ref_ptr<::vsg::ShaderSet>    depth_on_shader_set;
        ::vsg::ref_ptr<::vsg::ShaderSet>    depth_testonly_shader_set;
        ::vsg::ref_ptr<::vsg::ShaderSet>    depth_off_shader_set;
        bool                                initialized = false;
        // The device report is logged once, from the first submitted frame: the
        // window's Vulkan device / swapchain only materialises when it is first
        // used, so querying it during initialize() returns nothing.
        bool                                device_reported = false;

        /** @brief Identifies one content slot.
         *
         * Slots are keyed by SlotKey: the pass announced in beginPass() owns its
         * slot, so the retained state follows the pass (its camera / render target
         * may change without orphaning it) and two passes never alias. A direct
         * driver that skips the pass protocol falls back to the historical
         * (camera, explicit pass order) identity.
         */    struct ContentSlot {
            int                           order  = 0;   // explicit pipeline order (stacking)
            vine::graphics::DepthMode     depth_mode = vine::graphics::DepthMode::TestAndWrite;
            bool                          presenting = false; // this slot cleared the target (full-target main pass)
            bool                          headlight_seed = false; // its default light is the headlight (presenting window slot)
            ::vsg::ref_ptr<::vsg::Camera> vsg_camera;
            ::vsg::ref_ptr<::vsg::Group>  root;        // retained content root
            ::vsg::ref_ptr<::vsg::Group>  light_group; // lights under this slot's view
            ::vsg::ref_ptr<::vsg::View>   view;
            SceneBridge                   bridge;      // per-view pipelines (vsg compiles per viewID)
            // D22: true once this slot's (window/framebuffer render pass + view)
            // context has been registered into the viewer's CompileManager pool
            // (incrementalCompileViews()). Each slot is registered once, so the
            // pool gains exactly one context per slot View.
            bool                          compile_context_registered = false;
            // True while this slot's view is DETACHED from its target's graph
            // because the pass did not execute in the last submitted frame (see
            // retireInactivePassSlots): the retained data / pipelines are kept, so
            // re-enabling the pass simply re-attaches the view instead of
            // re-uploading the mesh and recompiling.
            bool                          detached = false;
            bool                          ready = false;
            // True once this slot has reported that its announced lights were all
            // unusable and it therefore keeps the seeded default light (see
            // setGroupLights). Re-armed when usable lights arrive, so each episode
            // reports once instead of every frame.
            bool                          light_fallback_reported = false;
        };

        // ---- Pass scope (RenderBackend::beginPass / endPass) ----
        //
        // ONE structure holds everything the engine announced for the pass being
        // executed. It used to be a handful of separate pending_* fields that had to
        // be reset in step and were read from three different entry points
        // (render() and both drawScreen*() calls) — the shape that made "which call
        // means what" depend on the call order. Now the request IS the state:
        //
        //   * scope attributes (pass identity, target, order, depth mode,
        //     presenting) stay valid for every draw call of the scope and are
        //     dropped by endPass() — a pass that draws twice keeps its stacking
        //     position and depth policy for both calls;
        //   * per-draw-call attributes (viewport, lights) are consumed by the draw
        //     call that follows them (takeViewport / takeLights).
        //
        // A direct driver that never calls beginPass keeps using this as a plain
        // request queue: nothing is dropped until it overwrites it with the next
        // set* call (the legacy behaviour).
        struct PassRequest
        {
            /// The pass announced by beginPass() (null for a direct driver).
            const vine::graphics::RenderPass* pass = nullptr;
            /// Target announced by setRenderTarget() (null = the window).
            vine::graphics::RenderTarget* target = nullptr;
            /// Pipeline order announced by setPassOrder (stacking position).
            int order = 0;
            /// Depth handling announced by setDepthMode(); explicit per pass.
            vine::graphics::DepthMode depth_mode = vine::graphics::DepthMode::TestAndWrite;
            /// Set by clear(): this pass fills the target (the "presenting" pass
            /// that seeds the window's default headlight). Independent of depth.
            bool presenting = false;
            /// Depth-clear request of the clear() call above — a scope attribute
            /// like presenting, so every draw call of the scope keeps it. Each pass
            /// has its OWN render pass, so the request is honoured exactly for the
            /// pass that made it whatever the target's other passes asked for.
            bool clear_depth = true;
            /// Colour the clear() call above asked for. Only consumed by an
            /// off-screen pass (the window clears from the viewer's own record).
            ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
            /// Sub-viewport announced by setViewport(), per draw call.
            std::optional<vine::graphics::Viewport> viewport;
            /// Lights announced by setLights(), per draw call. Empty keeps the
            /// slot's current/default lights (RenderBackend::clearLights() drops
            /// them), so an announcement and an empty announcement are equivalent.
            std::vector<const vine::graphics::Light*> lights;
            /// Draw calls (render / drawScreen*) this request served (diagnostic).
            std::size_t draws = 0;

            /** @brief Consumes the queued sub-viewport.
             *
             * @return The viewport announced for the next draw call, or empty.
             */
            std::optional<vine::graphics::Viewport> takeViewport()
            {
                std::optional<vine::graphics::Viewport> queued = viewport;
                viewport.reset();
                return queued;
            }

            /** @brief Consumes the queued lights.
             *
             * @return The lights announced for the next draw call (empty = keep).
             */
            std::vector<const vine::graphics::Light*> takeLights()
            {
                std::vector<const vine::graphics::Light*> queued = std::move(lights);
                lights.clear();
                return queued;
            }
        };

        /// The request in progress: the open pass scope, or the direct driver's queue.
        PassRequest request;
        /// True while a beginPass() scope is open (endPass() closes it).
        bool pass_open = false;
        // Passes announced since the last submitted frame (see
        // retireInactivePassSlots): a pass that did not execute this frame is
        // retired (its view detached) rather than left drawing stale content.
        std::set<const vine::graphics::RenderPass*> passes_active_this_frame;
        // STICKY: set once any pass is announced, i.e. this backend is being driven
        // through the engine's pass protocol. It is never cleared, so that a frame
        // in which EVERY pass is disabled (nothing announced) still retires the
        // retained views instead of leaving them on screen. A direct driver that
        // never calls beginPass keeps the legacy keying and is never retired.
        bool pass_protocol_used = false;

        // Successful off-screen target builds (diagnostic; see
        // VsgRenderer::offscreenBuildCount()).
        std::size_t offscreen_build_count = 0;
        // Successful fullscreen-program slot builds (diagnostic; see
        // VsgRenderer::programSlotBuildCount()). Counted in drawScreenProgram when
        // a slot becomes ready, so a program hot-reload is observable.
        std::size_t program_slot_build_count = 0;

        // ---- Retiring replaced GPU objects without stopping the device ---------

        // Renderer-owned objects whose Vulkan handles a SUBMITTED command buffer may
        // still reference after they were replaced (a pass' render pass / framebuffer
        // swapped for another load-op variant) or dropped (a fullscreen program slot
        // whose source revoked its depth promotion mid-frame). They are parked here
        // and released kRetireRingDepth frame advances later instead of holding the
        // frame with a device-wide idle: the depth and the "advance after the submit"
        // point are the ones the per-slot node ring already established
        // (SceneBridge::retireNode) — a command-buffer slot re-records the frame, it
        // never reuses a recording, kRetireRingDepth frames after it recorded an
        // object.
        //
        // Parking is not a nicety: destroying a VkRenderPass / VkFramebuffer /
        // VkPipeline whose last recording is still pending is undefined behaviour,
        // and the validation layer does not necessarily see it (the recording that
        // references the object may be several frames old).
        std::array<std::vector<::vsg::ref_ptr<::vsg::Object>>, SceneBridge::kRetireRingDepth> retire_ring;
        std::size_t retire_head = 0;
        // Objects released by the ring so far (diagnostic; see
        // VsgRenderer::retiredObjectCount()): a ring that never released would grow
        // without bound, so the policy-churn check asserts it advances.
        std::size_t retired_object_count = 0;
        // Device-wide idles taken so far (diagnostic; see
        // VsgRenderer::deviceWaitCount()). Avoiding them on the frame-assembly paths
        // is what the ring is for, so this is the judge of that change: no
        // policy-changing frame may raise it.
        std::size_t device_wait_count = 0;

        /** @brief Parks an object until every command buffer that could reference
         * it has been re-recorded.
         *
         * @param object Object to release later (null is ignored).
         */
        void retireObject(::vsg::ref_ptr<::vsg::Object> object);

        /** @brief Releases the objects parked kRetireRingDepth frame advances ago.
         *
         * Called once per submitted frame (submitFrame), beside the per-slot node
         * rings.
         */
        void advanceRetireRing();

        /** @brief Stops the device and counts the stop (see device_wait_count).
         *
         * Used by the DESTRUCTIVE teardown paths, which are exactly the ones that drop
         * a bridge's cache: SceneBridge::clearCache() releases the shared object
         * registry, and a pipeline / sampler in it needs no retained node to own it, so
         * those paths cannot be made wait-free by parking the slot's view (measured:
         * vkDestroyPipeline-00765 / vkDestroySampler-01082). Everything else — a
         * variant swap, the promotion cascade, a dropped program slot, an inactive
         * pass' view, a depth-mode state rebuild — PARKS instead (retireObject), and
         * the policy-churn check asserts that the two kinds stay apart.
         */
        void waitForIdle();

        // Content-slot VIEWs that gained new/rebuild subtrees this frame. D22
        // incremental compile: submitFrame() recompiles ONLY these views (not the
        // whole scene). The view (not a detached subtree) is the compile unit
        // because vsg assigns the per-View viewID only while traversing the View
        // node — compiling a detached subtree always uses viewID 0 and crashes at
        // record for any other slot's viewID.
        std::vector<::vsg::ref_ptr<::vsg::View>> pending_compile_views;

        // ---- Output targets: the window (nullptr key) + off-screen (RT* key) ----

        /** @brief One picture-in-picture view sampling another target's colour
         * attachment.
         *
         * Owned by the pass that draws it (see SlotKey); the sampled source and
         * attachment are slot ATTRIBUTES compared each frame, so a pass that
         * switches its input or destination is rebuilt instead of silently
         * sampling the old texture. */
        struct ScreenSlot {
            int                              order      = std::numeric_limits<int>::max(); // stacking order (engine pass order); PiP last by default
            const vine::graphics::RenderTarget* source_target = nullptr; // sampled target the slot was built for
            int                              attachment = 0;             // sampled colour attachment
            ::vsg::ref_ptr<::vsg::Camera>    camera;      // carries the sub-rect viewport
            ::vsg::ref_ptr<::vsg::View>      view;        // extra View of this target's render graph
            ::vsg::ref_ptr<::vsg::ImageView> source_view; // keeps the sampled attachment alive
            int                              source_w = 0;
            int                              source_h = 0;
            int                              dest_w   = 0; // destination surface the node was built for
            int                              dest_h   = 0;
            // See ContentSlot::detached: a retired slot keeps its node / pipeline
            // so re-enabling the pass re-attaches instead of rebuilding.
            bool                             detached = false;
            bool                             ready    = false;
        };

        /** @brief One retained fullscreen-program view sampling another target's
         * colour attachments through a user fragment program (deferred lighting).
         *
         * Owned by the pass that draws it (see SlotKey); rebuilt when the sampled
         * source, its size, the destination size or the program changes. The
         * per-frame push block (view-space lights, see LightPushBlock) is written
         * into @p push_data before each record.
         */
        struct ProgramSlot {
            int                              order      = std::numeric_limits<int>::min(); // stacking order (engine pass order); fullscreen first by default
            const vine::graphics::RenderTarget* source_target = nullptr; // sampled target the slot was built for
            // Whether the source's depth really ended in SHADER_READ_ONLY when this
            // node was built: the `gbuffer_depth` binding is only declared then (a
            // pass of the source that PRESERVES depth revokes the target's
            // promotion, so the image stays an attachment and cannot be sampled).
            // Part of the rebuild identity, so a policy change rebuilds the node.
            bool                             source_depth_sampleable = false;
            // Whether the node really BOUND the source's depth: the binding is only
            // declared when the shader samples it (the ABI gives the depth the
            // binding index the colour count sets), so a colour-only program never
            // puts the depth in its pipeline layout or its descriptor set and
            // records nothing that names the depth's layout. Only a slot that BINDS
            // it has to be dropped when the source revokes the promotion in the
            // same frame (see the promotion cascade in VsgRendererTargets.cpp).
            bool                             binds_source_depth = false;
            ::vsg::ref_ptr<::vsg::Camera>    camera;     // carries the sub-rect viewport
            ::vsg::ref_ptr<::vsg::View>      view;       // extra View of this target's render graph
            // The graph @p view is a child of. Kept so a slot can be taken out of the
            // frame it was built for (the depth-promotion revoke does that without a
            // host call: the slot was built while the promotion still stood).
            ::vsg::ref_ptr<::vsg::Group>     dest_graph;
            ::vsg::ref_ptr<::vsg::Node>      node;       // the fullscreen program drawable
            ::vsg::ref_ptr<::vsg::Data>      push_data;  // per-frame push-constant bytes
            // The program the node was compiled from, HELD (not merely compared):
            // the address is the slot's identity, so a released program replaced at
            // the same address must not read as "unchanged" (the ownership rule
            // SceneBridge's caches follow). Its content revision is part of the
            // rebuild identity too, so editing the program's GLSL in place
            // (ShaderProgram::replaceStages / setStage) rebuilds the node on the
            // next frame — the scene-geometry path keys its compiled state by the
            // revision the same way (D10).
            vine::intrusive_ptr<const vine::graphics::ShaderProgram> program;
            std::uint64_t                    program_revision = 0;
            int                              source_w = 0;
            int                              source_h = 0;
            int                              dest_w   = 0; // destination surface the node was built for
            int                              dest_h   = 0;
            // See ContentSlot::detached.
            bool                             detached = false;
            bool                             ready    = false;
        };

        /** @brief One output target (window = nullptr key, off-screen = RT* key).
         *
         * Unified (C6.4 / C6.5): window and off-screen targets are the SAME
         * shape — a RenderGraph whose children are content-slot Views (per
         * (camera, pass order)) plus optional PiP views (screen_slots). The
         * window target's graph is the shared swapchain graph created in
         * initialize(); each off-screen target owns its
         * own graph + attachments (images / views / render pass / framebuffer)
         * and lazily builds per-size shader sets, so one RT can bake several
         * content slots (different program / content / depth policy) the same
         * way the window does.
         */
        struct Target {
            /** @brief The parts of a RenderTarget's description that shape this
             * target's off-screen attachments and render pass.
             *
             * buildOffscreenTarget bakes all of them into the images, render pass
             * and framebuffer it creates, and a host may change any of them between
             * frames (attachColor / attachDepth / setDepthPromotion) — so a change
             * must rebuild. Comparing one key instead of listing the properties at
             * the rebuild predicate is what keeps a newly supported property from
             * being silently ignored: before this, an attachment added or a depth
             * promotion turned on after the first frame kept the old framebuffer
             * for the life of the target (readColorBuffer then reported the new
             * attachment as "out of range", and the promotion never happened).
             *
             * Size is tracked by the width / height fields below instead of here:
             * the borrow validation reads the built size, and releaseRenderTarget()
             * forces a rebuild by clearing them.
             */
            struct BuildKey {
                int                                                    color_count = 0;
                std::vector<vine::graphics::RenderTarget::ColorFormat> color_formats;
                bool                                                   has_depth = false;
                vine::graphics::RenderTarget::DepthFormat              depth_format{};
                bool                                                   depth_promotion = false;

                /** @brief Builds the key of @p target as it reads right now.
                 *
                 * @param target Render target to describe.
                 * @return The key of the target's current attachment / pass shape.
                 */
                [[nodiscard]] static BuildKey of(const vine::graphics::RenderTarget& target)
                {
                    BuildKey key;
                    key.color_count = target.colorCount();
                    key.color_formats.reserve(key.color_count > 0 ? static_cast<std::size_t>(key.color_count) : 0u);
                    for (int i = 0; i < key.color_count; ++i) {
                        key.color_formats.push_back(target.colorFormat(i));
                    }
                    key.has_depth       = target.hasDepth();
                    key.depth_format    = target.depthFormat();
                    key.depth_promotion = target.depthPromotion();
                    return key;
                }

                /** @brief Returns whether this key still describes @p target.
                 *
                 * The rebuild predicate runs for every pass into this target on
                 * every frame, so it compares against the target instead of
                 * building a key to compare with: the unchanged case allocates
                 * nothing and returns on the first difference.
                 *
                 * @param target Render target to compare against.
                 * @return true when every property this key watches still matches.
                 */
                [[nodiscard]] bool matches(const vine::graphics::RenderTarget& target) const
                {
                    if (color_count != target.colorCount() || has_depth != target.hasDepth() ||
                        depth_promotion != target.depthPromotion()) {
                        return false;
                    }
                    if (has_depth && depth_format != target.depthFormat()) {
                        return false;
                    }
                    for (int i = 0; i < color_count; ++i) {
                        if (color_formats[static_cast<std::size_t>(i)] != target.colorFormat(i)) {
                            return false;
                        }
                    }
                    return true;
                }
            };

            /** @brief Per-pass GPU objects for one pass under this target (§28).
             *
             * A render pass bakes ONE pair of attachment load-ops, so a pass that
             * clears and a pass that preserves cannot share one: each pass owns its
             * render pass + framebuffer + RenderGraph over the target's SHARED
             * attachments. The graph is a direct child of the command graph and
             * records in the passes' explicit pipeline order (see
             * reconcileOffscreenOrder). The window target is the exception — its
             * render pass is the swapchain's, so every window pass shares the
             * session graph and creates no entry here.
             */
            struct PassObjects {
                ::vsg::ref_ptr<::vsg::RenderPass>  render_pass;          ///< The pass' own colour/depth load-op variant.
                ::vsg::ref_ptr<::vsg::RenderPass>  render_pass_transient; ///< The one-frame variant of a pass whose depth image is in a transitional layout.
                ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer;         ///< Framebuffer for @ref render_pass.
                ::vsg::ref_ptr<::vsg::RenderGraph> graph;               ///< Render graph (one per pass).
                /// The pass' explicit pipeline order (setPassOrder). A target's pass
                /// graphs record in this order — the position the pass' content would
                /// have occupied as a View of a single target-wide render pass.
                int         order       = std::numeric_limits<int>::max();
                /// True when this pass preserves (LOADs) depth instead of clearing it.
                bool        load_depth  = false;
                /// True when this pass clears colour at its start; false LOADs the
                /// previous pass' colour (see planPassVariant). This is the
                /// STEADY variant's load-op, i.e. what @ref want_color_clear asks for.
                bool        color_clear = true;
                /// This pass' own clear requests (what the host asked), the input
                /// @ref color_clear / @ref load_depth were derived from. A run-time
                /// change of THESE is what makes a pass rebuild its variant
                /// (RenderPass::setClearEnabled / setShouldClearDepth): the
                /// materialised load-ops also carry the one-frame bootstrap, and the
                /// same frame builds one pass twice (setupContentSlot + render).
                bool        want_color_clear = false;
                /// True when this pass asked to clear depth (@ref load_depth is its
                /// materialised counterpart: a pass that asked for no clear LOADs).
                bool        want_depth_clear = false;
                /// This pass' clear colour (its own clear() request).
                ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
                /// True while @ref graph records @ref render_pass_transient instead
                /// of @ref render_pass, i.e. for the ONE frame in which this pass
                /// consumes a depth image that is in a transitional layout because
                /// no pass of the target has left it in the attachment layout yet:
                ///
                ///  - the image is still UNDEFINED (nothing defined it: the pass
                ///    records the CLEAR seed variant), or
                ///  - the image was PROMOTED to SHADER_READ_ONLY by an earlier
                ///    frame's pass and this pass is the first of this frame to use
                ///    it (the pass records the LOAD variant that names that layout).
                ///
                /// submitFrame() swaps the graph to the steady variant afterwards,
                /// once that frame has made the depth attachment-optimal again.
                bool        transient   = false;
            };

            // Per-pass objects, keyed by the owning pass (address-stable map so a
            // pass' objects keep their address). Empty until the following steps
            // create them.
            std::map<SlotKey, PassObjects> passes;
            // True once this target's images / views exist: the per-pass model's
            // "target is built" test (there is no single target-level graph).
            bool attachments_built = false;
            // True once this target's depth image has been defined (cleared) at
            // least once, so a LOAD pass may load it — an UNDEFINED image cannot be
            // loaded. Target-level, because the image is shared by every pass.
            bool depth_seeded = false;
            // True once any pass of this target LOADs depth: the depth must then
            // stay in the attachment layout, so NO pass of this target may promote
            // it to a sampleable texture (see the §28 invariants).
            bool any_load_pass = false;

            // ---- Retained slots, by kind ------------------------------------------

            /** @brief The three kinds of retained slot a target can hold.
             *
             * They share their LIFECYCLE (built by a pass, re-checked every frame,
             * detached when the pass stops executing, erased when the pass moves or
             * is released) and live in their own tables because what they RETAIN
             * differs: a content slot owns a SceneBridge, a screen / program slot
             * owns a sampling edge (source_target + attachment). Most walks do not
             * care which kind they are looking at — see forEachSlot() / visitSlot().
             */
            enum class SlotKind { Content, Screen, Program };

            /** @brief Visits every retained slot of this target, whatever its kind.
             *
             * @param visitor Called as visitor(key, slot, kind) for each slot.
             */
            template <class Visitor> void forEachSlot(Visitor&& visitor)
            {
                for (auto& entry : content_slots) {
                    visitor(entry.first, entry.second, SlotKind::Content);
                }
                for (auto& entry : screen_slots) {
                    visitor(entry.first, entry.second, SlotKind::Screen);
                }
                for (auto& entry : program_slots) {
                    visitor(entry.first, entry.second, SlotKind::Program);
                }
            }

            /** @brief Const counterpart of forEachSlot, for the diagnostic walks.
             *
             * Needed because the renderer holds its state BY VALUE (no PImpl): a const
             * renderer makes a const entry, and the read-only counters (detachedSlotCount
             * and friends) must still be able to walk the slot tables.
             *
             * @param visitor Called as visitor(key, slot, kind) for each slot.
             */
            template <class Visitor> void forEachSlot(Visitor&& visitor) const
            {
                for (const auto& entry : content_slots) {
                    visitor(entry.first, entry.second, SlotKind::Content);
                }
                for (const auto& entry : screen_slots) {
                    visitor(entry.first, entry.second, SlotKind::Screen);
                }
                for (const auto& entry : program_slots) {
                    visitor(entry.first, entry.second, SlotKind::Program);
                }
            }

            /** @brief Visits the ONE slot this target holds under @p key.
             *
             * The tables are keyed identically (a pass owns one slot of one kind per
             * target), so at most one of them holds @p key.
             *
             * @param key     Slot key to look for.
             * @param visitor Called as visitor(slot, kind) when the key is present.
             * @return true when a slot was visited.
             */
            template <class Visitor> bool visitSlot(const SlotKey& key, Visitor&& visitor)
            {
                if (const auto it = content_slots.find(key); it != content_slots.end()) {
                    visitor(it->second, SlotKind::Content);
                    return true;
                }
                if (const auto it = screen_slots.find(key); it != screen_slots.end()) {
                    visitor(it->second, SlotKind::Screen);
                    return true;
                }
                if (const auto it = program_slots.find(key); it != program_slots.end()) {
                    visitor(it->second, SlotKind::Program);
                    return true;
                }
                return false;
            }

            /** @brief Whether this target holds a slot of ANY kind under @p key.
             *
             * The early-out of every per-pass path: a pass that drew into another
             * target owns nothing here, and the teardown paths must not pay a device
             * wait for it.
             *
             * @param key Slot key to look for.
             * @return true when any of the slot tables holds @p key.
             */
            [[nodiscard]] bool hasSlot(const SlotKey& key) const
            {
                return content_slots.count(key) != 0u || screen_slots.count(key) != 0u || program_slots.count(key) != 0u;
            }

            /** @brief Erases the slot @p kind holds under @p key.
             *
             * @param kind Table the slot came from (see forEachSlot / visitSlot).
             * @param key  Slot key whose entry is erased.
             */
            void eraseSlot(SlotKind kind, const SlotKey& key)
            {
                switch (kind) {
                case SlotKind::Content: content_slots.erase(key); return;
                case SlotKind::Screen: screen_slots.erase(key); return;
                case SlotKind::Program: program_slots.erase(key); return;
                }
            }

            /** @brief The render graph a slot's view records into (the §28 model).
             *
             * The window target has ONE graph (the shared swapchain graph, created
             * with the window); an off-screen target has one per pass. A slot whose
             * pass has no entry yet — or an off-screen target that failed to build —
             * has none.
             *
             * @param owner     Target entry that holds the slot.
             * @param owner_key The key @p owner is registered under (nullptr = window).
             * @param key       Slot key (its owner pass indexes the pass table).
             * @return The graph, or null when the slot has none.
             */
            [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> slotGraph(Target& owner,
                                                                      vine::graphics::RenderTarget* owner_key,
                                                                      const SlotKey& key)
            {
                if (owner_key == nullptr) {
                    return owner.graph;
                }
                const auto pass = owner.passes.find(key);
                return pass == owner.passes.end() ? ::vsg::ref_ptr<::vsg::RenderGraph>() : pass->second.graph;
            }

            // ---- off-screen GPU attachments (window target: unused) ----
            // One image + view per colour attachment (MRT / G-buffer targets carry
            // several sampleable textures; single-colour targets keep one entry).
            // The render pass and framebuffer are PER PASS (@ref PassObjects): one
            // render pass bakes ONE pair of attachment load-ops, so a pass that
            // clears and a pass that preserves materialise their own variant over
            // these shared images. Only the window keeps a target-level graph, and
            // only because its render pass is the swapchain's, which cannot be
            // re-created per pass.
            std::vector<::vsg::ref_ptr<::vsg::Image>>     color_images;
            std::vector<::vsg::ref_ptr<::vsg::ImageView>> color_views;
            ::vsg::ref_ptr<::vsg::Image>       depth_image;
            ::vsg::ref_ptr<::vsg::ImageView>   depth_view;
            ::vsg::ref_ptr<::vsg::RenderGraph> graph; // window: the shared swapchain graph (off-screen: see passes)
            // Depth sharing (see RenderTarget::shareDepth): the target whose depth
            // this framebuffer borrows (null = owns its depth) plus the command
            // barrier that makes that depth visible between the two render graphs.
            vine::graphics::RenderTarget*    depth_source = nullptr;
            // The source's depth VIEW this framebuffer was baked with. A source
            // that is rebuilt (size or depth-policy change) replaces its depth
            // image, and the baked framebuffer would go on testing the replaced
            // image — which nobody writes any more — so render() rebuilds this
            // target as soon as the two differ and runs the borrow validation again.
            ::vsg::ref_ptr<::vsg::ImageView> depth_source_view;
            ::vsg::ref_ptr<::vsg::PipelineBarrier> depth_share_barrier;
            // Set when the borrowed source above was RELEASED while still borrowed:
            // its VkImage is gone, so the borrow cannot be honoured and this target
            // builds with its own depth instead (a later shareDepth() with a live
            // source clears the condition by being a different pointer).
            const vine::graphics::RenderTarget* unusable_depth_source = nullptr;
            // Per-size shader sets for off-screen slots (window slots share
            // impl->depth_on / depth_testonly / depth_off shader sets). Built lazily.
            ::vsg::ref_ptr<::vsg::ShaderSet> depth_on_shader_set;
            ::vsg::ref_ptr<::vsg::ShaderSet> depth_testonly_shader_set;
            ::vsg::ref_ptr<::vsg::ShaderSet> depth_off_shader_set;
            int width  = 0; // off-screen logical size
            int height = 0;
            // The attachment / pass shape these attachments were built from (see
            // BuildKey): a change means the images / render pass / framebuffer no
            // longer match the target's description and must be rebuilt.
            BuildKey build_key;
            // Engine clear() request for this target, persisted so a pass graph
            // created later clears to the last requested colour instead of a
            // hard-coded default. A pass' OWN clear request (the open scope) takes
            // precedence; this is the fallback for a pass that never asked.
            bool        clear_seen  = false;
            ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
            // Depth value a pass of this target clears to: the reverse-Z FAR plane
            // (0.0) for every target, colour or depth-only. The depth compare is
            // VK_COMPARE_OP_GREATER, so the buffer must start at the far plane for
            // any fragment to pass — a depth-only target cleared to the near plane
            // would reject everything (see buildOffscreenTarget).
            float       depth_clear_value = 0.0f;
            // True when a pass of this target leaves its depth in
            // SHADER_READ_ONLY_OPTIMAL because it promoted it to a sampled texture:
            // that image can no longer serve as ANOTHER target's depth attachment,
            // so a later shareDepth() of this target cannot be honoured (see the
            // borrow validation in buildOffscreenTarget). Recorded when the
            // ATTACHMENTS are built, from the target's own description
            // (RenderTarget::depthPromotion) — a consumer that borrows this depth is
            // validated in the same frame, before any of this target's passes
            // exists, so recording it only when a pass is created would be too late.
            bool        depth_sampleable = false;
            // True once a pass of this target has CLEARed its colour. A freshly
            // created colour image is UNDEFINED, and a render pass may not LOAD an
            // UNDEFINED image (VUID-VkRenderPassBeginInfo-image-...), so the FIRST
            // pass into a new target always clears it whatever that pass asked for
            // — the colour counterpart of @ref depth_seeded.
            bool        color_seeded = false;
            // True while a requested depth borrow could not be honoured YET because
            // the source had no depth image at build time, and the report for that
            // episode was already emitted. Transient: the borrow is retried as soon
            // as the source exists (see render()'s rebuild predicate) and this flag
            // is cleared when it is honoured, so a source that arrives late is
            // reported once, not every frame.
            bool        depth_borrow_pending_reported = false;

            // ---- content slots (retained Views under graph), keyed by owning pass ----
            std::map<SlotKey, ContentSlot> content_slots;
            // ---- PiP views sampling other targets (drawn under this graph) ----
            std::map<SlotKey, ScreenSlot> screen_slots;
            // ---- fullscreen-program views (deferred lighting), keyed by owning pass ----
            std::map<SlotKey, ProgramSlot> program_slots;

            // The target this entry belongs to. The entry OWNS it, exactly like the
            // content caches own the geometry they are keyed by (see the ownership
            // rule in the design notes): a raw pointer key whose entry does not hold
            // the object can outlive it, and then a NEW target allocated at the same
            // address silently inherits the dead one's attachments — its size, its
            // colour formats and its depth format included. Owning it makes that
            // address unreusable while the entry exists. Engine targets are released
            // through releaseRenderTarget(); a target the host dropped without
            // announcing it is swept in submitFrame().
            vine::intrusive_ptr<vine::graphics::RenderTarget> owner;
        };

        /** @brief Gets the output-target entry for @p target, pinning its address.
         *
         * Every path that touches the target table goes through here, so an entry can
         * never exist without owning the target it is keyed by (see Target::owner).
         *
         * @param target Target key (nullptr = the window).
         * @return The entry, created on first use.
         */
        /** @brief Stops a target's passes being recorded and makes their release safe.
         *
         * The destructive unhook both teardown paths need: a target about to be rebuilt
         * (buildOffscreenTarget) and a target about to be released
         * (releaseRenderTarget). Each pass graph is removed from the command graph, the
         * device is waited on, and every content slot's bridge cache is dropped together
         * with its queued compile view.
         *
         * The wait is REQUIRED here and is the counted one (Impl::waitForIdle), not a
         * park: clearCache() releases the bridge's shared object registry, whose
         * pipelines / samplers the retained nodes do not necessarily keep alive as the
         * only owner — parking the views instead was measured to trip
         * vkDestroyPipeline-00765 / vkDestroySampler-01082 (see the policy-churn notes).
         * Non-destructive paths (a replaced render pass, a dropped program slot) park
         * their objects with Impl::retireObject instead.
         *
         * @param t Target entry whose passes stop being recorded.
         */
        void unhookTargetPasses(Target& t);

        /** @brief Creates the barrier that orders a borrowed depth image's writes before
         * the borrower's pass reads / tests it.
         *
         * A depth borrow (RenderTarget::shareDepth) makes two targets share ONE depth
         * image in the attachment layout. The image is written by the source's passes and
         * then LOADed by the borrower's, so the write must be made visible before the
         * read: reconcileOffscreenOrder() inserts this barrier right after the source's
         * last pass graph (Target::depth_share_barrier).
         *
         * The subresource range covers both aspects when the source's depth format is a
         * COMBINED depth/stencil one (D24 / D32S8 / D16S8): with separateDepthStencilLayouts
         * disabled a barrier may not name only one aspect of such a format
         * (VUID-VkImageMemoryBarrier-image-03320).
         *
         * @param source Target whose depth image is borrowed.
         * @return The barrier, or null when @p source has no depth image to share.
         */
        [[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineBarrier> makeDepthShareBarrier(vine::graphics::RenderTarget* source) const;

        /** @brief Detaches one slot's view from the graph it records into.
         *
         * The single place that knows how a slot stops being recorded: the view is
         * removed from the target's (or the pass') render graph and dropped from the
         * pending incremental-compile queue. Every path that retires, moves or erases
         * a slot goes through it — a slot whose view is left attached keeps drawing.
         *
         * @param owner     Target entry that holds the slot.
         * @param owner_key Key @p owner is registered under (nullptr = window).
         * @param key       Slot key whose graph the view was attached to.
         * @param view      The slot's retained view (null is a no-op).
         */
        void detachSlotView(Target& owner, vine::graphics::RenderTarget* owner_key, const SlotKey& key,
                            const ::vsg::ref_ptr<::vsg::View>& view);

        /** @brief Forgets everything a previous build of a target's attachments produced.
         *
         * The second half of a rebuild (the first is unhookTargetPasses, which stops the old
         * passes being recorded and makes the release safe): every image / view / slot table and
         * every flag the build set goes back to its initial value, while the target's own entry
         * stays. Written as ONE list because it is exactly what a build OWNS — a Target field
         * added later and forgotten here would survive a rebuild as a stale image, a stale
         * "already built" flag or a stale borrow source, and nothing would report it.
         *
         * @param t Target entry being emptied (its key stays registered).
         */
        void resetTargetAttachments(Target& t);

        /** @brief Creates a target's GPU attachments: one colour image + view per attachment, plus
         * its depth (owned or borrowed from an earlier target this frame).
         *
         * The USAGE flags are the reason this lives in one place — they are what makes a colour
         * attachment also usable as a sampled texture (PiP / fullscreen-program sources) or as a
         * blit source (readColorBuffer), and what lets a depth image be copied out
         * (readDepthBuffer): without TRANSFER_SRC the depth cannot even be transitioned to
         * TRANSFER_SRC_OPTIMAL (VUID-VkImageMemoryBarrier-oldLayout-01212).
         *
         * Every view is created through createImageView(), which compiles the Image (creates the
         * VkImage and allocates / binds its memory) AND the ImageView: without it both handles stay
         * VK_NULL_HANDLE and a framebuffer built from them holds corrupt handles, which only shows
         * up as a crash in vkCmdBeginRenderPass.
         *
         * A BORROWED depth (see RenderTarget::shareDepth) attaches the SOURCE's image, so what this
         * target records is which source VIEW its framebuffer was baked with — the source replacing
         * that image invalidates the framebuffer and render() rebuilds this target by comparing the
         * two.
         *
         * @param t        Target entry whose images / views are set (it is being built).
         * @param device   Device that compiles the images and creates the views.
         * @param target   Render target description (attachment count / formats / depth).
         * @param w        Width to create the images with.
         * @param h        Height to create the images with.
         * @param depth_src Source whose depth is borrowed, or null to create this target's own.
         */
        void createTargetAttachments(Target& t, ::vsg::Device* device, const vine::graphics::RenderTarget& target,
                                     uint32_t w, uint32_t h, vine::graphics::RenderTarget* depth_src);

        /** @brief Drops every slot that SAMPLES @p target, which was just (re)built.
         *
         * A rebuild creates FRESH colour views, and a consumer's stale check only watches the
         * source's SIZE — which a same-size rebuild does not change — so a PiP / fullscreen-program
         * slot built against the old views would go on sampling an image nothing draws into any
         * more. Dropping the slot makes its owner's next drawScreenTexture / drawScreenProgram call
         * reattach against the new attachments.
         *
         * Consumers are found by inspecting the slot ATTRIBUTE (source_target), because a slot's
         * key is the pass that OWNS it, not the target it samples.
         *
         * @param target Target whose attachments were just rebuilt (the sampled source).
         */
        void dropConsumersSampling(const vine::graphics::RenderTarget* target);

        /** @brief Records @p commands into a fresh command buffer and waits for it.
         *
         * The readback paths (readColorBuffer / readDepthBuffer) are synchronous by
         * contract: they submit ONE immediate transfer — barriers plus a blit / a copy —
         * and may not return before the GPU is finished with it. Owning the submission
         * here also keeps the do-nothing guards, the queue choice and the (generous)
         * fence timeout in one place, instead of repeating them at every readback.
         *
         * @param commands Command list to record and complete.
         * @return false when the session has no usable device (nothing to submit to).
         */
        [[nodiscard]] bool submitOneShot(const ::vsg::ref_ptr<::vsg::Commands>& commands) const;

        /** @brief Allocates the memory a readback destination has to live in.
         *
         * Host-visible and host-coherent on purpose: the CPU reads the staging buffer
         * (or the LINEAR image a colour readback blits into) the moment the submission
         * above returns.
         *
         * @param device       Device to allocate on.
         * @param requirements Requirements of the resource it will be bound to.
         * @return The allocation; the caller binds it to its resource.
         */
        [[nodiscard]] ::vsg::ref_ptr<::vsg::DeviceMemory> hostVisibleMemory(::vsg::Device* device,
                                                                          const VkMemoryRequirements& requirements) const;

        /** @brief The built target entry a readback reads from, or null.
         *
         * The shared prologue of readColorBuffer / readDepthBuffer: the window session
         * and viewer must exist, the target must have been built (its attachments exist)
         * and have a usable size. It deliberately does NOT stop the device: both callers
         * check the format (and report why) before paying for the wait.
         *
         * @param target Target to read from (null = unsupported).
         * @return The entry, or null when this readback is unsupported.
         */
        [[nodiscard]] const Target* readbackTarget(vine::graphics::RenderTarget* target) const;

        /** @brief The Vulkan-side description of one target's attachment set.
         *
         * Every pass of a target attaches exactly the same images and only differs in
         * its load-ops, so this is computed once per pass build and then passed around:
         * the device the objects are created on, the attachment formats the render
         * pass must declare, and which attachments exist (owned or BORROWED — see
         * RenderTarget::shareDepth).
         */
        struct PassAttachments {
            ::vsg::ref_ptr<::vsg::Device> device;
            std::vector<VkFormat>         color_formats;
            VkFormat                      depth_format = VK_FORMAT_UNDEFINED;
            bool                          has_color    = false;
            bool                          has_depth    = false;
            bool                          borrowed     = false;
        };

        /** @brief Describes the attachment set passes into @p target will attach.
         *
         * @param t      Target entry the pass builds objects for.
         * @param target Render target whose description names the formats.
         * @return The description; @c device is null before the window session exists.
         */
        [[nodiscard]] PassAttachments passAttachments(const Target& t, const vine::graphics::RenderTarget& target) const;

        /** @brief What one pass of a target needs THIS frame (§28, decided in one place).
         *
         * Values only: everything the apply side needs, read at plan time — because applying
         * the plan (revoking a depth promotion, publishing the new objects) is exactly what
         * changes them. @ref current in particular points INTO the target's pass table, so it
         * has to be taken before the pass is published under its key.
         */
        struct PassPlan {
            /// The target's attachment set (owned by buildOffscreenTarget).
            PassAttachments att;
            /// The variant this pass has to record (load-ops, promotion, transient bootstrap).
            detail::PassVariant variant;
            /// The pass' recorded objects, or null when this is a NEW pass.
            const Target::PassObjects* current = nullptr;
            /// Whether the target has a colour / a depth attachment.
            bool has_color = false;
            /// Whether the target has a depth attachment (owned or borrowed).
            bool has_depth = false;
            /// This pass' own clear requests (what the host asked). The variant's materialised
            /// load-ops come from these — never from a materialised comparison (see planPass).
            bool want_color_clear = false;
            bool want_depth_clear = false;
            /// The variant's colour load-op is CLEAR (the pass clears rather than loads).
            bool color_clear = false;
            /// The variant's depth load-op is LOAD (the pass preserves the depth it finds).
            bool depth_load = false;
        };

        /** @brief Decides what one pass records this frame — device-free, no side effects.
         *
         * The whole per-pass decision: the attachment set, the load-op variant, the pass' own
         * clear requests, and whether its depth image still carries the layout a PROMOTING pass
         * left behind.
         *
         * THE LOAD-OP POLICY. A pass' OWN clear request (the open pass scope) decides its
         * load-ops — nothing else. A pass that never asked for a clear must LOAD what an earlier
         * pass left, or the stacked-pass pipelines break: the engine's deferred +
         * forward-composite pipeline stacks fullscreen lighting and the forward transparent
         * content on ONE off-screen target whose passes all set clearEnabled=false, so "clear it
         * anyway" wipes the lit result the next pass was meant to composite over. A pass that DID
         * ask for a clear clears, whatever its siblings asked for. The one exception is the
         * bootstrap: a target whose colour image has never been defined holds an UNDEFINED image,
         * and a render pass may not LOAD an UNDEFINED image, so the first pass into a NEW target
         * clears it ONCE (planPassVariant()'s transient bootstrap variant, swapped for the steady
         * one at the end of the frame like the depth seed) — otherwise the bootstrap would turn
         * into "this pass clears colour for ever", wiping what an earlier pass of the target drew
         * every frame.
         *
         * PROMOTION STATE. A pass that LOADs depth has to name the layout its image really is in,
         * and promotion is the only way the depth ends anywhere but the attachment layout. It is
         * in force while no pass of this target LOADs depth (@ref Target::depth_sampleable — the
         * cascade in passGraph revokes it) and the image is defined (@ref Target::depth_seeded);
         * it can then only have been replaced by a pass that RAN EARLIER in this frame. Passes
         * announce themselves in passes_active_this_frame as they render and record in their
         * explicit order, so a pass of this target that is announced AND ordered before this one
         * has already run (see depthStillPromoted).
         *
         * The variant itself is the device-free planPassVariant(), and the steady-frame guard is
         * passVariantIsStale(), which compares the pass' REQUESTS — never the materialised
         * load-ops: those carry the bootstrap too, and the very same frame builds one pass twice
         * (setupContentSlot() and render()), so a materialised comparison would rebuild the second
         * build into a LOAD against images nothing has defined yet.
         *
         * @param t      Target entry the pass belongs to.
         * @param key    Slot key of the pass.
         * @param target The render target itself (its description names the formats).
         * @return The plan; @ref PassPlan::current is null for a pass that has not recorded yet.
         */
        [[nodiscard]] PassPlan planPass(const Target& t, const SlotKey& key,
                                        const vine::graphics::RenderTarget& target) const;

        /** @brief Creates the render pass + framebuffer for ONE load-op combination.
         *
         * A render pass bakes one pair of attachment load-ops, so a pass that clears and
         * a pass that preserves cannot share one: this is where a variant becomes
         * objects. The pair is returned together because the framebuffer names the
         * attachments the render pass declares — they are replaced together or not at
         * all.
         *
         * @param t                Target entry the pass belongs to.
         * @param att              Its attachment description (see passAttachments).
         * @param pass_color_clear Whether this pass clears (rather than loads) colour.
         * @param depth_load       Whether the depth attachment is loaded, not cleared.
         * @param promote          Whether the pass may leave the depth sampleable.
         * @param depth_initial    Layout the depth image is in when this pass starts. A
         *                         CLEAR pass starts from UNDEFINED whatever its image
         *                         held, so this only matters for a depth-LOAD pass,
         *                         which must name the layout its image really is in.
         * @return The render pass and its framebuffer.
         */
        [[nodiscard]] std::pair<::vsg::ref_ptr<::vsg::RenderPass>, ::vsg::ref_ptr<::vsg::Framebuffer>>
        makePassObjects(const Target& t, const PassAttachments& att, bool pass_color_clear, bool depth_load, bool promote,
                        VkImageLayout depth_initial) const;

        /** @brief Creates the render graph of a NEW pass, with its clear values.
         *
         * One graph per pass (§28): a pass owns the load-ops of its own scope and therefore
         * cannot record into its target's graph. Everything the graph needs is derived from
         * the target's built attachments.
         *
         * The clear values follow the ATTACHMENT ORDER the framebuffer was built with —
         * colour attachments in order, then depth — as VkRenderPassBeginInfo requires:
         * attachment 0 carries THIS pass' clear colour, the extra MRT attachments stay
         * transparent black (their regions stay black until a fragment writes them), and the
         * depth entry carries the target's depth clear value, the reverse-Z far plane for
         * every target (see buildOffscreenTarget).
         *
         * @param t           Target entry whose attachments the graph renders into.
         * @param has_depth   Whether the framebuffer has a depth attachment (its clear value
         *                    is appended last).
         * @param clear_color Colour attachment 0 clears to (the pass' own request).
         * @return The graph, with a null render pass / framebuffer to be set by the caller.
         */
        [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> makePassGraph(const Target& t, bool has_depth,
                                                                      const ::vsg::vec4& clear_color) const;

        /** @brief Reuses a pass' recorded variant when its clear policy did not change.
         *
         * The whole steady-frame cost of a pass: its objects already encode the load-ops
         * its requests ask for, so only the clear VALUE can have changed — two map lookups,
         * no device call, no allocation. A pass that re-requests a clear updates ITS OWN
         * graph and never its siblings': a pass clears to its own request (§28), so one
         * pass' clear must not become another pass' background colour.
         *
         * The caller has already moved the pass to its record position when its explicit
         * pipeline order changed (VsgRenderer::reconcileOffscreenOrder).
         *
         * @param objects           The pass' recorded objects (its variant).
         * @param want_color_clear  Whether the pass asks to clear colour this frame.
         * @param want_depth_clear  Whether the pass asks to clear depth this frame.
         * @param has_color         Whether the target has a colour attachment (clear value 0
         *                          is the COLOUR entry only then: a depth-only target's single
         *                          entry is its DEPTH value, and VkClearValue is a union, so
         *                          writing .color there would clear the depth to a colour's
         *                          bit pattern).
         * @param clear_color       The colour the pass clears to (its own request).
         * @return The pass' graph, or null when the pass changed its clear policy and its
         *         variant has to be rebuilt.
         */
        [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> reuseSteadyPass(Target::PassObjects& objects, bool want_color_clear,
                                                                       bool want_depth_clear, bool has_color,
                                                                       const ::vsg::vec4& clear_color);

        /** @brief Records a (re)built pass and what it establishes for its target.
         *
         * The pass now exists and will record: its objects are stored under its slot key, the
         * target learns that its attachments have been written (a later pass may LOAD them) and
         * a pass that LOADs depth withdraws the target's depth promotion, because from here on
         * the image ends in the attachment layout (readDepthBuffer and a later borrow
         * validation are told by the same flag).
         *
         * Adding the pass' graph to the command graph and ordering it is the caller's job
         * (VsgRenderer::passGraph): only VsgRenderer owns the command graph.
         *
         * @param t         Target entry that owns the pass.
         * @param key       Slot key of the pass.
         * @param objects   The pass' materialised objects.
         * @param has_color Whether the target has a colour attachment.
         */
        void publishPass(Target& t, const SlotKey& key, const Target::PassObjects& objects, bool has_color);

        /** @brief Whether the target's depth still carries the layout a promoting pass left.
         *
         * A pass that LOADs depth has to name the layout its image really is in, and
         * promotion is the only way the depth ends anywhere but the attachment layout.
         * Promotion is in force while no pass of the target LOADs depth
         * (@ref Target::depth_sampleable — revokeDepthPromotion() withdraws it) and the
         * image is defined (@ref Target::depth_seeded); it can then only have been
         * replaced by a pass that RAN EARLIER in this frame — passes announce
         * themselves in passes_active_this_frame as they render and record in their
         * explicit order, so a pass of the target that is announced AND ordered before
         * this one has already run.
         *
         * @param t       Target entry the pass belongs to.
         * @param current The pass being built (skipped; null when it has no objects yet).
         * @param order   This pass' explicit record order.
         * @return true when the depth is still promoted, so a LOAD must name that layout.
         */
        [[nodiscard]] bool depthStillPromoted(const Target& t, const Target::PassObjects* current, int order) const;

        /** @brief Whether a target's recorded attachments have to be (re)built because of its
         * DEPTH BORROW.
         *
         * Two separate ways a borrowed depth outlives its usefulness, answered together because
         * they mean the same thing to the caller: the framebuffer recorded for this target no
         * longer matches the source it has to test against.
         *
         *  - PENDING: the requested borrow could not be honoured yet (the source had no depth
         *    image when this target was built), so the baked borrow differs from the requested one
         *    and is retried as soon as the source has an image. A source that is permanently
         *    unusable is remembered as such (@ref Target::unusable_depth_source), so this retries
         *    only while the borrow is merely WAITING — a disabled or never-built producer costs one
         *    map lookup per frame, not a rebuild loop.
         *  - STALE: an honoured borrow attaches the source's depth VIEW, and a source that is
         *    rebuilt (a size change, or the depth-policy change this same predicate watches for its
         *    own targets) replaces its depth image. The borrower's framebuffer would go on testing
         *    the replaced image, which nobody writes any more: the borrowed depth silently freezes
         *    while the old image stays alive. Comparing the source's current view against the one
         *    this target was baked with detects that, and the rebuild re-runs the borrow validation
         *    against the new image.
         *
         * @param t          Target entry to inspect.
         * @param target_key The target itself (nullptr = the window, which never borrows).
         * @return true when the caller has to rebuild the target's attachments.
         */
        [[nodiscard]] bool borrowNeedsRebuild(const Target& t, const vine::graphics::RenderTarget* target_key) const;

        /** @brief Re-creates every pass of @p t WITHOUT depth promotion.
         *
         * A pass that LOADs depth must find the image in a layout it named, so no pass
         * of the target may promote it any more: passes that were allowed to promote
         * (the first pass cleared depth and nothing loaded it) are rebuilt without it
         * and the target's @ref Target::depth_sampleable is withdrawn. A depth-only
         * target is exempt — promotion is not a choice there (its depth always ends
         * sampleable), so nothing has to be revoked.
         *
         * The pass being (re)built is skipped: its own variant is decided by the caller,
         * and rebuilding it here would be thrown away — and would hide the layout its
         * image is in (a pass that STOPS promoting is exactly the one whose depth may
         * still carry the promoted layout).
         *
         * @param t                    Target entry being revoked.
         * @param current              The pass being built, skipped (may be null).
         * @param steady_depth_initial Layout a depth-LOAD pass of this target expects.
         */
        void revokeDepthPromotion(Target& t, const Target::PassObjects* current, VkImageLayout steady_depth_initial);

        /** @brief The command graph's record plan for one frame (reconcileOffscreenOrder).
         *
         * The engine can build a target's graph out of dependency order — a producer
         * (re)built after its consumers existed, a consumer wired to a producer built
         * later — and a consumer only samples its source's CURRENT content when the
         * producer's graph is recorded first. So the plan is: which graphs each target
         * records this frame (in the target's own pass order), the targets' CURRENT
         * record order (the stable tie-break seed), and the dependency-valid order the
         * children end up in.
         */
        struct RecordPlan {
            ::vsg::ref_ptr<::vsg::RenderGraph> window_graph;
            std::map<vine::graphics::RenderTarget*, std::vector<::vsg::ref_ptr<::vsg::RenderGraph>>> graphs_of;
            std::vector<vine::graphics::RenderTarget*> present; ///< Targets recorded now, in current child order.
            std::vector<vine::graphics::RenderTarget*> order;   ///< Targets in dependency-valid order (see orderRecordPlan).
        };

        /** @brief Fills @p plan's graph map and current record order (phase 1).
         *
         * Collects, per off-screen target, the pass graphs that must record this frame
         * (skipping RETIRED ones: a pass whose slot is detached records nothing) in the
         * target's explicit pass order, seeding ties from the order the graphs are
         * recorded in right now.
         *
         * @param plan Plan to fill (@ref RecordPlan::window_graph must be set).
         */
        void fillRecordPlan(RecordPlan& plan) const;

        /** @brief Turns @p plan's current order into a dependency-valid one (phase 2).
         *
         * Edges: a target's off-screen SAMPLING sources (its non-detached screen /
         * program slots' source_target) and its DEPTH BORROW source (a borrower's pass
         * LOADs the depth its source writes this frame). The order itself is the pure
         * stableTopologicalOrder(), so unrelated targets keep their relative position.
         *
         * @param plan Plan whose graphs_of / present are filled.
         */
        void orderRecordPlan(RecordPlan& plan) const;

        /** @brief Rewrites the command graph's children from @p plan (phase 3).
         *
         * Each target's graphs in dependency order, the depth-share barrier of every
         * target borrowing THIS one right after its last graph (so the borrower's LOAD
         * sees the writes), and the window swapchain graph last (it may itself sample
         * off-screen targets). Reordering render-graph children only changes the
         * per-frame record order.
         *
         * @param plan Plan whose order / graphs_of are filled.
         */
        void applyRecordPlan(const RecordPlan& plan);

        Target& entryFor(vine::graphics::RenderTarget* target)
        {
            Target& entry = targets[target];
            if (target != nullptr && entry.owner.get() != target) {
                entry.owner = target;
            }
            return entry;
        }

        std::map<vine::graphics::RenderTarget*, Target> targets; // nullptr key == window
    };

  private:
    /** @brief Reports one backend diagnostic: stderr trace plus the host sink.
     *
     * The stderr half is this backend's built-in tracing (the validation
     * harness reads it); the sink half is the host's programmatic channel.
     * Failing to draw content must never be silent, so every place that gives
     * up on a request calls this instead of only printing.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, with the numbers involved.
     */
    /** @brief Installs this renderer's diagnostics route on @p bridge.
     *
     * The bridge reports a diagnostic; the renderer turns it into the single
     * route (stderr trace + backend counters + host sink), so a bridge report is
     * counted and delivered exactly like a renderer report.
     *
     * @param bridge Content-slot bridge to route.
     */
    void installDiagnosticRoute(SceneBridge& bridge);

    void reportFailure(vine::graphics::DiagnosticSeverity severity,
                       vine::graphics::DiagnosticCategory category,
                       const vine::String&          message);

    // Renderer-lifetime state, then the session state. Both BY VALUE: there is no
    // PImpl here — this header IS the implementation header (plugin-internal, since
    // the host only ever sees the graphics SDK interface), so the state structs are
    // complete types and no indirection has to be paid for them.
    Persistent persistent;
    Impl       impl;
};

V_VSG_NS_END
