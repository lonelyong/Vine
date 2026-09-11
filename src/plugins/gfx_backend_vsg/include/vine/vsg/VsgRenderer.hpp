#pragma once
#include "vsg_global.hpp"

#include <optional>

#include <vsg/app/Viewer.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/raw_ptr.hpp>

namespace vine::graphics
{

class Camera;
class Light;
class RenderPass;
class RenderTarget;
class ShaderProgram;
struct RenderCommand;

} // namespace vine::graphics

V_VSG_NS_BEGIN

// Defined in SceneBridge.hpp (a slot's retained-state bridge). Forward declared
// so this header stays independent of it: the renderer only routes diagnostics
// to it, and holds it inside its PImpl.
class SceneBridge;

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
     *         never built, has no depth attachment or uses a packed format.
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
     * @param target Output target whose graph receives the view (nullptr =
     *               the window).
     * @param view   The View to position (removed from any current position
     *               first).
     * @param order  The view's explicit stacking order.
     */
    void placeViewByOrder(vine::graphics::RenderTarget* target,
                          const ::vsg::ref_ptr<::vsg::View>& view,
                          int order);

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

    struct Impl;
    struct Persistent;
    std::unique_ptr<Impl> impl;
    std::unique_ptr<Persistent> persistent;
};

V_VSG_NS_END
