#pragma once
#include "graphics_global.hpp"

#include <functional>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/window/window_global.hpp>

#include "RenderPipeline.hpp"
#include "Viewport.hpp"

VN_WINDOW_NS_BEGIN
struct KeyEvent;
struct MouseEvent;
struct ScrollEvent;
VN_WINDOW_NS_END

VN_GRAPHICS_NS_BEGIN

class Camera;
class CameraManipulator;
class Pipeline;
class RenderEngine;
class RenderPass;
class Scene;

/**
 * @brief Host-agnostic interactive primary view: camera + content + navigation.
 *
 * SceneView bundles everything that makes up one interactive 3D view into a
 * single host-independent object:
 *
 *   - a Camera (the view / projection of this view),
 *   - a content Scene (the root drawn by this view),
 *   - a CameraManipulator (default: an orbit manipulator bound to the camera
 *     and scene) that receives window input, and
 *   - viewport bookkeeping: it keeps the camera projection aspect in step with
 *     the rendering surface as the host reports resizes.
 *
 * A SceneView is not a RenderEngine. It borrows one (setEngine): the engine
 * remains the pure pass scheduler / backend owner, while the view owns the
 * "primary view state" a camera used to be given by the engine. The engine
 * holds no content scene: the view keeps its content and binds it explicitly
 * to the passes that draw it (its default window pass binds it on
 * ensureWindowPass()). When attached, ensureWindowPass() registers an
 * order-0 window pass that presents this view's camera to the backbuffer -
 * unless the application already registered such a pass (e.g. a
 * deferred-lighting main pass that carries this view's camera).
 *
 * Several views may share one engine, but each view's DEFAULT window pass
 * clears the whole surface and draws full-surface: with two such passes the
 * second erases the first. Side-by-side views therefore need explicit layout -
 * give each pass a sub-viewport (RenderPass::setViewport) and disable clearing
 * on all but one (setClearEnabled), or build the pipeline yourself with
 * RenderPipelineBuilder. The engine never manages viewport layout.
 *
 * The engine stays camera- and content-agnostic: it forwards no mouse /
 * scroll / key input, holds no camera and no content scene. The host pushes
 * window input and surface resizes to the view (pushEvent /
 * onSurfaceResized), which forwards them to the manipulator and keeps the
 * camera projection correct.
 */
class VN_GRAPHICS_API SceneView : public RefCounted<SceneView> {
  public:
    /**
     * @brief The recipe of this view: how it draws (not how it is built).
     *
     * Deliberately small, and deliberately the ONLY settings object on a view: everything here is a decision
     * that changes which passes exist, so changing it REBUILDS this view's default window pipeline (see
     * setOptions). Whatever is geometry rather than recipe is a plain setter instead and never rebuilds
     * anything (viewport, surface size, overlay visibility - see setViewport / onSurfaceResized /
     * setGizmoVisible).
     *
     * The names follow the show-flag convention the industry uses for exactly these two overlays (Unreal's
     * ShowFlags.Gizmos, a stats readout toggled separately), and the defaults are the current behaviour:
     * forward, no HUD.
     */
    struct Options {
        /** @brief How the opaque content is shaded (the one structural choice). */
        ShadingPath path{ ShadingPath::Forward };

        /** @brief What the surface colour MEANS: lit, faceted, unlit, normals, or edges.
         *
         * Every mode is the SAME built-in program text with one define (see BuiltinShaders), applied as the
         * window pass' program override, and Wireframe adds the pass' rasterisation mode on top - so a mode
         * change is a pass property, NOT a pipeline rebuild (see setOptions). Only the Forward path can honour a
         * mode other than Lit: the Deferred path's colours come out of the G-buffer and its window pass is a
         * fullscreen lighting program, so a non-Lit mode there is reported on the engine's diagnostic route and
         * the picture stays lit.
         */
        enum class Mode
        {
            Lit,       ///< The full shading: ambient + directional lights + specular.
            Flat,      ///< The same lighting with the per-FACE normal (faceted surfaces, no vertex normals read).
            Unlit,     ///< The surface colour alone (material / map / vertex colour), no light term.
            Normals,   ///< View-space normals as colour: which way each surface faces.
            Wireframe, ///< Edges only (RenderPass::setPolygonModeOverride), in the surface's own colour.
        };

        /** @brief The shading mode (see Mode; default: the full shading). */
        Mode mode{ Mode::Lit };

        /** @brief Whether this view HAS an axis-gizmo overlay pass (not whether it is visible). */
        bool gizmo{ false };

        /** @brief Whether this view HAS a frame-rate readout pass (not whether it is visible). */
        bool stats{ false };
    };

  public:
    /** @brief Constructs a view with a default camera and an empty content
     * scene.
     *
     * The view is not attached to an engine until setEngine() is called.
     */
    SceneView();

