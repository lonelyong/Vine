#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "ShaderProgram.hpp"

VN_GRAPHICS_NS_BEGIN

class AxisGizmo;
class Camera;
class FpsOverlay;
class RenderEngine;
class RenderPass;
class RenderTarget;
class Scene;

/**
 * @brief How a pipeline shades its opaque content.
 *
 * The ONE structural choice a pipeline makes: a forward path draws the content
 * straight into the lit image (one scene pass), a deferred path first writes a
 * G-buffer off-screen and then shades it fullscreen.
 *
 * It is an OPTION rather than a type because the two carry the same behaviour —
 * a list of passes — and differ only in that data; and because the other things
 * a host wants (shadows, screen-space effects) are ORTHOGONAL to it: they add
 * passes and inputs to whichever path is chosen (see PipelineStage), so making
 * the path a type would need one subclass per combination.
 */
enum class ShadingPath {
    Forward,  ///< One window scene pass drawing the content.
    Deferred, ///< A G-buffer pass (offscreen MRT) + a fullscreen lighting pass.
};

/**
 * @brief Where a pass enters the frame's order.
 *
 * A pipeline is a list of passes the engine runs in ascending order, and a pass
 * an EFFECT adds has to enter at the right place without knowing the numbers
 * the passes around it happened to use: a shadow maps belongs before every pass
 * that samples it, a screen-space effect between the geometry that produced its
 * inputs and the shading that consumes them. A stage names that place by
 * intent — pipelineStageOrder() turns it into the number the engine sorts by,
 * and passes inside one stage run in the order they were planned (the engine
 * resolves equal orders by registration order).
 *
 * The scale is documented rather than private so a host that registers its own
 * pass (RenderEngine::addPass) can place it by intent too, instead of guessing
 * where the gaps are.
 */
enum class PipelineStage {
    Depth,       ///< Produces depth for later sampling: shadow maps, depth prepasses.
    Geometry,    ///< Opaque scene content (the G-buffer pass on a deferred path).
    Effect,      ///< Screen-space effects reading Geometry's outputs (e.g. SSAO).
    Shading,     ///< The lit result: the forward content pass, or the deferred lighting pass.
    Transparent, ///< Forward-only content composited over the shaded result.
    Present,     ///< Presents a baked result to the window (deferred + forward content).
    Overlay,     ///< HUD passes drawn over everything the pipeline shaded.
    Preview,     ///< Host / debug preview screens (PiP, attachment previews), above the HUD.
};

/**
 * @brief The order the engine sorts a pass of @p stage by.
 *
 * Effects state where they belong relative to the stage they serve, and this
 * table is the one place that turns intent into the engine's sort key. The
 * bands are spaced so a stage can gain passes later without renumbering any
 * other; a pass inside one stage runs by registration order, so a stage needs
 * exactly one number.
 *
 * @param stage Stage to place.
 * @return The pass order of @p stage.
 */
constexpr int pipelineStageOrder(PipelineStage stage) noexcept
{
    switch (stage)
    {
    case PipelineStage::Depth: return -100;
    case PipelineStage::Geometry: return -40;
    case PipelineStage::Effect: return -20;
    case PipelineStage::Shading: return 0;
    case PipelineStage::Transparent: return 20;
    case PipelineStage::Present: return 40;
    case PipelineStage::Overlay: return 60;
    case PipelineStage::Preview: return 100;
    }
    return 0;
}

/**
 * @brief Optional HUD overlay: a world-orientation axis gizmo.
 *
 * The gizmo is a self-contained HUD pass (AxisGizmo) stacked above the window
 * pass; it mirrors the source camera's orientation each frame. When
 * @ref source_camera is null the overlay is disabled and nothing is added.
 */
struct VN_GRAPHICS_API AxisGizmoOptions {
    /** @brief Camera the gizmo mirrors (e.g. the view's primary camera).
     *
     * Null disables the overlay.
     */
    raw_ptr<Camera> source_camera = nullptr;

    /** @brief Ratio between logical surface size and device pixels.
     *
     * Hosts on high-DPI displays supply their devicePixelRatio (default 1).
     */
    double pixel_ratio = 1.0;

    /** @brief Side length of the square gizmo viewport, in device pixels. */
    int box_size = 96;

    /** @brief Stick length in world units (default 1). */
    double axis_length = 1.0;

    /** @brief Stick cross-section half extent in world units (default 0.09). */
    double thickness = 0.09;

    /** @brief Stacking offset inside the HUD stage (default 10).
     *
     * Relative to the other HUD overlay, not to the scene: the pipeline places
     * every overlay at PipelineStage::Overlay and adds this offset, so the HUD
     * stays above the passes it annotates no matter what orders they took.
     */
    int order = 10;
};

