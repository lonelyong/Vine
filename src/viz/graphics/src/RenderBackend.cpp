#include <vine/graphics/RenderBackend.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(RenderBackend, vn::Object);

FrameDescription singlePassFrame(raw_ptr<const RenderPass> pass, int order,
                                 std::vector<raw_ptr<RenderTarget>> inputs,
                                 std::optional<PassDescription> description)
{
    // The ONE place a frame value is assembled outside the engine's compile, so a host that drives one pass
    // (RenderPass::execute) cannot build a shape of its own - there is one value in this SDK (see
    // RenderBackend::executeFrame).
    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass        = pass;
    entry.order       = order;
    entry.inputs      = std::move(inputs);
    entry.description = std::move(description);
    frame.passes.push_back(std::move(entry));
    return frame;
}

void RenderBackend::setDiagnosticsRoute(Diagnostics& route)
{
    // One pointer, so every accessor below re-reads the active route: a backend that keeps its own
    // per-object state (a session, an executor) must be handed `diagnosticsRoute()` rather than being
    // given a second Diagnostics, or the engine's count would miss its reports.
    route_ = &route;
}

void RenderBackend::setDiagnosticSink(DiagnosticSink sink)
{
    route_->setSink(std::move(sink));
}

const DiagnosticSink& RenderBackend::diagnosticSink() const
{
    return route_->sink();
}

std::size_t RenderBackend::diagnosticCount() const
{
    return static_cast<std::size_t>(route_->total());
}

std::size_t RenderBackend::diagnosticCount(DiagnosticCategory category) const
{
    return static_cast<std::size_t>(route_->count(category));
}

void RenderBackend::reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category,
                                     const String& message)
{
    // Counting and forwarding are the route's business (see Diagnostics.hpp): the count is what a phase
    // gates on, and a host that never installed a sink must still be able to see that something
    // happened.
    route_->report(severity, category, message);
}

VN_GRAPHICS_NS_END
