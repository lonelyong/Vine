#include <vine/graphics/PassChain.hpp>

#include <algorithm>
#include <vector>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/Texture.hpp>

#include "PixelGeometry.hpp"

VN_GRAPHICS_NS_BEGIN

PassChain::PassChain(PassRegistry registry, DiagnosticsHandle diagnostics)
  : registry_(std::move(registry))
  , diagnostics_(std::move(diagnostics))
{}

PassChain::~PassChain()
{
    // The chain IS its registration scope: every pass it applied is filed in the engine's slot table with this chain
    // as its owner (see RenderEngine::applyPlan(plan, owner)), so ONE call hands them all back - no second list to
    // keep in step and no move-only token to get wrong. A chain that outlived its engine has no table left to hand
    // anything back to: the handle answers "gone" and the passes died with the engine that owned them.
    if (scope_owned_) {
        registry_.clearOwnedBy(scope_);
    }
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
    return std::any_of(entries_.begin(), entries_.end(), [this, scene](const Entry& entry) {
        const raw_ptr<RenderPass> pass = registry_.findFor(entry.name, scope_);
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
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                String(u8"this chain's Deferred picture has no geometry pass to shade, so the shading mode was "
                                       u8"not changed: build with ChainDesc::mode (a non-Lit mode is built on the forward "
                                       u8"chain, see PassChain::effectivePath)"));
        }
        return false;
    }
    mode_refused_.rearm(); // the condition is over: the next refusal is a new episode

    RenderPass* const picture = presentingPass();
    if (picture == nullptr) {
        return false; // nothing draws: there is no picture pass to override
    }
    // Every mode is the same built-in program text with one define, applied to the WHOLE picture pass, and
    // Lit is the per-geometry program the content came with - which is exactly what a null override means.
    // Wireframe is the same idea one layer down: the pass RASTERISES the triangles as their edges (unlit, so
    // the structure reads instead of the shading), and clearing that override is how every other mode goes
    // back to surfaces. The edit is ONE DECLARATION DIFF (see PassChain::restate): nothing here rebuilds anything.
    PassDesc desc = picture->declaration();
    switch (mode) {
    case ShadingMode::Lit:
        desc.program_override = nullptr;
        desc.polygon_mode.reset();
        break;
    case ShadingMode::Flat:
        desc.program_override = vn::graphics::flatForwardProgram();
        desc.polygon_mode.reset();
        break;
    case ShadingMode::Unlit:
        desc.program_override = vn::graphics::unlitForwardProgram();
        desc.polygon_mode.reset();
        break;
    case ShadingMode::Normals:
        desc.program_override = vn::graphics::normalsForwardProgram();
        desc.polygon_mode.reset();
        break;
    case ShadingMode::Wireframe:
        desc.program_override = vn::graphics::unlitForwardProgram();
        desc.polygon_mode    = PolygonMode::Line;
        break;
    }
    if (!restate(desc)) {
        return false;
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

raw_ptr<RenderPass> PassChain::addOverlay(const String& role, raw_ptr<Object> content, const PassDesc& desc, OverlayPlacement layout)
{
    // The registration and the value are one step: a pass registered under a role nobody recorded, or a value pointing
    // at a pass nothing draws, are both states this method makes unreachable. The content is asked ONCE whether it
    // updates itself per frame (FrameUpdate), so the per-frame walk is a pointer call, not a cast per frame - and
    // setOverlayUpdater can restate that worker afterwards (the last statement wins).
    (void)addPass(desc, role);
    overlays_.push_back(Overlay{ role, content, dynamic_cast<FrameUpdate*>(content), std::move(layout) });
    if (!apply()) {
        return nullptr; // the engine is gone, or the plan was refused (reported once)
    }
    return overlayPass(role);
}

bool PassChain::setOverlayUpdater(const String& role, raw_ptr<FrameUpdate> updater)
{
    const auto found = std::find_if(overlays_.begin(), overlays_.end(), [&role](const Overlay& overlay) { return overlay.role == role; });
    if (found == overlays_.end()) {
        // The same condition setOverlayVisible refuses on - this chain has no such overlay - so it shares that
        // episode: a host asking about a role it never registered is told once, whatever it asked for. The sentence
        // names the call, because "nothing to show or hide" would not be what went wrong here.
        if (overlay_refused_.shouldReport()) {
            const String message = String(u8"this chain has no overlay registered as '") + role +
                                   u8"', so no per-frame worker was stated: register it with "
                                   u8"PassChain::addOverlay (or build with the recipe option that does)";
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest, message);
        }
        return false;
    }
    overlay_refused_.rearm();
    found->worker = updater;
    return true;
}

void PassChain::update(const FrameContext& context)
{
    // REGISTRATION ORDER, once per frame (see FrameUpdate): an overlay's own work happens before the frame that draws
    // it, and what it publishes is the value that frame describes. An overlay with no worker (a static content, or a
    // nullptr stated through setOverlayUpdater) is skipped. ONE context for the whole call: an overlay cannot see a
    // different frame than the one next to it.
    for (const Overlay& overlay : overlays_) {
        if (overlay.worker != nullptr) {
            overlay.worker->onFrame(context);
        }
    }
}

raw_ptr<RenderPass> PassChain::overlayPass(const String& role) const noexcept
{
    return passForRole(role);
}

raw_ptr<RenderPass> PassChain::passForRole(const String& role) const noexcept
{
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&role](const Entry& entry) { return entry.role == role; });
    return found != entries_.end() ? registry_.findFor(found->name, scope_) : nullptr;
}

raw_ptr<RenderPass> PassChain::backgroundPass() const noexcept
{
    return passForRole(String(kBackgroundRole));
}

