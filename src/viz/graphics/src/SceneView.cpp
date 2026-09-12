#include <vine/graphics/SceneView.hpp>

#include <utility>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraManipulator.hpp>
#include <vine/graphics/OrbitCameraManipulator.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/window/InputEvent.hpp>
#include <vine/window/MouseButton.hpp>

V_GRAPHICS_NS_BEGIN

SceneView::SceneView()
  : camera_(make_intrusive<Camera>())
  , scene_(make_intrusive<Scene>())
{}

SceneView::~SceneView()
{
    // Remove the window pass this view registered while the engine is still
    // alive (the host owns the engine and destroys the view before it).
    removeWindowPass();
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
    // The application may already present this view's camera to the window
    // (e.g. a deferred-lighting main pass carrying this camera); in that case
    // no default window pass is needed.
    if (engine_->hasWindowPass(camera_.get())) {
        return;
    }
    // The default viewer is the shared Forward preset: assembled by the same
    // recipe RenderPipelineBuilder exposes, so the view default and an
    // application's explicit main pipeline stay one code path.
    RenderPipelineBuilder builder(engine_);
    builder.setCamera(camera_.get());
    builder.setContent(scene_);
    default_pipeline_ = builder.build(PipelinePreset::Forward);
}

void SceneView::removeWindowPass()
{
    if (engine_ != nullptr && default_pipeline_ != nullptr) {
        if (RenderPass* window = default_pipeline_->windowPass(); window != nullptr) {
            engine_->removePass(window);
        }
    }
    default_pipeline_ = nullptr;
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

void SceneView::pushEvent(const vine::window::MouseEvent& event)
{
    CameraManipulator* manip = manipulator();
    if (manip == nullptr) {
        return;
    }
    if (event.button == vine::window::MouseButton::None) {
        manip->onMouseMove(event);
    } else if (event.pressed) {
        manip->onMousePress(event);
    } else {
        manip->onMouseRelease(event);
    }
}

void SceneView::pushEvent(const vine::window::ScrollEvent& event)
{
    CameraManipulator* manip = manipulator();
    if (manip != nullptr) {
        manip->onScroll(event);
    }
}

void SceneView::pushEvent(const vine::window::KeyEvent& event)
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
    if (width <= 0 || height <= 0) {
        return;
    }
    // 1) The interactive view camera stays in step with the surface.
    if (CameraManipulator* manip = manipulator(); manip != nullptr) {
        manip->onResize(vine::window::ResizeEvent{ width, height });
    } else if (camera_->projectionType() == Camera::ProjectionType::Perspective) {
        const double aspect = static_cast<double>(width) / static_cast<double>(height);
        camera_->setProjectionMatrixAsPerspective(camera_->fieldOfView(), aspect,
                                                  camera_->nearPlane(), camera_->farPlane());
    }
    // 2) Creator-managed layout steps: each pipeline's code updates the
    // targets / viewports it owns with its own policy.
    for (const auto& layout : surface_layouts_) {
        layout(width, height);
    }
}

void SceneView::addSurfaceLayout(std::function<void(int width, int height)> layout)
{
    surface_layouts_.push_back(std::move(layout));
}

void SceneView::clearSurfaceLayouts()
{
    surface_layouts_.clear();
}

void SceneView::frame(double dt)
{
    if (engine_ != nullptr) {
        engine_->frame(dt);
    }
}

V_GRAPHICS_NS_END
