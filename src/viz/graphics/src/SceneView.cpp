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
    // Leave the surface first, and while the adopted pipeline is still there to hold the engine alive: the surface
    // re-states its clear on the presentations that remain, and that reads the engine's schedule (see
    // RenderSurface::applyClear). The surface also stops naming this view, so the other order - the surface
    // destroyed first - leaves this pointer null and nothing to do.
    if (surface_ != nullptr) {
        surface_->dismiss(this);
    }
    // Drop the pipeline this view adopted. Dropping the handle is all it takes: the pipeline unregisters every
    // pass it registered, and it holds the engine strongly, so the engine is still there to be unregistered from
    // even if the host released it first (see Pipeline).
    setPipeline(nullptr);
}

void SceneView::setSurface(raw_ptr<RenderSurface> surface) noexcept
{
    if (surface == nullptr && surface_ != nullptr) {
        // The size the surface owned becomes this view's own statement - exactly what happens to the rectangle
        // (see RenderSurface::dismiss): a view that stops presenting keeps drawing where it was, so it must keep
        // being sizeable by its host from here on, and the surface is not there to be asked any more.
        own_width_       = surface_->width_;
        own_height_      = surface_->height_;
        own_pixel_ratio_ = surface_->pixel_ratio_;
    }
    surface_ = surface;
}

void SceneView::setEngine(raw_ptr<RenderEngine> engine)
{
    if (engine_ == engine) {
        return;
    }
    // The adopted pipeline's passes live in the OLD engine's frame: dropping the handle takes them out of it
    // (see Pipeline), and the host adopts one built on the new engine if it wants this view to keep drawing.
    setPipeline(nullptr);
    engine_ = engine;
}

raw_ptr<RenderEngine> SceneView::engine() const
{
    return engine_;
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
    return pipeline_.get();
}

void SceneView::setPipeline(intrusive_ptr<Pipeline> pipeline)
{
    if (pipeline_.get() == pipeline.get()) {
        return;
    }
    pipeline_ = std::move(pipeline);
    // The view brings what a pipeline cannot know: this view's rectangle, the surface size it comes out of, and
    // the pixel ratio. Everything else (which passes exist, the shading mode, the overlays) is the pipeline's,
    // which is why adopting one is one call and why a pipeline change never has to re-state any of it.
    applySurfaceGeometry();
    // A new pipeline's window pass is a NEW pass (created enabled): the ONE clear of a surface belongs to the
    // presentation that draws first, so the table that lets this view present has to state that again.
    if (surface_ != nullptr) {
        surface_->applyClear();
    }
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

void SceneView::paneSize(int& width, int& height) const
{
    const NormalizedViewport rect = viewport();
    width  = std::max(1, static_cast<int>(std::lround(rect.width * static_cast<double>(surfaceWidth()))));
    height = std::max(1, static_cast<int>(std::lround(rect.height * static_cast<double>(surfaceHeight()))));
}

int SceneView::surfaceWidth() const noexcept
{
    return surface_ != nullptr ? surface_->width_ : own_width_;
}

int SceneView::surfaceHeight() const noexcept
{
    return surface_ != nullptr ? surface_->height_ : own_height_;
}

double SceneView::surfacePixelRatio() const noexcept
{
    return surface_ != nullptr ? surface_->pixel_ratio_ : own_pixel_ratio_;
}

void SceneView::applySurfaceGeometry()
{
    const int    surface_w = surfaceWidth();
    const int    surface_h = surfaceHeight();
    const double ratio     = surfacePixelRatio();
    if (surface_w <= 0 || surface_h <= 0) {
        return;
    }

    // The rectangle this view draws into: its own, or - while it presents a surface - the one that surface's
    // table states (see viewport()). Read once here: everything below is a use of this ONE value.
    const NormalizedViewport rect = viewport();

    int pane_w = 0;
    int pane_h = 0;
    paneSize(pane_w, pane_h);

    // 1) The camera's shape: this view's rectangle IS the picture its projection is built for (one camera, one
    //    shape - see Camera), so the shape is pushed where the rectangle is known. The manipulator is told the
    //    same rectangle for its own aspect (the pane's, not the surface's: a half-width view of a square surface
    //    is 1:2), which it derives from the size, and it drags the camera rather than holding a shape of its own.
    //    A camera shared by two views of different shapes is a shape mismatch the engine reports, not a state
    //    this can arbitrate: whoever applies last wins.
    if (camera_ != nullptr && pane_w > 0 && pane_h > 0) {
        camera_->setAspect(static_cast<double>(pane_w) / static_cast<double>(pane_h));
    }
    if (CameraManipulator* manip = manipulator(); manip != nullptr) {
        manip->onResize(vn::window::ResizeEvent{ pane_w, pane_h });
    }

    // 2) The adopted pipeline: its off-screen targets and HUD boxes are sized for this view's rectangle, and its
    //    window pass draws into it. The HUD passes anchor to the corner of their AREA, so they are pointed at the
    //    rectangle's origin before they are laid out.
    if (pipeline_ != nullptr) {
        const Viewport device_rect = deviceViewport(rect, surface_w, surface_h, ratio);
        if (AxisGizmo* gizmo = pipeline_->gizmo(); gizmo != nullptr) {
            gizmo->setAreaOrigin(device_rect.x, device_rect.y);
        }
        if (FpsOverlay* fps = pipeline_->fpsOverlay(); fps != nullptr) {
            fps->setAreaOrigin(device_rect.x, device_rect.y);
        }
        pipeline_->resize(pane_w, pane_h, ratio);
        if (RenderPass* window = pipeline_->windowPass(); window != nullptr) {
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
        layout(pane_w, pane_h, ratio);
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
    // The adopted pipeline's window pass was built with the scene that was current then, and the pass' content
    // is bound to the pass, not to this view: without re-binding it the view would keep drawing the replaced
    // scene.
    if (pipeline_ != nullptr && engine_ != nullptr) {
        if (RenderPass* window = pipeline_->windowPass(); window != nullptr) {
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

bool SceneView::hasSurface() const noexcept
{
    return surface_ != nullptr;
}

void SceneView::onSurfaceResized(int width, int height)
{
    onSurfaceResized(width, height, surfacePixelRatio());
}

void SceneView::onSurfaceResized(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (surface_ != nullptr) {
        // While this view presents a surface, the size of the thing the picture ends up in is that SURFACE's
        // fact: it is recorded there and fanned out to every view that presents it, so a host still spelling the
        // size this way (the entry a view with no surface uses) still states ONE size for the whole surface - and
        // the other panes are not left behind.
        surface_->resize(width, height, pixel_ratio);
        return;
    }
    setOwnSurfaceSize(width, height, pixel_ratio);
}

void SceneView::setOwnSurfaceSize(int width, int height, double pixel_ratio)
{
    own_width_  = width;
    own_height_ = height;
    if (pixel_ratio > 0.0) {
        own_pixel_ratio_ = pixel_ratio;
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
