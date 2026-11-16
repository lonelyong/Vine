#include "SurfaceLedger.hpp"

#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/RenderArea.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/WindowPresentTarget.hpp>

#include "EngineDiagnostics.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

SurfaceLedger::SurfaceLedger(Diagnostics& diagnostics) noexcept
  : diagnostics_(diagnostics)
{}

SurfaceLedger::~SurfaceLedger() = default;

intrusive_ptr<RenderArea> SurfaceLedger::createArea(RenderEngine& engine)
{
    // ONE engine drives ONE area, and the ledger owns it (see the class note): the relation has an owner instead of a
    // paragraph of documentation, so "the area must die first" and "a second area is refused" stop being conventions.
    // A second area is refused rather than silently taking over - the first one would keep declaring its picture into
    // a frame that no longer resolves it, and its own size would stop being the one the passes lay out on, both
    // silent.
    if (area_ != nullptr) {
        if (area_refused_.shouldReport()) {
            diagnostics_.report(engineProblem(u8"engine.area.second_refused",
                                              DiagnosticCategory::UnsupportedRequest,
                                              u8"RenderEngine::createArea: this engine already drives one render area, so a "
                                              u8"second one was refused - an engine drives one area, and a second window wants "
                                              u8"its own engine (one device may serve several engines)"));
        }
        return nullptr;
    }
    auto area = intrusive_ptr<RenderArea>(new RenderArea(engine));
    area_     = area;
    return area;
}

void SurfaceLedger::announce(raw_ptr<const RenderArea> from, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    // ONLY the engine's own area feeds the frame context (see createArea): letting anything else through would lay
    // this frame's passes out on another window's size.
    if (from == nullptr || area_ == nullptr || area_.get() != from) {
        return;
    }
    // Recorded even when there is no device yet: the frame context is what the passes lay themselves out on, and the
    // first frame hands it to the target at its head (see applyFrameSize) - one place that resizes a swapchain.
    width_  = width;
    height_ = height;
}

void SurfaceLedger::attach(RenderBackend& device)
{
    // A device that brings up a window of its OWN (the host named none) is asked for that target here, once: the
    // frame path has to have something to present through, and the device is the party that knows whether it
    // presents at all (see RenderDevice::createSurfaceTarget). A device that presents nothing answers nullptr with
    // NoPresentation and NO report - a display-less device is a first-class state, and the frame path then presents
    // nowhere. An AREA that holds one keeps it (a host that bound a target is never overridden); an engine with no
    // area at all keeps the answer here, because its frames still have to name a target when the device presents
    // through a window of its own.
    if (area_ != nullptr && !area_->hasTarget()) {
        SurfaceTargetResult why = SurfaceTargetResult::Ok;
        if (raw_ptr<WindowPresentTarget> own = device.createSurfaceTarget(nullptr, &why); own != nullptr) {
            area_->bindTarget(own);
        }
    }
    else if (area_ == nullptr) {
        SurfaceTargetResult why = SurfaceTargetResult::Ok;
        own_target_             = device.createSurfaceTarget(nullptr, &why);
    }
}

void SurfaceLedger::detach()
{
    // The target is one of the device's objects, so nothing may be left naming it: the creator may destroy it right
    // after this call (see RenderEngine::shutdown).
    if (area_ != nullptr) {
        area_->bindTarget(nullptr);
    }
    own_target_     = nullptr;
    applied_width_  = 0;
    applied_height_ = 0;
}

raw_ptr<RenderArea> SurfaceLedger::area() const noexcept
{
    return area_.get();
}

bool SurfaceLedger::hasArea() const noexcept
{
    return area_ != nullptr;
}

raw_ptr<PresentTarget> SurfaceLedger::frameTarget() const noexcept
{
    if (area_ != nullptr) {
        if (raw_ptr<WindowPresentTarget> bound = area_->target(); bound != nullptr) {
            return bound;
        }
    }
    return own_target_;
}

void SurfaceLedger::applyFrameSize(raw_ptr<PresentTarget> target, int width, int height)
{
    // THE WINDOW HALF OF THE SIZE FOLLOWS THE FRAME (design 2.12): the size is the FRAME's (taken when it started, see
    // EngineFrame), so a host resizing while this frame is answered costs the NEXT frame its resize and leaves this
    // one whole. The swapchain is told here, at the frame's head, once per changed size - it is the one place a frame
    // waits on the device, and a host resizing twice between two frames costs one resize instead of two.
    if (target == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (width == applied_width_ && height == applied_height_) {
        return;
    }
    target->resize(width, height);
    applied_width_  = width;
    applied_height_ = height;
}

void SurfaceLedger::forgetFrameSize() noexcept
{
    applied_width_  = 0;
    applied_height_ = 0;
}

int SurfaceLedger::width() const noexcept
{
    return width_;
}

int SurfaceLedger::height() const noexcept
{
    return height_;
}

} // namespace detail

VN_GRAPHICS_NS_END
