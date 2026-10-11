#include <vine/graphics/SceneView.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraManipulator.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/OrbitCameraManipulator.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/window/InputEvent.hpp>
#include <vine/window/MouseButton.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

/** @brief The device-pixel rectangle a normalized viewport describes on a logical surface. */
Viewport deviceViewport(const NormalizedViewport& viewport, int width, int height, double ratio)
{
    const double device_w = static_cast<double>(width) * ratio;
    const double device_h = static_cast<double>(height) * ratio;
    // Left/top round down, right/bottom round up: two neighbouring viewports then meet exactly, and the
    // one-pixel overlap that can result is a smaller sin than a seam the cleared background shows through.
    const int x0 = static_cast<int>(std::floor(viewport.x * device_w));
    const int y0 = static_cast<int>(std::floor(viewport.y * device_h));
    const int x1 = static_cast<int>(std::ceil((viewport.x + viewport.width) * device_w));
    const int y1 = static_cast<int>(std::ceil((viewport.y + viewport.height) * device_h));
    return Viewport{ x0, y0, x1 - x0, y1 - y0 };
}

} // namespace

SceneView::SceneView()
  : camera_(make_intrusive<Camera>())
  , scene_(make_intrusive<Scene>())
{}

SceneView::~SceneView()
{
    // Leave the surface first, and while the default pipeline is still there to hold the engine alive: the surface
    // re-states its clear on the presentations that remain, and that reads the engine's schedule (see
    // RenderSurface::applyClear). The surface also stops naming this view, so the other order - the surface
    // destroyed first - leaves this pointer null and nothing to do.
    if (surface_ != nullptr) {
        surface_->dismiss(this);
    }
    // Drop the window pass this view registered. Dropping the handle is all it
    // takes: the pipeline unregisters every pass it registered, and it holds the
    // engine strongly, so the engine is still there to be unregistered from even
    // if the host released it first (see Pipeline).
    removeWindowPass();
}

void SceneView::setSurface(raw_ptr<RenderSurface> surface) noexcept
{
    surface_ = surface;
}

void SceneView::setEngine(raw_ptr<RenderEngine> engine)
{
    if (engine_ == engine) {
        return;
    }
    removeWindowPass();
    engine_ = engine;
}

raw_ptr<RenderEngine> SceneView::engine() const
{
    return engine_;
}

void SceneView::ensureWindowPass()
{
    if (engine_ == nullptr || default_pipeline_ != nullptr) {
        return;
    }
    // The engine is borrowed, and everything below goes to it: hold it for the length of the operation, so a host
    // that dropped its last reference meanwhile cannot leave a dangling handle behind (R2 in
    // .ai/design/appfw-render-view.md §13 - "borrowed" means "only alive as long as the operation needs it", not
    // "length is somebody else's problem").
    intrusive_ptr<RenderEngine> const keep(engine_, true);

    // The default viewer is the shared preset RenderPipelineBuilder exposes, configured by this view's recipe, so
    // the view default and an application's explicit main pipeline stay one code path.
    PipelineOptions pipeline_options;
    pipeline_options.path                 = effectivePath();
    pipeline_options.gizmo.source_camera  = options_.gizmo ? camera_.get() : nullptr;
    pipeline_options.gizmo.pixel_ratio    = pixel_ratio_;
    pipeline_options.fps.enabled          = options_.stats;
    pipeline_options.fps.pixel_ratio      = pixel_ratio_;

    RenderPipelineBuilder builder(engine_);
    builder.setCamera(camera_.get());
    builder.setContent(scene_);
    default_pipeline_ = builder.build(pipeline_options);

    // The pipeline was built at the builder's default geometry: this puts it at the surface size / viewport /
    // ratio the view already knows about (a no-op before the first resize, exactly like today).
    applySurfaceGeometry();
    // ... and at the recipe's shading mode, which the builder has no option for (it is a pass property, see
    // applyModeToGeometryPass).
    applyModeToGeometryPass();
    // ... and at the visibility the caller last asked for: a fresh overlay pass is created ENABLED, so without
    // this a rebuild would switch a hidden readout back on (see applyOverlayIntent).
    applyOverlayIntent();
}

