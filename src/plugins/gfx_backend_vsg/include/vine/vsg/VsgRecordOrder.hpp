#pragma once

/**
 * @brief The command graph's record order: what records when, decided in named phases.
 *
 * The engine can build a target's graph out of dependency order — a producer (re)built after
 * its consumers existed, a consumer wired to a producer built later — and a consumer only
 * samples its source's CURRENT content when the producer's graph is recorded first. So a
 * frame's ordering is planned as three units, each explaining what it guarantees:
 *
 *   1. fillRecordPlan()  — which graphs each target records this frame, in the target's own
 *                          pass order (the stable tie-break seed is the order they are
 *                          recorded in right now);
 *   2. orderRecordPlan() — that order turned into a dependency-valid one (sampling edges plus
 *                          depth borrows), through the pure stableTopologicalOrder(), so
 *                          unrelated targets keep their relative position;
 *   3. applyRecordPlan() — the command graph's children rewritten from the plan, with each
 *                          borrowed target's depth-share barrier right after its last graph
 *                          and the window's swapchain graph last.
 *
 * Only render-graph CHILDREN are reordered, so this changes the per-frame record order and
 * nothing else.
 */

#include <vine/vsg/vsg_global.hpp>

#include <map>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief The command graph's record plan for one frame (reconcileOffscreenOrder).
 *
 * The engine can build a target's graph out of dependency order — a producer
 * (re)built after its consumers existed, a consumer wired to a producer built
 * later — and a consumer only samples its source's CURRENT content when the
 * producer's graph is recorded first. So the plan is: which graphs each target
 * records this frame (in the target's own pass order), the targets' CURRENT
 * record order (the stable tie-break seed), and the dependency-valid order the
 * children end up in.
 */
struct RecordPlan {
    ::vsg::ref_ptr<::vsg::RenderGraph> window_graph;
    std::map<vine::graphics::RenderTarget*, std::vector<::vsg::ref_ptr<::vsg::RenderGraph>>> graphs_of;
    std::vector<vine::graphics::RenderTarget*> present; ///< Targets recorded now, in current child order.
    std::vector<vine::graphics::RenderTarget*> order;   ///< Targets in dependency-valid order (see orderRecordPlan).
};

/** @brief Fills @p plan's graph map and current record order (phase 1).
 *
 * Collects, per off-screen target, the pass graphs that must record this frame
 * (skipping RETIRED ones: a pass whose slot is detached records nothing) in the
 * target's explicit pass order, seeding ties from the order the graphs are
 * recorded in right now.
 *
 * @param state Session whose target table and command graph are read.
 * @param plan  Plan to fill (@ref RecordPlan::window_graph must be set).
 */
void fillRecordPlan(const VsgRendererState& state, RecordPlan& plan);

/** @brief Turns @p plan's current order into a dependency-valid one (phase 2).
 *
 * Edges: a target's off-screen SAMPLING sources (its non-detached screen /
 * program slots' source_target) and its DEPTH BORROW source (a borrower's pass
 * LOADs the depth its source writes this frame). The order itself is the pure
 * stableTopologicalOrder(), so unrelated targets keep their relative position.
 *
 * @param state Session whose target table the edges are read from.
 * @param plan  Plan whose graphs_of / present are filled.
 */
void orderRecordPlan(const VsgRendererState& state, RecordPlan& plan);

/** @brief Rewrites the command graph's children from @p plan (phase 3).
 *
 * Each target's graphs in dependency order, the depth-share barrier of every
 * target borrowing THIS one right after its last graph (so the borrower's LOAD
 * sees the writes), and the window swapchain graph last (it may itself sample
 * off-screen targets). Reordering render-graph children only changes the
 * per-frame record order.
 *
 * @param state Session whose command graph is rewritten.
 * @param plan  Plan whose order / graphs_of are filled.
 */
void applyRecordPlan(VsgRendererState& state, const RecordPlan& plan);

/** @brief Plans and applies one frame's record order (the driver of the three phases).
 *
 * A consumer's graph must be ordered after every target it samples (its PiP screen /
 * fullscreen-program sources), so a same-frame producer chain (A -> B -> window) samples the
 * CURRENT frame, and the window's swapchain graph stays the last child. Called whenever an
 * off-screen graph is (re)built or a sampling slot is newly attached / dropped: creation
 * order alone cannot guarantee it (a producer rebuilt after its consumers existed, or a
 * consumer wired to a producer built later, would otherwise record the consumer first and
 * sample stale / undefined content).
 *
 * @param state Session whose command graph is reconciled (a no-op before the session exists).
 */
void reconcileOffscreenOrder(VsgRendererState& state);

} // namespace detail

V_VSG_NS_END
