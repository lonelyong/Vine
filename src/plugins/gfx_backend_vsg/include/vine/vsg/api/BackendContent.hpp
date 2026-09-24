#pragma once

#include <vine/vsg/api/ContentAssembly.hpp>
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/HostTargets.hpp>
#include <vine/vsg/api/PassRegistry.hpp>
#include <vine/vsg/api/VsgBackend.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
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
VN_VSG_NS_BEGIN

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

    /** @brief Gets the session's window as the frame's default-framebuffer target.
     *
     * The window's own answers are what a size event has to have moved: its extent (a live read), its shape
     * facts, and the render area prepare() writes from it. The tests read it to tell "the surface was
     * followed" from "the old size was kept".
     *
     * @param backend Backend to ask.
     * @return The window target, or null while the session is not up.
     */
    [[nodiscard]] static WindowTarget* windowTarget(VsgBackend& backend) noexcept;

    /** @brief Gets the content tables the facade's frames are built from.
     *
     * The store is where a frame's facts come from and where its rebuild counters live: the tests read
     * `builds()` around a frame to tell "this frame built something" from "this frame touched and found
     * nothing to do" - the steady-state claim a compare-and-write touch has to keep.
     *
     * @param backend Backend to ask.
     * @return The store, or null while the session is not up.
     */
    [[nodiscard]] static ContentStore* store(VsgBackend& backend) noexcept;

    /** @brief Gets the material-image cache the declared sets resolve their maps from.
     *
     * The cache is where the device's own anisotropy limit lands (the session reads it once, see
     * VsgBackend::initialize), and its samplers are built from that number - so a test that wants to tell
     * "the cache was told" from "nothing ever called it" asks here.
     *
     * @param backend Backend to ask.
     * @return The cache, or null while the content world is not up.
     */
    [[nodiscard]] static MaterialImages* images(VsgBackend& backend) noexcept;
};

}  // namespace detail

VN_VSG_NS_END