ShadingPath SceneView::effectivePath(const Options& options) noexcept
{
    // A non-Lit mode is a geometry-stage override: it needs the pass that turns geometry into the picture, and a
    // deferred recipe has no such pass in the picture's path (its window pass is a fullscreen lighting program
    // reading the G-buffer). So the chain is built forward while such a mode is active - see the Mode docs.
    if (options.mode != Options::Mode::Lit && options.path == ShadingPath::Deferred) {
        return ShadingPath::Forward;
    }
    return options.path;
}

ShadingPath SceneView::effectivePath() const noexcept
{
    return effectivePath(options_);
}

void SceneView::applyOverlayIntent()
{
    if (default_pipeline_ == nullptr) {
        return;
    }
    if (RenderPass* const gizmo = default_pipeline_->gizmo(); gizmo != nullptr && !gizmo_visible_) {
        gizmo->setEnabled(false);
    }
    if (RenderPass* const readout = default_pipeline_->fpsOverlay(); readout != nullptr && !stats_visible_) {
        readout->setEnabled(false);
    }
}

void SceneView::applyModeToGeometryPass()
{
    if (default_pipeline_ == nullptr) {
        return;
    }
    // The pass that turns geometry into the picture. On the forward chain (which is what a Deferred recipe is
    // built with while a non-Lit mode is active - see effectivePath()) that IS the window pass: geometry, shading
    // and present are one pass, so the override here is a geometry-stage one by construction.
    RenderPass* const picture = default_pipeline_->windowPass();
    if (picture == nullptr) {
        return;
    }

    // Every mode is the same program text with one define (BuiltinShaders) applied to the whole pass, and Lit is
    // the per-geometry program the content came with - which is exactly what a null override means. Wireframe is
    // the same idea one layer down: the pass RASTERISES the triangles as their edges (the same unlit colour, so
    // the structure reads instead of the shading), and clearing that override is how every other mode goes back
    // to surfaces. Both are pass properties: nothing here rebuilds anything.
    switch (options_.mode) {
        case Options::Mode::Lit:
            picture->setProgramOverride(nullptr);
            picture->clearPolygonModeOverride();
            break;
        case Options::Mode::Flat:
            picture->setProgramOverride(vn::graphics::flatForwardProgram());
            picture->clearPolygonModeOverride();
            break;
        case Options::Mode::Unlit:
            picture->setProgramOverride(vn::graphics::unlitForwardProgram());
            picture->clearPolygonModeOverride();
            break;
        case Options::Mode::Normals:
            picture->setProgramOverride(vn::graphics::normalsForwardProgram());
            picture->clearPolygonModeOverride();
            break;
        case Options::Mode::Wireframe:
            picture->setProgramOverride(vn::graphics::unlitForwardProgram());
            picture->setPolygonModeOverride(PolygonMode::Line);
            break;
    }
}

void SceneView::rebuildDefaultWindowPass()
{
    if (engine_ == nullptr) {
        return;
    }
    // R2: the hold must span the WHOLE operation, not one half of it - dropping the old pipeline can drop the
    // last reference to the engine (the pipeline was holding it), and the build that follows needs it alive.
    intrusive_ptr<RenderEngine> const keep(engine_, true);
    // Drop first, build through ensureWindowPass(): the old pipeline unregisters its passes as it goes away, so
    // the rebuild leaves exactly the new pipeline's passes registered (no stale slot, no double registration).
    removeWindowPass();
    ensureWindowPass();
    // ... and tell the surface this view presents: the ONE clear of that surface belongs to its first presentation,
    // and this rebuild created NEW passes, each with its own defaults (a window pass clears). Without this the
    // surface's decision would be undone on every recipe change.
    if (surface_ != nullptr) {
        surface_->applyClear();
    }
}

void SceneView::removeWindowPass()
{
    if (default_pipeline_ == nullptr) {
        return;  // guarded first: with no pipeline, engine_ may be a host-released (dangling) borrow.
    }
    // See rebuildDefaultWindowPass(): the pipeline's own reference is what keeps the engine alive across the
    // unregistration this drop performs, and this hold keeps it alive if the pipeline was the last holder.
    intrusive_ptr<RenderEngine> const keep(engine_, true);
    default_pipeline_ = nullptr;
}

const SceneView::Options& SceneView::options() const noexcept
{
    return options_;
}

