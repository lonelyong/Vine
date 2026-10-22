#include <vine/graphics/DiagnosticsRoute.hpp>

#include <vine/graphics/RenderEngine.hpp>

VN_GRAPHICS_NS_BEGIN

DiagnosticsRoute::DiagnosticsRoute(Lease<RenderEngine> engine) noexcept : engine_(engine)
{
}

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

void DiagnosticsRoute::report(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category,
                              const String& message)
{
    if (raw_ptr<RenderEngine> engine = engine_.get()) {
        engine->reportEngineProblem(severity, category, message);
    }
}

DiagnosticsRoute RenderEngine::diagnostics() noexcept
{
    return DiagnosticsRoute(lease());
}

VN_GRAPHICS_NS_END
