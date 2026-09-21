#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Learning how many frames may be in flight, from the one source that actually knows.
 *
 * WHY THIS IS NOT A CONFIGURATION. The deferred-release machinery may park an object for exactly as long
 * as it takes the slot that recorded it to be reused (see FrameTimeline), and a wrong number is silent in
 * both directions: too small and an object dies under a command buffer that still names it, too large and
 * memory is held for frames nobody needs.
 *
 * THE COUNT IS NOT READABLE, BUT IT IS OBSERVABLE - AND ONLY GRADUALLY. The framework this backend drives
 * keeps the count as a local constant with no accessor, and its per-slot index table starts out holding
 * an "unset" sentinel for every slot: `fence(i)` answers "no such slot" until that entry has been filled,
 * and EXACTLY ONE ENTRY IS FILLED PER FRAME. So the first frame answers one slot, the second two, and
 * only after as many frames as there are slots does the answer stop growing. Probing once and believing
 * it is therefore the worst possible reading: on frame one it returns 1, which as a parking window is an
 * object destroyed a frame before its command buffer is recycled.
 *
 * Hence two steps, and they are separate types because they are separate questions:
 *   * `probeSlotCount` asks how many slots the framework can answer RIGHT NOW (through a predicate, so it
 *     is testable with no device);
 *   * `SlotTracker` folds those answers over frames and says when the number has STOPPED GROWING, which is
 *     the moment it is the session's real in-flight count.
 *
 * UNTIL THEN THERE IS NO WINDOW: a session that has not learned its count parks nothing and releases
 * replaced objects under a counted device wait instead (see RetirementQueue). "We do not know yet" is a
 * state the rest of the backend can act on; a guess is not.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief What one slot probe learned. */
struct SlotProbeOutcome
{
    std::uint32_t slots{0};             ///< Slots the framework answered for; 0 means "could not learn".
    bool          probe_failed{false};  ///< No slot could be answered at all.
};

/** @brief The slot count this backend expects a viewer of this fleet to own.
 *
 * Not a configuration and not used to decide anything: it exists so a session can say whether what it
 * LEARNED matches what this code was written against, which is the difference between "unchanged" and
 * "the framework changed something and we should look".
 */
inline constexpr std::uint32_t kAssumedInFlightSlots = 3;

/** @brief Asks how many slots the framework can answer for right now.
 *
 * @param has_slot Predicate answering "is there a slot @p index right now?".
 * @param limit Highest slot count to look for; the probe stops there.
 * @return The count, and whether probing failed (no slot answered at all).
 */
[[nodiscard]] SlotProbeOutcome probeSlotCount(const std::function<bool(std::uint32_t)>& has_slot,
                                              std::uint32_t limit = 64) noexcept;

/**
 * @brief Folds per-frame probe answers into the session's real in-flight count.
 *
 * Growth is the signal: the answer grows by one per frame until the framework's slot table is full, and a
 * count that did NOT grow since the previous frame has stopped being filled - that is the session's real
 * number. Two consecutive equal answers are therefore what "known" means here, and a session acts on the
 * number only once it is known.
 */
class SlotTracker
{
  public:
    /** @brief Folds one frame's probe answer in.
     *
     * @param outcome What this frame's probe learned.
     * @return true once the count has stopped growing (the number is now the session's real one).
     */
    bool observe(const SlotProbeOutcome& outcome) noexcept;

    /** @brief Gets the highest count seen so far (0 while nothing is known). */
    [[nodiscard]] std::uint32_t slots() const noexcept;

    /** @brief Gets whether the count has stopped growing (see observe()). */
    [[nodiscard]] bool known() const noexcept;

    /** @brief Gets how many frames were observed. */
    [[nodiscard]] std::size_t framesObserved() const noexcept;

    /** @brief Gets whether the known count matched kAssumedInFlightSlots.
     *
     * Meaningful only once known(); before that the answer is false by construction.
     */
    [[nodiscard]] bool agreesWithAssumption() const noexcept;


  private:
    std::uint32_t best_{0};        ///< Highest answer seen.
    std::uint32_t previous_{0};    ///< The previous frame's answer.
    bool          known_{false};   ///< The answer stopped growing.
    std::size_t   frames_{0};      ///< Frames observed.
};

}  // namespace core

V_VSG_NS_END
