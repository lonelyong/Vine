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

RenderSurface::~RenderSurface() = default;

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
    // Declaring a view as what presents this surface IS asking for its default window pass: that is the pass the
    // table resolves to (a host that meant to present a pass of its own declares that pass - see the other
    // overload, and then this one is never called).
    view.ensureWindowPass();
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
    return true;
}

bool RenderSurface::hasPresentation() const noexcept
{
    return !entries_.empty();
}

std::vector<RenderSurface::Presentation> RenderSurface::presentations() const
{
    // Ordered by the ENGINE's schedule rather than by declaration: the first entry is the pass that draws first on
    // this surface - the one a clear would have to land on. Ties (one pass order, two presentations) keep
    // declaration order.
    struct Placed {
        int          order = 0;
        Presentation entry{};
    };

    std::vector<Placed> placed;
    if (engine_ != nullptr) {
        placed.reserve(entries_.size());
        for (const Entry& entry : entries_) {
            raw_ptr<RenderPass> const pass = passOf(entry);
            // A pass the engine does not know does not run: leaving it out is what makes this answer describe the
            // frame the engine would draw now instead of what was once declared.
            const std::optional<int> order = engine_->passOrder(pass);
            if (!order.has_value()) {
                continue;
            }
            placed.push_back(Placed{ *order, Presentation{ pass, entry.view } });
        }
        std::stable_sort(placed.begin(), placed.end(), [](const Placed& left, const Placed& right) {
            return left.order < right.order;
        });
    }

    std::vector<Presentation> ordered;
    ordered.reserve(placed.size());
    for (const Placed& entry : placed) {
        ordered.push_back(entry.entry);
    }
    return ordered;
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
