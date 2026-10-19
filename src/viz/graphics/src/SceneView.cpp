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
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/window/InputEvent.hpp>
#include <vine/window/MouseButton.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{

}  // namespace

SceneView::SceneView()
  : camera_(make_intrusive<Camera>())
  , scene_(make_intrusive<Scene>())
{}

SceneView::~SceneView()
{
    // Leave the surface first, and while the adopted pipeline is still there to hold the engine alive. The surface
    // also stops naming this view, so the other order - the surface destroyed first - leaves this pointer null and
    // nothing to do.
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
        // The size the surface owned becomes this view's own statement (see RenderSurface::dismiss): a view that
        // stops presenting must keep being sizeable by its host from here on, and the surface is not there to be
        // asked any more.
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

raw_ptr<RenderPipeline> SceneView::pipeline() const
{
    return pipeline_.get();
}

void SceneView::setPipeline(intrusive_ptr<RenderPipeline> pipeline)
{
    if (pipeline_.get() == pipeline.get()) {
        return;
    }
    pipeline_ = std::move(pipeline);
    // The view brings what a pipeline cannot know: the surface size it comes out of and the pixel ratio.
    // Everything else (which passes exist, the shading mode, the overlays) is the pipeline's, which is why adopting
    // one is one call and why a pipeline change never has to re-state any of it.
    applySurfaceGeometry();
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

    // 1) The camera's shape: the surface IS the picture this view's projection is built for (one camera, one shape
    //    - see Camera), so the shape is pushed where the size is known. The manipulator is told the same size for
    //    its own aspect, and it drags the camera rather than holding a shape of its own. A camera shared by two
    //    views is a shape mismatch the engine reports, not a state this can arbitrate: whoever applies last wins.
    if (camera_ != nullptr) {
        camera_->setAspect(static_cast<double>(surface_w) / static_cast<double>(surface_h));
    }
    if (CameraManipulator* manip = manipulator(); manip != nullptr) {
        manip->onResize(vn::window::ResizeEvent{ surface_w, surface_h });
    }

    // 2) The adopted pipeline: its off-screen targets and HUD boxes are sized for the surface, and its window pass
    //    draws the whole of it (a pass with no viewport of its own - the view states none: one area draws one
    //    picture, and its rectangle is the area).
    if (pipeline_ != nullptr) {
        pipeline_->resize(surface_w, surface_h, ratio);
    }

    // 3) Creator-managed layout steps: told the surface size, so a pipeline built for it sizes its targets without
    //    knowing anything about this view's own business.
    for (const auto& layout : surface_layouts_) {
        layout(surface_w, surface_h);
    }
    for (const auto& layout : surface_layouts_with_ratio_) {
        layout(surface_w, surface_h, ratio);
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

void SceneView::resize(int width, int height)
{
    resize(width, height, surfacePixelRatio());
}

void SceneView::resize(int width, int height, double pixel_ratio)
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

VN_GRAPHICS_NS_END
