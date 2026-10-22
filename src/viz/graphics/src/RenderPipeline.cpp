#include <vine/graphics/RenderPipeline.hpp>

#include <algorithm>
#include <vector>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>

#include "PixelGeometry.hpp"

VN_GRAPHICS_NS_BEGIN

RenderPipeline::RenderPipeline(Lease<RenderEngine> engine) : engine_(engine)
{
    if (raw_ptr<RenderEngine> raw = engine_.get()) {
        group_ = raw->createPassGroup();
    }
}

RenderPipeline::~RenderPipeline()
{
    // Nothing to do here: the group is a token over the engine's own slot table, and dropping it is what hands every
    // pass this pipeline registered back to the engine (see PassGroup). A pass left registered keeps executing with
    // nobody holding the handle that would have removed it, which is exactly what the token exists to prevent.
    //
    // A pipeline can outlive its engine (a host may keep one after dropping the control that owned the engine): the
    // lease then answers nullptr, the engine's own list went with it, and the passes die with this pipeline. That is
    // why the borrow is a lease and not a raw pointer.
}

raw_ptr<RenderEngine> RenderPipeline::engine() const noexcept
{
    return engine_.get();
}

bool RenderPipeline::draws(raw_ptr<const Scene> scene) const noexcept
{
    if (scene == nullptr) {
        return false;
    }
    // Every pass of the chain, not just the presenting one: on a deferred chain the scene is drawn into the G-buffer
    // and the window pass presents a target (see the declaration).
    const std::vector<intrusive_ptr<RenderPass>> passes = group_.passes();
    return std::any_of(passes.begin(), passes.end(), [scene](const intrusive_ptr<RenderPass>& pass) {
        return pass != nullptr && pass->content() == scene;
    });
}

raw_ptr<RenderPass> RenderPipeline::presentingPass() const
{
    return window_pass_.get();
}

raw_ptr<RenderTarget> RenderPipeline::offscreenTarget() const
{
    return offscreen_target_.get();
}

raw_ptr<RenderTarget> RenderPipeline::compositeTarget() const
{
    return composite_target_.get();
}

raw_ptr<AxisGizmo> RenderPipeline::gizmo() const
{
    return gizmo_.get();
}

raw_ptr<FpsOverlay> RenderPipeline::fpsOverlay() const
{
    return fps_overlay_.get();
}

raw_ptr<RenderPass> RenderPipeline::gizmoPass() const
{
    return group_.pass(kGizmoRole);
}

raw_ptr<RenderPass> RenderPipeline::fpsOverlayPass() const
{
    return group_.pass(kStatsRole);
}

ShadingPath RenderPipeline::shadingPath() const noexcept
{
    return path_;
}

ShadingMode RenderPipeline::shadingMode() const noexcept
{
    return mode_;
}

bool RenderPipeline::setShadingMode(ShadingMode mode)
{
    raw_ptr<RenderEngine> engine = engine_.get();
    if (mode != ShadingMode::Lit && path_ == ShadingPath::Deferred) {
        // A Deferred picture comes out of a fullscreen lighting pass: there is no geometry pass left to
        // override, so this mode has to be ASSEMBLED for (RenderPipelineOptions::mode) - nothing here can make the
        // picture mean it. Reported once per episode, because the alternative is a host that believes it
        // switched something.
        if (mode_refused_.shouldReport() && engine != nullptr) {
            engine->reportEngineProblem(
                vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                String(u8"this pipeline's Deferred chain has no geometry pass to shade, so the shading mode was "
                       u8"not changed: build with RenderPipelineOptions::mode (a non-Lit mode is built on the forward "
                       u8"chain, see RenderPipelineBuilder::effectivePath)"));
        }
        return false;
    }
    mode_refused_.rearm();  // the condition is over: the next refusal is a new episode

    RenderPass* const picture = presentingPass();
    if (picture == nullptr) {
        return false;  // nothing draws: there is no picture pass to override
    }
    // Every mode is the same built-in program text with one define, applied to the WHOLE picture pass, and
    // Lit is the per-geometry program the content came with - which is exactly what a null override means.
    // Wireframe is the same idea one layer down: the pass RASTERISES the triangles as their edges (unlit, so
    // the structure reads instead of the shading), and clearing that override is how every other mode goes
    // back to surfaces. Both are pass properties: nothing here rebuilds anything.
    switch (mode) {
        case ShadingMode::Lit:
            picture->setProgramOverride(nullptr);
            picture->clearPolygonModeOverride();
            break;
        case ShadingMode::Flat:
            picture->setProgramOverride(vn::graphics::flatForwardProgram());
            picture->clearPolygonModeOverride();
            break;
        case ShadingMode::Unlit:
            picture->setProgramOverride(vn::graphics::unlitForwardProgram());
            picture->clearPolygonModeOverride();
            break;
        case ShadingMode::Normals:
            picture->setProgramOverride(vn::graphics::normalsForwardProgram());
            picture->clearPolygonModeOverride();
            break;
        case ShadingMode::Wireframe:
            picture->setProgramOverride(vn::graphics::unlitForwardProgram());
            picture->setPolygonModeOverride(PolygonMode::Line);
            break;
    }
    mode_ = mode;
    return true;
}

