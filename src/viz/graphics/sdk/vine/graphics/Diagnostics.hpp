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
 * @details
 * - ONE ROUTE, BECAUSE A SECOND ROUTE LIES. Every report has to end up in the same place for two reasons that have
 *   both bitten this engine: the host installs ONE sink (so a module reporting straight to its own log is invisible
 *   to `diagnosticCount()` ), and the counts are what a phase gates on (so a report that bypasses the counter makes
 *   "nothing unexpected happened" unfalsifiable). Whoever reports - the engine about its wiring, a backend about what
 *   it could not serve, a pass about content it skipped - hands the sentence to the same instance instead of writing
 *   it itself.
 * - WHO OWNS IT: the engine, and the backend it drives is HANDED that instance (see
 *   RenderBackend::setDiagnosticsRoute) instead of keeping a second one. That is what makes
 *   RenderEngine::diagnosticCount() the whole truth instead of half of it, and it is why a host needs no rule of the
 *   form "add the two counts". A backend driven without an engine (a test, a tool) is never handed one and keeps the
 *   private route it was born with; every accessor answers from whichever route is active, so such a user sees
 *   exactly the old behaviour.
 * - WHAT IT NEVER DOES: decide. A report describes what something could not serve; it never substitutes a different
 *   picture, and it never turns a refusal into a silent fallback. Severities and categories are the SDK's vocabulary
 *   (`RenderDiagnostic.hpp`) because the host switches on them.
 * - "Report once per episode" is the type next door (`ReportOnce.hpp`): the rule is shared, the episode's boundary is
 *   each site's decision.
 */
VN_GRAPHICS_NS_BEGIN

/**
 * @brief The diagnostic route: counting, and forwarding to the host's sink (see the file note).
 */
class VN_GRAPHICS_API Diagnostics {
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
     * @details
     * - THE PRIMARY FORM: the value carries the severity, the category, the stable @ref RenderDiagnostic::code, the
     *   subject and (when it applies) the limit, so a host can act on it without parsing the sentence.
     *
     * @param diagnostic What to report (see RenderDiagnostic).
     */
    void report(const vn::graphics::RenderDiagnostic& diagnostic);

    /** @brief Reports @p diagnostic for @p episode, unless that episode was already reported.
     *
     * @param episode    Episode state this condition is reported through.
     * @param diagnostic What to report.
     * @return true when this call was the one that reported it.
     */
    bool reportOnce(ReportOnce& episode, const vn::graphics::RenderDiagnostic& diagnostic);

    /** @brief Reports a diagnostic whose cause is fully described by its category (no finer code).
     *
     * @details
     * - THE MIGRATION FORM, and the gate knows it: `check_diagnostic_formats.py` refuses NEW call sites of this shape
     *   (a report with a code is what a host can match on), while the ones already written are ratcheted by a
     *   baseline. A site that CAN name its cause should use @ref report(const RenderDiagnostic&) instead.
     *
     * @param severity How bad it is (Info / Warning / Error).
     * @param category Machine-matchable category the host can switch on.
     * @param message Human-readable sentence; the category carries the meaning.
     */
    void report(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const vn::String& message);

    /** @brief Reports @p message for @p episode, unless that episode was already reported.
     *
     * @param episode Episode state this condition is reported through.
     * @param severity How bad it is.
     * @param category Machine-matchable category.
     * @param message Human-readable sentence.
     * @return true when this call was the one that reported it.
     */
    bool reportOnce(ReportOnce& episode, vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category, const vn::String& message);

    /** @brief Gets how many diagnostics the ENGINE and its DEVICE reported (not the host's own).
     *
     * @details
     * - THE SCOPE IS THE POINT (see DiagnosticOrigin): a report the layer above injected through
     *   RenderEngine::reportEngineProblem is counted, but not here - "how many problems did the engine find"
     *   has to stay decidable, and a host bug is not one of them (A-06).
     *
     * @return The total of the Engine and Device origins.
     */
    [[nodiscard]] std::uint64_t total() const noexcept;

    /** @brief Gets how many diagnostics @p origin reported.
     *
     * @param origin Origin to count; an out-of-range value counts nothing.
     * @return The count for that origin.
     */
    [[nodiscard]] std::uint64_t total(vn::graphics::DiagnosticOrigin origin) const noexcept;

    /** @brief Gets how many diagnostics of @p category the ENGINE and its DEVICE reported.
     *
     * @details
     * - The same scope as @ref total(): the per-category numbers answer "what did the engine and its device
     *   run into", and a host's own injection is visible through @ref total(DiagnosticOrigin::Host).
     *
     * @param category Category to count; an out-of-range value counts nothing.
     */
    [[nodiscard]] std::uint64_t count(vn::graphics::DiagnosticCategory category) const noexcept;

    /** @brief Gets whether nothing was reported by the ENGINE or its DEVICE.
     *
     * @details
     * - The gate a phase asserts on: a steady run that is expected to be uneventful asks this instead of reading
     *   the log. It uses @ref total()'s scope, so `diagnosticCount() == 0` and this answer agree.
     */
    [[nodiscard]] bool clean() const noexcept;


  private:
    /// One counter per (origin, category); `DiagnosticOrigin::Count` sizes the first dimension, the SDK's `Count` the second.
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(vn::graphics::DiagnosticCategory::Count)>,
               static_cast<std::size_t>(vn::graphics::DiagnosticOrigin::Count)>
        per_category_{};
    /// One counter per origin; `DiagnosticOrigin::Count` is its size.
    std::array<std::uint64_t, static_cast<std::size_t>(vn::graphics::DiagnosticOrigin::Count)> per_origin_{};
    vn::graphics::DiagnosticSink                                                               sink_;
};

VN_GRAPHICS_NS_END
