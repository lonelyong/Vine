#pragma once

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/Session.hpp>
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

    /** @brief Compiles content that was attached after the session came up.
     *
     * @param session Session to compile.
     * @return true when the compile pass ran; false when the session is not up.
     */
    static bool recompile(api::Session& session);
};

}  // namespace detail

V_VSG_NS_END