bool PassChain::setBackgroundMap(intrusive_ptr<CubeMap> map)
{
    raw_ptr<RenderPass> pass = backgroundPass();
    if (pass == nullptr || background_material_ == nullptr) {
        // The shape setOverlayVisible refuses in: a switch that silently did nothing would leave a host believing the
        // environment was installed. Once per episode, because a host may ask every frame and the condition - not the
        // call - is the news.
        if (background_refused_.shouldReport()) {
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                String(u8"this chain has no background, so the environment map was not installed: build with "
                                       u8"ChainDesc::background to get one"));
        }
        return false;
    }
    background_refused_.rearm();
    // THE MAP AND THE ENABLE ARE ONE ANSWER (see the declaration): a null map hides the background, and a map nobody
    // draws would be a chain whose picture disagrees with what it samples. The material's revision moves with the
    // write, so the next publication carries the new map - no pass is re-registered and no target is resized.
    const bool draws = map != nullptr;
    if (draws) {
        background_material_->setTexture(vn::graphics::TextureSlot::BaseColor, std::move(map));
    }
    else {
        background_material_->clearTexture(vn::graphics::TextureSlot::BaseColor);
    }
    PassDesc desc = pass->declaration();
    desc.enabled  = draws;
    return restate(desc);
}

raw_ptr<Object> PassChain::overlayContent(const String& role) const noexcept
{
    const auto found = std::find_if(overlays_.begin(), overlays_.end(), [&role](const Overlay& overlay) { return overlay.role == role; });
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
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest, message);
        }
        return false;
    }
    overlay_refused_.rearm();
    PassDesc desc = pass->declaration();
    desc.enabled   = on;
    return restate(desc);
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
    const detail::DeviceSize device = detail::toDeviceSize(detail::LogicalSize{ width, height }, ratio);
    if (offscreen_target_ != nullptr) {
        offscreen_target_->setSize(device.width, device.height);
    }
    if (composite_target_ != nullptr) {
        composite_target_->setSize(device.width, device.height);
    }
    // Each overlay lays ITSELF out (see addOverlay): the pipeline hands over the surface size and the ratio, and the
    // overlay restates its own content's box and ANSWERS the rectangle its pass draws into - the chain states that
    // rectangle, so a layout callback never writes pass state outside it. A third overlay therefore needs no change
    // here.
    for (const Overlay& overlay : overlays_) {
        if (!overlay.layout) {
            continue;
        }
        if (raw_ptr<RenderPass> pass = overlayPass(overlay.role)) {
            const Viewport box = overlay.layout(width, height, ratio.value);
            PassDesc desc     = pass->declaration();
            if (box.isValid()) {
                desc.viewport = box;
            }
            else {
                desc.viewport.reset();
            }
            (void)restate(desc);
        }
    }
}

intrusive_ptr<RenderPass> PassChain::addPass(const PassDesc& desc, const String& role)
{
    // THE SHAPE IS A PLAN, HERE TOO (see RenderEngine::applyPlan(plan, owner)): a recipe only STATES its passes, and
    // `apply()` hands the whole plan over once - a pass that reads a name another pass of the same chain publishes
    // cannot be validated one row at a time. The node comes back after apply(), by name, which is the identity a plan
    // uses; the role is this chain's own bookkeeping (what overlayPass asks for).
    pending_.passes.push_back(desc);
    entries_.push_back(Entry{ role, desc.name });
    return nullptr;
}

bool PassChain::apply()
{
    if (scope_ == nullptr) {
        scope_ = this; // a chain built through the bare constructor owns its own scope
    }
    if (!registry_.isValid()) {
        return false;
    }
    // THE PENDING SHAPE IS THE CURRENT SHAPE: every live edit is a restate() of one declaration (see E-04), so there is
    // nothing to refresh from the nodes - the nodes ARE what this chain last applied, and the plan diff makes an
    // unchanged row an in-place update.
    Plan plan = pending_;
    const PlanResult result = registry_.apply(plan, scope_);
    if (!result.applied()) {
        for (const PlanProblem& problem : result.problems) {
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::PassProtocolViolation,
                                String(u8"chain pass refused [") + detail::planCodeText(problem.code) + String(u8"]: ") + problem.detail);
        }
        return false;
    }
    pending_     = std::move(plan);
    window_pass_ = registry_.findFor(window_name_, scope_);
    return true;
}

bool PassChain::restate(const PassDesc& desc)
{
    const auto found = std::find_if(pending_.passes.begin(), pending_.passes.end(), [&desc](const PassDesc& row) { return row.name == desc.name; });
    if (found == pending_.passes.end()) {
        return false; // not this chain's row (a host's own rows are edited through applyPlan's scoped form)
    }
    *found = desc;
    return apply();
}

void PassChain::setTransparentContent(intrusive_ptr<Scene> scene)
{
    transparent_content_ = std::move(scene);
}

void PassChain::setBackgroundContent(intrusive_ptr<Scene> scene, intrusive_ptr<Material> material)
{
    background_content_  = std::move(scene);
    background_material_ = std::move(material);
}

void PassChain::publishContent()
{
    // ONE OWNER PER SCENE: this chain owns the scenes it draws that nobody else keeps alive (the transparent scene,
    // ChainDesc::transparent, and the background, ChainDesc::background), so publishing them is this call's job - the
    // host publishes the content it owns (SceneView::publishContent), and a frame only draws what was published.
    if (transparent_content_ != nullptr) {
        (void)transparent_content_->publish();
    }
    if (background_content_ != nullptr) {
        (void)background_content_->publish();
    }
}

void PassChain::setWindowPass(const String& name)
{
    window_name_ = name;
    // Before apply() there is no node yet; after it (and after every re-apply) the name resolves.
    window_pass_ = registry_.findFor(window_name_, scope_);
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
