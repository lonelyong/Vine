#pragma once
#include "graphics_global.hpp"

#include <map>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "FrameContext.hpp"
#include "ShaderPreset.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class Scene;
class RenderPass;
class RenderTarget;
class RenderBackend;

/**
 * @brief High-level render engine managing the frame loop and render state.
 *
 * RenderEngine does not impose a fixed pipeline: nothing is rendered unless
 * the caller registers render passes with addPass(). Each frame, frame() runs
 * the registered passes in ascending order, then ends and swaps buffers. A
 * top / HUD pass (an axis gizmo, a minimap, a crosshair) is just a pass
 * registered with a higher order than the main view; there is no separate
 * overlay concept. The engine is a pure scheduler: it carries no content
 * scene and no camera state - every registered pass draws the content bound
 * to it explicitly (addPass(pass, content, order)), or nothing when it has
 * none. It owns no camera and forwards no mouse / scroll / key input. The
 * primary interactive view (its camera, content scene and navigation) lives
 * in a SceneView that borrows this engine; the pass presenting that view's
 * camera
 * to the window is simply a registered pass carrying the view's camera with
 * a null render target (see hasWindowPass). The engine is
 * platform-independent and delegates actual drawing to a RenderBackend
 * supplied by the caller via setBackend().
 *
 * The engine may be given a host native window (setWindowHandle) so the
 * backend can attach its render surface to it; the host reports surface
 * resizes via resize() and drives camera input through its SceneView. The
 * engine stays platform-independent and owns none of the window objects.
 */
class V_GRAPHICS_API RenderEngine : public Object, public RefCounted<RenderEngine> {
    V_OBJECT_META_DECL;

  public:
    /** @brief Constructs an empty engine with no backend attached yet.
     *
     * The engine starts with no registered pass; the caller configures the
     * pipeline explicitly (see addPass()) or through a RenderPipelineBuilder.
     * Call setBackend() before initialize().
     */
    RenderEngine();
    ~RenderEngine();

  public:
    /** @brief Gets the bound render backend, or nullptr when unset. */
    raw_ptr<RenderBackend> backend() const;

    /** @brief Sets the render backend used for drawing.
     *
     * The engine keeps a reference to the backend for as long as it is set,
     * so the backend stays alive at least until the engine is destroyed or
     * a different backend (or nullptr) is set. Call before initialize().
     * Setting the same backend instance again is a no-op.
     *
     * @param backend Backend used for drawing, or null to clear.
     */
    void setBackend(intrusive_ptr<RenderBackend> backend);

    /** @brief Initializes the backend.
     *
     * @return true when the backend initialized successfully.
     */
    bool initialize();

    /** @brief Releases backend resources. */
    void shutdown();

    /** @brief Renders one frame (begin, ordered passes, end, swap).
     *
     * Nothing is drawn when no pass is registered. Each registered pass is
     * skipped when disabled (RenderPass::setEnabled); otherwise it resolves
     * its declared inputs, executes (drawing its bound content when set,
     * nothing otherwise), and publishes its named output.
     *
     * @param dt Seconds elapsed since the previous frame (recorded in
     *           frameContext(); reserved for future per-pass updates).
     */
    void frame(double dt = 0.0);

    /** @brief Gets this frame's shared context (elapsed time, surface size).
     *
     * Updated by frame() and the surface size reported via resize(); it is
     * the per-frame data shared across the pass pipeline (to be extended
     * later with previous-frame view-projection matrices and active lights).
     *
     * @return The current frame context.
     */
    const FrameContext& frameContext() const;

    /** @brief Returns whether a pass currently presents @p camera to the
     * window.
     *
     * A pass presents the camera to the window when it is enabled, carries
     * @p camera and renders to the default framebuffer (null render target).
     * A SceneView uses this (see SceneView::ensureWindowPass) to decide
     * whether it must register its default window pass: a pipeline that only
     * registers helper / HUD passes (which draw through their own cameras)
     * still needs one, while a pipeline that already presents the view's
     * camera keeps full control.
     *
     * @param camera The view camera to test for window presentation.
     * @return true when an enabled pass presents @p camera to the window.
     */
    bool hasWindowPass(raw_ptr<Camera> camera) const;

