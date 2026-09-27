#pragma once

#include "graphics_global.hpp"

/**
 * @brief "Report this condition once, until something says the episode ended".
 *
 * REPORT ONCE PER EPISODE, and the episode's END is the caller's decision - a new frame, a new scope, a
 * usable target size, every announced light lit again. The rule is shared, the boundary is not, which is
 * why re-arming is explicit here: `if (!flag) { flag = true; report(); }` in one place and a bare
 * `flag = false;` in another are two halves of one rule that nothing checks, and a type that owns both
 * halves is the only way a reader can see the rule in one piece.
 *
 * It sits beside the route it feeds (`Diagnostics.hpp`) rather than inside it: the engine, the backend
 * and the engine's own sites all obey the same rule, and a rule shared by everyone is not a backend's
 * private tool. Feeding it is the caller's job - this type never reports, it only remembers.
 */
VN_GRAPHICS_NS_BEGIN

/** @brief "Report this condition once, until something says the episode ended". */
class VN_GRAPHICS_API ReportOnce
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

VN_GRAPHICS_NS_END
