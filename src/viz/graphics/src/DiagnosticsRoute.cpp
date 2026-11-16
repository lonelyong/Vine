#include <vine/graphics/DiagnosticsRoute.hpp>

#include <vine/graphics/RenderEngine.hpp>

VN_GRAPHICS_NS_BEGIN

DiagnosticsRoute::DiagnosticsRoute(Lease<RenderEngine> engine) noexcept
  : engine_(engine)
{}

bool DiagnosticsRoute::isValid() const noexcept
{
    return engine_.get() != nullptr;
}

void DiagnosticsRoute::setSink(vn::graphics::DiagnosticSink sink)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->setDiagnosticSink(std::move(sink));
    }
}

std::size_t DiagnosticsRoute::count() const noexcept
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->diagnosticCount() : 0;
}

std::size_t DiagnosticsRoute::count(vn::graphics::DiagnosticOrigin origin) const noexcept
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->diagnosticCount(origin) : 0;
}

std::size_t DiagnosticsRoute::count(vn::graphics::DiagnosticCategory category) const noexcept
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine != nullptr ? engine->diagnosticCount(category) : 0;
}

bool DiagnosticsRoute::isClean() const noexcept
{
    raw_ptr<RenderEngine> engine = engine_.get();
    return engine == nullptr || engine->diagnosticsClean();
}

void DiagnosticsRoute::report(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const String& message)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        // THE ENGINE'S FINDING, delivered through its handle (see the header): a recipe says what it could not
        // express on the engine's behalf, so the value carries the Engine origin and the engine applies its own
        // compile/episode rule to it.
        vn::graphics::RenderDiagnostic diagnostic;
        diagnostic.origin   = vn::graphics::DiagnosticOrigin::Engine;
        diagnostic.severity = severity;
        diagnostic.category = category;
        diagnostic.message  = message;
        engine->reportEngineProblem(diagnostic);
    }
}

DiagnosticsRoute RenderEngine::diagnostics() noexcept
{
    return DiagnosticsRoute(lease());
}

VN_GRAPHICS_NS_END
