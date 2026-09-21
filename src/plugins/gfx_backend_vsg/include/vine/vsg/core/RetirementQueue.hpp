#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Deferred destruction: "release this when the timeline says the GPU is past it".
 *
 * WHAT THIS IS NOT. It is not ownership. The ResourceManager owns every resource; this queue owns only
 * the DECISION to destroy one, and only for the window in which the GPU may still be reading it. That
 * split is deliberate: a dependency graph (who names whose view) and a destruction schedule (how long
 * the GPU may still hold a reference) are two different questions with two different lifetimes - the
 * graph lives inside a frame, the schedule spans frames - and a single type answering both becomes the
 * object nobody dares change.
 *
 * THE TWO WAYS A RESOURCE DIES, and why only one of them is here:
 *   * destructive teardown (a session replaced, a target released): the object may be named by a
 *     command buffer that is in flight right now, so the device is idled and that idle is COUNTED.
 *     That path is the ResourceManager's, and the count is what keeps it honest - see deviceWaits().
 *   * replacement (a resize re-points a descriptor, a variant swaps a render pass): nothing is really
 *     gone, so idling the device would be pure loss. The object is parked here instead, and released
 *     once the timeline says the slot that recorded it has been recycled.
 *
 * ORDER MATTERS AT BOTH ENDS. Park BEFORE the frame's advance (pushing an object that was already
 * releasable would release it a frame early, which is how a render pass once died under a submitted
 * command buffer), and advance as the LAST step of a committed frame. Both rules are here as the
 * documented contract of retire()/advance(); the frame's own order is the executor's to keep.
 */
V_VSG_NS_BEGIN

namespace core
{

/**
 * @brief The park queue (see the file note for what it owns and the two rules that order it).
 */
class RetirementQueue
{
  public:
    /** @brief What the queue runs when a resource's turn comes. Owns nothing; it is the release act. */
    using ReleaseFn = std::function<void()>;

  public:
    /** @brief Creates a queue whose parking window is derived from @p slots.
     *
     * A session that could not learn the count passes 0, which DISABLES parking: see parkingAvailable().
     *
     * @param slots Command-buffer slots in flight as the session probed them, or 0 when unknown.
     */
    explicit RetirementQueue(std::uint32_t slots);

    /** @brief Parks @p release until the timeline is past the slot that could still need it.
     *
     * Call it during the frame that replaces the object, before advance() of that same frame.
     *
     * @param timeline The frame timeline the park is dated against.
     * @param release Release act, run later; empty is ignored.
     * @return true when the object was parked; false when parking is unavailable (see
     *         parkingAvailable()), in which case the caller must release it under a COUNTED device wait
     *         instead - the object has no safe window without the evidence.
     */
    [[nodiscard]] bool retire(const FrameTimeline& timeline, ReleaseFn release);

    /** @brief Sets the parking window, once the session learns how many frames may be in flight.
     *
     * In place, and not by constructing a new queue: this object also counts the device idles the
     * destructive paths took, and a session that replaced it when it learned its count would silently reset
     * those numbers (the point of counting them is that they cannot be lost on the way).
     *
     * @param slots Slots in flight, or 0 when the session could not learn them (parking stays off).
     */
    void setSlots(std::uint32_t slots) noexcept;

    /** @brief Gets whether parking is available at all.
     *
     * False when the session could not learn how many frames may be in flight. A parking window guess
     * that is too short destroys an object a submitted command buffer still names, so the honest answer
     * to "we do not know" is to idle the device (and count it) rather than to park.
     */
    [[nodiscard]] bool parkingAvailable() const noexcept;

    /** @brief Runs every parked release whose turn has come.
     *
     * The frame's last step. Releases run AFTER the queue was compacted, so a release callback that
     * parks something new cannot invalidate the iteration it is running inside.
     *
     * @param timeline The frame timeline, whose completed watermark decides what is due.
     */
    void advance(const FrameTimeline& timeline);

    /** @brief Gets the number of command-buffer slots the parking window is derived from. */
    [[nodiscard]] std::uint32_t slots() const noexcept;

    /** @brief Gets the frame an object parked right now would be released at. */
    [[nodiscard]] std::uint64_t retirePoint(const FrameTimeline& timeline) const noexcept;

    /** @brief Gets how many objects are parked. */
    [[nodiscard]] std::size_t pending() const noexcept;

    /** @brief Gets how many objects have been released. */
    [[nodiscard]] std::size_t released() const noexcept;

    /** @brief Gets how many counted device idles have been taken.
     *
     * The judge of the "no device wait on a frame-assembly path" invariant: this queue exists to avoid
     * those waits, so a frame that raises it is doing the thing the queue was built to prevent.
     */
    [[nodiscard]] std::size_t deviceWaits() const noexcept;

    /** @brief Records that the device was idled.
     *
     * Called by the destructive paths (session teardown, target release) - never by parking.
     */
    void noteDeviceWait() noexcept;


  private:
    struct Entry
    {
        std::uint64_t retire_at{0};  ///< First completed frame at which the object is safe to release.
        ReleaseFn     release;       ///< What to run then.
    };

    std::uint32_t      slots_{0};  ///< 0 = the session never learned the count: parking is off.
    std::vector<Entry> parked_;
    std::size_t        released_{0};
    std::size_t        device_waits_{0};
};

}  // namespace core

V_VSG_NS_END
