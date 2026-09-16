#pragma once

/**
 * @brief Compiling the content slots that gained content this frame (D22).
 *
 * A content slot's compiled vsg state is per VIEW (vsg assigns the viewID while traversing a
 * View node), so the compile unit is the slot's View — never a detached subtree, which would
 * always compile under viewID 0 and crash at record time for any other slot's viewID.
 *
 * Two paths:
 *
 *   * incrementalCompileViews() — compile ONLY the queued views (the frame's newly added /
 *     rebuilt content), one at a time, restricting each compile to the context whose
 *     pre-assigned view matches. Each queued entry names the slot it belongs to (the queue has ONE
 *     producer, see PendingCompileView), so the context is looked up rather than searched for — and
 *     an entry whose slot no longer holds that view (dropped in between) is skipped instead of
 *     handing the frame to the full compile. The slot's (render pass + view) context is registered
 *     into the viewer's CompileManager pool the first time its view is compiled: the pool's pooled
 *     traversal was built while the graph was still empty, so without that registration no
 *     context matches the view and compile() would compile nothing.
 *
 *     THE SLOT OWNS ITS REGISTRATION (see VsgCompileRegistration): vsg 1.1.16's CompileManager only ever
 *     ADDS a context and offers no way to take one back, so a registration that outlived its slot would
 *     hold a VkCommandPool plus the render pass it was registered against for the rest of the session
 *     (measured: 60 of them against 3 live content slots). The pool and the traversal it hands out are
 *     `protected` -- the seam vsg leaves for a subclass -- so @ref VsgCompileManager::forget takes one
 *     back, and the object that owns the registration is what calls it: the release happens where the
 *     slot dies, which no teardown has to remember and no path that drops a slot can skip.
 *
 *     That record is what VsgRetentionStats::compile_contexts reports -- counted from the pool itself
 *     (@ref compileContextCount) -- and the measurements behind it are in docs/backend.md 5.3.1.
 *   * compilePendingViews() — the driver: use the incremental path unless
 *     VINE_VSG_DISABLE_INCREMENTAL_COMPILE is set (the A/B escape hatch), otherwise fall back
 *     to vsg's full compile over the whole scene. A failure on either path is REPORTED and the
 *     frame is still submitted (an acquired swapchain image must be presented).
 *
 * The queue itself lives in the session state (VsgRendererState::pending_compile_views): its
 * entries are views the session owns, so shutdown() must replace it with the session.
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstddef>

#include <vsg/app/CompileManager.h>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgFwd.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief The session's compile manager: vsg's own, plus the one operation its pool does not expose.
 *
 * WHY IT EXISTS. vsg's CompileManager only ever ADDS a context: `add()` appends it to the traversal in
 * the pool, and 1.1.16 has no way to take one back. A registration would therefore outlive the slot it
 * was made for -- a Context holds a VkCommandPool and the render pass it was registered against, so what
 * stays behind is driver objects, for as long as the session runs. The pool and the traversal it hands
 * out are `protected` (the seam vsg leaves for a subclass) and `CompileTraversal::contexts` is public,
 * so @ref forget does what an upstream `remove(view)` would, without patching vsg.
 *
 * WHO CALLS IT. The slot that owns the registration (@ref VsgCompileRegistration), which keeps the
 * release where the slot dies instead of at teardowns that would have to remember it: the pool's
 * contents are then "one context per live slot" by construction, and no sweep has to reconcile them.
 * A registration left behind after its view died is not merely stale -- the next compile walks it and
 * takes a ref_ptr of the dead view (see VsgCompileRegistration) -- so the release belongs exactly there.
 *
 * IT ALSO STARTS THE POOL EMPTY. The base constructor builds its traversal as
 * `CompileTraversal(viewer, requirements)`, which walks the command graph and adds a context for every
 * View it finds. Each slot's own registration is what serves it here, so those derived contexts are not
 * needed -- and a pool that carries a second context for every view in the graph (with its command pool
 * and the render pass behind it) is work this backend does not ask for. A pool whose traversal starts
 * with no contexts keeps the pool's contents exactly what this backend registered -- which is also why
 * REPLACING the manager (the only release a plain CompileManager offers) is no longer needed: the walk a
 * replacement would do is the very thing this avoids.
 */
class VsgCompileManager : public ::vsg::Inherit<::vsg::CompileManager, VsgCompileManager>
{
  public:
    /** @brief Creates the manager and installs a pool with one context-free traversal.
     *
     * @param viewer Viewer whose status the pool's queue uses.
     * @param hints  Resource hints for the traversal (see VsgRenderer::initialize).
     */
    VsgCompileManager(::vsg::Viewer& viewer, ::vsg::ref_ptr<::vsg::ResourceHints> hints);

    /** @brief Drops every compile context registered for @p view.
     *
     * Called by the slot that owns the registration (@ref VsgCompileRegistration), where that slot dies:
     * the context's render pass and command pool belong to the registration, and nothing can use them
     * afterwards. Harmless (and safe) if no context is registered for the view, which is what the return
     * value tells the caller.
     *
     * @param view View whose registrations to drop (null is a no-op).
     * @return How many contexts were dropped.
     */
    std::size_t forget(const ::vsg::View* view);

    /** @brief How many contexts the pool holds.
     *
     * The registration record, counted where it lives: VsgRetentionStats::compile_contexts reads this
     * instead of a counter that had to be kept in step with the pool by hand.
     *
     * @return Number of contexts in the pool.
     */
    std::size_t contextCount();

  private:
    /** @brief Runs @p visit over the pool's traversal, borrowing it for the visit.
     *
     * The one place that knows how to touch the pool, so the borrowing contract is stated once: the pool
     * hands its traversal out for the duration of a compile and every compile gives it back, so this
     * waits only while a compile is actually running -- which the callers above cannot be doing.
     *
     * @param visit Callable taking the borrowed ::vsg::CompileTraversal&.
     */
    template <typename Visitor>
    void visitPool(Visitor&& visit);
};

/** @brief How many compile contexts the session's manager holds.
 *
 * A query rather than a tracked number: what the pool holds is a fact about the slots that are alive (a
 * registration cannot outlive its slot, see VsgCompileRegistration), so it is read from the pool.
 *
 * @param state Session whose manager is asked.
 * @return Number of registered contexts (0 without a session, or without one of ours) -- see
 *         VsgRetentionStats::compile_contexts.
 */
std::size_t compileContextCount(const VsgRendererState& state);

/** @brief Compiles only the queued views (the incremental path).
 *
 * @param state Session whose viewer / targets supply the compile contexts.
 * @return true when every queued view that still belongs to a slot was compiled; false when the
 *         incremental path cannot serve this frame (no CompileManager, a null view, or a failed
 *         compile), so the caller falls back to the full compile. A queued view whose slot is gone
 *         is not a failure: nothing records it any more.
 */
[[nodiscard]] bool incrementalCompileViews(VsgRendererState& state);

/** @brief Compiles what the frame added: incrementally, or fully as the fallback.
 *
 * Called once per submitted frame, after the passes have announced their content (the
 * queue's producer) and before the frame is presented.
 *
 * @param state       Session whose pending views are compiled and then cleared.
 * @param diagnostics Route a failed compile is reported on.
 */
void compilePendingViews(VsgRendererState& state, const VsgDiagnostics& diagnostics);

} // namespace detail

V_VSG_NS_END
