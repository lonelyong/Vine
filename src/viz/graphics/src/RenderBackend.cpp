#include <vine/graphics/RenderBackend.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(RenderBackend, vine::Object);

void RenderBackend::setDiagnosticSink(DiagnosticSink sink)
{
    diagnostic_sink_ = std::move(sink);
}

std::size_t RenderBackend::diagnosticCount(DiagnosticCategory category) const
{
    const auto index = static_cast<std::size_t>(category);
    if (index >= diagnostic_counts_.size()) {
        return 0u;
    }
    return diagnostic_counts_[index];
}

void RenderBackend::reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category,
                                     const String& message)
{
    ++diagnostic_count_;
    const auto index = static_cast<std::size_t>(category);
    if (index < diagnostic_counts_.size()) {
        ++diagnostic_counts_[index];
    }
    if (diagnostic_sink_) {
        diagnostic_sink_(RenderDiagnostic{ severity, category, message });
    }
}

V_GRAPHICS_NS_END
