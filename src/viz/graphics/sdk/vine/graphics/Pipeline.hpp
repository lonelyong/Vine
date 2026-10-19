#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Node.hpp"
#include "ReportOnce.hpp"
#include "ShaderProgram.hpp"
#include "ShadingMode.hpp"
#include "ShadingPath.hpp"

VN_GRAPHICS_NS_BEGIN

class AxisGizmo;
class Camera;
class FpsOverlay;
// Declared rather than included: PassDesc is a declaration OF a pass, so it includes this header for
// PipelineStage, and a pipeline only ever passes one straight on to the engine (Pipeline::addPass).
struct PassDesc;
class RenderEngine;
class RenderPass;
class RenderTarget;
class Scene;

/**
 * @brief Where a pass enters the frame's order.
 *
 * @details
 * - A pipeline is a list of passes the engine runs in ascending order, and a pass an EFFECT adds has to enter at the
 *   right place without knowing the numbers the passes around it happened to use: a shadow map belongs before every
 *   pass that samples it, a screen-space effect between the geometry that produced its inputs and the shading that
 *   consumes them.
 * - A stage names that place by intent — pipelineStageOrder() turns it into the number the engine sorts by, and
 *   passes inside one stage run in the order they were planned (the engine resolves equal orders by registration
 *   order).
 * - The scale is documented rather than private so a host that registers its own pass (RenderEngine::addPass) can
 *   place it by intent too, instead of guessing where the gaps are.
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
 * @details
 * - Effects state where they belong relative to the stage they serve, and this table is the one place that turns
 *   intent into the engine's sort key.
 * - The bands are spaced so a stage can gain passes later without renumbering any other; a pass inside one stage runs
 *   by registration order, so a stage needs exactly one number.
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
 * @details
 * - The overlay's CONTENT is an AxisGizmo (its scene, its framing camera, its box) and the recipe declares the pass
 *   that shows it (a HUD-tagged PassDesc - see RenderPipelineBuilder): it is stacked above the window pass and
 *   mirrors the source camera's orientation each frame.
 * - When source_camera is null the overlay is disabled and nothing is added.
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
     * @note Relative to the other HUD overlay, not to the scene: the pipeline places every overlay at
     *       PipelineStage::Overlay and adds this offset, so the HUD stays above the passes it annotates no matter
     *       what orders they took.
     */
    int order = 10;
};

/**
 * @brief Optional HUD overlay: a frame-rate readout in the bottom-right corner.
 *
 * @details
 * - The readout's CONTENT is an FpsOverlay and the recipe declares the pass that shows it (a HUD-tagged PassDesc),
 *   stacked above the window pass; it measures the actual render-loop frame rate and needs no source camera.
 * - Like the axis gizmo it is opt-in per application — set enabled to true to draw it (the app-shell demo enables it
 *   by default).
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
 * @details
 * - The description of a pipeline: the ONE structural choice (path) plus the per-pass options of the passes the path
 *   and the overlays need.
 * - Shadows are NOT here — a shadow is requested by the LIGHT that casts it (Light::castShadow) and honoured by the
 *   pipeline that draws that light's content, because a light outlives any one pipeline and a scene may be drawn by
 *   several (see PipelineStage::Depth and .ai/design/graphics-pipeline.md §2).
 */
struct VN_GRAPHICS_API PipelineOptions {
    /** @brief How the opaque content is shaded (the structural choice). */
    ShadingPath path = ShadingPath::Forward;

    /** @brief What the picture MEANS (the viewing mode the pipeline is built for; see ShadingMode).
     *
     * @details
     * - A mode is a pass property, so this is not a second structural knob: what it decides at BUILD time is whether
     *   the chain has to be assembled differently to be able to express it - a non-Lit mode needs the geometry pass,
     *   so a Deferred request is built FORWARD for it (see effectivePath).
     * - Switching among the modes the built chain CAN express is one override on a live pipeline
     *   (Pipeline::setShadingMode).
     */
    ShadingMode mode = ShadingMode::Lit;

    /** @brief Off-screen G-buffer size in pixels for the Deferred path.
     *
     * @details
     * - 0 (the default) uses the engine's current surface size, falling back to a fixed 640 x 360 when the surface is
     *   not known yet.
     * - The host keeps the G-buffer in step with the surface via Pipeline::resize() (e.g. from a SceneView
     *   surface-layout step).
     */
    int offscreen_width = 0;
    int offscreen_height = 0;

