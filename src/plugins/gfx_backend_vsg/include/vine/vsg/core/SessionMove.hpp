#pragma once

#include <cstdint>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief What to do when the host announces a window: keep the session, move it, or build a new one.
 *
 * WHY THIS IS A DECISION TABLE. "The host handed us a new window" has four answers whose costs differ by
 * two orders of magnitude, and the cheap ones are easy to miss:
 *
 *   * KEEP - the host re-announced the window this session is already on (a show, a resize, a repaint).
 *     Answering anything but "kept" here starts a fresh session and costs the instance, the device and
 *     every compiled pipeline; the host did not ask for any of that.
 *   * MOVE - the windowing system destroyed the host's window and made a new one. The surface and the
 *     swapchain are recreated on the instance the window already owns, and the device, the render pass
 *     and every pipeline compiled against them survive. This is the case the whole "the host gives the
 *     timing, the control maintains the session" contract exists for.
 *   * REBUILD - there is nothing to move (no live session, no announced window, or a session on a window
 *     of this backend's own), or the move was attempted and refused. The session starts over, and the
 *     cost is real, so it is REPORTED with its reason rather than applied quietly.
 *
 * Written as data, the caller cannot take the expensive path by accident, and the reasons a host can act
 * on ("your new window presents a different swapchain format") are one table instead of three branches
 * inside the setup code. The table is device-free: what it decides on is what the caller knows before it
 * touches anything.
 *
 * WHAT IS NOT IN HERE: whether the new surface's FORMAT can serve the current render pass. That answer
 * only exists once the new window has been created and asked, so it is the move attempt's own result
 * (see MoveAttemptResult), not a fact the caller could have supplied up front.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief What a session should do about an announced window. */
enum class MoveAction : std::uint8_t
{
    Keep,     ///< Already on that window: nothing changes, nothing is rebuilt.
    Move,     ///< Move onto it: recreate the surface and the swapchain, keep the device and the pipelines.
    Rebuild,  ///< Start a fresh session (the reason names what made a move impossible).
};

/** @brief What the caller knows about the announced window before touching anything. */
struct MoveFacts
{
    bool has_live_session{false};  ///< There is a session that could be moved.
    bool on_host_window{false};    ///< That session adopted a host window (rather than owning its own).
    bool handle_announced{false};  ///< The host announced a window (a non-null handle).
    bool handle_differs{false};    ///< It is not the one the session is on.
};

/** @brief The decision, with the sentence a host can be told when the expensive path is taken. */
struct MoveDecision
{
    MoveAction  action{MoveAction::Rebuild};  ///< What to do.
    const char* reason{""};                   ///< Why, phrased for a diagnostic.
};

/** @brief Decides what to do about an announced window (see the file note for the costs involved).
 *
 * @param facts What the caller knows before touching anything.
 * @return The action, and the reason a rebuild (or a move) is the answer.
 */
[[nodiscard]] MoveDecision planSessionMove(const MoveFacts& facts) noexcept;

/** @brief The outcome of TRYING a move: the part only the new surface can answer. */
enum class MoveAttemptResult : std::uint8_t
{
    Moved,                ///< The session is on the new window; the device and the pipelines survived.
    FormatIncompatible,   ///< The new surface cannot serve the current render pass: a rebuild is required.
    Refused,              ///< The window refused for another reason (the reason is reported by the caller).
};

}  // namespace core

VN_VSG_NS_END
