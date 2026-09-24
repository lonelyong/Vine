#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/String.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The backend's one diagnostic route, and "report this condition once per episode" as a type.
 *
 * ONE ROUTE, BECAUSE A SECOND ROUTE LIES. Every report has to end up in the same place for two reasons
 * that have both bitten this backend: the host installs ONE sink (so a module reporting straight to its
 * own log is invisible to `diagnosticCount()`), and the counts are what a phase gates on (so a report
 * that bypasses the counter makes "nothing unexpected happened" unfalsifiable). Modules therefore hand
 * their reports to a Diagnostics instance instead of writing them themselves.
 *
 * REPORT ONCE PER EPISODE, and the episode's END is the caller's decision - a new frame, a new scope, a
 * usable target size, every announced light lit again. The rule is shared, the boundary is not, which is
 * why re-arming is explicit here: `if (!flag) { flag = true; report(); }` in one place and a bare
 * `flag = false;` in another are two halves of one rule that nothing checks, and a type that owns both
 * halves is the only way a reader can see the rule in one piece.
 *
 * WHAT IT NEVER DOES: decide. A report describes what the backend could not serve; it never substitutes
 * a different picture, and it never turns a refusal into a silent fallback. Severities and categories
 * are the SDK's vocabulary (`RenderDiagnostic.hpp`) because the host switches on them.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief "Report this condition once, until something says the episode ended". */
class ReportOnce
{
  public:
    /** @brief Gets whether this episode still needs reporting, and records that it was.
     *
     * @return true on the first call of an episode, false for the rest of it.
     */
    [[nodiscard]] bool shouldReport() noexcept;

    /** @brief Gets whether the episode has been reported, without changing it.
     *
     * For the sites and tests that assert on the state - a rule about episodes is not observable
     * otherwise.
     *
     * @return true while the episode has been reported and not re-armed.
     */
    [[nodiscard]] bool reported() const noexcept;

    /** @brief Ends the episode: the same condition reports again.
     *
     * Called by whoever can tell the condition ended; that boundary is each site's own.
     */
    void rearm() noexcept;


  private:
    bool reported_{false};
};

/**
 * @brief The diagnostic route: counting, and forwarding to the host's sink (see the file note).
 */
class Diagnostics
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

}  // namespace core

VN_VSG_NS_END