    /** @brief Sets the shading-model preset for scene geometry.
     *
     * Forwarded to the backend before initialize(). Presets without a backend
     * implementation (Pbr / ShadowedPhong) fall back to StandardPhong until
     * their slice lands. The default is StandardPhong.
     *
     * @param preset Shading-model preset.
     */
    void setShaderPreset(ShaderPreset preset);

    /** @brief Gets the shading-model preset. */
    ShaderPreset shaderPreset() const;

    /** @brief Registers a scene render pass executed every frame.
     *
     * Passes run in ascending @p order each frame (equal orders keep
     * insertion order, stable):
     *
     *   - negative orders run first (e.g. a shadow-map pass driven by a light
     *     camera, or a depth / g-buffer pre-pass);
     *   - the pass presenting the primary view to the window (the view's
     *     camera, null render target) conventionally sits at order 0;
     *   - positive orders run after it (e.g. post-processing / compositing);
     *     top / HUD passes register with the highest orders so they draw
     *     last, over every earlier pass.
     *
     * This overload registers @p pass without bound content: a base scene
     * pass then draws nothing, while content-agnostic or self-contained
     * passes (a ScreenPass compositing its inputs, an AxisGizmo HUD) still
     * execute with their own content. Bind a scene with the addPass(pass,
     * content, order) overload. Nothing is auto-registered: without at least
     * one pass the engine draws nothing. Registering the same pass instance
     * twice is ignored.
     *
     * Two DIFFERENT passes may share a camera and an order: the backend keys
     * its retained state by the pass, so they stay separate content (the order
     * only decides their stacking inside the target). Give them distinct
     * orders (and a sub-viewport / clear policy) when they must layer.
     *
     * @param pass  Pass to add (the engine keeps a reference).
     * @param order Execution order (ascending; any integer allowed).
     */
    void addPass(intrusive_ptr<RenderPass> pass, int order);

