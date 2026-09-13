#include <vine/graphics/RenderPipeline.hpp>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>

V_GRAPHICS_NS_BEGIN

Pipeline::Pipeline(intrusive_ptr<RenderEngine> engine) : engine_(std::move(engine)) {}

Pipeline::~Pipeline()
{
    // Hand every pass back to the engine that runs it: a pass left registered
    // keeps executing with nobody holding the handle that would have removed it
    // (see the class comment). Removing one the engine no longer has is a no-op,
    // so a host that removed a pass itself is not punished for it.
    for (const auto& pass : passes_) {
        engine_->removePass(pass.get());
    }
}

raw_ptr<RenderPass> Pipeline::windowPass() const
{
    return window_pass_.get();
}

raw_ptr<RenderTarget> Pipeline::offscreenTarget() const
{
    return offscreen_target_.get();
}

raw_ptr<RenderTarget> Pipeline::compositeTarget() const
{
    return composite_target_.get();
}

raw_ptr<AxisGizmo> Pipeline::gizmo() const
{
    return gizmo_.get();
}

raw_ptr<FpsOverlay> Pipeline::fpsOverlay() const
{
    return fps_overlay_.get();
}

void Pipeline::resize(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    if (offscreen_target_ != nullptr) {
        offscreen_target_->setSize(width, height);
    }
    if (composite_target_ != nullptr) {
        composite_target_->setSize(width, height);
    }
    if (gizmo_ != nullptr) {
        gizmo_->onSurfaceResized(width, height);
    }
    if (fps_overlay_ != nullptr) {
        fps_overlay_->onSurfaceResized(width, height);
    }
}

void Pipeline::addPass(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order)
{
    if (pass == nullptr) {
        return;
    }
    // Remember it BEFORE registering: the pass has to be reachable from the
    // handle in the same call that puts it in the frame, or a later failure in
    // the same build could leave a registered pass nobody can unregister.
    passes_.push_back(pass);
    engine_->addPass(std::move(pass), std::move(content), order);
}

void Pipeline::setWindowPass(intrusive_ptr<RenderPass> pass)
{
    window_pass_ = std::move(pass);
}

void Pipeline::setOffscreenTarget(intrusive_ptr<RenderTarget> target)
{
    offscreen_target_ = std::move(target);
}

void Pipeline::setCompositeTarget(intrusive_ptr<RenderTarget> target)
{
    composite_target_ = std::move(target);
}

void Pipeline::setGizmo(intrusive_ptr<AxisGizmo> gizmo)
{
    gizmo_ = std::move(gizmo);
}

void Pipeline::setFpsOverlay(intrusive_ptr<FpsOverlay> fps)
{
    fps_overlay_ = std::move(fps);
}

V_GRAPHICS_NS_END
