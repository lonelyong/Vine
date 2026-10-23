#pragma once
#include "graphics_global.hpp"

#include <functional>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "DiagnosticsRoute.hpp"
#include "Node.hpp"
#include "PassRegistry.hpp"
#include "PassStage.hpp"
#include "ReportOnce.hpp"
#include "Scene.hpp"
#include "ShaderProgram.hpp"
#include "ShadingMode.hpp"
#include "ShadingPath.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{
class ChainBuilder;
}  // namespace detail

class AxisGizmo;
class Camera;
class FpsOverlay;
// Declared rather than included: a chain only ever passes a declaration straight on to the engine
// (RenderEngine::addPassOwnedBy), so its own header does not need the one that defines it.
struct PassDesc;
class RenderEngine;
class RenderPass;
class RenderTarget;
class Scene;

/**
 * @brief Optional HUD overlay: a world-orientation axis gizmo.
 *
 * @details
 * - The overlay's CONTENT is an AxisGizmo (its scene, its framing camera, its box) and the recipe declares the pass
 *   that shows it (a HUD-tagged PassDesc - see PassChain::build): it is stacked above the window pass and
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
     * @note Relative to the other HUD overlay, not to the scene: the chain places every overlay at
     *       PassStage::Overlay and adds this offset, so the HUD stays above the passes it annotates no matter
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
 * @brief Options controlling PassChain::build().
 *
 * @details
 * - The description of a chain: WHAT to draw (camera, content, optional transparent scene), the ONE structural choice
 *   (path), and the per-pass options of the passes the path and the overlays need.
 * - Shadows are NOT here — a shadow is requested by the LIGHT that casts it (Light::isCastingShadow) and honoured by the
 *   chain that draws that light's content, because a light outlives any one chain and a scene may be drawn by
 *   several (see PassStage::Depth and .ai/design/graphics-architecture.md §2).
 */
struct VN_GRAPHICS_API ChainDesc {
    /** @brief Camera the produced scene passes frame (borrowed).
     *
     * @note A scene-pass recipe refuses to build without one (null is reported once per build).
     */
    raw_ptr<Camera> camera = nullptr;

    /** @brief Content scene the produced scene passes draw.
     *
     * @note A Deferred request needs one; null leaves the content passes contentless (a forward chain then draws
     *       nothing, a deferred one is refused).
     */
    intrusive_ptr<Scene> content;

    /** @brief Optional forward-only (transparent / overlay) scene composited over the lit result.
     *
     * @details
     * - When set, the produced chain also renders this scene as transparent content composited over the lit opaque
     *   content WITH depth occlusion (alpha-blended geometry half behind the opaque scene is clipped correctly)
     *   instead of being drawn depth-less on top:
     *   - Forward: a depth-on pass stacked right after the main content pass in the same window render pass.
     *   - Deferred: the lighting pass shades into an off-screen composite target together with the opaque depth; this
     *     scene is drawn depth-on over it and the composite is presented to the window.
     * - The scene is lit by its own lights (like any forward pass).
     */
    intrusive_ptr<Scene> transparent;

    /** @brief How the opaque content is shaded (the structural choice). */
    ShadingPath path = ShadingPath::Forward;

    /** @brief What the picture MEANS (the viewing mode the chain is built for; see ShadingMode).
     *
     * @details
     * - A mode is a pass property, so this is not a second structural knob: what it decides at BUILD time is whether
     *   the chain has to be assembled differently to be able to express it - a non-Lit mode needs the geometry pass,
     *   so a Deferred request is built FORWARD for it (see effectivePath).
     * - Switching among the modes the built chain CAN express is one override on a live chain
     *   (PassChain::setShadingMode).
     */
    ShadingMode mode = ShadingMode::Lit;

