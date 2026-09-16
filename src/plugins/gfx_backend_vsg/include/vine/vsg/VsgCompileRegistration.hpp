#pragma once

/**
 * @brief One content slot's registration with the session's compile manager, released where the slot dies.
 *
 * A slot's view is compiled through a context REGISTERED with the viewer's CompileManager (see
 * VsgViewCompiler), and vsg 1.1.16 has no way to take one back: each Context owns a VkCommandPool and
 * holds a strong reference to the render pass it was registered against, so a registration that outlives
 * its slot is driver objects held for the rest of the session (measured: 60 of them against 3 live
 * content slots). This object owns that registration and releases it in its destructor, so the release
 * happens exactly where the slot that made it dies -- no teardown owes the manager a call, and no path
 * that drops a slot can forget one.
 *
 * WHY THE RELEASE CANNOT BE DEFERRED TO A LATER SWEEP. A Context holds an `observer_ptr<View>` -- a weak
 * reference -- and vsg's own CompileTraversal::apply(View&) turns it into a ref_ptr for every context it
 * walks (`context->view.ref_ptr()`), which INCREMENTS that view's count. A context whose view has died is
 * therefore not merely stale: the next compile walks it and writes to freed memory. Tying the release to
 * the slot's death is what keeps such a context out of the pool at all, and the member is declared AFTER
 * the view it names (see ContentSlot) so that it is destroyed -- and the registration released -- while
 * the view still exists.
 *
 * It is also what makes VsgRetentionStats::compile_contexts a count of LIVE slots: the pool holds one
 * context per slot that is alive because a registration cannot outlive its slot, and the number is read
 * from the pool instead of a counter kept in step with it by hand.
 */

#include <vine/vsg/vsg_global.hpp>

#include <vine/vsg/VsgFwd.hpp>

V_VSG_NS_BEGIN

namespace detail
{

class VsgCompileManager;

/** @brief Owns one slot's compile-context registration: adopted on registration, released on destruction.
 *
 * Move-only (a registration has one owner), and inert until @ref adopt is called, which is what makes it
 * safe to hold by value in every content slot -- including the slots of a session that never compiles
 * anything (a device-free test), where it holds nothing and its destructor does nothing.
 */
class VsgCompileRegistration
{
  public:
    /** @brief Holds no registration (see @ref adopt). */
    VsgCompileRegistration() = default;

    /** @brief Releases the registration, if one is held (see @ref release). */
    ~VsgCompileRegistration();

    VsgCompileRegistration(const VsgCompileRegistration&) = delete;
    VsgCompileRegistration& operator=(const VsgCompileRegistration&) = delete;

    /** @brief Moves @p other's registration here; @p other holds none afterwards.
     *
     * @param other Registration to move from.
     */
    VsgCompileRegistration(VsgCompileRegistration&& other) noexcept;

    /** @brief Moves @p other's registration here, releasing whatever this object held first.
     *
     * @param other Registration to move from.
     * @return This object.
     */
    VsgCompileRegistration& operator=(VsgCompileRegistration&& other) noexcept;

    /** @brief Takes on @p view's registration with @p manager, releasing whatever was held before.
     *
     * @param manager Manager whose pool now holds the context for @p view.
     * @param view    View the registration was made for (null releases without adopting).
     */
    void adopt(VsgCompileManager& manager, const ::vsg::View* view);

    /** @brief Releases the registration now. Idempotent, and a no-op when none is held.
     *
     * A session's manager is destroyed before the slots of a session it dropped wholesale are (see
     * VsgRenderer::shutdown, which drops the slots while the viewer is still there), so a registration
     * that is released here is always released while the manager it names is alive.
     */
    void release();

    /** @brief Whether a registration is held.
     *
     * @return true when a view's registration is held by this object.
     */
    bool isRegistered() const noexcept { return view_ != nullptr; }

  private:
    VsgCompileManager* manager_ = nullptr; ///< Manager holding the registration (null: none held).
    const ::vsg::View* view_    = nullptr; ///< View the registration was made for (null: none held).
};

} // namespace detail

V_VSG_NS_END