void SceneView::setOptions(const Options& options)
{
    const bool unchanged = options_.path == options.path && options_.mode == options.mode &&
                           options_.gizmo == options.gizmo && options_.stats == options.stats;
    if (unchanged) {
        return;
    }
    const Options previous = options_;
    options_             = options;

    // Before the window pass is asked for, this is just the recipe the next ensureWindowPass() will build from
    // (see the header), so a change costs nothing here.
    if (default_pipeline_ == nullptr) {
        return;
    }

    // STRUCTURE means "which passes exist": the CHAIN in use (a non-Lit mode on a deferred recipe is built
    // forward - see effectivePath()) and the two overlays the recipe asks for. A mode that keeps the chain has no
    // structural consequence: it is a pass property (applyModeToGeometryPass).
    const bool structure_changed =
        effectivePath(previous) != effectivePath(options) || previous.gizmo != options.gizmo || previous.stats != options.stats;
    if (structure_changed) {
        rebuildDefaultWindowPass();
        return;
    }

    // Only the shading mode changed, on the same chain: that is the picture pass' program override, so the pass
    // graph is untouched (same pipeline, same registered passes) and the next frame simply shades the other way.
    applyModeToGeometryPass();
}

NormalizedViewport SceneView::viewport() const noexcept
{
    // While this view presents a surface, that surface's table holds the rectangle (RenderSurface::setViewport
    // is where it is stated, and the surface is what a host reads the split from): reading it HERE rather than
    // keeping a second copy is what makes "the view's rectangle" and "the surface's split" the same fact.
    if (surface_ != nullptr) {
        return surface_->viewportOf(this);
    }
    return viewport_;
}

raw_ptr<Pipeline> SceneView::pipeline() const
{
    return default_pipeline_.get();
}

void SceneView::setViewport(const NormalizedViewport& viewport)
{
    const NormalizedViewport next = viewport.isValid() ? viewport : NormalizedViewport{};
    if (surface_ != nullptr) {
        // Forwarded: the surface states the rectangle and hands the geometry back through
        // applySurfaceGeometry(), so the one place the split is written stays one place.
        surface_->setViewport(this, next);
        return;
    }
    if (next.x == viewport_.x && next.y == viewport_.y && next.width == viewport_.width &&
        next.height == viewport_.height) {
        return;
    }
    viewport_ = next;
    // Geometry: no rebuild, only the numbers every consumer reads (see applySurfaceGeometry).
    applySurfaceGeometry();
}

bool SceneView::isOverlayVisible(bool gizmo) const noexcept
{
    if (default_pipeline_ == nullptr) {
        return false;
    }
    const RenderPass* pass = gizmo ? static_cast<const RenderPass*>(default_pipeline_->gizmo())
                                   : static_cast<const RenderPass*>(default_pipeline_->fpsOverlay());
    // The pass IS the state: there is no second bool that could disagree with a host that toggled the handle.
    return pass != nullptr && pass->isEnabled();
}

void SceneView::setOverlayVisible(bool on, bool gizmo)
{
    bool& wanted = gizmo ? options_.gizmo : options_.stats;
    // Remember the REQUEST first, whatever path this call takes: it is what a later rebuild starts from
    // (applyOverlayIntent). `on == false` with no pass yet is normal - it is still what the caller asked for.
    (gizmo ? gizmo_visible_ : stats_visible_) = on;

    if (on && default_pipeline_ != nullptr && !isOverlayVisible(gizmo)) {
        RenderPass* pass = gizmo ? static_cast<RenderPass*>(default_pipeline_->gizmo())
                                 : static_cast<RenderPass*>(default_pipeline_->fpsOverlay());
        if (pass == nullptr) {
            // The pipeline exists but carries no such overlay: the recipe never asked for one. Add it and rebuild
            // ONCE - from here on this switch is a flag, forever (see the header).
            wanted = true;
            rebuildDefaultWindowPass();
            pass = gizmo ? static_cast<RenderPass*>(default_pipeline_ ? default_pipeline_->gizmo() : nullptr)
                         : static_cast<RenderPass*>(default_pipeline_ ? default_pipeline_->fpsOverlay() : nullptr);
        }
        if (pass != nullptr) {
            pass->setEnabled(true);
            return;
        }
    }

    if (default_pipeline_ == nullptr) {
        // Nothing to switch: the view has no default window pipeline. Remember the intent for the pipeline that
        // may still be built, and SAY SO - a silently ignored show request is the kind of thing that turns into a
        // bug report about a missing overlay.
        if (on) {
            wanted = true;
        }
        if (engine_ != nullptr) {
            engine_->reportEngineProblem(DiagnosticSeverity::Warning,
                                         DiagnosticCategory::UnsupportedRequest,
                                         gizmo ? String(u8"SceneView::setGizmoVisible: this view has no default "
                                                        u8"window pipeline (an application built its own main pass, "
                                                        u8"or the window pass was never ensured); the overlay was "
                                                        u8"remembered for the next ensureWindowPass() - an "
                                                        u8"application pipeline configures its overlays through "
                                                        u8"RenderPipelineBuilder::PipelineOptions instead")
                                               : String(u8"SceneView::setStatsVisible: this view has no default "
                                                        u8"window pipeline (an application built its own main pass, "
                                                        u8"or the window pass was never ensured); the overlay was "
                                                        u8"remembered for the next ensureWindowPass() - an "
                                                        u8"application pipeline configures its overlays through "
                                                        u8"RenderPipelineBuilder::PipelineOptions instead"));
        }
        return;
    }

    RenderPass* pass = gizmo ? static_cast<RenderPass*>(default_pipeline_->gizmo())
                             : static_cast<RenderPass*>(default_pipeline_->fpsOverlay());
    if (pass != nullptr) {
        pass->setEnabled(on);
    }
}

