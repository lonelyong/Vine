#include <vine/appfw/gui/Viewer.hpp>

#include <utility>
#include <vector>

#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/SceneView.hpp>

VN_APPFWGUI_NS_BEGIN

struct Viewer::Impl {
    /// One pane: the view, and the request its chain is built from. The rectangle is NOT here - it belongs to
    /// the area's table (see views / setPaneRect).
    struct Pane
    {
        intrusive_ptr<vn::graphics::SceneView> view;
        ViewerOptions                         options;
    };

    /// The engine the panes' chains are built on and registered with (borrowed; must outlive the viewer).
    raw_ptr<vn::graphics::RenderEngine> engine;
    /// The area whose panes this viewer manages (borrowed; must outlive the viewer).
    raw_ptr<vn::graphics::RenderSurface> surface;
    /// This viewer's panes, in registration order (the area's own view first; see the constructor).
    std::vector<Pane> panes;
    /// "This view draws through another engine" is one condition, not a per-call event.
    mutable bool foreign_view_reported = false;

    /// @brief The pane of @p view, or null when this viewer does not manage it.
    Pane* paneOf(raw_ptr<vn::graphics::SceneView> view)
    {
        for (Pane& pane : panes) {
            if (pane.view.get() == view) {
                return &pane;
            }
        }
        return nullptr;
    }

    const Pane* paneOf(raw_ptr<vn::graphics::SceneView> view) const
    {
        for (const Pane& pane : panes) {
            if (pane.view.get() == view) {
                return &pane;
            }
        }
        return nullptr;
    }

    /// @brief Builds @p pane's chain from its request (with this area's context), adopts it on the view, and
    /// carries the live chain's runtime choices onto it.
    bool buildPipeline(Pane& pane)
    {
        if (engine == nullptr || pane.view == nullptr) {
            return false;
        }

        // A rebuild changes the STRUCTURE, never the picture: whether the user has an overlay showing is a
        // runtime choice that lives on the live pipeline (Pipeline::setStatsVisible), so it is read from there and
        // re-stated on the new one. Only an overlay that WAS there and was hidden is carried - one this build adds
        // comes up showing, because nothing has hidden it yet.
        vn::graphics::Pipeline* const previous = pane.view->pipeline();
        const bool gizmo_was_hidden =
            previous != nullptr && previous->gizmo() != nullptr && !previous->isGizmoVisible();
        const bool stats_was_hidden =
            previous != nullptr && previous->fpsOverlay() != nullptr && !previous->isStatsVisible();

        // The request is the host-facing half; the CONTEXTUAL fields are filled in HERE, because this is the party
        // that knows them: the gizmo mirrors the pane's camera (an overlay with no camera to mirror is exactly what
        // "no overlay" means), and the overlays are laid out in device pixels, so they take the area's ratio.
        const double ratio = surface != nullptr && surface->pixelRatio() > 0.0 ? surface->pixelRatio() : 1.0;

        vn::graphics::PipelineOptions pipeline_options;
        pipeline_options.path                = pane.options.path;
        pipeline_options.mode                = pane.options.mode;
        pipeline_options.offscreen_width     = pane.options.offscreen_width;
        pipeline_options.offscreen_height    = pane.options.offscreen_height;
        pipeline_options.gizmo.source_camera = pane.options.gizmo ? pane.view->camera() : nullptr;
        pipeline_options.gizmo.pixel_ratio   = ratio;
        pipeline_options.fps.enabled         = pane.options.stats;
        pipeline_options.fps.pixel_ratio     = ratio;

        vn::graphics::RenderPipelineBuilder builder(engine);
        builder.setCamera(pane.view->camera());
        builder.setContent(pane.view->scene());
        vn::intrusive_ptr<vn::graphics::Pipeline> pipeline = builder.build(pipeline_options);
        if (pipeline == nullptr) {
            return false;  // the builder reports what it refused (see RenderPipelineBuilder::build)
        }
        if (gizmo_was_hidden) {
            (void)pipeline->setGizmoVisible(false);
        }
        if (stats_was_hidden) {
            (void)pipeline->setStatsVisible(false);
        }
        // Adopting the pipeline is what gives the pane its picture: its rectangle, the area size behind it and the
        // layout steps are applied by the view itself, and the area's table keeps resolving this pane's pass from
        // the new pipeline (see SceneView::setPipeline).
        pane.view->setPipeline(std::move(pipeline));
        return true;
    }

