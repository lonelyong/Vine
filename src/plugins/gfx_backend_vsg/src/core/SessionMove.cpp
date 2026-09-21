#include <vine/vsg/core/SessionMove.hpp>

V_VSG_NS_BEGIN

namespace core
{

MoveDecision planSessionMove(const MoveFacts& facts) noexcept
{
    // Nothing to move: there is no session, or the host announced no window. Both are "build one", and
    // neither is an error - a host that never had a window runs exactly this path.
    if (!facts.has_live_session)
    {
        return {MoveAction::Rebuild, "no live session to move"};
    }
    if (!facts.handle_announced)
    {
        return {MoveAction::Rebuild, "no host window was announced, so the session is (re)built on a window of its own"};
    }

    // A session that owns its window has no host surface to follow: its swapchain belongs to a window this
    // backend created, and the host's window cannot be served by it.
    if (!facts.on_host_window)
    {
        return {MoveAction::Rebuild, "the session is not on a host window, so it has no host surface to move"};
    }

    // The same window again: the host is telling us it still exists (a show, a resize). Keeping the session
    // is the whole point - reporting anything else would cost the device and every compiled pipeline.
    if (!facts.handle_differs)
    {
        return {MoveAction::Keep, "the session is already on the announced window"};
    }

    return {MoveAction::Move, "moving onto the announced window keeps the device and the pipelines"};
}

}  // namespace core

V_VSG_NS_END
