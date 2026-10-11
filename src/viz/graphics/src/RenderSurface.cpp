#include <vine/graphics/RenderSurface.hpp>

#include <algorithm>
#include <optional>

#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipeline.hpp>
#include <vine/graphics/SceneView.hpp>

VN_GRAPHICS_NS_BEGIN

RenderSurface::RenderSurface(raw_ptr<RenderEngine> engine)
  : engine_(engine)
{}

RenderSurface::~RenderSurface()
{
    // The views are the host's, and they outlive this table whenever the host says so: stop naming it, so a view
    // destroyed later does not call back into a surface that is gone (see SceneView::~SceneView). Each view takes
    // its rectangle back - it keeps drawing into the same area, which is its own statement again from here on.
    // Nothing here touches the engine - the whole point of doing it in this direction.
    for (const Entry& entry : entries_) {
        if (entry.view != nullptr) {
            entry.view->setSurface(nullptr);
            entry.view->setViewport(entry.rect);
        }
    }
}

bool RenderSurface::present(SceneView& view)
{
    // The view's own rectangle (what it draws into while it presents nothing) becomes the presentation's: a view
    // that was already drawing into part of a surface keeps that part once it is presented.
    return present(view, view.viewport());
}

bool RenderSurface::present(SceneView& view, const NormalizedViewport& rect)
{
    for (const Entry& entry : entries_) {
        if (entry.view == &view) {
            return false;
        }
    }

    // A presentation is a statement about THIS engine's frame: the pass is registered there, and the order comes
    // from there. A view bound to another engine (or to none) could present this surface never, and "declared, but
    // nothing is drawn" is exactly the silence this table exists to end - so it is refused out loud.
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

    // One view draws ONE picture, and its rectangle lives in the table of the surface it presents: a view that
    // already presents another surface would have two rectangles and no way to tell which one it draws into (it
    // reads its rectangle from the surface it presents - see SceneView::viewport).
    if (view.surface_ != nullptr && view.surface_ != this) {
        engine_->reportEngineProblem(
            DiagnosticSeverity::Warning,
            DiagnosticCategory::PassProtocolViolation,
            u8"RenderSurface::present: this view already presents another surface, and a view draws one picture "
            u8"(its rectangle is read from the surface it presents); dismiss it there first");
        return false;
    }

    entries_.push_back(Entry{ &view, nullptr, rect.isValid() ? rect : NormalizedViewport{} });
    // The view and the surface name each other while the presentation stands: the surface resolves the view's
    // window pass from it, and the view tells the surface when it rebuilt that pass.
    view.setSurface(this);
    // DECLARING A VIEW IS DECLARING ITS PIPELINE. What presents this surface is the pass of the pipeline the view
    // ADOPTED (SceneView::setPipeline - built by RenderPipelineBuilder), and this table reads it back through the
    // view, so it never has to know a recipe, a chain or an overlay: a view with no pipeline has nothing to
    // resolve, and a declaration without one would leave a host believing a window is being drawn into.
    if (view.pipeline() == nullptr) {
        entries_.pop_back();
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
    // The size this surface last saw reaches the new presentation AT ONCE (the view reads it from here): a pane
    // added to a running surface is sized without the host feeding it (that is what makes the surface the ONE
    // owner of the size). Its geometry follows the rectangle THIS table now states (its window pass' device
    // rectangle, its targets, its HUD boxes): a use of the rectangle, never a second copy of it.
    view.applySurfaceGeometry();
    applyClear();
    return true;
}

bool RenderSurface::present(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return false;
    }
    for (const Entry& entry : entries_) {
        if (entry.pass == pass) {
            return false;
        }
    }
    entries_.push_back(Entry{ nullptr, intrusive_ptr<RenderPass>(pass) });
    applyClear();
    return true;
}