    /** @brief Destroys the view, removing its default window pass from the
     * bound engine (when this view registered one).
     *
     * The bound engine must outlive the view.
     */
    ~SceneView();

  public:
    // ---- Engine binding ----

    /** @brief Binds the engine this view renders through (borrowed).
     *
     * Re-binding replaces any previous engine. The engine stays content-free;
     * this view binds its content to the passes it registers.
     *
     * @param engine Engine to borrow; must outlive the view. Null detaches.
     */
    void setEngine(raw_ptr<RenderEngine> engine);

    /** @brief Gets the bound engine, or nullptr when unset. */
    raw_ptr<RenderEngine> engine() const;

    /** @brief Registers an order-0 window pass presenting this view's camera
     * to the backbuffer, drawing this view's content scene.
     *
     * The default viewer is the shared Forward preset (RenderPipelineBuilder):
     * the view assembles it through the same recipe an application would use
     * for an explicit forward pipeline. The pass is only added when the
     * engine does not already present this view's camera to the window (see
     * RenderEngine::hasWindowPass), so an application that built its own main
     * pass with this view's camera (e.g. a Deferred lighting pass) keeps full
     * control. Calling this twice is a no-op while the pipeline is
     * registered. A host convenience layer calls this once after the
     * application has registered its pipeline.
     */
    void ensureWindowPass();

  public:
    // ---- Recipe / geometry / overlay visibility ----

    /** @brief Gets this view's recipe (Options). */
    const Options& options() const noexcept;

    /** @brief Sets this view's recipe, doing the cheapest thing that satisfies the change.
     *
     * The STRUCTURE (path, gizmo, stats) is what a pipeline build answers: changing any of them REBUILDS this
     * view's default window pipeline (the old one unregisters its passes, the new one is assembled through the
     * same RenderPipelineBuilder recipe an application would use). That is ONE pipeline build, not a per-frame
     * cost; on the Deferred path it also reallocates the G-buffer.
     *
     * The SHADING MODE is not a structure: every mode is the window pass' program override (see Options::Mode), so
     * switching between Lit / Flat / Unlit / Normals costs an override and NOT a rebuild - the picture changes on
     * the next frame with nothing re-registered (measured by the same pipeline() handle).
     *
     * Nothing is built when the view has no default pipeline (the window pass has not been asked for yet, or an
     * application presents this camera itself): the recipe is remembered and the next ensureWindowPass() builds
     * from it. That is what makes "configure right after construction" cost nothing at all.
     *
     * @param options New recipe; passing the current one is a no-op.
     */
    void setOptions(const Options& options);

    /** @brief Gets this view's default window pipeline (the one its recipe built), or nullptr.
     *
     * For inspection and micro-tuning - `setEnabled()`, `gizmo()`, `fpsOverlay()`, `offscreenTarget()` - and for
     * the two facts a host may want to check: null means "this view has no default pipeline" (the window pass was
     * never ensured, or an application presents this camera itself), and `windowPass()` tells it what presents
     * this view. The pipeline is owned by the view; letting it go is what removes the passes.
     *
     * @return The default pipeline, or nullptr when this view has none.
     */
    raw_ptr<Pipeline> pipeline() const;

    /** @brief Gets the area of the render surface this view draws into (normalized). */
    const NormalizedViewport& viewport() const noexcept;

    /** @brief Sets the area of the render surface this view draws into (normalized [0,1]; default: all of it).
     *
     * GEOMETRY, not recipe - this never rebuilds the pipeline. It is the ONE input this view derives its
     * rectangle from: the camera/manipulator aspect, the device-pixel viewport of its window pass, the size its
     * off-screen targets and HUD boxes are resized to, and the size its surface-layout steps are told. A host
     * that splits a surface therefore states the split once, in normalized terms, and keeps feeding the
     * SURFACE size to onSurfaceResized() - it never converts to device pixels itself, and never has to work out
     * "half the surface" for the view.
     *
     * @param viewport Area to draw into; an invalid viewport (non-positive size) resets to the full surface.
     */
    void setViewport(const NormalizedViewport& viewport);

    /** @brief Shows or hides the axis-gizmo overlay at runtime.
     *
     * A switch, not a recipe change: when the view's pipeline carries the overlay, this is one
     * RenderPass::setEnabled() and nothing is rebuilt. When it does not (the recipe had Options::gizmo false),
     * the overlay is added to the recipe and the pipeline is rebuilt ONCE - every later call is free again. A
     * view with no default window pipeline has nowhere to put an overlay: that is reported on the engine's
     * diagnostic route (the host's own main pipeline owns its overlays - see RenderPipelineBuilder's
     * AxisGizmoOptions).
     *
     * What this call was last asked for is what a LATER rebuild starts from: a new pass is created enabled, so
     * without that a readout the user switched off would come back the next time the recipe changed the pipeline
     * (a path switch, or adding the other overlay). The pass is still the state a reader sees (isGizmoVisible
     * reads it), and a host that skips this API entirely is unaffected - a fresh view has both overlays visible.
     *
     * @param on True to show the overlay, false to hide it.
     */
    void setGizmoVisible(bool on);

