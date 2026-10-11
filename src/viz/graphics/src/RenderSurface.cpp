#include <vine/graphics/RenderSurface.hpp>

#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/SceneView.hpp>

VN_GRAPHICS_NS_BEGIN

RenderSurface::RenderSurface(raw_ptr<RenderEngine> engine)
  : engine_(engine)
{
    // The engine drives ONE area (see RenderEngine): this is it, and the registration is what lets the engine read
    // this area's TARGET - the frame it hands over names it, and the engine holds no handle of its own.
    if (engine_ != nullptr) {
        engine_->adoptArea(this);
    }
}

RenderSurface::~RenderSurface()
{
    // The view is the host's, and it outlives this area whenever the host says so: stop naming it, so a view
    // destroyed later does not call back into a surface that is gone (see SceneView::~SceneView).
    if (view_ != nullptr) {
        view_->setSurface(nullptr);
    }
    // The registration goes with the area: a borrowed pointer must not outlive what it points at (the engine
    // reads the area's target and keeps none of its own - see bindTarget).
    if (engine_ != nullptr) {
        engine_->releaseArea(this);
    }
}

raw_ptr<RenderPass> RenderSurface::passOf(raw_ptr<SceneView> view) const
{
    if (view == nullptr) {
        return nullptr;
    }
    // Resolved on every read, never remembered: a view's window pass is a NEW pass each time a recipe change
    // rebuilds its pipeline, and the area has to name the one that is in the frame.
    const raw_ptr<Pipeline> pipeline = view->pipeline();
    return pipeline != nullptr ? pipeline->windowPass() : nullptr;
}

void RenderSurface::reportSecondPresentation()
{
    if (refused_.shouldReport()) {
        // ONE condition, not a per-call event: a host that tries every frame must not be flooded, and the moment
        // it stops trying (or takes the first presentation away) is what arms the next report.
        engine_->reportEngineProblem(DiagnosticSeverity::Warning, DiagnosticCategory::PassProtocolViolation,
                                     u8"RenderSurface::present: this area already draws a picture, and one area "
                                     u8"draws one picture - dismiss the presentation that is there first (a second "
                                     u8"one is a second pass writing the same window, which is neither an overlay "
                                     u8"nor a split)");
    }
}

bool RenderSurface::present(SceneView& view)
{
    // Idempotent: declaring the same view twice is one presentation, and it is NOT the "already draws a picture"
    // refusal below (the host said nothing new).
    if (view_ == &view) {
        return false;
    }
    // A presentation is a statement about THIS engine's frame: the pass is registered there, and the order comes
    // from there. A view bound to another engine (or to none) could present this surface never, and "declared, but
    // nothing is drawn" is exactly the silence this area exists to end - so it is refused out loud.
    if (engine_ == nullptr || view.engine() != engine_) {
        if (engine_ != nullptr) {
            engine_->reportEngineProblem(
                DiagnosticSeverity::Warning,
                DiagnosticCategory::PassProtocolViolation,
                u8"RenderSurface::present: this view is not bound to the engine of this surface, so its window pass "
                u8"is registered on another frame and will never present this surface; bind the view to the same "
                u8"engine first (SceneView::setEngine)");
        }
        return false;
    }
    // One view draws ONE picture, and its area lives in the area it presents: a view that already presents another
    // surface would have two areas and no way to tell which one it draws into (it reads its size from the surface it
    // presents - see SceneView::surfaceWidth).
    if (view.surface_ != nullptr && view.surface_ != this) {
        engine_->reportEngineProblem(
            DiagnosticSeverity::Warning,
            DiagnosticCategory::PassProtocolViolation,
            u8"RenderSurface::present: this view already presents another surface, and a view draws one picture "
            u8"(its area is read from the surface it presents); dismiss it there first");
        return false;
    }
    if (view_ != nullptr || pass_ != nullptr) {
        // The same picture declared again is not a second one: a host that names the very pass this area already
        // draws through (its view's window pass) has said nothing new.
        if (passOf(&view) == presentingPass()) {
            return false;
        }
        reportSecondPresentation();
        return false;
    }
    view_ = &view;
    // The view and the area name each other while the presentation stands: the area resolves the view's window pass
    // from it, and the view reads this surface's size and reports back when its pipeline is rebuilt.
    view.setSurface(this);
    // DECLARING A VIEW IS DECLARING ITS PIPELINE. What presents this surface is the pass of the pipeline the view
    // ADOPTED (SceneView::setPipeline - built by RenderPipelineBuilder), and this area reads it back through the
    // view, so it never has to know a recipe, a chain or an overlay: a view with no pipeline has nothing to
    // resolve, and a declaration without one would leave a host believing a window is being drawn into.
    if (view.pipeline() == nullptr) {
        view_ = nullptr;
        view.setSurface(nullptr);
        if (refused_.shouldReport()) {
            engine_->reportEngineProblem(
                DiagnosticSeverity::Warning, DiagnosticCategory::PassProtocolViolation,
                u8"RenderSurface::present: this view has no pipeline to draw with, so nothing could declare this "
                u8"surface: build one (RenderPipelineBuilder) and adopt it (SceneView::setPipeline) first");
        }
        return false;
    }
    refused_.rearm();  // the condition is over: the next refusal is a new episode
    // The size this surface last saw reaches the new presentation AT ONCE (the view reads it from here): a view
    // added to a running surface is sized without the host feeding it (that is what makes the surface the ONE owner
    // of the size), and its geometry follows from it - its window pass keeps no viewport of its own, so a full-area
    // view costs nothing extra.
    view.applySurfaceGeometry();
    return true;
}