bool RenderSurface::setViewport(raw_ptr<SceneView> view, const NormalizedViewport& rect)
{
    const NormalizedViewport next = rect.isValid() ? rect : NormalizedViewport{};
    for (Entry& entry : entries_) {
        if (entry.view != view) {
            continue;
        }
        entry.rect = next;
        if (view != nullptr) {
            // The view re-derives what follows from this one rectangle - its window pass' device rectangle, the
            // size its targets and HUD boxes get, the shape its picture is projected with (the backend derives
            // that from the pass' viewport at render time). Geometry: nothing is rebuilt (see the header).
            view->applySurfaceGeometry();
        }
        return true;
    }
    return false;
}

bool RenderSurface::dismiss(raw_ptr<SceneView> view)
{
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [view](const Entry& candidate) {
        return candidate.view == view;
    });
    if (entry == entries_.end()) {
        return false;
    }
    const NormalizedViewport rect = entry->rect;
    entries_.erase(entry);
    if (focused_ == view) {
        // The focused pane is gone: forget it. What it buys is not the answer (the search in focusedView() could not
        // find a dismissed entry either, and that is what makes the two of them agree) but SAFETY: the pointer must
        // not stay around to be compared with a view allocated later at the same address, which would hand a fresh
        // pane a focus nobody gave it.
        focused_ = nullptr;
    }
    // Called from ~SceneView as well: the view stops naming this surface (it may be half gone - nothing here
    // touches it beyond this pointer), and what is left of the table re-states its clear. The rectangle goes back
    // to the view - it must come off the surface BEFORE the view is told, or the view would read the rectangle
    // from a table that no longer holds its entry.
    if (view != nullptr) {
        view->setSurface(nullptr);
        view->setViewport(rect);
    }
    applyClear();
    return true;
}

bool RenderSurface::dismiss(raw_ptr<RenderPass> pass)
{
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [pass](const Entry& candidate) {
        return candidate.pass == pass;
    });
    if (entry == entries_.end()) {
        return false;
    }
    entries_.erase(entry);
    applyClear();
    return true;
}

bool RenderSurface::hasPresentation() const noexcept
{
    return !entries_.empty();
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
    // Handed to every view that presents this surface, and only to those: a pass a host declared has no view to
    // feed, and it states its own rectangle in device pixels. The views RE-DERIVE their geometry from the numbers
    // recorded here (they read them - see SceneView::surfaceWidth), so there is one size and no copy to keep in
    // step. One call therefore sizes a split, and the next present() gets the same numbers through the entry it
    // adds (see present(SceneView&, const NormalizedViewport&)).
    for (const Entry& entry : entries_) {
        if (entry.view != nullptr) {
            entry.view->applySurfaceGeometry();
        }
    }
}

double RenderSurface::pixelRatio() const noexcept
{
    return pixel_ratio_;
}

std::vector<RenderSurface::Presentation> RenderSurface::presentations() const
{
    std::vector<Presentation> ordered;
    for (const Resolved& entry : resolvedEntries()) {
        const Entry& source = entries_[entry.index];
        // A host-declared pass has no rectangle here: it states its own in device pixels (see Presentation::rect).
        const std::optional<NormalizedViewport> rect =
            source.view != nullptr ? std::optional<NormalizedViewport>(source.rect) : std::nullopt;
        ordered.push_back(Presentation{ entry.pass, source.view, rect });
    }
    return ordered;
}

raw_ptr<SceneView> RenderSurface::viewAt(double nx, double ny) const
{
    const std::vector<Resolved> entries = resolvedEntries();
    // Backwards: the entry drawn LAST is the picture the point is on when rectangles overlap.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const Entry& entry = entries_[it->index];
        if (entry.view != nullptr && entry.rect.contains(nx, ny)) {
            return entry.view;
        }
    }
    return nullptr;
}

NormalizedViewport RenderSurface::viewportOf(raw_ptr<const SceneView> view) const noexcept
{
    for (const Entry& entry : entries_) {
        if (entry.view == view) {
            return entry.rect;
        }
    }
    return NormalizedViewport{};
}