/**
 * @brief Optional HUD overlay: a frame-rate readout in the bottom-right
 * corner.
 *
 * The readout is a self-contained HUD pass (FpsOverlay) stacked above the
 * window pass; it measures the actual render-loop frame rate and needs no
 * source camera. Like the axis gizmo it is opt-in per application — set @ref
 * enabled to true to draw it (the app-shell demo enables it by default).
 */
struct VN_GRAPHICS_API FpsOverlayOptions {
    /** @brief Whether the readout is drawn. Disabled by default. */
    bool enabled = false;

    /** @brief Ratio between logical surface size and device pixels.
     *
     * Hosts on high-DPI displays supply their devicePixelRatio (default 1).
     */
    double pixel_ratio = 1.0;

    /** @brief Readout box width in device pixels (default 105). */
    int width_px = 105;

    /** @brief Readout box height in device pixels (default 36). */
    int height_px = 36;

    /** @brief Stacking offset inside the HUD stage (default 30, above the gizmo). */
    int order = 30;
};

/**
 * @brief Options controlling RenderPipelineBuilder::build().
 *
 * The description of a pipeline: the ONE structural choice (@ref path) plus the
 * per-pass options of the passes the path and the overlays need. Shadows are
 * NOT here — a shadow is requested by the LIGHT that casts it
 * (Light::castShadow) and honoured by the pipeline that draws that light's
 * content, because a light outlives any one pipeline and a scene may be drawn by
 * several (see PipelineStage::Depth and .ai/design/render-pipeline.md §2).
 */
struct VN_GRAPHICS_API PipelineOptions {
    /** @brief How the opaque content is shaded (the structural choice). */
    ShadingPath path = ShadingPath::Forward;

    /** @brief Off-screen G-buffer size in pixels for the Deferred path.
     *
     * 0 (the default) uses the engine's current surface size, falling back to
     * a fixed 640 x 360 when the surface is not known yet. The host keeps the
     * G-buffer in step with the surface via Pipeline::resize() (e.g. from a
     * SceneView surface-layout step).
     */
    int offscreen_width = 0;
    int offscreen_height = 0;

    /** @brief Deferred G-buffer geometry program (one scene traversal -> MRT).
     *
     * Must write the canonical four outputs the builder's G-buffer declares
     * (albedo / view normal + shininess / specular / view position; see the
     * builder docs). Optional: when omitted, RenderPipelineBuilder::build()
     * supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> gbuffer_program;

    /** @brief Deferred fullscreen lighting program (samples the G-buffer).
     *
     * Runs as the window pass and shades from the resolved G-buffer
     * attachments. Optional: when omitted, RenderPipelineBuilder::build()
     * supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> lighting_program;

    /** @brief Optional axis-gizmo HUD overlay stacked above the window pass.
     *
     * Disabled when AxisGizmoOptions::source_camera is null.
     */
    AxisGizmoOptions gizmo;

    /** @brief Optional frame-rate readout HUD overlay (bottom-right corner).
     *
     * Opt-in (like the gizmo); the app-shell demo enables it by default via
     * FpsOverlayOptions::enabled = true.
     */
    FpsOverlayOptions fps;
};

/**
 * @brief The main-window pipeline produced by RenderPipelineBuilder::build().
 *
 * Owns the passes it created AND their registration: it registers them on the
 * engine as they are planned and unregisters them when it dies, so dropping the
 * handle takes the passes out of the frame instead of leaving them to run with
 * nobody holding them (RenderEngine::addPass keeps its own reference, so a
 * reference alone would keep a dropped pipeline's passes alive and drawing).
 *
 * It also keeps the engine ALIVE (a strong reference, not a borrow): the
 * passes live in that engine's list, so "this pipeline is still alive" is
 * exactly "its engine must still be". There is no cycle — the engine does not
 * know its pipelines.
 *
 * Exposes the window-presenting pass, and for deferred paths the off-screen
 * G-buffer, whose size its owner maintains through resize() (the backend
 * rebuilds the off-screen attachments whenever the target size changes between
 * frames).
 */
class VN_GRAPHICS_API Pipeline : public RefCounted<Pipeline> {
    friend class RenderPipelineBuilder;

  public:
    /** @brief Constructs a pipeline that registers its passes on @p engine.
     *
     * The engine is a constructor argument rather than a setter because a
     * pipeline without one could not do the ONE thing this class does with a
     * pass (register it, and later take it back).
     *
     * @param engine Engine the pipeline's passes are registered on (held
     *               strongly; must not be null).
     */
    explicit Pipeline(intrusive_ptr<RenderEngine> engine);

