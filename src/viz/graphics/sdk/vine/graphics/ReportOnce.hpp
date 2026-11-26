#pragma once

#include <atomic>

#include "graphics_global.hpp"

/**
 * @brief "Report this condition once, until something says the episode ended".
 *
 * @details
 * - REPORT ONCE PER EPISODE, and the episode's END is the caller's decision - a new frame, a new scope, a usable
 *   target size, every announced light lit again. The rule is shared, the boundary is not, which is why re-arming is
 *   explicit here: `if (!flag) { flag = true; report(); }` in one place and a bare `flag = false;` in another are two
 *   halves of one rule that nothing checks, and a type that owns both halves is the only way a reader can see the
 *   rule in one piece.
 * - It sits beside the route it feeds (`Diagnostics.hpp`) rather than inside it: the engine, the backend and the
 *   engine's own sites all obey the same rule, and a rule shared by everyone is not a backend's private tool. Feeding
 *   it is the caller's job - this type never reports, it only remembers.
 * - THREAD-SAFE BY CONSTRUCTION (`G1` / `G6`): "has this episode been reported?" is asked by whatever thread noticed the
 *   condition, and the engine's own shape gate is reachable from an application's pool thread while it paints - so the
 *   answer is one atomic test-and-set and EXACTLY ONE caller hears `true`: a report is not doubled by a second thread
 *   arriving at the same moment, and no caller has to hold a lock of its own to use this type. (The backend used to
 *   wrap this one flag in a mutex; a type shared by every site owns the guarantee, not each site.)
 */
VN_GRAPHICS_NS_BEGIN

/** @brief "Report this condition once, until something says the episode ended". */
class VN_GRAPHICS_API ReportOnce {
  public:
    /** @brief Starts an episode nobody has reported yet. */
    ReportOnce() noexcept = default;

    /**
     * @brief Copies an episode's state: the copy is "already reported" exactly when @p other is.
     *
     * @details
     * - WHY A COPY EXISTS AT ALL (`G4`): an episode that outlives one compile - "this pass' picture shape is wrong" -
     *   is CARRIED from the frame that reported it to the one after, and the carried fact is this flag. Deleting the
     *   copy would force every such site to hand-roll the carry out of `reported()`, which is the rule this type owns
     *   being re-spelled outside it.
     * - A COPY IS NOT A SHARED FLAG: the two reason about the same history but end their episodes independently, which
     *   is what "this frame's entry for that pass" means.
     *
     * @param other The episode to copy the state of.
     */
    ReportOnce(const ReportOnce& other) noexcept;

    /** @brief Copies @p other's episode state (see the copy constructor). */
    ReportOnce& operator=(const ReportOnce& other) noexcept;

    /** @brief Gets whether this episode still needs reporting, and records that it was.
     *
     * @details
     * - ONE ATOMIC TEST-AND-SET: two threads arriving together are answered `true` once between them, never twice, and
     *   neither has to hold a lock (see the class note).
     *
     * @return true on the first call of an episode, false for the rest of it.
     */
    [[nodiscard]] bool shouldReport() noexcept;

    /** @brief Gets whether the episode has been reported, without changing it.
     *
     * @details
     * - For the sites and tests that assert on the state - a rule about episodes is not observable otherwise.
     *
     * @return true while the episode has been reported and not re-armed.
     */
    [[nodiscard]] bool reported() const noexcept;

    /** @brief Ends the episode: the same condition reports again.
     *
     * @note Called by whoever can tell the condition ended; that boundary is each site's own.
     */
    void rearm() noexcept;

  private:
    /// Readable from any thread (see shouldReport); the exchange is what makes "who reports" a decidable question.
    std::atomic<bool> reported_{ false };
};

VN_GRAPHICS_NS_END
