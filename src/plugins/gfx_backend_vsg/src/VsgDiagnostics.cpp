#include <vine/vsg/VsgDiagnostics.hpp>

#include <cstdio>

#include <vine/vsg/SceneBridge.hpp>
#include <utility>

V_VSG_NS_BEGIN


namespace
{
/** @brief The severity word the stderr trace carries (what a harness greps for).
 *
 * @param severity Severity to name.
 * @return The word, never null.
 */
const char* severityTraceTag(vine::graphics::DiagnosticSeverity severity) noexcept
{
    switch (severity) {
    case vine::graphics::DiagnosticSeverity::Info: return "info";
    case vine::graphics::DiagnosticSeverity::Warning: return "warning";
    case vine::graphics::DiagnosticSeverity::Error: return "error";
    }
    return "diagnostic";
}
} // namespace

void VsgDiagnostics::report(vine::graphics::DiagnosticSeverity severity, vine::graphics::DiagnosticCategory category,
                            const vine::String& message) const
{
    // The trace is this backend's built-in fallback: a host that installs no sink (and the
    // validation harness, which installs none) must still see that content was dropped.
    std::fprintf(stderr, "[VsgRenderer] %s: %s\n", severityTraceTag(severity), message.stdstr().c_str());
    if (downstream) {
        downstream(vine::graphics::RenderDiagnostic{ severity, category, message });
    }
}

vine::graphics::DiagnosticSink VsgDiagnostics::route() const
{
    return [this](const vine::graphics::RenderDiagnostic& diagnostic) {
        report(diagnostic.severity, diagnostic.category, diagnostic.message);
    };
}

void VsgDiagnostics::setDownstream(vine::graphics::DiagnosticSink downstream)
{
    this->downstream = std::move(downstream);
}

void detail::installDiagnosticRoute(const VsgDiagnostics& diagnostics, SceneBridge& bridge)
{
    // The bridge reports a diagnostic; the renderer turns it into the single
    // route (stderr trace + backend counters + host sink). Routing it through
    // the renderer instead of handing the host sink straight to the bridge is
    // what keeps diagnosticCount() honest: a bridge report is a backend report.
    bridge.setDiagnosticSink(diagnostics.route());
}
V_VSG_NS_END
