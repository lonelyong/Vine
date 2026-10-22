#include <vine/graphics/PassChain.hpp>

#include <algorithm>
#include <vector>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>

#include "PixelGeometry.hpp"

VN_GRAPHICS_NS_BEGIN

PassChain::PassChain(PassRegistry registry, DiagnosticsRoute diagnostics)
  : registry_(std::move(registry))
  , diagnostics_(std::move(diagnostics))
{
}

PassChain::~PassChain()
{
    // The chain IS its registration scope: every pass it added is filed in the engine's slot table with this chain as
    // its owner (see PassRegistry::addOwnedBy), so ONE call hands them all back - no second list to keep in step and
    // no move-only token to get wrong. A chain that outlived its engine has no table left to hand anything back to:
    // the handle answers "gone" and the passes died with the engine that owned them.
    registry_.removeOwnedBy(this);
}

bool PassChain::isValid() const noexcept
{
    return registry_.isValid();
}

bool PassChain::draws(raw_ptr<const Scene> scene) const noexcept
{
    if (scene == nullptr) {
        return false;
    }
    // Every pass of the chain, not just the presenting one: on a deferred chain the scene is drawn into the G-buffer
    // and the window pass presents a target (see the declaration).
    const std::vector<intrusive_ptr<RenderPass>> passes = registry_.passesOwnedBy(this);
    return std::any_of(passes.begin(), passes.end(), [scene](const intrusive_ptr<RenderPass>& pass) {
        return pass != nullptr && pass->content() == scene;
    });
}

raw_ptr<RenderPass> PassChain::presentingPass() const
{
    return window_pass_.get();
}

raw_ptr<RenderTarget> PassChain::offscreenTarget() const
{
    return offscreen_target_.get();
}

raw_ptr<RenderTarget> PassChain::compositeTarget() const
{
    return composite_target_.get();
}

raw_ptr<AxisGizmo> PassChain::gizmo() const
{
    return obj_cast<AxisGizmo>(overlayContent(kGizmoRole));
}

raw_ptr<FpsOverlay> PassChain::fpsOverlay() const
{
    return obj_cast<FpsOverlay>(overlayContent(kStatsRole));
}

raw_ptr<RenderPass> PassChain::gizmoPass() const
{
    return overlayPass(kGizmoRole);
}

raw_ptr<RenderPass> PassChain::fpsOverlayPass() const
{
    return overlayPass(kStatsRole);
}

ShadingPath PassChain::shadingPath() const noexcept
{
    return path_;
}

ShadingMode PassChain::shadingMode() const noexcept
{
    return mode_;
}

bool PassChain::setShadingMode(ShadingMode mode)
{
    if (mode != ShadingMode::Lit && path_ == ShadingPath::Deferred) {
        // A Deferred picture comes out of a fullscreen lighting pass: there is no geometry pass left to
        // override, so this mode has to be ASSEMBLED for (ChainDesc::mode) - nothing here can make the
        // picture mean it. Reported once per episode, because the alternative is a host that believes it
        // switched something.
        if (mode_refused_.shouldReport()) {
            diagnostics_.report(
                vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                String(u8"this chain's Deferred picture has no geometry pass to shade, so the shading mode was "
                       u8"not changed: build with ChainDesc::mode (a non-Lit mode is built on the forward "
                       u8"chain, see PassChainBuilder::effectivePath)"));
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

bool PassChain::setGizmoVisible(bool on)
{
    return setOverlayVisible(kGizmoRole, on);
}

bool PassChain::setStatsVisible(bool on)
{
    return setOverlayVisible(kStatsRole, on);
}

bool PassChain::isGizmoVisible() const noexcept
{
    return isOverlayVisible(kGizmoRole);
}

bool PassChain::isStatsVisible() const noexcept
{
    return isOverlayVisible(kStatsRole);
}

raw_ptr<RenderPass> PassChain::addOverlay(const String&   role,
                                              raw_ptr<Object> content,
                                              const PassDesc& desc,
                                              OverlayLayout   layout)
{
    // The registration and the value are one step: a pass registered under a role nobody recorded, or a value pointing
    // at a pass nothing draws, are both states this method makes unreachable.
    intrusive_ptr<RenderPass> pass = addPass(desc, role);
    if (pass == nullptr) {
        return nullptr;  // the engine is gone, or that pass was already registered
    }
    overlays_.push_back(Overlay{ role, content, std::move(layout) });
    return pass.get();
}

raw_ptr<RenderPass> PassChain::overlayPass(const String& role) const noexcept
{
    return registry_.passOwnedBy(this, role);
}

raw_ptr<Object> PassChain::overlayContent(const String& role) const noexcept
{
    const auto found = std::find_if(overlays_.begin(), overlays_.end(),
                                    [&role](const Overlay& overlay) { return overlay.role == role; });
    return found != overlays_.end() ? found->content : nullptr;
}

bool PassChain::setOverlayVisible(const String& role, bool on)
{
    const raw_ptr<RenderPass> pass = overlayPass(role);
    if (pass == nullptr) {
        // A switch that silently did nothing would leave a host believing the overlay was just hidden: its existence
        // is whoever registered it (addOverlay). Once per episode, because a host may ask every frame and the
        // condition - not the call - is the news.
        if (overlay_refused_.shouldReport()) {
            const String message = String(u8"this chain has no overlay registered as '") + role +
                                   u8"', so there was nothing to show or hide: register it with "
                                   u8"PassChain::addOverlay (or build with the recipe option that does)";
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::UnsupportedRequest, message);
        }
        return false;
    }
    overlay_refused_.rearm();
    pass->setEnabled(on);
    return true;
}

bool PassChain::isOverlayVisible(const String& role) const noexcept
{
    const raw_ptr<RenderPass> pass = overlayPass(role);
    return pass != nullptr && pass->isEnabled();
}

void PassChain::resize(int width, int height, double pixel_ratio)
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

intrusive_ptr<RenderPass> PassChain::addPass(const PassDesc& desc, const String& role)
{
    // The engine's row carries this chain as the pass' owner (see the destructor): registering and taking back are
    // the same relation, and the role is what a reader asks for by name instead of a field per pass (see gizmoPass).
    return registry_.addOwnedBy(desc, this, role);
}

void PassChain::setWindowPass(intrusive_ptr<RenderPass> pass)
{
    window_pass_ = std::move(pass);
}

void PassChain::setOffscreenTarget(intrusive_ptr<RenderTarget> target)
{
    offscreen_target_ = std::move(target);
}

void PassChain::setCompositeTarget(intrusive_ptr<RenderTarget> target)
{
    composite_target_ = std::move(target);
}

VN_GRAPHICS_NS_END