    /** @brief Which layers of the CONTENT the built chain draws (see Node::setLayers / RenderPass::setLayerMask).
     *
     * @details
     * - Stamped on every pass of the chain that walks this content (shadow, forward opaque, forward transparent,
     *   G-buffer, deferred transparent, preview): one recipe, one mask, so "this chain draws only casters" is a
     *   property of the recipe rather than of each pass the builder happens to create.
     * - The two fullscreen lighting passes are NOT stamped - their content scene is where their lights come from, not
     *   something they walk - and neither are the HUD / gizmo overlays: they are UI, and a content selection must not
     *   hide the reader's own tools.
     * - kAllLayers (the default) draws every layer, which is what a host that never heard of layers gets. A mask of 0
     *   draws no content at all (reported once as a frame-plan problem).
     */
    std::uint32_t layer_mask = kAllLayers;

    /** @brief Deferred G-buffer geometry program (one scene traversal -> MRT).
     *
     * @details
     * - Must write the canonical four outputs the builder's G-buffer declares (albedo / view normal + shininess /
     *   specular / view position; see the builder docs).
     * - Optional: when omitted, RenderPipelineBuilder::build() supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> gbuffer_program;

    /** @brief Deferred fullscreen lighting program (samples the G-buffer).
     *
     * @details
     * - Runs as the window pass and shades from the resolved G-buffer attachments.
     * - Optional: when omitted, RenderPipelineBuilder::build() supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> lighting_program;

    /** @brief Optional axis-gizmo HUD overlay stacked above the window pass.
     *
     * @note Disabled when AxisGizmoOptions::source_camera is null.
     */
    AxisGizmoOptions gizmo;

    /** @brief Optional frame-rate readout HUD overlay (bottom-right corner).
     *
     * @note Opt-in (like the gizmo); the app-shell demo enables it by default via FpsOverlayOptions::enabled = true.
     */
    FpsOverlayOptions fps;
};

/**
 * @brief The main-window pipeline produced by RenderPipelineBuilder::build().
 *
 * @details
 * - Owns the passes it created AND their registration: it registers them on the engine as they are planned and
 *   unregisters them when it dies, so dropping the handle takes the passes out of the frame instead of leaving them
 *   to run with nobody holding them (RenderEngine::addPass keeps its own reference, so a reference alone would keep a
 *   dropped pipeline's passes alive and drawing).
 * - It also keeps the engine ALIVE (a strong reference, not a borrow): the passes live in that engine's list, so
 *   "this pipeline is still alive" is exactly "its engine must still be". There is no cycle — the engine does not
 *   know its pipelines.
 * - Exposes the window-presenting pass, and for deferred paths the off-screen G-buffer, whose size its owner
 *   maintains through resize() (the backend rebuilds the off-screen attachments whenever the target size changes
 *   between frames).
 *
 * @note
 * - This is a PASS CHAIN and its registration, not a graphics-API pipeline state object (PSO): the GPU pipeline
 *   variants are the backend's (see `VariantPool.hpp` / `PipelineKey`), and a host never builds or names one
 *   here. "Pipeline" is used in the FrameGraph / RDG sense: a compiled chain of passes.
 */
class VN_GRAPHICS_API Pipeline : public RefCounted<Pipeline> {
    friend class RenderPipelineBuilder;

  public:
    /** @brief Constructs a pipeline that registers its passes on @p engine.
     *
     * @details
     * - The engine is a constructor argument rather than a setter because a pipeline without one could not do the ONE
     *   thing this class does with a pass (register it, and later take it back).
     *
     * @param engine Engine the pipeline's passes are registered on (held strongly; must not be null).
     */
    explicit Pipeline(intrusive_ptr<RenderEngine> engine);

    /** @brief Destroys the pipeline, unregistering its passes from the engine. */
    ~Pipeline();

  public:
    /** @brief Gets the pass presenting the view camera to the window.
     *
     * @return The order-0 window pass (a ScreenPass for the Deferred presets), or null when nothing was built.
     */
    raw_ptr<RenderPass> windowPass() const;

    /** @brief Gets the off-screen G-buffer of a Deferred pipeline.
     *
     * @return The G-buffer target, or null for the forward presets.
     */
    raw_ptr<RenderTarget> offscreenTarget() const;

    /** @brief Gets the off-screen composite target of a composite pipeline.
     *
     * @details
     * - A Deferred pipeline that also receives forward-only (transparent / overlay) content bakes an off-screen
     *   composite target holding the lit opaque image plus the opaque depth the forward content occludes against; the
     *   window pass then presents that composite.
     * - Null when the pipeline has no transparent content.
     *
     * @return The composite target, or null when none was created.
     */
    raw_ptr<RenderTarget> compositeTarget() const;

