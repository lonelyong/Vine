#include <vine/graphics/Pipeline.hpp>

#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>

VN_GRAPHICS_NS_BEGIN

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

raw_ptr<RenderPass> Pipeline::gizmoPass() const
{
    return gizmo_pass_.get();
}

raw_ptr<RenderPass> Pipeline::fpsOverlayPass() const
{
    return fps_overlay_pass_.get();
}

ShadingPath Pipeline::shadingPath() const noexcept
{
    return path_;
}

ShadingMode Pipeline::shadingMode() const noexcept
{
    return mode_;
}

bool Pipeline::setShadingMode(ShadingMode mode)
{
    if (mode != ShadingMode::Lit && path_ == ShadingPath::Deferred) {
        // A Deferred picture comes out of a fullscreen lighting pass: there is no geometry pass left to
        // override, so this mode has to be ASSEMBLED for (PipelineOptions::mode) - nothing here can make the
        // picture mean it. Reported once per episode, because the alternative is a host that believes it
        // switched something.
        if (mode_refused_.shouldReport()) {
            engine_->reportEngineProblem(
                vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                String(u8"this pipeline's Deferred chain has no geometry pass to shade, so the shading mode was "
                       u8"not changed: build with PipelineOptions::mode (a non-Lit mode is built on the forward "
                       u8"chain, see RenderPipelineBuilder::effectivePath)"));
        }
        return false;
    }
    mode_refused_.rearm();  // the condition is over: the next refusal is a new episode

    RenderPass* const picture = windowPass();
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

bool Pipeline::setGizmoVisible(bool on)
{
    if (gizmo_pass_ == nullptr) {
        // A switch that silently did nothing would leave a host believing the overlay was just hidden: its
        // existence is a build decision (PipelineOptions::gizmo). Once per episode, because a host may ask every
        // frame and the condition - not the call - is the news.
        if (overlay_refused_.shouldReport()) {
            engine_->reportEngineProblem(
                vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                String(u8"this pipeline has no axis-gizmo overlay, so there was nothing to show or hide: build with "
                       u8"PipelineOptions::gizmo to get one"));
        }
        return false;
    }
    overlay_refused_.rearm();
    gizmo_pass_->setEnabled(on);
    return true;
}

bool Pipeline::setStatsVisible(bool on)
{
    if (fps_overlay_pass_ == nullptr) {
        if (overlay_refused_.shouldReport()) {
            engine_->reportEngineProblem(
                vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::UnsupportedRequest,
                String(u8"this pipeline has no frame-rate readout, so there was nothing to show or hide: build with "
                       u8"PipelineOptions::fps to get one"));
        }
        return false;
    }
    overlay_refused_.rearm();
    fps_overlay_pass_->setEnabled(on);
    return true;
}

bool Pipeline::isGizmoVisible() const noexcept
{
    return gizmo_pass_ != nullptr && gizmo_pass_->isEnabled();
}

bool Pipeline::isStatsVisible() const noexcept
{
    return fps_overlay_pass_ != nullptr && fps_overlay_pass_->isEnabled();
}

void Pipeline::resize(int width, int height, double pixel_ratio)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    // The ratio is stated once by the host and applied where it belongs (see the declaration): the
    // off-screen targets are the consumer that wants device pixels, and the HUD overlays are laid out on
    // the logical surface they were written against (they apply their own ratio).
    const double ratio    = pixel_ratio > 0.0 ? pixel_ratio : 1.0;
    const int    device_w = static_cast<int>(width * ratio);
    const int    device_h = static_cast<int>(height * ratio);
    if (offscreen_target_ != nullptr) {
        offscreen_target_->setSize(device_w, device_h);
    }
    if (composite_target_ != nullptr) {
        composite_target_->setSize(device_w, device_h);
    }
    // The overlays own their box (the object computes it from the surface size and its area origin); the pass
    // that draws it is a DECLARATION, so the box it states is restated here - this is the one place that lays
    // an overlay out for a new surface size (see setGizmo).
    if (gizmo_ != nullptr) {
        gizmo_->setPixelRatio(ratio);
        gizmo_->onSurfaceResized(width, height);
        if (gizmo_pass_ != nullptr) {
            gizmo_pass_->setViewport(gizmo_->boxViewport());
        }
    }
    if (fps_overlay_ != nullptr) {
        fps_overlay_->setPixelRatio(ratio);
        fps_overlay_->onSurfaceResized(width, height);
        if (fps_overlay_pass_ != nullptr) {
            fps_overlay_pass_->setViewport(fps_overlay_->boxViewport());
        }
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

intrusive_ptr<RenderPass> Pipeline::addPass(const PassDesc& desc)
{
    // The engine makes the node (see RenderEngine::addPass): remembering it here is the same contract as the
    // overload above - a pass this pipeline registered is a pass this pipeline unregisters - and the node goes
    // back to the caller because that is the handle a recipe needs (the window pass it presents, a pass a mode
    // override is applied to).
    intrusive_ptr<RenderPass> node = engine_->addPass(desc);
    if (node != nullptr) {
        passes_.push_back(node);
    }
    return node;
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

void Pipeline::setGizmo(intrusive_ptr<AxisGizmo> gizmo, intrusive_ptr<RenderPass> pass)
{
    gizmo_      = std::move(gizmo);
    gizmo_pass_ = std::move(pass);
}

void Pipeline::setFpsOverlay(intrusive_ptr<FpsOverlay> fps, intrusive_ptr<RenderPass> pass)
{
    fps_overlay_      = std::move(fps);
    fps_overlay_pass_ = std::move(pass);
}

VN_GRAPHICS_NS_END
