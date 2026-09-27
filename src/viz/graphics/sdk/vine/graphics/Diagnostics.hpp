#pragma once

#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <vine/String.hpp>

#include "RenderDiagnostic.hpp"
#include "ReportOnce.hpp"

/**
 * @brief The one diagnostic route of a render engine: counting, then forwarding to the host's sink.
 *
 * ONE ROUTE, BECAUSE A SECOND ROUTE LIES. Every report has to end up in the same place for two reasons
 * that have both bitten this engine: the host installs ONE sink (so a module reporting straight to its
 * own log is invisible to `diagnosticCount()`), and the counts are what a phase gates on (so a report
 * that bypasses the counter makes "nothing unexpected happened" unfalsifiable). Whoever reports - the
 * engine about its wiring, a backend about what it could not serve, a pass about content it skipped -
 * hands the sentence to the same instance instead of writing it itself.
 *
 * WHO OWNS IT: the engine, and the backend it drives is HANDED that instance (see
 * RenderBackend::setDiagnosticsRoute) instead of keeping a second one. That is what makes
 * RenderEngine::diagnosticCount() the whole truth instead of half of it, and it is why a host needs no
 * rule of the form "add the two counts". A backend driven without an engine (a test, a tool) is never
 * handed one and keeps the private route it was born with; every accessor answers from whichever route
 * is active, so such a user sees exactly the old behaviour.
 *
 * WHAT IT NEVER DOES: decide. A report describes what something could not serve; it never substitutes a
 * different picture, and it never turns a refusal into a silent fallback. Severities and categories are
 * the SDK's vocabulary (`RenderDiagnostic.hpp`) because the host switches on them.
 *
 * "Report once per episode" is the type next door (`ReportOnce.hpp`): the rule is shared, the episode's
 * boundary is each site's decision.
 */
VN_GRAPHICS_NS_BEGIN
/**
 * @brief The diagnostic route: counting, and forwarding to the host's sink (see the file note).
 */
class VN_GRAPHICS_API Diagnostics
{
  public:
    /** @brief Installs the host's sink, or clears it with an empty one.
     *
     * @param sink Callback invoked for every report; must only record and return.
     */
    void setSink(vn::graphics::DiagnosticSink sink);

    /** @brief Gets the installed sink (empty when unset). */
    [[nodiscard]] const vn::graphics::DiagnosticSink& sink() const noexcept;

    /** @brief Reports one diagnostic: it is counted and forwarded to the sink when one is installed.
     *
     * @param severity How bad it is (Info / Warning / Error).
     * @param category Machine-matchable category the host can switch on.
     * @param message Human-readable sentence; the category carries the meaning.
     */
    void report(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category,
                const vn::String& message);

    /** @brief Reports @p message for @p episode, unless that episode was already reported.
     *
     * @param episode Episode state this condition is reported through.
     * @param severity How bad it is.
     * @param category Machine-matchable category.
     * @param message Human-readable sentence.
     * @return true when this call was the one that reported it.
     */
    bool reportOnce(ReportOnce& episode, vn::graphics::DiagnosticSeverity severity,
                    vn::graphics::DiagnosticCategory category, const vn::String& message);

    /** @brief Gets how many diagnostics were reported in total. */
    [[nodiscard]] std::uint64_t total() const noexcept;

    /** @brief Gets how many diagnostics of @p category were reported.
     *
     * @param category Category to count; an out-of-range value counts nothing.
     */
    [[nodiscard]] std::uint64_t count(vn::graphics::DiagnosticCategory category) const noexcept;

    /** @brief Gets whether nothing at all was reported.
     *
     * The gate a phase asserts on: a steady run that is expected to be uneventful asks this instead of
     * reading the log.
     */
    [[nodiscard]] bool clean() const noexcept;


  private:
    /// One counter per category; the SDK's `Count` enumerator is the table's size.
    std::array<std::uint64_t, static_cast<std::size_t>(vn::graphics::DiagnosticCategory::Count)> per_category_{};
    std::uint64_t                       total_{0};
    vn::graphics::DiagnosticSink      sink_;
};

VN_GRAPHICS_NS_END
