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
 *     A REGISTRATION LIVES AS LONG AS ITS SLOT, because this backend owns the manager it registers
 *     with. vsg 1.1.16's CompileManager only ever ADDS a context and offers no way to take one back,
 *     so a registration would otherwise outlive the slot it was made for and hold a VkCommandPool
 *     plus the render pass it was registered against for the rest of the session (measured: 60 of
 *     them against 3 live content slots). The pool and the traversal it hands out are `protected` --
 *     the seam vsg leaves for a subclass -- so @ref VsgCompileManager does what an upstream
 *     `remove(view)` would, without patching vsg, and the teardowns that drop a slot call
 *     @ref forgetCompileContext for it.
 *
 *     That record is what VsgRetentionStats::compile_contexts reports, and the measurements behind it
 *     are in docs/backend.md 5.3.1.
 *   * compilePendingViews() — the driver: use the incremental path unless
 *     VINE_VSG_DISABLE_INCREMENTAL_COMPILE is set (the A/B escape hatch), otherwise fall back
 *     to vsg's full compile over the whole scene. A failure on either path is REPORTED and the
 *     frame is still submitted (an acquired swapchain image must be presented).
 *
 * The queue itself lives in the session state (VsgRendererState::pending_compile_views): its
 * entries are views the session owns, so shutdown() must replace it with the session.
 */

#include <vine/vsg/vsg_global.hpp>

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
 * the pool, and 1.1.16 has no way to take one back. A registration therefore outlives the slot it was
 * made for -- a Context holds a VkCommandPool and the render pass it was registered against, so what
 * stays behind is driver objects, for as long as the session runs. The pool and the traversal it hands
 * out are `protected` (the seam vsg leaves for a subclass) and `CompileTraversal::contexts` is public,
 * so @ref forget can do what an upstream `remove(view)` would do without patching vsg.
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
     * Called where the slot that owns @p view dies: the context's render pass and command pool belong
     * to that slot's registration, and nothing can use them afterwards. Harmless (and safe) if no
     * context is registered for the view, which is what the return value tells the caller.
     *
     * @param view View whose registrations to drop (null is a no-op).
     * @return How many contexts were dropped.
     */
    std::size_t forget(const ::vsg::View* view);
};

/** @brief Drops the compile context registered for a slot's view, where that slot dies.
 *
 * The one obligation a teardown owes the compile manager (see VsgTargetBookkeeping's note on the
 * three functions that call it): the registration it made belongs to the slot that is going away, so
 * releasing it here is what keeps `VsgRetentionStats::compile_contexts` a count of LIVE slots rather
 * than of the slots a session has ever created.
 *
 * @param state Session whose manager holds the registration.
 * @param view  The dying slot's retained view (null is a no-op).
 */
void forgetCompileContext(VsgRendererState& state, const ::vsg::View* view);

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
