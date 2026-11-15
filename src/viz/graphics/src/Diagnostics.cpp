#include <vine/graphics/Diagnostics.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

void Diagnostics::setSink(vn::graphics::DiagnosticSink sink)
{
    sink_ = std::move(sink);
}

const vn::graphics::DiagnosticSink& Diagnostics::sink() const noexcept
{
    return sink_;
}

void Diagnostics::report(const vn::graphics::RenderDiagnostic& diagnostic)
{
    // Counted first, and counted whether or not a sink is installed: the count is what a phase gates
    // on, and a host that never installed a sink must still be able to see that something happened.
    ++total_;
    const auto index = static_cast<std::size_t>(diagnostic.category);
    if (index < per_category_.size()) {
        ++per_category_[index];
    }

    if (sink_) {
        sink_(diagnostic);
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
    return total_;
}

std::uint64_t Diagnostics::count(vn::graphics::DiagnosticCategory category) const noexcept
{
    const auto index = static_cast<std::size_t>(category);
    return index < per_category_.size() ? per_category_[index] : 0;
}

bool Diagnostics::clean() const noexcept
{
    return total_ == 0;
}

VN_GRAPHICS_NS_END
