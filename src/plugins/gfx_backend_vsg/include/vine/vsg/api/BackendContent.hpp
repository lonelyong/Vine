#pragma once

#include <vine/vsg/api/ContentAssembly.hpp>
#include <vine/vsg/api/HostTargets.hpp>
#include <vine/vsg/api/PassRegistry.hpp>
#include <vine/vsg/api/VsgBackend.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The api-internal view of the facade's own pieces (see the file note of SessionContent.hpp for the
 * pattern).
 *
 * WHY A SEPARATE HEADER. `VsgBackend` is the SDK's seam: its public surface answers what the ENGINE asks
 * and nothing else. But a test that wants to see WHY a frame drew what it drew - how many halves and sets
 * the content world built, whether a pass kept its identity across frames - needs the objects behind the
 * seam, and widening the facade's own interface for that would make the SDK's view and the test's view the
 * same one. So the facade befriends this class, and the accessors live here.
 *
 * WHAT IT IS NOT: not a second API. Nothing in the frame drive goes through this header - the tests are
 * its only caller, and every accessor answers a borrowed pointer that is valid only between the facade's
 * own calls (see each note).
 */
V_VSG_NS_BEGIN

namespace detail
{

/** @brief The api-internal view of a facade's content world and pass identities. */
class BackendContentAccess
{
  public:
    /** @brief Gets the pass identities the facade has adopted (see api/PassRegistry).
     *
     * @param backend Backend to ask.
     * @return The registry, valid for the backend's lifetime.
     */
    [[nodiscard]] static PassRegistry& passes(VsgBackend& backend) noexcept;

    /** @brief Gets the content world the facade assembles frames with.
     *
     * @param backend Backend to ask.
     * @return The assembly, or null while the session is not up (it needs the session's device).
     */
    [[nodiscard]] static ContentAssembly* assembly(VsgBackend& backend) noexcept;

    /** @brief Gets the host targets the facade holds (their entries are the plan's off-screen world).
     *
     * @param backend Backend to ask.
     * @return The registry, valid for the backend's lifetime.
     */
    [[nodiscard]] static HostTargets& targets(VsgBackend& backend) noexcept;

    /** @brief Gets the executor the frame drive records through (its log is this frame's evidence).
     *
     * @param backend Backend to ask.
     * @return The executor, valid for the backend's lifetime.
     */
    [[nodiscard]] static VsgExecutor& executor(VsgBackend& backend) noexcept;
};

}  // namespace detail

V_VSG_NS_END