    /** @brief Off-screen G-buffer size in pixels for the Deferred path.
     *
     * @details
     * - 0 (the default) uses the engine's current surface size, falling back to a fixed 640 x 360 when the surface is
     *   not known yet.
     * - The host keeps the G-buffer in step with the surface via PassChain::resize() (e.g. from a SceneView
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
     * - Optional: when omitted, PassChain::build() supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> gbuffer_program;

    /** @brief Deferred fullscreen lighting program (samples the G-buffer).
     *
     * @details
     * - Runs as the window pass and shades from the resolved G-buffer attachments.
     * - Optional: when omitted, PassChain::build() supplies its built-in temporary default program.
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
 * @brief The chain of one picture, built by PassChain::build().
 *
 * @details
 * - Owns the passes it created AND their registration: it registers them on the engine as they are planned and
 *   unregisters them when it dies, so dropping the handle takes the passes out of the frame instead of leaving them
 *   to run with nobody holding them (RenderEngine::addPass keeps its own reference, so a reference alone would keep a
 *   dropped pipeline's passes alive and drawing).
 * - It also BORROWS the engine through two NARROW handles rather than owning it (see PassRegistry /
 *   DiagnosticsRoute): the passes live in the engine's table, so the chain must be able to reach it, but keeping the
 *   engine alive from below would invert ownership - a released engine would live on for as long as any chain it
 *   once handed a pass to. The handles are leases, so the engine dies exactly when its owners drop it, and a chain
 *   that outlives it answers "gone" (isValid()) and takes its passes with it instead of calling into freed memory.
 * - Exposes the window-presenting pass, and for deferred paths the off-screen G-buffer, whose size its owner
 *   maintains through resize() (the backend rebuilds the off-screen attachments whenever the target size changes
 *   between frames).
 *
 * @note
 * - This is a PASS CHAIN and its registration, not a graphics-API pipeline state object (PSO): the GPU pipeline
 *   variants are the backend's (see `VariantPool.hpp` / `PipelineKey`), and a host never builds or names one
 *   here. The name follows the FrameGraph / RDG sense: a compiled chain of passes (the industry term, not
 *   the retired backend type).
 */
class VN_GRAPHICS_API PassChain : public Object, public RefCounted<PassChain> {
    friend class detail::ChainBuilder;

  public:
    /** @brief Constructs a chain that registers its passes through @p registry and reports through @p diagnostics.
     *
     * @details
     * - The handles are constructor arguments rather than setters because a chain without them could not do the ONE
     *   thing this class does with a pass (register it, and later take it back) nor say what it refused.
     * - They are leases, not owners: the chain must reach the engine, it must not keep it alive (see the class
     *   note). Handles of an engine that is already gone mint a chain that registers nothing - the caller's own bug,
     *   and a harmless one.
     *
     * @param registry    Handle over the engine's one pass table (empty is accepted; nothing is registered then).
     * @param diagnostics Handle over the engine's one diagnostic route (empty silences the chain's refusals).
     */
    PassChain(PassRegistry registry, DiagnosticsRoute diagnostics);

    /** @brief Destroys the chain: it hands every pass it registered back to the engine while the engine is still
     *         there, and answers nothing once it is gone. */
    ~PassChain();
  public:
    /** @brief Builds the chain @p desc describes, registering its passes on @p engine.
     *
     * @details
     * - The ONE build entry: the request is a value (ChainDesc) and the result is a handle that owns the
     *   registration - dropping it takes its passes out of the frame (see the class note). No separate builder
     *   object has to be kept alive or kept in step with the request.
     * - A build that cannot assemble the requested picture (a Deferred path without camera/content, or one whose
     *   shading mode needs the geometry pass and therefore builds the forward path) answers per the same rules as
     *   before: `effectivePath()` says what will be built, and a refusal answers null after reporting once.
     *
     * @param engine Engine the passes are registered on (null answers null: nothing could be built).
     * @param desc   What to build (see ChainDesc).
     * @return The built chain, or null when the request cannot be assembled on @p engine.
     */
    [[nodiscard]] static intrusive_ptr<PassChain> build(raw_ptr<RenderEngine> engine, const ChainDesc& desc);

    /** @brief The chain a build with @p desc assembles, given what the requested mode needs.
     *
     * @details
     * - A shading mode is a GEOMETRY-stage override, and a Deferred picture comes out of a fullscreen lighting pass
     *   that has no geometry left to shade - so a non-Lit mode can only be expressed by the FORWARD chain, and a
     *   Deferred request carrying one is built forward for it (the requested path in @p desc is untouched: switching
     *   back to Lit builds deferred again).
     * - Public because it is the rule a host reasons about: `desc.path` is what a host ASKS for, this is what it
     *   gets, and PassChain::shadingPath() answers which one a live chain was built as.
     *
     * @param desc The chain request.
     * @return The chain the build assembles.
     */
    [[nodiscard]] static ShadingPath effectivePath(const ChainDesc& desc) noexcept;
    /** @brief Gets whether the engine this chain registered on is still alive.
     *
     * @details
     * - A chain a host kept after dropping the control that owned the engine answers false: its passes went with
     *   the engine, and every query below answers nothing instead of reaching into freed memory.
     *
     * @return true while the handles still reach their engine.
     */
    [[nodiscard]] bool isValid() const noexcept;