void SceneView::setGizmoVisible(bool on)
{
    setOverlayVisible(on, /* gizmo */ true);
}

void SceneView::setStatsVisible(bool on)
{
    setOverlayVisible(on, /* gizmo */ false);
}

bool SceneView::isGizmoVisible() const noexcept
{
    return isOverlayVisible(/* gizmo */ true);
}

bool SceneView::isStatsVisible() const noexcept
{
    return isOverlayVisible(/* gizmo */ false);
}

void SceneView::paneSize(int& width, int& height) const
{
    const NormalizedViewport rect = viewport();
    width  = std::max(1, static_cast<int>(std::lround(rect.width * static_cast<double>(surface_width_))));
    height = std::max(1, static_cast<int>(std::lround(rect.height * static_cast<double>(surface_height_))));
}

void SceneView::applySurfaceGeometry()
{
    if (surface_width_ <= 0 || surface_height_ <= 0) {
        return;
    }

    // The rectangle this view draws into: its own, or - while it presents a surface - the one that surface's
    // table states (see viewport()). Read once here: everything below is a use of this ONE value.
    const NormalizedViewport rect = viewport();

    int pane_w = 0;
    int pane_h = 0;
    paneSize(pane_w, pane_h);

    // 1) This view's rectangle drives the manipulator's own aspect (the pane's aspect, not the surface's: a
    //    half-width view of a square surface is 1:2). The CAMERA is told nothing: it holds no aspect, and the
    //    projection is built where the picture's shape is known (the pass' rectangle - see Camera).
    if (CameraManipulator* manip = manipulator(); manip != nullptr) {
        manip->onResize(vn::window::ResizeEvent{ pane_w, pane_h });
    }

    // 2) The default pipeline: its off-screen targets and HUD boxes are sized for this view's rectangle, and its
    //    window pass draws into it. The HUD passes anchor to the corner of their AREA, so they are pointed at the
    //    rectangle's origin before they are laid out.
    if (default_pipeline_ != nullptr) {
        const Viewport device_rect = deviceViewport(rect, surface_width_, surface_height_, pixel_ratio_);
        if (AxisGizmo* gizmo = default_pipeline_->gizmo(); gizmo != nullptr) {
            gizmo->setAreaOrigin(device_rect.x, device_rect.y);
        }
        if (FpsOverlay* fps = default_pipeline_->fpsOverlay(); fps != nullptr) {
            fps->setAreaOrigin(device_rect.x, device_rect.y);
        }
        default_pipeline_->resize(pane_w, pane_h, pixel_ratio_);
        if (RenderPass* window = default_pipeline_->windowPass(); window != nullptr) {
            if (rect.isFull()) {
                // The full surface is what a pass without a viewport already means - leaving it unset keeps the
                // default path byte-for-byte the pipeline it was before viewports existed.
                window->clearViewport();
            } else {
                window->setViewport(device_rect);
            }
        }
    }

    // 3) Creator-managed layout steps: told in THIS view's space (its rectangle), so a pipeline built for a split
    //    view sizes its targets for the pane without knowing anything about the layout.
    for (const auto& layout : surface_layouts_) {
        layout(pane_w, pane_h);
    }
    for (const auto& layout : surface_layouts_with_ratio_) {
        layout(pane_w, pane_h, pixel_ratio_);
    }
}

