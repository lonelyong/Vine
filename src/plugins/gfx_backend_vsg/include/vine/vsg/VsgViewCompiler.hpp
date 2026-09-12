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
 *     pre-assigned view matches. The slot's (render pass + view) context is registered into
 *     the viewer's CompileManager pool the first time its view is compiled: the pool's pooled
 *     traversal was built while the graph was still empty, so without that registration no
 *     context matches the view and compile() would compile nothing.
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
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief Compiles only the queued views (the incremental path).
 *
 * @param state Session whose viewer / targets supply the compile contexts.
 * @return true when every queued view was compiled; false when the incremental path cannot
 *         serve this frame (no CompileManager, a queued view that belongs to no ready content
 *         slot, or a failed compile), so the caller falls back to the full compile.
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