    /** @brief Gets whether any pass of this chain draws @p scene.
     *
     * @details
     * - "This chain draws that content" is a property of the CHAIN, not of its window pass: a deferred recipe draws
     *   the scene in its G-buffer pass and shows the result through a screen pass, so asking the window pass would
     *   answer no for every deferred picture.
     * - It is what a view checks when it adopts a chain (see SceneView::setChain): a chain built for one
     *   scene and adopted by a view holding another draws nothing the view has, and nothing downstream can see it
     *   (each pass is individually legal).
     *
     * @param scene Content scene to look for (null draws nothing, so the answer is false).
     * @return true when at least one registered pass of this chain has @p scene as its content.
     */
    [[nodiscard]] bool draws(raw_ptr<const Scene> scene) const noexcept;
    /** @brief Gets the pass presenting the picture to the window.
     *
     * @details
     * - "Presenting" is the pass whose output is what the window shows, which is not necessarily the pass that draws
     *   the content: a Deferred chain bakes the picture off-screen and presents it through a screen pass, so a host
     *   that wants "the picture pass" (to override its program, see setShadingMode) must ask this, not the content.
     * - It is set by the recipe (PassChain::build), because only the recipe knows what its chain ends with.
     *
     * @return The presenting pass (the order-0 window pass, or a ScreenPass for the Deferred presets), or null when
     *         nothing was built.
     */
    raw_ptr<RenderPass> presentingPass() const;

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

    /** @brief How an overlay lays itself out for a surface size (see addOverlay).
     *
     * @details
     * - The pass is handed over so one layout can restate both halves: the content's own box (a gizmo's framing
     *   camera) and the rectangle the pass draws into.
     */
    using OverlayLayout = std::function<void(RenderPass& pass, int width, int height, double pixel_ratio)>;

    /** @brief Adds a HUD overlay: a pass registered under @p role, plus the content that lays itself out.
     *
     * @details
     * - An overlay is a VALUE, not a subclass: what draws it (a HUD pass, registered under @p role - see
     *   PassChain::addPass), what it IS (@p content, an Object - the half a pass cannot hold: a gizmo's
     *   framing camera and box, a readout's digits), and how it is laid out. A THIRD overlay is therefore a call to
     *   this method, not a field on this class; the two the recipe builds are gizmo() and fpsOverlay().
     * - Ownership stays where it already is: whoever registers an overlay keeps its content alive (through
     *   @p layout's capture and through the pass' before-draw hook), so this class holds a non-owning @p content.
     * - @p layout runs from resize() with the surface size and the ratio; empty means the pass states its own
     *   rectangle and nothing has to be re-laid-out.
     *
     * @param role    Name the overlay's pass is registered under - also the key overlayPass/overlayContent look up
     *                (not enforced unique: the first match answers).
     * @param content The overlay's content object, or null for a pass with no content half.
     * @param desc    Declaration of the HUD pass that draws it.
     * @param layout  Lays the overlay out for a surface size (see OverlayLayout).
     * @return The registered pass, or null when the engine is gone or that pass was already registered.
     */
    raw_ptr<RenderPass> addOverlay(const String&   role,
                                   raw_ptr<Object> content,
                                   const PassDesc& desc,
                                   OverlayLayout   layout = {});

    /** @brief Gets an overlay's pass (see addOverlay).
     *
     * @param role Role the overlay was registered under.
     * @return The HUD pass - whose enabled flag IS the overlay's visibility - or null when this pipeline has no
     *         overlay with that role.
     */
    [[nodiscard]] raw_ptr<RenderPass> overlayPass(const String& role) const noexcept;

    /** @brief Gets an overlay's CONTENT object (see addOverlay).
     *
     * @details
     * - Non-owning and untyped: a caller that registered it casts it (`obj_cast<AxisGizmo>`), which is why the two
     *   overlays the recipe builds also have typed accessors (gizmo(), fpsOverlay()).
     *
     * @param role Role the overlay was registered under.
     * @return The content object, or null when there is no overlay with that role (or it has no content half).
     */
    [[nodiscard]] raw_ptr<Object> overlayContent(const String& role) const noexcept;

    /** @brief Shows or hides an overlay (a switch, never a rebuild).
     *
     * @details
     * - An overlay's EXISTENCE is the decision of whoever registered it (addOverlay); this is the runtime switch over
     *   that registration - the pass' enabled flag, the one state a reader sees (see isOverlayVisible).
     * - A role this pipeline has no overlay for answers false and says so once per episode: a switch that silently
     *   did nothing would leave a host believing the overlay was just hidden.
     *
     * @param role Role the overlay was registered under.
     * @param on   True to show the overlay, false to hide it.
     * @return true when this pipeline HAS that overlay (its visibility is now @p on); false when it has none.
     */
    bool setOverlayVisible(const String& role, bool on);

    /** @brief Gets whether an overlay is currently visible (see setOverlayVisible).
     *
     * @param role Role the overlay was registered under.
     * @return true when the overlay exists and its pass is enabled.
     */
    [[nodiscard]] bool isOverlayVisible(const String& role) const noexcept;

