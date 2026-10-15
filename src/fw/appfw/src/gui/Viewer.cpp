#include <vine/appfw/gui/Viewer.hpp>

#include <utility>

#include <vine/graphics/Pipeline.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/SceneView.hpp>

VN_APPFWGUI_NS_BEGIN

struct Viewer::Impl {
    /// The engine the chain is built on and registered with (borrowed; must outlive the viewer).
    raw_ptr<vn::graphics::RenderEngine> engine = nullptr;
    /// The area this viewer draws (borrowed; must outlive the viewer).
    raw_ptr<vn::graphics::RenderSurface> surface = nullptr;
    /// The view that draws the area, and the request its chain is built from. One area draws one picture: the view
    /// is the picture's camera and content, the request is how it is drawn.
    intrusive_ptr<vn::graphics::SceneView> view;
    ViewerOptions                         options;
    /// "This view draws through another engine" is one condition, not a per-call event.
    mutable bool foreign_view_reported = false;

    /// @brief Builds the chain from the request (with this area's context), adopts it on the view, and carries the
    /// live chain's runtime choices onto it.
    bool buildPipeline()
    {
        if (engine == nullptr || view == nullptr) {
            return false;
        }

        // A rebuild changes the STRUCTURE, never the picture: whether the user has an overlay showing is a runtime
        // choice that lives on the live pipeline (Pipeline::setGizmoVisible / setStatsVisible), so it is read from
        // there and re-stated on the new one. Only an overlay that WAS there and was hidden is carried - one this
        // build adds comes up showing, because nothing has hidden it yet.
        vn::graphics::Pipeline* const previous = view->pipeline();
        const bool gizmo_was_hidden =
            previous != nullptr && previous->gizmo() != nullptr && !previous->isGizmoVisible();
        const bool stats_was_hidden =
            previous != nullptr && previous->fpsOverlay() != nullptr && !previous->isStatsVisible();

        // The request is the host-facing half; the CONTEXTUAL fields are filled in HERE, because this is the party
        // that knows them: the gizmo mirrors the view's camera (an overlay with no camera to mirror is exactly what
        // "no overlay" means), and the overlays are laid out in device pixels, so they take the area's ratio.
        const double ratio = surface != nullptr && surface->pixelRatio() > 0.0 ? surface->pixelRatio() : 1.0;

        vn::graphics::PipelineOptions pipeline_options;
        pipeline_options.path                = options.path;
        pipeline_options.mode                = options.mode;
        pipeline_options.offscreen_width     = options.offscreen_width;
        pipeline_options.offscreen_height    = options.offscreen_height;
        pipeline_options.layer_mask          = options.layer_mask;
        pipeline_options.gizmo.source_camera = options.gizmo ? view->camera() : nullptr;
        pipeline_options.gizmo.pixel_ratio   = ratio;
        pipeline_options.fps.enabled         = options.stats;
        pipeline_options.fps.pixel_ratio     = ratio;

        vn::graphics::RenderPipelineBuilder builder(engine);
        builder.setCamera(view->camera());
        builder.setContent(view->scene());
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
        // Adopting the pipeline is what gives the area its picture: the surface size behind it and the layout steps
        // are applied by the view itself, and the area keeps resolving the view's pass from the new pipeline (see
        // SceneView::setPipeline).
        view->setPipeline(std::move(pipeline));
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
                                    u8"registered on that engine's frame), so it was refused");
    }
};

Viewer::Viewer(raw_ptr<vn::graphics::RenderEngine> engine, raw_ptr<vn::graphics::RenderSurface> surface,
               intrusive_ptr<vn::graphics::SceneView> view)
  : d(new Impl())
{
    d->engine  = engine;
    d->surface = surface;
    if (view != nullptr) {
        // The area's view: registered from the start, but with no chain and no declaration - the first attach is
        // where the default request becomes a picture (see takeArea), so a host that never touches the viewer pays
        // no build until there is an area to draw into.
        d->view = std::move(view);
    }
}

Viewer::~Viewer()
{
    delete d;
}

ViewerOptions Viewer::viewerOptions() const
{
    return d->options;
}

bool Viewer::setViewerOptions(const ViewerOptions& options)
{
    if (d->engine == nullptr || d->view == nullptr) {
        return false;
    }
    if (d->view->engine() != d->engine) {
        d->reportForeignView();
        return false;
    }

    const ViewerOptions previous = d->options;
    d->options                   = options;
    if (!d->buildPipeline()) {
        // The builder refused the request: the previous chain stays in place, so the previous request does too (the
        // two describe the same picture - see ViewerOptions).
        d->options = previous;
        return false;
    }

    // The picture is declared on the area unless the host brought one of its own (a pass a host declared is not
    // overruled, and the area reports a second presentation by itself - see RenderSurface::present).
    if (d->surface != nullptr && d->surface->presentedView() != d->view.get()) {
        (void)d->surface->present(*d->view);
    }
    return true;
}

bool Viewer::setShadingMode(vn::graphics::ShadingMode mode)
{
    if (d->view == nullptr) {
        return false;
    }

    // The cheap path first, and it has to check the REQUEST, not just the mode: a non-Lit mode is realized on the
    // forward chain while the request still says Deferred (see effectivePath), so "the chain is live and can express
    // this mode" is only the cheap path when the live chain is the one this request resolves to. Picking Lit again
    // after such a mode therefore REBUILDS the deferred chain - which is what the host asked for.
    vn::graphics::PipelineOptions request;
    request.path = d->options.path;
    request.mode = mode;
    vn::graphics::Pipeline* const pipeline = d->view->pipeline();
    if (pipeline != nullptr &&
        pipeline->shadingPath() == vn::graphics::RenderPipelineBuilder::effectivePath(request) &&
        pipeline->setShadingMode(mode)) {
        d->options.mode = mode;  // remembered: the next build (any structural change) starts from here
        return true;
    }
    // The chain cannot express it (a deferred picture has no geometry pass to shade): the request itself has to
    // change, and the builder then assembles the chain that CAN express it.
    ViewerOptions options = d->options;
    options.mode          = mode;
    return setViewerOptions(options);
}

bool Viewer::release(raw_ptr<vn::graphics::SceneView> view)
{
    if (view == nullptr || d->view == nullptr || d->view.get() != view) {
        return false;
    }
    if (d->surface != nullptr) {
        // Withdrawing the declaration is what takes the picture out of the area; the chain stays adopted on the view,
        // which is the host's again (see the declaration).
        (void)d->surface->dismiss(view);
    }
    d->view = nullptr;
    return true;
}

raw_ptr<vn::graphics::SceneView> Viewer::view() const
{
    return d->view.get();
}

bool Viewer::isActive() const
{
    if (d->surface == nullptr || d->view == nullptr) {
        return false;
    }
    // The area's answer, not a remembered one: a host-declared pass IS the picture there, and a declaration
    // dismissed elsewhere is gone from here too.
    return d->surface->presentedView() == d->view.get();
}

void Viewer::takeArea()
{
    if (d->surface == nullptr || d->view == nullptr) {
        return;
    }
    // A pass a host declared means the host took the area over - build nothing and declare nothing, which is what
    // keeps a default chain from being drawn beside it.
    if (d->surface->hasPresentation() && d->surface->presentedView() == nullptr) {
        return;
    }
    if (d->view->pipeline() == nullptr) {
        // The defaults, when the host never configured the view: the request becomes its chain here, where an area
        // to draw into exists.
        (void)d->buildPipeline();
    }
    if (d->view->pipeline() != nullptr) {
        (void)d->surface->present(*d->view);
    }
}

VN_APPFWGUI_NS_END
