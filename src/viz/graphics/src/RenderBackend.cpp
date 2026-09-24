#include <vine/graphics/RenderBackend.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(RenderBackend, vn::Object);

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

VN_GRAPHICS_NS_END
