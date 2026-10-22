#pragma once
#include "graphics_global.hpp"

#include <cstddef>

#include <vine/Lease.hpp>
#include <vine/String.hpp>

#include "RenderDiagnostic.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderEngine;

/**
 * @brief The engine's ONE diagnostic route as a narrow handle (counting, then the host's sink).
 *
 * @details
 * - WHY A HANDLE: a recipe has to say what it refused ("this chain cannot express that mode", "no such overlay"),
 *   and it must say it through the same route every other report uses - otherwise the host installs one sink and the
 *   count is half the truth. A chain therefore holds this handle, not the engine.
 * - REPORTING DURING A COMPILE goes through the engine's rule: what a compile finds is collected and said once per
 *   episode (see RenderEngine::reportEngineProblem), and this handle preserves that - it is a view, not a second
 *   route.
 * - The engine may be destroyed first and the handle knows it (a `Lease`): once it is gone a report is a no-op and
 *   the counts answer zero.
 */
class VN_GRAPHICS_API DiagnosticsRoute
{
  public:
    /** @brief Constructs an empty handle that refers to no engine (see isValid). */
    DiagnosticsRoute() noexcept = default;

  public:
    /** @brief Gets whether the engine this handle reads is still alive.
     *
     * @return true when the engine is there and the calls below reach it.
     */
    [[nodiscard]] bool isValid() const noexcept;

    /** @brief Installs the host's sink, or clears it with an empty one.
     *
     * @param sink Callback invoked for every report; must only record and return.
     */
    void setSink(vn::graphics::DiagnosticSink sink);

    /** @brief Gets how many diagnostics the route has seen.
     *
     * @return The total count (0 when the engine is gone).
     */
    [[nodiscard]] std::size_t count() const noexcept;

    /** @brief Gets how many diagnostics of @p category the route has seen.
     *
     * @param category Category to count.
     * @return The count for that category (0 when the engine is gone).
     */
    [[nodiscard]] std::size_t count(vn::graphics::DiagnosticCategory category) const noexcept;

    /** @brief Gets whether nothing has been reported.
     *
     * @return true when the count is zero (also true when the engine is gone).
     */
    [[nodiscard]] bool isClean() const noexcept;

    /** @brief Reports one diagnostic through the engine's route.
     *
     * @details
     * - The call is counted and forwarded to the sink exactly as a report from the engine itself; during a compile
     *   it is collected and the episode rule decides whether it is said out loud.
     *
     * @param severity How bad it is.
     * @param category Machine-matchable category the host can switch on.
     * @param message  Human-readable sentence.
     */
    void report(vn::graphics::DiagnosticSeverity severity, vn::graphics::DiagnosticCategory category,
                const String& message);

  private:
    friend class RenderEngine;

    /** @brief Mints a handle over @p engine's route (only the engine does this).
     *
     * @param engine Engine whose route this handle reads.
     */
    explicit DiagnosticsRoute(Lease<RenderEngine> engine) noexcept;

  private:
    Lease<RenderEngine> engine_;
};

VN_GRAPHICS_NS_END
