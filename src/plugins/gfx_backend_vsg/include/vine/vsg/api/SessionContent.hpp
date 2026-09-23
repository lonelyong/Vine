#pragma once

#include <vsg/app/CommandGraph.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/vsg_global.hpp>

namespace vsg
{
class Group;
class Node;
}  // namespace vsg

/**
 * @brief The typed view of what a session renders: its content root, its device, and a recompile.
 *
 * WHY IT IS A SEPARATE HEADER. A session's own header deliberately carries no graphics types - the rules a
 * host-facing layer needs (open a frame, commit it, learn the slot count, park an object) do not mention the
 * API at all. But a layer that DRAWS needs exactly three things the session owns: the node it renders every
 * frame, the device its objects must belong to, and a way to compile content that was attached after the
 * session came up. Those three are api-internal facts, so they live here, in one accessor class the session
 * befriends, instead of widening the session's public surface.
 *
 * WHY A RECOMPILE IS NEEDED AT ALL. vsg compiles a command graph once and keeps the created objects; content
 * attached afterwards has no implementation and would record nothing. A later compile pass creates the new
 * objects and leaves the already-compiled ones alone (a compiled pipeline is only built while its
 * implementation is missing), so a caller attaches its content and asks for one more pass - no rebuild of the
 * session, no recompiled pipelines.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

namespace detail
{

/** @brief The api-internal view of a session's content (see the file note). */
class SessionContentAccess
{
  public:
    /** @brief Gets the node the session renders every frame.
     *
     * The node is a group the session owns: a caller adds its recorded content to it. It exists only while
     * the session is up, and it is EMPTY after a rebuild (a new session has a new root, and content built
     * against the old device is stale by definition - see `Session::generation`).
     *
     * @param session Session to ask.
     * @return The content root, or null when the session is not up.
     */
    [[nodiscard]] static ::vsg::ref_ptr<::vsg::Group> root(const api::Session& session) noexcept;

    /** @brief Gets the device a session's content must be built on.
     *
     * Content built on another device cannot be referenced by this session's command buffers: the answer is
     * the session's own device, and a null answer means the session is not up.
     *
     * @param session Session to ask.
     * @return The session's device, or null.
     */
    [[nodiscard]] static ::vsg::ref_ptr<::vsg::Device> device(const api::Session& session) noexcept;

    /** @brief Stops the device: every submission has finished, and the wait is COUNTED (see Session::deviceWaits).
     *
     * The one spelling of "wait for the device" for a caller that needs the last submission to have landed -
     * a synchronous readback above all, whose copy must not race the frame that wrote the pixels. The count
     * is the same one the session's own waits go into, so "this call stopped the device exactly once" stays
     * checkable (the frame path itself never calls this).
     *
     * @param session Session whose device is stopped (a session with no viewer waits for nothing).
     */
    static void waitDeviceIdle(api::Session& session) noexcept;

    /** @brief Gets the session's window as the frame's default-framebuffer target.
     *
     * This is the target an executor is told about so that passes targeting the default framebuffer (a null
     * target in the plan) have somewhere to go, and the object that answers the window's shape for a
     * pipeline key - one window per session, so there is one.
     *
     * @param session Session to ask.
     * @return The window target, or null when the session is not up.
     */
    [[nodiscard]] static WindowTarget* windowTarget(const api::Session& session) noexcept;

    /** @brief Creates the command graph ONE frame records into, bound to the session's window.
     *
     * One graph per frame, not one retained graph: the plan's execution order decides where each pass graph
     * sits (an off-screen graph before the window's when the window samples it, the other way round when it
     * does not), and the executor appends them to the graph it is given. A graph that survived the frame
     * would keep the previous frame's order and collect the new one behind it. The session's knowledge here
     * is only what a graph needs to be usable at all: the window it presents, and with it the device and the
     * queue family.
     *
     * @param session Session to ask.
     * @return The graph, or null when the session is not up.
     */
    [[nodiscard]] static ::vsg::ref_ptr<::vsg::CommandGraph> makeFrameGraph(const api::Session& session) noexcept;

    /** @brief Hands the session the command graphs THIS frame records, replacing the ones it set up.
     *
     * A session renders what it was given: its own (empty-frame) graph while nothing else was handed over,
     * and from then on the caller's - a graph holding the window target's graph and whatever off-screen
     * graphs the frame's plan produced. The compile pass is folded in, because a graph handed over for the
     * first time has objects (descriptor sets, buffers) whose implementations do not exist yet, and a caller
     * that forgot it would record nothing and see an empty frame with no reason.
     *
     * @param session Session to hand the graphs to.
     * @param graphs  The graphs the next commitFrame() submits (the window's graph must be one of them).
     * @return true when the graphs were assigned and compiled; false when the session is not up or the
     *         compile pass failed (reported).
     */
    static bool assignFrameGraphs(api::Session& session, const ::vsg::CommandGraphs& graphs);

    /** @brief Compiles content that was attached after the session came up.
     *
     * @param session Session to compile.
     * @return true when the compile pass ran; false when the session is not up.
     */
    static bool recompile(api::Session& session);
};

}  // namespace detail

V_VSG_NS_END
