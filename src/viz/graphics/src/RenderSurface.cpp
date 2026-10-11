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
    // destroyed later does not call back into a surface that is gone (see SceneView::~SceneView). Nothing here
    // touches the engine - the whole point of doing it in this direction.
    for (const Entry& entry : entries_) {
        if (entry.view != nullptr) {
            entry.view->setSurface(nullptr);
        }
    }
}

bool RenderSurface::present(SceneView& view)
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

    entries_.push_back(Entry{ &view, nullptr });
    // The view and the surface name each other while the presentation stands: the surface resolves the view's
    // window pass from it, and the view tells the surface when it rebuilt that pass.
    view.setSurface(this);
    // Declaring a view as what presents this surface IS asking for its default window pass: that is the pass the
    // table resolves to (a host that meant to present a pass of its own declares that pass - see the other
    // overload, and then this one is never called).
    view.ensureWindowPass();
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

bool RenderSurface::dismiss(raw_ptr<SceneView> view)
{
    const auto entry = std::find_if(entries_.begin(), entries_.end(), [view](const Entry& candidate) {
        return candidate.view == view;
    });
    if (entry == entries_.end()) {
        return false;
    }
    entries_.erase(entry);
    // Called from ~SceneView as well: the view stops naming this surface (it may be half gone - nothing here
    // touches it beyond this pointer), and what is left of the table re-states its clear.
    if (view != nullptr) {
        view->setSurface(nullptr);
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

std::vector<RenderSurface::Presentation> RenderSurface::presentations() const
{
    std::vector<Presentation> ordered;
    for (const Resolved& entry : resolvedEntries()) {
        ordered.push_back(Presentation{ entry.pass, entries_[entry.index].view });
    }
    return ordered;
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