    /** @brief Registers a scene render pass bound to explicit content.
     *
     * The pass renders @p content each frame. The content association is
     * stored by the engine, not on the pass object, so a RenderPass stays a
     * reusable stage. Registering the same pass instance twice is ignored
     * (use bindPassContent() to change its content).
     *
     * @param pass    Pass to add (the engine keeps a reference).
     * @param content Scene the pass renders (the engine keeps a reference);
     *                null leaves the pass without content (a base scene pass
     *                then draws nothing).
     * @param order   Execution order (ascending; any integer allowed).
     */
    void addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order);

    /** @brief Removes a previously added pass.
     *
     * The pass is dropped from the ordered list. Its backend resources are
     * released: the retained per-pass GPU state (RenderBackend::releasePass,
     * keyed by the pass itself), the legacy window layer keyed by the pass's
     * camera (RenderBackend::releaseWindowLayer), plus any off-screen render
     * target the pass owns (RenderBackend::releaseRenderTarget).
     *
     * @param pass Pass to remove (by pointer).
     */
    void removePass(raw_ptr<RenderPass> pass);

    /** @brief Removes all registered passes.
     *
     * Every registered pass is removed and its backend resources released
     * (the retained per-pass GPU state, the legacy camera-keyed window layer,
     * plus any off-screen render target the pass owns).
     */
    void clearPasses();

    /** @brief Gets the number of registered scene passes.
     *
     * @return Number of passes added via addPass().
     */
    std::size_t passCount() const;

    /** @brief Rebinds which scene a registered pass renders.
     *
     * The binding is managed by the engine, not stored on the pass object.
     *
     * @param pass    Pass registered via addPass() (by pointer).
     * @param content New content scene the pass renders; null clears the
     *                binding (a base scene pass then draws nothing).
     */
    void bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

    /** @brief Gets the content scene a registered pass renders.
     *
     * @param pass Pass registered via addPass() (by pointer).
     * @return The pass's bound content scene, or nullptr when the pass has no
     *         bound content or is not registered.
     */
    raw_ptr<Scene> contentOf(raw_ptr<RenderPass> pass) const;

    /** @brief Publishes a render target under a named output slot.
     *
     * The engine keeps a reference so the target stays alive while published.
     * Passes that declare an output name (RenderPass::setOutputName) publish
     * automatically after they run; this lets application code publish extra
     * producers (e.g. CPU-generated textures) under the same mechanism.
     *
     * @param name   Slot name consumers resolve against.
     * @param target Target to publish (the engine keeps a reference).
     */
    void publish(const String& name, intrusive_ptr<RenderTarget> target);

    /** @brief Looks up a published render target by slot name.
     *
     * @param name Slot name to look up.
     * @return The published target, or nullptr when nothing is published
     *         under @p name.
     */
    raw_ptr<RenderTarget> resolve(const String& name) const;

    /** @brief Removes a published output slot.
     *
     * @param name Slot name to remove (no-op when not published).
     */
    void unpublish(const String& name);

    /** @brief Resizes the rendering surface.
     *
     * Records the surface size in the shared frame context and rebuilds the
     * backend swapchain (only when the backend is initialized). The engine
     * never manages camera / target / pass-viewport layout: those are
     * maintained by their creators on the new surface size (e.g.
     * SceneView::addSurfaceLayout and SceneView::onSurfaceResized for the
     * view camera).
     *
     * @param width  New surface width in pixels.
     * @param height New surface height in pixels.
     */
    void resize(int width, int height);

    /** @brief Supplies the native window the backend attaches to.
     *
     * Stored until initialize(); the backend reads the handle from it to
     * attach its render surface (e.g. a Qt QWindow). The handle value is
     * captured at call time, so the host must call it again before a
     * re-initialize whenever the native window was recreated. Pass nullptr
     * to clear.
     *
     * @param native_handle Native window handle (HWND on Windows), or nullptr.
     */
    void setWindowHandle(void* native_handle);

  private:
    /** @brief Executes a scene pass against the given content scene. */
    void drawScenePass(raw_ptr<RenderPass> pass, raw_ptr<Scene> content);

    /** @brief Resolves a pass's declared inputs from the named-output registry.
     *
     * Called just before executing a pass: for every name in
     * pass->inputNames() the matching published target is handed to
     * pass->resolveInputTextures() (missing names resolve to nullptr).
     *
     * @param pass Pass whose inputs to resolve.
     */
    void resolvePassInputs(raw_ptr<RenderPass> pass);

    /** @brief Publishes a pass's output target under its output name.
     *
     * Called just after executing a pass. When the pass declares a non-empty
     * output name (RenderPass::setOutputName) and renders into a non-null
     * RenderTarget, that target is registered for later consumers.
     *
     * @param pass Pass whose output to publish.
     */
    void publishPassOutput(raw_ptr<RenderPass> pass);

    /** @brief One registered draw slot in the engine's ordered pass list.
     *
     * A slot draws its bound @p content each frame; content may be null (a
     * base scene pass then draws nothing). Disabled passes
     * (RenderPass::setEnabled) are skipped. Top / HUD passes are ordinary
     * slots whose @p order sits above the main view.
     */
    struct Slot {
        intrusive_ptr<RenderPass> pass;
        intrusive_ptr<Scene>      content;   // may be null (draws nothing)
        int                       order = 0;
    };

    // ---- Fields ----

    intrusive_ptr<RenderBackend>        backend_;
    ShaderPreset                        shader_preset_{ ShaderPreset::StandardPhong };
    std::vector<Slot>                   slots_;         // uniform ordered draw registry
    FrameContext                        frame_ctx_;
    void*                               native_handle_      = nullptr;
    bool                                initialized_        = false;

    /// Named-output registry: slot name -> published render target. Cleared at
    /// the start of every frame and rebuilt as the ordered passes publish.
    std::map<String, intrusive_ptr<RenderTarget>> outputs_;
};

V_GRAPHICS_NS_END
