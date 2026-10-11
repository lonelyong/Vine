#include <vine/graphics/RenderBackend.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(RenderBackend, vn::Object);

void announcePassTo(const PassDescription& description, raw_ptr<RenderBackend> backend)
{
    backend->setRenderTarget(description.target);
    if (description.viewport.has_value()) {
        const Viewport& viewport = *description.viewport;
        backend->setViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    }
    if (description.clear.has_value()) {
        backend->setClearPolicy(*description.clear);
    }
    backend->setPassState(description.state);
}

void announceDrawsTo(const PassDescription& description, raw_ptr<RenderBackend> backend)
{
    for (const DrawDescription& draw : description.draws) {
        if (draw.lights.has_value()) {
            backend->setLights(*draw.lights);
        }
        if (draw.camera == nullptr) {
            continue;
        }
        if (draw.kind == DrawDescription::Kind::Screen) {
            backend->drawScreenProgram(draw.source, draw.program.get(), draw.camera);
        }
        else if (draw.commands != nullptr) {
            backend->render(*draw.commands, draw.camera);
        }
    }
}

void RenderBackend::executeFrame(const FrameDescription& frame)
{
    // The default frame body: the value, announced through the per-call entries in the order this interface
    // documents. Every backend that keeps no state it wants to build from the whole frame gets exactly the
    // sequence it always got, from ONE input - so adding the value to the contract changed nothing for them, and
    // a backend that would rather build from the value overrides this instead of implementing the entries twice.
    for (const FrameDescription::Pass& pass : frame.passes) {
        if (pass.pass == nullptr) {
            continue;   // a frame's entry always names a pass; a null one is nothing to run
        }
        beginPass(pass.pass);
        setPassOrder(pass.order);
        setPassInputs(pass.inputs);
        if (pass.description.has_value()) {
            announcePassTo(*pass.description, this);
            announceDrawsTo(*pass.description, this);
        }
        endPass();
    }
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