    /** @brief Shows or hides the frame-rate readout at runtime (same rules as setGizmoVisible).
     *
     * @param on True to show the readout, false to hide it.
     */
    void setStatsVisible(bool on);

    /** @brief Gets whether the axis-gizmo overlay is currently visible.
     *
     * The overlay pass is the single source of truth (its enabled flag), so this stays in step with the
     * recipe, with setGizmoVisible and with a host that toggles the pass handle itself
     * (view->pipeline()->gizmo()).
     *
     * @return true when the overlay exists and is enabled.
     */
    bool isGizmoVisible() const noexcept;

    /** @brief Gets whether the frame-rate readout is currently visible.
     *
     * @return true when the readout exists and is enabled.
     */
    bool isStatsVisible() const noexcept;

  public:
    // ---- Camera / content ----

    /** @brief Gets the view camera (non-owning). */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the content scene drawn by this view.
     *
     * The returned reference shares ownership with the view, so it can be
     * bound explicitly to engine passes (addPass(pass, content, order)) that
     * render this view's content.
     *
     * @return The content scene (never null; an empty scene draws nothing).
     */
    intrusive_ptr<Scene> scene() const;

    /** @brief Sets the content scene drawn by this view.
     *
     * The view keeps a reference.
     *
     * @param scene Content scene, or null to clear (draws nothing).
     */
    void setScene(intrusive_ptr<Scene> scene);

  public:
    // ---- Navigation ----

    /** @brief Gets the camera manipulator driving this view's camera.
     *
     * A default orbit manipulator is created on first access when none was
     * set with setManipulator(). Creation is lazy so the manipulator snapshots
     * its home view from the camera's final placement.
     *
     * @return The manipulator (never null).
     */
    raw_ptr<CameraManipulator> manipulator();

    /** @brief Sets the camera manipulator driving this view's camera.
     *
     * @param manipulator Manipulator bound to this view's camera; the view
     *                    keeps a reference.
     */
    void setManipulator(intrusive_ptr<CameraManipulator> manipulator);

    /** @brief Fits the content scene into the view.
     *
     * @return true when a scene with valid bounds was fitted.
     */
    bool fitToScreen();

    /** @brief Restores the view captured as home by the manipulator. */
    void home();

  public:
    // ---- Window input / surface (forwarded to the manipulator) ----

    /** @brief Pushes a mouse event to the manipulator.
     *
     * @param event Mouse event to forward.
     */
    void pushEvent(const vn::window::MouseEvent& event);

    /** @brief Pushes a scroll (wheel) event to the manipulator.
     *
     * @param event Scroll event to forward.
     */
    void pushEvent(const vn::window::ScrollEvent& event);

    /** @brief Pushes a keyboard event to the manipulator.
     *
     * @param event Key event to forward.
     */
    void pushEvent(const vn::window::KeyEvent& event);

    /** @brief Refreshes the geometry of this view for a new surface size (keeps the current pixel ratio).
     *
     * @param width  New surface width in pixels (logical, as the host reports it).
     * @param height New surface height in pixels (logical, as the host reports it).
     */
    void onSurfaceResized(int width, int height);

    /** @brief Refreshes the geometry of this view for a new surface size and device pixel ratio.
     *
     * The surface size is the whole surface the host renders into, NOT this view's part of it: with a viewport
     * set (see setViewport), this view's own rectangle - its camera aspect, its window-pass viewport, the size
     * its targets and HUD boxes get - is derived from the surface size, the viewport and the ratio here, in
     * one place. Feeding a split view "half the surface" would state the same rectangle twice and let the two
     * statements disagree.
     *
     * Then it runs this view's surface-layout steps, with this view's own rectangle: the manipulator / camera
     * aspect first, then every callback registered with addSurfaceLayout(). Its default pipeline (when it has
     * one) is resized here too, so a view that uses the built-in recipe needs no layout step at all.
     *
     * @param width        New surface width in logical pixels.
     * @param height       New surface height in logical pixels.
     * @param pixel_ratio  Device pixels per logical pixel (devicePixelRatio); <= 0 keeps the current one.
     */
    void onSurfaceResized(int width, int height, double pixel_ratio);