    /// @brief Reports, once per viewer, a view that does not draw through this viewer's engine.
    void reportForeignView() const
    {
        if (foreign_view_reported || engine == nullptr) {
            return;
        }
        foreign_view_reported = true;
        engine->reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                    u8"Viewer: a view bound to another engine cannot draw this area (its passes are "
                                    u8"registered on that engine's frame), so the pane was refused");
    }
};

Viewer::Viewer(raw_ptr<vn::graphics::RenderEngine> engine, raw_ptr<vn::graphics::RenderSurface> surface,
               intrusive_ptr<vn::graphics::SceneView> view)
  : d(new Impl())
{
    d->engine  = engine;
    d->surface = surface;
    if (view != nullptr) {
        // The area's own view: a pane from the start, but with no chain and no declaration - the first attach is
        // where the default request becomes a picture (see takeArea), so a host that never touches the viewer pays
        // no build until there is an area to draw into.
        d->panes.push_back(Impl::Pane{ std::move(view), ViewerOptions{} });
    }
}

Viewer::~Viewer()
{
    delete d;
}

bool Viewer::addView(intrusive_ptr<vn::graphics::SceneView> view, const vn::graphics::NormalizedViewport& rect,
                     const ViewerOptions& options)
{
    if (view == nullptr || d->engine == nullptr || d->surface == nullptr) {
        return false;
    }
    if (view->engine() != d->engine) {
        d->reportForeignView();
        return false;
    }

    Impl::Pane* pane  = d->paneOf(view.get());
    const bool  is_new = pane == nullptr;
    if (is_new) {
        d->panes.push_back(Impl::Pane{ std::move(view), ViewerOptions{} });
        pane = &d->panes.back();
    }

    const vn::intrusive_ptr<vn::graphics::Pipeline> previous_chain(pane->view->pipeline());
    const ViewerOptions previous_options = pane->options;
    pane->options = options;
    if (!d->buildPipeline(*pane)) {
        pane->options = previous_options;
        if (is_new) {
            d->panes.pop_back();
        }
        return false;
    }

    // Declared with the rectangle the caller stated. A view that already presents this area is one presentation
    // (present says so) - the rectangle is restated instead; a declaration the table REFUSES (a view that presents
    // another area, a view drawing through another engine) is reported there, and this call gives the view back
    // what it had, so a refused pane leaves no trace behind.
    if (!d->surface->present(*pane->view, rect) && !d->surface->setViewport(pane->view.get(), rect)) {
        pane->view->setPipeline(previous_chain);
        pane->options = previous_options;
        if (is_new) {
            d->panes.pop_back();
        }
        return false;
    }
    return true;
}

bool Viewer::removeView(raw_ptr<vn::graphics::SceneView> view)
{
    auto pane = d->panes.begin();
    for (; pane != d->panes.end(); ++pane) {
        if (pane->view.get() == view) {
            break;
        }
    }
    if (pane == d->panes.end()) {
        return false;
    }
    if (d->surface != nullptr) {
        // Withdrawing the declaration is what takes the pane out of the picture; the table hands the rectangle back
        // to the view, which keeps drawing into it once somebody declares it again.
        (void)d->surface->dismiss(pane->view.get());
    }
    d->panes.erase(pane);
    return true;
}

bool Viewer::release(raw_ptr<vn::graphics::SceneView> view)
{
    return removeView(view);
}

std::vector<raw_ptr<vn::graphics::SceneView>> Viewer::views() const
{
    std::vector<raw_ptr<vn::graphics::SceneView>> answer;
    if (d->surface == nullptr) {
        return answer;
    }
    for (const vn::graphics::RenderSurface::Presentation& presentation : d->surface->presentations()) {
        if (presentation.view != nullptr) {
            answer.push_back(presentation.view);
        }
    }
    return answer;
}