raw_ptr<Camera> SceneView::camera() const
{
    return camera_.get();
}

intrusive_ptr<Scene> SceneView::scene() const
{
    return scene_;
}

void SceneView::setScene(intrusive_ptr<Scene> scene)
{
    if (scene_ == scene) {
        return;
    }
    scene_ = std::move(scene);
    // The default window pass was built with the scene that was current then,
    // and the pass' content is bound to the pass, not to this view: without
    // re-binding it the viewer would keep drawing the replaced scene.
    if (default_pipeline_ != nullptr && engine_ != nullptr) {
        if (RenderPass* window = default_pipeline_->windowPass(); window != nullptr) {
            engine_->bindPassContent(window, scene_);
        }
    }
    // A scene-aware manipulator (the lazily created orbit default) holds a
    // NON-OWNING pointer to the scene it was given, for ray picking and
    // fitToScreen(). Re-point it, or it would keep navigating the replaced
    // scene — a dangling pointer once the host drops its last reference to it.
    // A manipulator that is not scene-aware has nothing to re-point.
    if (auto* orbit = dynamic_cast<OrbitCameraManipulator*>(manipulator_.get()); orbit != nullptr) {
        orbit->setScene(scene_.get());
    }
}

raw_ptr<CameraManipulator> SceneView::manipulator()
{
    if (manipulator_ == nullptr) {
        // Default orbit manipulator bound to this view's camera and content
        // scene; created lazily so it snapshots its home from the camera's
        // final placement (after the application has framed the view).
        manipulator_ = intrusive_ptr<CameraManipulator>(
            new OrbitCameraManipulator(camera_.get(), scene_.get()));
    }
    return manipulator_.get();
}

void SceneView::setManipulator(intrusive_ptr<CameraManipulator> manipulator)
{
    manipulator_ = std::move(manipulator);
}

bool SceneView::fitToScreen()
{
    CameraManipulator* manip = manipulator();
    if (manip == nullptr) {
        return false;
    }
    return manip->fitToScreen();
}

void SceneView::home()
{
    CameraManipulator* manip = manipulator();
    if (manip != nullptr) {
        manip->home();
    }
}

void SceneView::pushEvent(const vn::window::MouseEvent& event)
{
    CameraManipulator* manip = manipulator();
    if (manip == nullptr) {
        return;
    }
    if (event.button == vn::window::MouseButton::None) {
        manip->onMouseMove(event);
    } else if (event.pressed) {
        manip->onMousePress(event);
    } else {
        manip->onMouseRelease(event);
    }
}

void SceneView::pushEvent(const vn::window::ScrollEvent& event)
{
    CameraManipulator* manip = manipulator();
    if (manip != nullptr) {
        manip->onScroll(event);
    }
}

void SceneView::pushEvent(const vn::window::KeyEvent& event)
{
    CameraManipulator* manip = manipulator();
    if (manip == nullptr) {
        return;
    }
    if (event.pressed) {
        manip->onKeyDown(event);
    } else {
        manip->onKeyUp(event);
    }
}

void SceneView::onSurfaceResized(int width, int height)
{
    onSurfaceResized(width, height, pixel_ratio_);
}

void SceneView::onSurfaceResized(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    surface_width_  = width;
    surface_height_ = height;
    if (pixel_ratio > 0.0) {
        pixel_ratio_ = pixel_ratio;
    }
    // One place turns {surface size, viewport, ratio} into this view's rectangle: the camera aspect, the window
    // pass viewport, the size the default pipeline and the layout steps are told (see applySurfaceGeometry).
    applySurfaceGeometry();
}

void SceneView::addSurfaceLayout(std::function<void(int width, int height)> layout)
{
    surface_layouts_.push_back(std::move(layout));
}

void SceneView::addSurfaceLayout(std::function<void(int width, int height, double pixel_ratio)> layout)
{
    surface_layouts_with_ratio_.push_back(std::move(layout));
}

void SceneView::clearSurfaceLayouts()
{
    surface_layouts_.clear();
    surface_layouts_with_ratio_.clear();
}

void SceneView::frame(double dt)
{
    if (engine_ != nullptr) {
        engine_->frame(dt);
    }
}

VN_GRAPHICS_NS_END