    /** @brief Registers a creator-managed surface-layout step.
     *
     * The callback receives the new surface size every time the view's surface changes and updates whatever
     * that code owns - an off-screen target's size (target->setSize), a pass's viewport
     * (pass->setViewport(Viewport{...})), etc. This keeps each pipeline's resize policy with its creator
     * (deferred chains at native / fractional resolution, fixed shadow maps doing nothing, corner-anchored
     * previews / gizmos re-anchoring) while the engine stays a pure scheduler.
     *
     * The size is in the space the HOST reports (for the Qt host, the LOGICAL size its widget has - see
     * RenderControl). That matters because the consumers want different spaces: a pass viewport is in
     * DEVICE pixels, while AxisGizmo / FpsOverlay are laid out on the logical size and apply the ratio
     * themselves. A creator whose consumer needs device pixels therefore scales by the host's ratio here
     * (see Pipeline::resize, which takes the ratio for exactly that reason).
     *
     * @param layout Callback taking the new surface width and height the host reported.
     */
    void addSurfaceLayout(std::function<void(int width, int height)> layout);

    /** @brief Registers a creator-managed surface-layout step that also wants the device pixel ratio.
     *
     * Same contract as the two-argument overload, with the ratio the host reported added: the one consumer
     * that needs it is a creator laying out in DEVICE pixels (`pass->setViewport(Viewport{...})`, an off-screen
     * target at native resolution). Multiplying here is what keeps a layer-1 host from reaching back into
     * appfw for the ratio (RenderControl::devicePixelRatio()).
     *
     * The size is again THIS VIEW's rectangle (its part of the surface), in the host's logical space.
     *
     * @param layout Callback taking the view's width, height and the host's pixel ratio.
     */
    void addSurfaceLayout(std::function<void(int width, int height, double pixel_ratio)> layout);

    /** @brief Removes all registered surface-layout steps. */
    void clearSurfaceLayouts();

  public:
    /** @brief Renders one frame through the bound engine.
     *
     * @param dt Seconds elapsed since the previous frame.
     */
    void frame(double dt = 0.0);

  private:
    /** @brief Removes the default window pass this view registered, if any. */
    void removeWindowPass();

    /** @brief Drops the default pipeline (if any) and rebuilds it from the current recipe. */
    void rebuildDefaultWindowPass();

    /** @brief Applies the recipe's shading mode to the default pipeline's window pass (an override, no rebuild). */
    void applyModeToWindowPass();

    /** @brief Turns {surface size, viewport, pixel ratio} into this view's rectangle, in one place. */
    void applySurfaceGeometry();

    /** @brief Gets this view's rectangle in the host's logical pixels (the whole surface when full). */
    void paneSize(int& width, int& height) const;

    /** @brief Shows/hides an overlay, adding it to the recipe (one rebuild) when it is missing. */
    void setOverlayVisible(bool on, bool gizmo);

    /** @brief Returns whether the overlay pass exists and is enabled. */
    bool isOverlayVisible(bool gizmo) const noexcept;

    /** @brief Puts the last requested overlay visibility back onto a freshly built pipeline's overlays.
     *
     * A rebuild (recipe change) creates its overlays ENABLED, so the one thing that has to be carried across is
     * "the caller asked for this one hidden" - see setGizmoVisible.
     */
    void applyOverlayIntent();

    // The bound engine is borrowed (the host owns it and keeps it alive).
    raw_ptr<RenderEngine> engine_ = nullptr;

    // This view's recipe and its geometry. Both are inputs to "what does my pipeline look like", and only the
    // recipe rebuilds it (see setOptions).
    Options            options_{};
    NormalizedViewport viewport_{};
    double             pixel_ratio_ = 1.0;
    int                surface_width_ = 0;
    int                surface_height_ = 0;
    // One line per view about a shading mode the chosen path cannot honour (see applyModeToWindowPass).
    bool mode_unavailable_reported_ = false;

    // The visibility the caller last ASKED for (setGizmoVisible / setStatsVisible). A reader still reads the pass
    // (isGizmoVisible), but a rebuilt pipeline creates its overlays enabled, so this is what applyOverlayIntent()
    // puts back after every build. Both default to visible: a view nobody has switched anything off on.
    bool gizmo_visible_ = true;
    bool stats_visible_ = true;

    intrusive_ptr<Camera>  camera_;
    intrusive_ptr<Scene>   scene_;
    intrusive_ptr<CameraManipulator> manipulator_;

    // The default (Forward) window pipeline this view registered, if any.
    intrusive_ptr<Pipeline> default_pipeline_;

    // Creator-managed surface-layout steps run on every surface resize.
    std::vector<std::function<void(int width, int height)>> surface_layouts_;
    // ... and the ones that asked for the ratio too.
    std::vector<std::function<void(int width, int height, double pixel_ratio)>> surface_layouts_with_ratio_;
};

VN_GRAPHICS_NS_END
