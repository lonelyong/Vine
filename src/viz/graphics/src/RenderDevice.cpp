#include <vine/graphics/RenderDevice.hpp>

VN_GRAPHICS_NS_BEGIN

void RenderDevice::setDiagnosticsRoute(Diagnostics& route)
{
    // One pointer, so every accessor below re-reads the active route: a device that keeps its own
    // per-object state (a session, an executor) must be handed `diagnosticsRoute()` rather than being
    // given a second Diagnostics, or the engine's count would miss its reports.
    route_ = &route;
}

void RenderDevice::setDiagnosticSink(DiagnosticSink sink)
{
    route_->setSink(std::move(sink));
}

const DiagnosticSink& RenderDevice::diagnosticSink() const
{
    return route_->sink();
}

std::size_t RenderDevice::diagnosticCount() const
{
    return static_cast<std::size_t>(route_->total());
}

std::size_t RenderDevice::diagnosticCount(DiagnosticCategory category) const
{
    return static_cast<std::size_t>(route_->count(category));
}

void RenderDevice::reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category,
                                    const String& message)
{
    // Counting and forwarding are the route's business (see Diagnostics.hpp): the count is what a phase
    // gates on, and a host that never installed a sink must still be able to see that something
    // happened.
    route_->report(severity, category, message);
}

VN_GRAPHICS_NS_END
