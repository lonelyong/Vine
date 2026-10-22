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
    return obj_cast<AxisGizmo>(overlayContent(kGizmoRole));
}

raw_ptr<FpsOverlay> RenderPipeline::fpsOverlay() const
{
    return obj_cast<FpsOverlay>(overlayContent(kStatsRole));
}

raw_ptr<RenderPass> RenderPipeline::gizmoPass() const
{
    return overlayPass(kGizmoRole);
}

raw_ptr<RenderPass> RenderPipeline::fpsOverlayPass() const
{
    return overlayPass(kStatsRole);
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
    return setOverlayVisible(kGizmoRole, on);
}

bool RenderPipeline::setStatsVisible(bool on)
{
    return setOverlayVisible(kStatsRole, on);
}

bool RenderPipeline::isGizmoVisible() const noexcept
{
    return isOverlayVisible(kGizmoRole);
}

bool RenderPipeline::isStatsVisible() const noexcept
{
    return isOverlayVisible(kStatsRole);
}

raw_ptr<RenderPass> RenderPipeline::addOverlay(const String&   role,
                                              raw_ptr<Object> content,
                                              const PassDesc& desc,
                                              OverlayLayout   layout)
{
    // The registration and the value are one step: a pass registered under a role nobody recorded, or a value pointing
    // at a pass nothing draws, are both states this method makes unreachable (see the group's contract).
    intrusive_ptr<RenderPass> pass = group_.add(desc, role);
    if (pass == nullptr) {
        return nullptr;  // the engine is gone, or that pass was already registered
    }
    overlays_.push_back(Overlay{ role, content, std::move(layout) });
    return pass.get();
}

raw_ptr<RenderPass> RenderPipeline::overlayPass(const String& role) const noexcept
{
    return group_.pass(role);
}

raw_ptr<Object> RenderPipeline::overlayContent(const String& role) const noexcept
{
    const auto found = std::find_if(overlays_.begin(), overlays_.end(),
                                    [&role](const Overlay& overlay) { return overlay.role == role; });
    return found != overlays_.end() ? found->content : nullptr;
}

bool RenderPipeline::setOverlayVisible(const String& role, bool on)
{
    const raw_ptr<RenderPass> pass = overlayPass(role);
    if (pass == nullptr) {
        // A switch that silently did nothing would leave a host believing the overlay was just hidden: its existence
        // is whoever registered it (addOverlay). Once per episode, because a host may ask every frame and the
        // condition - not the call - is the news.
        if (overlay_refused_.shouldReport()) {
            if (raw_ptr<RenderEngine> engine = engine_.get(); engine != nullptr) {
                const String message = String(u8"this pipeline has no overlay registered as '") + role +
                                       u8"', so there was nothing to show or hide: register it with "
                                       u8"RenderPipeline::addOverlay (or build with the recipe option that does)";
                engine->reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                            vn::graphics::DiagnosticCategory::UnsupportedRequest, message);
            }
        }
        return false;
    }
    overlay_refused_.rearm();
    pass->setEnabled(on);
    return true;
}

bool RenderPipeline::isOverlayVisible(const String& role) const noexcept
{
    const raw_ptr<RenderPass> pass = overlayPass(role);
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
    // Each overlay lays ITSELF out (see addOverlay): the pipeline hands over the surface size, the ratio and the pass
    // the overlay's role names, and the overlay restates its own content's box and the pass' rectangle. A third
    // overlay therefore needs no change here.
    for (const Overlay& overlay : overlays_) {
        if (!overlay.layout) {
            continue;
        }
        if (raw_ptr<RenderPass> pass = overlayPass(overlay.role)) {
            overlay.layout(*pass, width, height, ratio.value);
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

VN_GRAPHICS_NS_END
