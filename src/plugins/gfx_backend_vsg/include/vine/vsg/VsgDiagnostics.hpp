#pragma once

/**
 * @brief The plugin's one diagnostic route: the stderr trace, then the host's channel.
 *
 * The SDK's RenderBackend already owns the host-facing sink and its counters
 * (`reportDiagnostic` / `diagnosticCount()`), so a backend has nothing to invent there. What
 * this plugin needs is a NAME for "where a failing path reports": one place that traces the
 * report on stderr (the validation harness reads it) and then hands it to the host's channel.
 *
 * It lives outside `VsgRenderer` because the reporting paths are not all renderer methods —
 * the free-function layers (the pass materialisation, the target bookkeeping, the overlay
 * assembly) report too, and they hold no renderer to call one on.
 *
 * A report is synchronous and must not re-enter the backend (RenderBackend's contract); a
 * sink that does is the host's mistake and this route does not guard against it.
 *
 * @ref route() hands a MODULE (SceneBridge) the same route, so a module's report is counted
 * and delivered exactly like a renderer's. The returned sink captures this object, so it may
 * not outlive it: the bridges it is installed on are owned by the session state, which the
 * renderer outlives.
 */

#include <vine/vsg/vsg_global.hpp>

#include <vine/String.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>

V_VSG_NS_BEGIN

struct SceneBridge;

struct VsgDiagnostics
{
    /** @brief Traces one report and hands it to the host's channel.
     *
     * @param severity How bad the situation is (the SDK's vocabulary).
     * @param category What it is about (the SDK's vocabulary: a host switches on it, so the
     *                 message may change without breaking a caller).
     * @param message  Human-readable detail, with the numbers involved.
     */
    void report(vine::graphics::DiagnosticSeverity severity, vine::graphics::DiagnosticCategory category,
                const vine::String& message) const;

    /** @brief Gets a sink that routes another module's reports into this route.
     *
     * See the note above on the capture's lifetime.
     *
     * @return Sink to install on the module's own diagnostic channel.
     */
    [[nodiscard]] vine::graphics::DiagnosticSink route() const;

    /** @brief Installs the channel a report is forwarded to after the trace.
     *
     * The renderer installs the SDK channel here (its own `reportDiagnostic`), which keeps
     * the host's sink and the SDK's counters the single authority for what the host can
     * query.
     *
     * @param downstream Sink to forward to (empty = the trace only, still counted).
     */
    void setDownstream(vine::graphics::DiagnosticSink downstream);

    /// Where a report goes after the trace: the backend's host-facing channel.
    vine::graphics::DiagnosticSink downstream;
};


namespace detail
{

/** @brief Installs a module's reports on the backend's single diagnostic route.
 *
 * The module (a content slot's SceneBridge) reports a diagnostic; routing it through the
 * backend's route instead of handing the host sink straight to the module is what keeps the
 * backend's counters honest: a module report IS a backend report.
 *
 * @param diagnostics Route the module's reports are delivered to.
 * @param bridge      Module whose diagnostic channel is installed on (null is a no-op).
 */
void installDiagnosticRoute(const VsgDiagnostics& diagnostics, SceneBridge& bridge);

} // namespace detail
V_VSG_NS_END
