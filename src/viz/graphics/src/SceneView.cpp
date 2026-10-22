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
#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/PassChainBuilder.hpp>
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
    setChain(nullptr);
}

void SceneView::setSurface(raw_ptr<RenderSurface> surface) noexcept
{
    if (surface == nullptr && surface_ != nullptr) {
        // The size the surface owned becomes this view's own statement (see RenderSurface::dismiss): a view that
        // stops presenting must keep being sizeable by its host from here on, and the surface is not there to be
        // asked any more.
        layout_.resize(surface_->width_, surface_->height_, surface_->pixel_ratio_);
    }
    surface_ = surface;
}

void SceneView::setEngine(raw_ptr<RenderEngine> engine)
{
    if (engine_.get() == engine) {
        return;
    }
    // The adopted pipeline's passes live in the OLD engine's frame: dropping the handle takes them out of it
    // (see Pipeline), and the host adopts one built on the new engine if it wants this view to keep drawing.
    setChain(nullptr);
    // A BORROW, not an ownership: the engine is the owner's to keep alive, and this lease only says whether that is
    // still true (see Lease). Taking a strong reference here would invert ownership - a view would keep a whole
    // device alive after its owner released it.
    engine_ = (engine != nullptr) ? engine->lease() : Lease<RenderEngine>();
}

raw_ptr<RenderEngine> SceneView::engine() const
{
    return engine_.get();
}

raw_ptr<PassChain> SceneView::chain() const
{
    return pipeline_.get();
}

void SceneView::setChain(intrusive_ptr<PassChain> pipeline)
{
    if (pipeline_.get() == pipeline.get()) {
        return;
    }
    pipeline_ = std::move(pipeline);
    // The view brings what a chain cannot know: the surface size it comes out of and the pixel ratio.
    // Everything else (which passes exist, the shading mode, the overlays) is the chain's, which is why adopting
    // one is one call and why a chain change never has to re-state any of it.
    applySurfaceGeometry();
    // The one thing the two halves can disagree on: the chain may have been built for OTHER content. Reported once
    // per episode (see checkChainContent) - the structure is legal, and drawing a scene nobody has is exactly
    // what an empty surface looks like from the host's side.
    checkChainContent();
}

void SceneView::checkChainContent()
{
    raw_ptr<RenderEngine> engine = engine_.get();
    if (pipeline_ == nullptr || scene_ == nullptr || engine == nullptr) {
        content_mismatch_.rearm();   // nothing to be wrong about yet: a later break is a new episode
        return;
    }
    if (pipeline_->draws(scene_.get())) {
        content_mismatch_.rearm();
        return;
    }
    if (!content_mismatch_.shouldReport()) {
        return;
    }
    engine->reportEngineProblem(
        vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::ContentSkipped,
        u8"this view adopted a pipeline whose passes draw other content: none of them draws the view's scene, so the"
        u8" view's content never reaches the surface - build the chain for the view's scene"
        u8" (PassChainBuilder::setContent), or bind the pass that should draw it"
        u8" (RenderEngine::bindPassContent)");
}

int SceneView::surfaceWidth() const noexcept
{
    return surface_ != nullptr ? surface_->width_ : layout_.width();
}

int SceneView::surfaceHeight() const noexcept
{
    return surface_ != nullptr ? surface_->height_ : layout_.height();
}

double SceneView::surfacePixelRatio() const noexcept
{
    return surface_ != nullptr ? surface_->pixel_ratio_ : layout_.pixelRatio();
}

void SceneView::applySurfaceGeometry()
{
    const int    surface_w = surfaceWidth();
    const int    surface_h = surfaceHeight();
    if (surface_w <= 0 || surface_h <= 0) {
        return;
    }

    // The view's own step runs first and in one place: the camera's shape, the manipulator's aspect, then the
    // adopted pipeline's targets and HUD boxes. Everything else is a step registered against the layout (see
    // LayoutParticipant), which runs after this one and in registration order. The SIZE is the effective one
    // (surface's while this view presents one), not the participant's recorded copy.
    layout_.applySize(surface_w, surface_h, surfacePixelRatio(), [this](int width, int height, double ratio) {
        // 1) The camera's shape: the surface IS the picture this view's projection is built for (one camera, one
        //    shape - see Camera), so the shape is pushed where the size is known. The manipulator is told the same
        //    size for its own aspect, and it drags the camera rather than holding a shape of its own.
        if (camera_ != nullptr) {
            camera_->setAspect(static_cast<double>(width) / static_cast<double>(height));
        }
        if (CameraManipulator* manip = manipulator(); manip != nullptr) {
            manip->onResize(vn::window::ResizeEvent{ width, height });
        }

        // 2) The adopted pipeline: its off-screen targets and HUD boxes are sized for the surface, and its window
        //    pass draws the whole of it (a pass with no viewport of its own - the view states none: one area draws
        //    one picture, and its rectangle is the area).
        if (pipeline_ != nullptr) {
            pipeline_->resize(width, height, ratio);
        }
    });
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
    if (pipeline_ != nullptr) {
        if (raw_ptr<RenderEngine> engine = engine_.get(); engine != nullptr) {
            if (RenderPass* window = pipeline_->presentingPass(); window != nullptr) {
                engine->bindPassContent(window, scene_);
            }
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
    // Rebinding may have been exactly what the adopted chain drew: the wire is right again, so a later break is a
    // new episode (see checkChainContent).
    checkChainContent();
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
    // One place keeps the router pointed at the manipulator that is really here, including the lazy default -
    // otherwise the first event would find a router that never heard of it.
    input_router_.setManipulator(manipulator_.get());
    return manipulator_.get();
}

void SceneView::setManipulator(intrusive_ptr<CameraManipulator> manipulator)
{
    manipulator_ = std::move(manipulator);
    input_router_.setManipulator(manipulator_.get());
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
    // The lazy default manipulator exists before the first event, exactly as it did when this branch lived here.
    manipulator();
    input_router_.pushEvent(event);
}

void SceneView::pushEvent(const vn::window::ScrollEvent& event)
{
    manipulator();
    input_router_.pushEvent(event);
}

void SceneView::pushEvent(const vn::window::KeyEvent& event)
{
    manipulator();
    input_router_.pushEvent(event);
}

InputRouter& SceneView::inputRouter() noexcept
{
    return input_router_;
}

const InputRouter& SceneView::inputRouter() const noexcept
{
    return input_router_;
}

bool SceneView::hasSurface() const noexcept
{
    return surface_ != nullptr;
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
    setOwnSurfaceSize(width, height, pixel_ratio > 0.0 ? pixel_ratio : surfacePixelRatio());
}

void SceneView::setOwnSurfaceSize(int width, int height, double pixel_ratio)
{
    layout_.resize(width, height, pixel_ratio);
    // One place turns {surface size, viewport, ratio} into this view's rectangle: the camera aspect, the window
    // pass viewport, the size the default pipeline and the layout steps are told (see applySurfaceGeometry).
    applySurfaceGeometry();
}

void SceneView::addSurfaceLayout(std::function<void(int width, int height)> layout)
{
    layout_.addStep(std::move(layout));
}

void SceneView::addSurfaceLayout(std::function<void(int width, int height, double pixel_ratio)> layout)
{
    layout_.addStep(std::move(layout));
}

void SceneView::clearSurfaceLayouts()
{
    layout_.clearSteps();
}

VN_GRAPHICS_NS_END