bool RenderSurface::present(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return false;
    }
    if (pass_ == pass) {
        return false;
    }
    if (view_ != nullptr) {
        // The view's window pass IS this pass: the same picture, already declared (see the note above).
        if (passOf(view_) == pass) {
            return false;
        }
    }
    if (view_ != nullptr || pass_ != nullptr) {
        reportSecondPresentation();
        return false;
    }
    pass_ = intrusive_ptr<RenderPass>(pass);
    refused_.rearm();
    return true;
}

bool RenderSurface::dismiss(raw_ptr<SceneView> view)
{
    if (view_ != view) {
        return false;
    }
    view_ = nullptr;
    // Called from ~SceneView as well: the view stops naming this surface. Nothing here touches it beyond this
    // pointer, so a half-destroyed view is safe to pass.
    if (view != nullptr) {
        view->setSurface(nullptr);
    }
    return true;
}

bool RenderSurface::dismiss(raw_ptr<RenderPass> pass)
{
    if (pass_ == nullptr || pass_.get() != pass) {
        return false;
    }
    pass_ = nullptr;
    return true;
}

bool RenderSurface::hasPresentation() const noexcept
{
    return view_ != nullptr || pass_ != nullptr;
}

raw_ptr<RenderPass> RenderSurface::presentingPass() const
{
    return pass_ != nullptr ? pass_.get() : passOf(view_);
}

raw_ptr<SceneView> RenderSurface::presentedView() const noexcept
{
    return view_;
}

void RenderSurface::resize(int width, int height)
{
    resize(width, height, pixel_ratio_);
}

void RenderSurface::resize(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    width_  = width;
    height_ = height;
    if (pixel_ratio > 0.0) {
        pixel_ratio_ = pixel_ratio;
    }
    // The frame hears the same ONE announcement: the engine records it, and its frame context is what passes lay
    // themselves out on (the engine owns no size of its own - see RenderEngine).
    if (engine_ != nullptr) {
        engine_->announceAreaSize(width, height);
    }
    // And the WINDOW's own half of it goes to the target, which is the object that owns the swapchain (see
    // RenderSurfaceTarget::resize): the surface is what owns its size, and the target is what follows it.
    if (target_ != nullptr) {
        target_->resize(width, height);
    }
    // Handed to the view that presents this surface, and only to it: a pass a host declared has no view to feed,
    // and it states its own rectangle in device pixels. The view RE-DERIVES its geometry from the numbers recorded
    // here (it reads them - see SceneView::surfaceWidth), so there is one size and no copy to keep in step.
    if (view_ != nullptr) {
        view_->applySurfaceGeometry();
    }
}

double RenderSurface::pixelRatio() const noexcept
{
    return pixel_ratio_;
}

void RenderSurface::bindTarget(raw_ptr<RenderSurfaceTarget> target)
{
    // The area holds the object its device made (see RenderDevice::createSurfaceTarget): nothing has to be handed
    // on, refused or reported here - a device that presents nothing never made one, and said so where it was
    // asked. What a recreated window is announced on is the target itself (RenderSurfaceTarget::setWindowHandle).
    target_ = target;
}

RenderSurfaceTarget* RenderSurface::target() const noexcept
{
    return target_;
}

bool RenderSurface::hasTarget() const noexcept
{
    return target_ != nullptr;
}

VN_GRAPHICS_NS_END
