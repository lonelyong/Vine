#include <vine/graphics/Diagnostics.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

void Diagnostics::setSink(vn::graphics::DiagnosticSink sink)
{
    // The swap is a short critical section around the FUNCTION object only: a report copies the sink out and calls
    // it after releasing the lock, so this never waits for a host's callback to finish (see the class note).
    const std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = std::move(sink);
}

vn::graphics::DiagnosticSink Diagnostics::sink() const
{
    const std::lock_guard<std::mutex> lock(sink_mutex_);
    return sink_;
}

void Diagnostics::report(const vn::graphics::RenderDiagnostic& diagnostic)
{
    // Counted first, and counted whether or not a sink is installed: the count is what a phase gates
    // on, and a host that never installed a sink must still be able to see that something happened.
    // The ORIGIN decides which numbers move (see DiagnosticOrigin): a host's own injection is counted, but
    // not in the engine's total - that is what keeps "the engine found nothing" decidable.
    // Relaxed: a counter is a running total, not a publication, and the reader only needs each increment to be
    // atomic (never half-written). Anything stronger would order nothing a reader could use.
    const auto origin = static_cast<std::size_t>(diagnostic.origin);
    if (origin < per_origin_.size()) {
        per_origin_[origin].fetch_add(1U, std::memory_order_relaxed);
        const auto category = static_cast<std::size_t>(diagnostic.category);
        if (category < per_category_[origin].size()) {
            per_category_[origin][category].fetch_add(1U, std::memory_order_relaxed);
        }
    }

    vn::graphics::DiagnosticSink sink;
    {
        const std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = sink_;
    }
    if (sink) {
        // A SINK THAT THROWS IS THE HOST'S ERROR, NOT THE FRAMEWORK'S FAILURE. Every path in this module reports
        // through here - a frame's own failure answer, a pass that skipped its content, a backend's refusal - and an
        // exception out of the callback would travel back through whichever of them was running: it would cost a
        // frame its answer (design 2.12, "a frame is always answered") and, on a frame thread, reach the thread
        // function. The report has already been counted above, so what is lost is only the host's own observation of
        // its own bug - and it cannot be reported through the route that just threw.
        try {
            sink(diagnostic);
        }
        catch (...) {
        }
    }
}

void Diagnostics::report(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const vn::String& message)
{
    // THE MIGRATION FORM (see the header): no code, because the caller named no finer cause than the category.
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.message  = message;
    report(diagnostic);
}

bool Diagnostics::reportOnce(ReportOnce& episode, const vn::graphics::RenderDiagnostic& diagnostic)
{
    if (!episode.shouldReport()) {
        return false;
    }
    report(diagnostic);
    return true;
}

bool Diagnostics::reportOnce(ReportOnce&                      episode,
                             vn::graphics::DiagnosticSeverity severity,
                             vn::graphics::DiagnosticCategory category,
                             const vn::String&                message)
{
    if (!episode.shouldReport()) {
        return false;
    }
    report(severity, category, message);
    return true;
}

std::uint64_t Diagnostics::total() const noexcept
{
    // THE ENGINE'S NUMBER, NOT THE ROUTE'S: Engine + Device, per DiagnosticOrigin. A host's injection is a
    // separate answer (total(DiagnosticOrigin::Host)), so a host bug cannot read as an engine problem.
    return per_origin_[static_cast<std::size_t>(vn::graphics::DiagnosticOrigin::Engine)].load(std::memory_order_relaxed) +
           per_origin_[static_cast<std::size_t>(vn::graphics::DiagnosticOrigin::Device)].load(std::memory_order_relaxed);
}

std::uint64_t Diagnostics::total(const vn::graphics::DiagnosticOrigin origin) const noexcept
{
    const auto index = static_cast<std::size_t>(origin);
    return index < per_origin_.size() ? per_origin_[index].load(std::memory_order_relaxed) : 0;
}

std::uint64_t Diagnostics::count(const vn::graphics::DiagnosticCategory category) const noexcept
{
    const auto index = static_cast<std::size_t>(category);
    if (index >= per_category_[0].size()) {
        return 0;
    }
    // Same scope as total(): a host's own report does not change what the engine addressed.
    return per_category_[static_cast<std::size_t>(vn::graphics::DiagnosticOrigin::Engine)][index].load(std::memory_order_relaxed) +
           per_category_[static_cast<std::size_t>(vn::graphics::DiagnosticOrigin::Device)][index].load(std::memory_order_relaxed);
}

bool Diagnostics::clean() const noexcept
{
    return total() == 0;
}

VN_GRAPHICS_NS_END
