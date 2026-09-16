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
 *     A REGISTRATION BELONGS TO THE MANAGER IT WAS MADE INTO. vsg 1.1.16's CompileManager has no
 *     remove, so a registration cannot be taken back once made -- but the manager itself can be
 *     REPLACED, and every context in it goes with it. Each of those contexts owns a VkCommandPool and
 *     holds the render pass it was registered against, so a manager that lives as long as the session
 *     accumulates one per slot CREATION rather than per slot alive: measured at 111 registrations for
 *     at most 9 live content slots before this was fixed. renewCompileContexts() is the replacement,
 *     and it happens at the teardowns that already stop the device (see its own note) -- while the
 *     slot records WHICH manager it registered into instead of a flag, so that replacing the manager
 *     invalidates every registration at once and no teardown path can forget to re-arm one.
 *
 *     That record is a field of the retention picture (VsgRetentionStats::compile_contexts) and the
 *     measurement behind it is in docs/backend.md 5.3.1.
 *   * compilePendingViews() — the driver: use the incremental path unless
 *     VINE_VSG_DISABLE_INCREMENTAL_COMPILE is set (the A/B escape hatch), otherwise fall back
 *     to vsg's full compile over the whole scene. A failure on either path is REPORTED and the
 *     frame is still submitted (an acquired swapchain image must be presented).
 *
 * The queue itself lives in the session state (VsgRendererState::pending_compile_views): its
 * entries are views the session owns, so shutdown() must replace it with the session.
 */

#include <vine/vsg/vsg_global.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgFwd.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief The hints a session's compile manager is built with.
 *
 * `Viewer::compile()` takes the hints as a PARAMETER and keeps none (it builds a manager from them),
 * so there is no viewer field to read back: the one call that lets vsg create this session's manager
 * (VsgRenderer::initialize) and the one that replaces it (@ref renewCompileContexts) take the value
 * from here -- one home for "which hints this backend's managers are built with", so the two cannot
 * drift apart.
 *
 * @return The resource hints to build a compile manager for this session with.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::ResourceHints> compileManagerHints() noexcept;

/** @brief Replaces the session's compile manager, dropping the registrations a teardown has orphaned.
 *
 * vsg only ever ADDS: a registration appends a Context (with its own VkCommandPool and a strong
 * reference to the render pass it was registered against) to the manager's pooled traversal, and
 * 1.1.16 offers no way to take one back. A session that drops and recreates content slots therefore
 * accumulates a context per CREATION while only the live slots can use one -- memory and command
 * pools held for the whole session, measured at 111 registrations against at most 9 live slots. The
 * answer is to replace the manager: the old one, and every context in it, is released with it.
 *
 * TWO KINDS OF SLOT, and this is why the renewal is one function rather than a line per caller:
 *
 *   * a slot this teardown DROPPED needs nothing -- its registration goes with the slot, and the
 *     replacement is what releases that registration now rather than at the end of the session;
 *   * a slot it did NOT drop (the target's other passes, every other target, a slot only rebuilt)
 *     must register again, because the context it registered into no longer exists. Its generation is
 *     therefore left behind by the bump below, so its next compile registers it into the new manager.
 *     Skipping that is worse than the leak this fixes: a slot believing it is registered while no
 *     context matches its view makes `compile(view, selector)` select NO context at all, which reports
 *     success with nothing compiled -- content that silently stops being drawn.
 *
 * WHERE, AND WHEN IT IS WORTH IT. It is called at the teardowns that already stop the device for
 * exactly the reason this needs (no compile in flight): see the three call sites in VsgTargetBookkeeping.
 * The session's own end needs no call -- its manager is dropped with the session state
 * (VsgRenderer::shutdown).
 *
 * A replacement is not free, and the price is NOT the new manager: it is that a live slot's
 * registration is invalidated with it, so that slot registers and compiles again. Measured on the
 * self-test (376 frames, the densest churn in it): 82 replacements cost 20 ms inside this function,
 * while the re-registrations they caused cost about 0.4 s of the run's 2.8 s -- so the rule below is
 * "replace once at least half of what the manager holds is waste", which is the state in which
 * releasing is worth at least as much as the churn it causes. It also keeps the replacement off a
 * per-FRAME path (a target rebuilt every frame, with one slot dying out of a dozen live ones, keeps
 * its registrations instead of rebuilding the manager each frame), which is what makes the retained
 * set bounded at under twice the slots alive rather than proportional to the teardowns.
 *
 * The new manager ALSO carries the contexts vsg derives from the views in the command graph at that
 * moment (CompileTraversal's Viewer constructor), and those are equivalent to the ones this backend
 * registers by hand in every inspected field (view ID, mask, render pass, transfer task,
 * view-dependent state, pipeline states, transfer hint -- measured), so a slot the derivation serves
 * is served correctly. Nothing of this is a pipeline rebuild: a context matching an already-compiled
 * view reuses that implementation (GraphicsPipeline::compile matches one by viewID and render pass),
 * which is what the stage counter of the self-test measures across the churn phase (flat).
 *
 * @param state Session whose manager may be replaced and whose slots then register again.
 */
void renewCompileContexts(VsgRendererState& state);

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