bool Viewer::setPaneRect(raw_ptr<vn::graphics::SceneView> view, const vn::graphics::NormalizedViewport& rect)
{
    return d->surface != nullptr && d->surface->setViewport(view, rect);
}

ViewerOptions Viewer::viewerOptions(raw_ptr<vn::graphics::SceneView> view) const
{
    const Impl::Pane* const pane = d->paneOf(view);
    return pane != nullptr ? pane->options : ViewerOptions{};
}

bool Viewer::setViewerOptions(raw_ptr<vn::graphics::SceneView> view, const ViewerOptions& options)
{
    if (view == nullptr || d->engine == nullptr) {
        return false;
    }
    if (view->engine() != d->engine) {
        d->reportForeignView();
        return false;
    }

    Impl::Pane* pane   = d->paneOf(view);
    const bool  is_new = pane == nullptr;
    if (is_new) {
        d->panes.push_back(Impl::Pane{ intrusive_ptr<vn::graphics::SceneView>(view, true), ViewerOptions{} });
        pane = &d->panes.back();
    }

    const ViewerOptions previous = pane->options;
    pane->options = options;
    if (!d->buildPipeline(*pane)) {
        // The builder refused the request: the previous chain stays in place, so the previous request does too
        // (the two describe the same picture - see ViewerOptions).
        pane->options = previous;
        if (is_new) {
            d->panes.pop_back();
        }
        return false;
    }
    return true;
}

bool Viewer::setShadingMode(raw_ptr<vn::graphics::SceneView> view, vn::graphics::ShadingMode mode)
{
    Impl::Pane* const pane = d->paneOf(view);
    if (pane == nullptr) {
        return false;
    }

    // The cheap path first, and it has to check the REQUEST, not just the mode: a non-Lit mode is realized on the
    // forward chain while the request still says Deferred (see effectivePath), so "the chain is live and can
    // express this mode" is only the cheap path when the live chain is the one this request resolves to. Picking
    // Lit again after such a mode therefore REBUILDS the deferred chain - which is what the host asked for.
    vn::graphics::PipelineOptions request;
    request.path = pane->options.path;
    request.mode = mode;
    vn::graphics::Pipeline* const pipeline = pane->view->pipeline();
    if (pipeline != nullptr && pipeline->shadingPath() == vn::graphics::RenderPipelineBuilder::effectivePath(request) &&
        pipeline->setShadingMode(mode)) {
        pane->options.mode = mode;  // remembered: the next build (any structural change) starts from here
        return true;
    }
    // The chain cannot express it (a deferred picture has no geometry pass to shade): the request itself has to
    // change, and the builder then assembles the chain that CAN express it.
    ViewerOptions options = pane->options;
    options.mode          = mode;
    return setViewerOptions(pane->view.get(), options);
}

bool Viewer::isActive() const
{
    if (d->surface == nullptr) {
        return false;
    }
    bool presenting = false;
    for (const vn::graphics::RenderSurface::Presentation& presentation : d->surface->presentations()) {
        if (presentation.view == nullptr) {
            // A pass a host declared presents this area: the host's own pipeline owns the picture, so the viewer is
            // not the presenter even while it has a pane left on the table.
            return false;
        }
        if (d->paneOf(presentation.view) != nullptr) {
            presenting = true;
        }
    }
    return presenting;
}

void Viewer::takeArea()
{
    if (d->surface == nullptr) {
        return;
    }
    for (const vn::graphics::RenderSurface::Presentation& presentation : d->surface->presentations()) {
        if (presentation.view == nullptr) {
            // The host took the area over with a pass of its own - build nothing and declare nothing, which is what
            // keeps a default chain from being drawn beside it.
            return;
        }
    }
    for (Impl::Pane& pane : d->panes) {
        if (pane.view->pipeline() == nullptr) {
            // The area's own view, when the host never configured it: the default request becomes its chain here,
            // where an area to draw into exists.
            (void)d->buildPipeline(pane);
        }
        if (pane.view->pipeline() != nullptr) {
            (void)d->surface->present(*pane.view);
        }
    }
}

VN_APPFWGUI_NS_END