    /** @brief Destroys the pipeline, unregistering its passes from the engine. */
    ~Pipeline();

  public:
    /** @brief Gets the pass presenting the view camera to the window.
     *
     * @return The order-0 window pass (a ScreenPass for the Deferred
     *         presets), or null when nothing was built.
     */
    raw_ptr<RenderPass> windowPass() const;

    /** @brief Gets the off-screen G-buffer of a Deferred pipeline.
     *
     * @return The G-buffer target, or null for the forward presets.
     */
    raw_ptr<RenderTarget> offscreenTarget() const;

    /** @brief Gets the off-screen composite target of a composite pipeline.
     *
     * A Deferred pipeline that also receives forward-only (transparent /
     * overlay) content bakes an off-screen composite target holding the lit
     * opaque image plus the opaque depth the forward content occludes
     * against; the window pass then presents that composite. Null when the
     * pipeline has no transparent content.
     *
     * @return The composite target, or null when none was created.
     */
    raw_ptr<RenderTarget> compositeTarget() const;

    /** @brief Gets the configured axis-gizmo overlay, if any.
     *
     * @return The gizmo HUD pass, or null when no gizmo was configured.
     */
    raw_ptr<AxisGizmo> gizmo() const;

    /** @brief Gets the configured frame-rate overlay, if any.
     *
     * @return The FPS HUD pass, or null when the readout was disabled.
     */
    raw_ptr<FpsOverlay> fpsOverlay() const;

    /** @brief Resizes the off-screen targets and re-anchors the HUD overlays for a new surface size.
     *
     * Creator-maintained sizing: the host calls this on surface changes (e.g. from a
     * SceneView::addSurfaceLayout step) so a deferred G-buffer tracks the window (the backend rebuilds the
     * off-screen attachments on the next frame) and an axis-gizmo overlay stays pinned to its corner.
     *
     * ONE call carries the surface size AND the ratio, because this handle's consumers read the same number
     * in two different spaces: the off-screen targets are sized in DEVICE pixels (they are sampled 1:1 with
     * the swapchain), while the HUD passes are laid out on the LOGICAL surface the host has - they apply the
     * ratio themselves (see AxisGizmo::onSurfaceResized). The host owns the surface, so it is the only party
     * that knows the ratio. Handing both consumers the same number is what this used to do, and it laid a
     * gizmo out on the device size (twice its box on a high-DPI display) until the host undid it with a
     * second call.
     *
     * @param width       New surface width in LOGICAL pixels (<= 0 is ignored), as reported to
     *                    SceneView::onSurfaceResized.
     * @param height      New surface height in LOGICAL pixels (<= 0 is ignored).
     * @param pixel_ratio Device pixel ratio of that surface (<= 0 is treated as 1).
     */
    void resize(int width, int height, double pixel_ratio);

  private:
    /** @brief Registers a pass on the engine and remembers it.
     *
     * The two halves are one call on purpose: a pass registered without being
     * remembered is a pass nothing will ever unregister (the defect this
     * method exists to make impossible).
     *
     * @param pass    Pass to register (the pipeline keeps it alive).
     * @param content Content scene the pass draws, or null for none.
     * @param order   Pass order (see pipelineStageOrder).
     */
    void addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order);

    /** @brief Sets the window-presenting pass. */
    void setWindowPass(intrusive_ptr<RenderPass> pass);

    /** @brief Sets the off-screen G-buffer (Deferred path). */
    void setOffscreenTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the off-screen composite target (Deferred + transparent
     * content). */
    void setCompositeTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the axis-gizmo overlay (optional). */
    void setGizmo(intrusive_ptr<AxisGizmo> gizmo);

    /** @brief Sets the frame-rate overlay (optional). */
    void setFpsOverlay(intrusive_ptr<FpsOverlay> fps);

    // The engine the passes were registered on, held strongly (see the class
    // comment). Declared with a forward declaration and destroyed by the
    // out-of-line destructor, so this header does not pull RenderEngine.hpp in.
    intrusive_ptr<RenderEngine> engine_;
    // A handle, not just a reference: the pipeline owns the registration of
    // every pass here (see addPass) and tears them all down in its destructor.
    std::vector<intrusive_ptr<RenderPass>> passes_;
    intrusive_ptr<RenderPass> window_pass_;
    intrusive_ptr<RenderTarget> offscreen_target_;
    intrusive_ptr<RenderTarget> composite_target_;
    intrusive_ptr<AxisGizmo> gizmo_;
    intrusive_ptr<FpsOverlay> fps_overlay_;
};

VN_GRAPHICS_NS_END