    /** @brief Gets the configured axis-gizmo overlay's CONTENT, if any.
     *
     * @details
     * - The object holds the half a pass cannot (its scene, its framing camera and the box it is laid out in - see
     *   AxisGizmo); the pass that draws it is gizmoPass().
     * - A reader that wants to know whether the gizmo is ON (and that can toggle it) wants the pass; a caller that
     *   moves or reshapes it wants this.
     *
     * @return The gizmo, or null when no gizmo was configured.
     */
    raw_ptr<AxisGizmo> gizmo() const;

    /** @brief Gets the pass that draws the axis gizmo (see gizmo()).
     *
     * @return The HUD pass (its state is the gizmo's visible/hidden - RenderPass::setEnabled), or null when no
     *         gizmo was configured.
     */
    raw_ptr<RenderPass> gizmoPass() const;

    /** @brief Gets the configured frame-rate overlay's CONTENT, if any (see gizmo()).
     *
     * @return The readout, or null when the readout was disabled.
     */
    raw_ptr<FpsOverlay> fpsOverlay() const;

    /** @brief Gets the pass that draws the frame-rate readout (see fpsOverlay()).
     *
     * @return The HUD pass, or null when the readout was disabled.
     */
    raw_ptr<RenderPass> fpsOverlayPass() const;

    /** @brief Resizes the off-screen targets and re-anchors the HUD overlays for a new surface size.
     *
     * @details
     * - Creator-maintained sizing: the host calls this on surface changes (e.g. from a SceneView::addSurfaceLayout
     *   step) so a deferred G-buffer tracks the window (the backend rebuilds the off-screen attachments on the next
     *   frame) and an axis-gizmo overlay stays pinned to its corner.
     * - The overlays own their box (the object computes it from the surface size and the area origin it was pointed
     *   at); the BOX the pass states is restated here, which is the one place an overlay is laid out for a new
     *   surface size (see gizmoPass).
     * - ONE call carries the surface size AND the ratio, because this handle's consumers read the same number in two
     *   different spaces: the off-screen targets are sized in DEVICE pixels (they are sampled 1:1 with the
     *   swapchain), while the HUD passes are laid out on the LOGICAL surface the host has - they apply the ratio
     *   themselves (see AxisGizmo::onSurfaceResized).
     * - The host owns the surface, so it is the only party that knows the ratio. Handing both consumers the same
     *   number is what this used to do, and it laid a gizmo out on the device size (twice its box on a high-DPI
     *   display) until the host undid it with a second call.
     *
     * @param width       New surface width in LOGICAL pixels (<= 0 is ignored), as reported to SceneView::resize.
     * @param height      New surface height in LOGICAL pixels (<= 0 is ignored).
     * @param pixel_ratio Device pixel ratio of that surface (<= 0 is treated as 1).
     */
    void resize(int width, int height, double pixel_ratio);

    /** @brief Gets the chain this pipeline was built as (see RenderPipelineBuilder::effectivePath).
     *
     * @details
     * - The EFFECTIVE path, not the requested one: a Deferred request carrying a non-Lit mode is built Forward, and
     *   this answers what was built - which is what "can this chain express that mode" is decided on (see
     *   setShadingMode).
     *
     * @return The path the chain was assembled as.
     */
    [[nodiscard]] ShadingPath shadingPath() const noexcept;

    /** @brief Gets the mode this pipeline's picture is currently drawn with.
     *
     * @return The current mode (Lit until one is set - a fresh pipeline is the full shading).
     */
    [[nodiscard]] ShadingMode shadingMode() const noexcept;

    /** @brief Sets what the picture MEANS on a LIVE pipeline (see ShadingMode).
     *
     * @details
     * - The picture pass' program override (and, for Wireframe, its rasterisation mode) - a pass property, so a mode
     *   this chain can express costs one override and NOT a rebuild: the next frame draws differently with nothing
     *   re-registered.
     * - A mode this chain CANNOT express is answered false and changes nothing: a Deferred picture comes out of a
     *   fullscreen lighting pass with no geometry left to shade, so its non-Lit modes have to be ASSEMBLED for (build
     *   with PipelineOptions::mode, which builds the chain that can express it - see effectivePath).
     * - The false answer is reported once per episode, because a host that ignored it would be looking at a picture
     *   that did not change and would call that "switched".
     *
     * @param mode Mode to draw with.
     * @return true when this pipeline's picture now means @p mode (its picture pass was overridden); false when this
     *         chain cannot express it (nothing changed - rebuild with PipelineOptions::mode instead).
     */
    bool setShadingMode(ShadingMode mode);

