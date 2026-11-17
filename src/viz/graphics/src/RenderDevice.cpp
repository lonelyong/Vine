#include <vine/graphics/RenderDevice.hpp>

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

intrusive_ptr<ImageRef> RenderDevice::readbackImage(const RenderTarget* target, int attachment, ImageRef::Kind kind)
{
    // A ONE-VERSION BRIDGE (BC-16): the legacy (target, index) spelling names the same image a pass input does. A
    // null target stays unbound, which the backend answers as Invalid - the answer the old spelling gave.
    auto image = make_intrusive<ImageRef>(String(u8"(readback)"), kind);
    if (target != nullptr) {
        image->bind(intrusive_ptr<RenderTarget>(const_cast<RenderTarget*>(target)), attachment);
    }
    return image;
}

void RenderDevice::setDiagnosticsRoute(Diagnostics& route)
{
    // A BORROWED ROUTE, NOT AN OWNED ONE: a device may be driven by several engines (they borrow it, see the class
    // note), each with its own route, and every report goes to every one of them - so a second engine cannot take
    // the first one's count away. The device's own route stays the one its accessors answer from, and it receives
    // every report too.
    if (std::find(borrowed_routes_.begin(), borrowed_routes_.end(), &route) != borrowed_routes_.end()) {
        return; // the same engine re-installing its own route is not a second installation
    }
    borrowed_routes_.push_back(&route);
}

void RenderDevice::removeDiagnosticsRoute(Diagnostics& route)
{
    borrowed_routes_.erase(std::remove(borrowed_routes_.begin(), borrowed_routes_.end(), &route), borrowed_routes_.end());
}

void RenderDevice::setDiagnosticSink(DiagnosticSink sink)
{
    owned_route_.setSink(std::move(sink));
}

const DiagnosticSink& RenderDevice::diagnosticSink() const
{
    // The sink this device's reports actually reach: an engine that installed one speaks for the device it borrowed -
    // that is what handing the route over means - and the device's own sink is the answer when no engine did. With
    // several engines borrowing one device, the first non-empty one answers; the reports reach all of them.
    for (const Diagnostics* borrowed : borrowed_routes_) {
        if (borrowed->sink()) {
            return borrowed->sink();
        }
    }
    return owned_route_.sink();
}

std::size_t RenderDevice::diagnosticCount() const
{
    return static_cast<std::size_t>(route_->total());
}

std::size_t RenderDevice::diagnosticCount(DiagnosticCategory category) const
{
    return static_cast<std::size_t>(route_->count(category));
}

void RenderDevice::reportDiagnostic(const RenderDiagnostic& diagnostic)
{
    // Counting and forwarding are the route's business (see Diagnostics.hpp): the count is what a phase
    // gates on, and a host that never installed a sink must still be able to see that something
    // happened. Every borrowed route gets the same report, so each engine's count is complete.
    route_->report(diagnostic);
    for (Diagnostics* borrowed : borrowed_routes_) {
        borrowed->report(diagnostic);
    }
}

void RenderDevice::reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category, const String& message)
{
    // THE MIGRATION FORM (see the header): the caller named no finer cause than the category.
    RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.message  = message;
    reportDiagnostic(diagnostic);
}

VN_GRAPHICS_NS_END