    /** @brief Gets the axis-gizmo overlay's CONTENT (the typed view of the recipe's gizmo overlay).
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

    /** @brief Gets the shading path this chain was built as (see PassChain::effectivePath).
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
     *   with ChainDesc::mode, which builds the chain that can express it - see effectivePath).
     * - The false answer is reported once per episode, because a host that ignored it would be looking at a picture
     *   that did not change and would call that "switched".
     *
     * @param mode Mode to draw with.
     * @return true when this chain's picture now means @p mode (its picture pass was overridden); false when this
     *         chain cannot express it (nothing changed - rebuild with ChainDesc::mode instead).
     */
    bool setShadingMode(ShadingMode mode);

    /** @brief Shows or hides the axis-gizmo overlay (see setOverlayVisible).
     *
     * @param on True to show the overlay, false to hide it.
     * @return true when this chain HAS the overlay (its visibility is now @p on); false when it has none - build
     *         with ChainDesc::gizmo to get one.
     */
    bool setGizmoVisible(bool on);

    /** @brief Shows or hides the frame-rate readout (see setOverlayVisible).
     *
     * @param on True to show the readout, false to hide it.
     * @return true when this chain HAS the readout; false when it has none (ChainDesc::fps).
     */
    bool setStatsVisible(bool on);

    /** @brief Gets whether the axis-gizmo overlay is currently visible (see isOverlayVisible).
     *
     * The overlay pass is the single source of truth (its enabled flag), so this stays in step with
     * setGizmoVisible and with a host that toggles gizmoPass() itself.
     *
     * @return true when the overlay exists and is enabled.
     */
    [[nodiscard]] bool isGizmoVisible() const noexcept;

    /** @brief Gets whether the frame-rate readout is currently visible (see isOverlayVisible).
     *
     * @return true when the readout exists and is enabled.
     */
    [[nodiscard]] bool isStatsVisible() const noexcept;

  private:
    /** @brief Registers a DECLARED pass on the engine under this chain's ownership.
     *
     * @details
     * - The declaration says what each pass IS (its kind, stage, tag, target, inputs), the engine makes the node, and
     *   the row records this chain as its owner - so the recipe reads as declarations while the unregistration stays
     *   one owner's job (see the destructor).
     *
     * @param desc The declaration (see PassDesc).
     * @param role Name the pass is registered under, so a reader can find it again (see gizmoPass); empty for the
     *             passes nothing asks for by name.
     * @return The node the declaration became, or null when that pass was already registered.
     */
    intrusive_ptr<RenderPass> addPass(const PassDesc& desc, const String& role = String());

    /** @brief Sets the window-presenting pass. */
    void setWindowPass(intrusive_ptr<RenderPass> pass);

    /** @brief Sets the off-screen G-buffer (Deferred path). */
    void setOffscreenTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the off-screen composite target (Deferred + transparent content). */
    void setCompositeTarget(intrusive_ptr<RenderTarget> target);

    /** @brief One HUD overlay: the role its pass is registered under, its content, and how it lays itself out. */
    struct Overlay
    {
        String          role;
        raw_ptr<Object> content{ nullptr };
        OverlayLayout   layout;
    };

    /** @brief Role the axis-gizmo HUD pass is registered under. */
    inline static constexpr const char8_t kGizmoRole[] = u8"gizmo";

    /** @brief Role the frame-rate readout pass is registered under. */
    inline static constexpr const char8_t kStatsRole[] = u8"stats";

    // The engine the passes were registered on, held strongly (see the class
    // comment). Declared with a forward declaration and destroyed by the
    // out-of-line destructor, so this header does not pull RenderEngine.hpp in.
    // The two narrow contracts this chain depends on (see the class note). Both carry a lease, so the engine may
    // die first and the chain answers "gone" instead of reaching into freed memory.
    PassRegistry    registry_;
    DiagnosticsRoute diagnostics_;
    // Every pass this chain registered lives in the engine's own slot table with `owner = this` (see
    // RenderEngine::addPassOwnedBy), so "a pass the chain registered is a pass the chain unregisters" needs no second
    // list here (see the destructor) and a named pass is found by its role instead of by a field per pass.
    intrusive_ptr<RenderPass> window_pass_;
    intrusive_ptr<RenderTarget> offscreen_target_;
    intrusive_ptr<RenderTarget> composite_target_;
    // The overlays, as VALUES (see addOverlay): a role (which is also the name the pass is registered under), the
    // content object and its layout. No field per overlay and no pass handle here - the pass lives in the group and is
    // found by role, and the content stays alive with whoever registered it.
    std::vector<Overlay> overlays_;
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