    /** @brief Shows or hides the axis-gizmo overlay (a switch, never a rebuild).
     *
     * @details
     * - The overlay's EXISTENCE is a build decision (PipelineOptions::gizmo); this is the runtime switch over it (the
     *   pass' enabled flag - the one state a reader sees, see isGizmoVisible).
     * - A pipeline with no gizmo answers false and says so once per episode: a switch that silently did nothing would
     *   leave a host believing the overlay was just hidden.
     *
     * @param on True to show the overlay, false to hide it.
     * @return true when this pipeline HAS the overlay (its visibility is now @p on); false when it has none - build
     *         with PipelineOptions::gizmo to get one.
     */
    bool setGizmoVisible(bool on);

    /** @brief Shows or hides the frame-rate readout (same rules as setGizmoVisible).
     *
     * @param on True to show the readout, false to hide it.
     * @return true when this pipeline HAS the readout; false when it has none (PipelineOptions::fps).
     */
    bool setStatsVisible(bool on);

    /** @brief Gets whether the axis-gizmo overlay is currently visible.
     *
     * The overlay pass is the single source of truth (its enabled flag), so this stays in step with
     * setGizmoVisible and with a host that toggles gizmoPass() itself.
     *
     * @return true when the overlay exists and is enabled.
     */
    [[nodiscard]] bool isGizmoVisible() const noexcept;

    /** @brief Gets whether the frame-rate readout is currently visible (see isGizmoVisible). */
    [[nodiscard]] bool isStatsVisible() const noexcept;

  private:
    /** @brief Registers a pass on the engine and remembers it.
     *
     * @details
     * - The two halves are one call on purpose: a pass registered without being remembered is a pass nothing will
     *   ever unregister (the defect this method exists to make impossible).
     *
     * @param pass    Pass to register (the pipeline keeps it alive).
     * @param content Content scene the pass draws, or null for none.
     * @param order   Pass order (see pipelineStageOrder).
     */
    void addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order);

    /** @brief Registers a DECLARED pass on the engine and remembers what it became.
     *
     * @details
     * - The declared half of addPass() above, for the recipes: the declaration says what each pass IS (its kind,
     *   stage, tag, target, inputs), the engine makes the node, and the pipeline keeps it - so the recipe reads as
     *   declarations while the pipeline holds the same one handle it always did (see RenderEngine::addPass).
     *
     * @param desc The declaration (see PassDesc).
     * @return The node the declaration became, or null when that pass was already registered.
     */
    intrusive_ptr<RenderPass> addPass(const PassDesc& desc);

    /** @brief Sets the window-presenting pass. */
    void setWindowPass(intrusive_ptr<RenderPass> pass);

    /** @brief Sets the off-screen G-buffer (Deferred path). */
    void setOffscreenTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the off-screen composite target (Deferred + transparent content). */
    void setCompositeTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the axis-gizmo overlay and the pass that draws it (optional). */
    void setGizmo(intrusive_ptr<AxisGizmo> gizmo, intrusive_ptr<RenderPass> pass);

    /** @brief Sets the frame-rate overlay and the pass that draws it (optional). */
    void setFpsOverlay(intrusive_ptr<FpsOverlay> fps, intrusive_ptr<RenderPass> pass);

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
    // The declarations that draw them: the overlay object owns the CONTENT (its scene, camera, box), these are
    // what the frame runs and what carries "is it on" (see gizmo()/gizmoPass()).
    intrusive_ptr<RenderPass> gizmo_pass_;
    intrusive_ptr<RenderPass> fps_overlay_pass_;
    // The chain this pipeline was built as (the EFFECTIVE one) and the mode its picture means: what
    // setShadingMode decides "can this be expressed" on, and what a rebuild would start from. Set by the
    // builder, which is the only thing that builds chains.
    ShadingPath path_{ShadingPath::Forward};
    ShadingMode mode_{ShadingMode::Lit};
    // "This chain cannot express that mode" is one condition, not a per-call event (see setShadingMode), and so is
    // "this chain has no such overlay" (see setGizmoVisible / setStatsVisible).
    ReportOnce mode_refused_;
    ReportOnce overlay_refused_;
};

VN_GRAPHICS_NS_END