bool RenderSurface::focus(raw_ptr<SceneView> view)
{
    if (view == nullptr) {
        focused_ = nullptr;
        return true;
    }
    // Focus is a pane of THIS surface: a view that presents nothing here has no rectangle to be focused in (it is
    // the host's own view drawing through a pass it declared, say), so remembering it would answer later questions
    // - which view does this key belong to, which view does this command mean - with a view this table cannot
    // describe.
    for (const Entry& entry : entries_) {
        if (entry.view == view) {
            focused_ = view;
            return true;
        }
    }
    return false;
}

raw_ptr<SceneView> RenderSurface::focusedView() const
{
    const std::vector<Resolved> live = resolvedEntries();
    // The focused view has to be a presentation of the frame the engine would draw NOW: a pump that dropped the
    // pass (a disabled recipe, a dismissed presentation) must not keep the focus on a picture nobody draws.
    if (focused_ != nullptr) {
        for (const Resolved& entry : live) {
            if (entries_[entry.index].view == focused_) {
                return focused_;
            }
        }
    }
    for (const Resolved& entry : live) {
        if (entries_[entry.index].view != nullptr) {
            return entries_[entry.index].view;
        }
    }
    return nullptr;
}

std::vector<RenderSurface::Resolved> RenderSurface::resolvedEntries() const
{
    // Ordered by the ENGINE's schedule rather than by declaration: the first entry is the pass that draws first on
    // this surface - the one the clear lands on. Ties (one pass order, two presentations) keep declaration order.
    std::vector<Resolved> placed;
    if (engine_ == nullptr) {
        return placed;
    }

    placed.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        raw_ptr<RenderPass> const pass = passOf(entries_[index]);
        // A pass the engine does not know does not run: leaving it out is what makes this answer describe the
        // frame the engine would draw now instead of what was once declared.
        const std::optional<int> order = engine_->passOrder(pass);
        if (!order.has_value()) {
            continue;
        }
        placed.push_back(Resolved{ index, pass, *order });
    }
    std::stable_sort(placed.begin(), placed.end(), [](const Resolved& left, const Resolved& right) {
        return left.order < right.order;
    });
    return placed;
}

void RenderSurface::applyClear()
{
    std::vector<raw_ptr<RenderPass>> claimed;
    for (const Resolved& resolved : resolvedEntries()) {
        Entry&              entry = entries_[resolved.index];
        raw_ptr<RenderPass> pass  = resolved.pass;
        // The same pass can be reached twice (a view's window pass, declared as a pass of its own as well): one
        // pass, one decision - the first entry that names it speaks for it.
        if (std::find(claimed.begin(), claimed.end(), pass) != claimed.end()) {
            continue;
        }
        claimed.push_back(pass);

        // A pass this entry has not been seen with before is a FRESH ask (a rebuilt pipeline's window pass is a new
        // pass, created enabled): read what it asks for itself - before this surface takes anything away.
        if (entry.asked_of != pass) {
            entry.asked_of = intrusive_ptr<RenderPass>(pass);
            entry.clears   = pass->isClearEnabled();
        }

        // The first presentation announces the clear; every other one is told to stop claiming to clear, because
        // the backend applies the first announcement per attachment and silently drops the rest. The colour and the
        // depth are NOT decided here: they stay on the pass that asked for them, so a surface clears to what its
        // first presentation said, and a presentation that does not want a clear (a Deferred present pass draws
        // over the whole surface) is not made to want one by being first.
        pass->setClearEnabled(claimed.size() == 1u && entry.clears);
    }
}

raw_ptr<RenderPass> RenderSurface::passOf(const Entry& entry) const
{
    if (entry.pass != nullptr) {
        return entry.pass.get();
    }
    // Resolved on every read, never remembered: a view's window pass is a NEW pass each time a recipe change
    // rebuilds its pipeline, and the table has to name the one that is in the frame.
    SceneView* const view = entry.view;
    if (view == nullptr) {
        return nullptr;
    }
    Pipeline* const pipeline = view->pipeline();
    return pipeline != nullptr ? pipeline->windowPass() : nullptr;
}

VN_GRAPHICS_NS_END