bool RenderPipeline::setGizmoVisible(bool on)
{
    if (gizmoPass() == nullptr) {
        // A switch that silently did nothing would leave a host believing the overlay was just hidden: its
        // existence is a build decision (RenderPipelineOptions::gizmo). Once per episode, because a host may ask every
        // frame and the condition - not the call - is the news.
        if (overlay_refused_.shouldReport()) {
            if (raw_ptr<RenderEngine> engine = engine_.get(); engine != nullptr) {
                engine->reportEngineProblem(
                    vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                    String(u8"this pipeline has no axis-gizmo overlay, so there was nothing to show or hide: build "
                           u8"with RenderPipelineOptions::gizmo to get one"));
            }
        }
        return false;
    }
    overlay_refused_.rearm();
    gizmoPass()->setEnabled(on);
    return true;
}

bool RenderPipeline::setStatsVisible(bool on)
{
    if (fpsOverlayPass() == nullptr) {
        if (overlay_refused_.shouldReport()) {
            if (raw_ptr<RenderEngine> engine = engine_.get(); engine != nullptr) {
                engine->reportEngineProblem(
                    vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                    String(u8"this pipeline has no frame-rate readout, so there was nothing to show or hide: build "
                           u8"with RenderPipelineOptions::fps to get one"));
            }
        }
        return false;
    }
    overlay_refused_.rearm();
    fpsOverlayPass()->setEnabled(on);
    return true;
}

bool RenderPipeline::isGizmoVisible() const noexcept
{
    const raw_ptr<RenderPass> pass = gizmoPass();
    return pass != nullptr && pass->isEnabled();
}

bool RenderPipeline::isStatsVisible() const noexcept
{
    const raw_ptr<RenderPass> pass = fpsOverlayPass();
    return pass != nullptr && pass->isEnabled();
}

void RenderPipeline::resize(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    // The ratio is stated once by the host and applied where it belongs (see the declaration): the
    // off-screen targets are the consumer that wants device pixels, and the HUD overlays are laid out on
    // the logical surface they were written against (they apply their own ratio).
    const detail::PixelRatio ratio{ pixel_ratio };
    const detail::DeviceSize device =
        detail::toDeviceSize(detail::LogicalSize{ width, height }, ratio);
    if (offscreen_target_ != nullptr) {
        offscreen_target_->setSize(device.width, device.height);
    }
    if (composite_target_ != nullptr) {
        composite_target_->setSize(device.width, device.height);
    }
    // The overlays own their box (the object computes it from the surface size and its area origin); the pass
    // that draws it is a DECLARATION, so the box it states is restated here - this is the one place that lays
    // an overlay out for a new surface size (see setGizmo).
    if (gizmo_ != nullptr) {
        gizmo_->setPixelRatio(ratio.value);
        gizmo_->onSurfaceResized(width, height);
        if (raw_ptr<RenderPass> pass = gizmoPass()) {
            pass->setViewport(gizmo_->boxViewport());
        }
    }
    if (fps_overlay_ != nullptr) {
        fps_overlay_->setPixelRatio(ratio.value);
        fps_overlay_->onSurfaceResized(width, height);
        if (raw_ptr<RenderPass> pass = fpsOverlayPass()) {
            pass->setViewport(fps_overlay_->boxViewport());
        }
    }
}

intrusive_ptr<RenderPass> RenderPipeline::addPass(const PassDesc& desc, const String& role)
{
    // The group is the registration (see the class note): a pass it registered is a pass it takes back, and the role
    // is what a reader asks for by name instead of a field per pass (see gizmoPass).
    return group_.add(desc, role);
}

void RenderPipeline::setWindowPass(intrusive_ptr<RenderPass> pass)
{
    window_pass_ = std::move(pass);
}

void RenderPipeline::setOffscreenTarget(intrusive_ptr<RenderTarget> target)
{
    offscreen_target_ = std::move(target);
}

void RenderPipeline::setCompositeTarget(intrusive_ptr<RenderTarget> target)
{
    composite_target_ = std::move(target);
}

void RenderPipeline::setGizmo(intrusive_ptr<AxisGizmo> gizmo)
{
    gizmo_ = std::move(gizmo);
}

void RenderPipeline::setFpsOverlay(intrusive_ptr<FpsOverlay> fps)
{
    fps_overlay_ = std::move(fps);
}

VN_GRAPHICS_NS_END
