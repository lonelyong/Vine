/**
 * @brief Command-graph dependency ordering tests (§28, device-free).
 *
 * reconcileOffscreenOrder() must record each target's render graph after every
 * target it samples (PiP / fullscreen-program slots) or whose depth it LOADs
 * (shareDepth). The ordering core is factored into stableTopologicalOrder() so
 * the rules that are invisible to the validation layer — a consumer recorded
 * before its producer silently samples stale content, with no VUID — are
 * pinned here without a device. The §28 per-pass model feeds this same core
 * with per-PASS graphs, so the tests stay valid across that rework.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>

using vine::vsg::detail::collectOrderEdges;
using vine::vsg::detail::GraphOrderEdge;
using vine::vsg::detail::stableTopologicalOrder;
using vine::vsg::SlotKey;
using vine::vsg::VsgRendererState;

TEST(GraphOrderTest, NoEdgesKeepsInputOrder)
{
    EXPECT_EQ(stableTopologicalOrder(3, {}), (std::vector<std::size_t>{ 0, 1, 2 }));
}

TEST(GraphOrderTest, ConsumerFollowsItsSource)
{
    // Node 0 must be recorded after node 1.
    const std::vector<GraphOrderEdge> edges{ { /*consumer*/ 0, /*source*/ 1 } };
    const auto                        order = stableTopologicalOrder(2, edges);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 0u);
}

TEST(GraphOrderTest, ChainIsOrderedSourceFirst)
{
    // 2 depends on 1 depends on 0, with the input order reversed.
    const std::vector<GraphOrderEdge> edges{ { 2, 1 }, { 1, 0 } };
    EXPECT_EQ(stableTopologicalOrder(3, edges), (std::vector<std::size_t>{ 0, 1, 2 }));
}

TEST(GraphOrderTest, UnrelatedNodesStayStable)
{
    // 0 and 2 are independent; 3 depends on 1. The seeded order must survive.
    const std::vector<GraphOrderEdge> edges{ { 3, 1 } };
    EXPECT_EQ(stableTopologicalOrder(4, edges), (std::vector<std::size_t>{ 0, 1, 2, 3 }));
}

TEST(GraphOrderTest, CycleIsNotDropped)
{
    const std::vector<GraphOrderEdge> edges{ { 0, 1 }, { 1, 0 } };
    const auto                        order = stableTopologicalOrder(2, edges);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 0u);
    EXPECT_EQ(order[1], 1u);
}

TEST(GraphOrderTest, SelfAndOutOfRangeEdgesAreIgnored)
{
    const std::vector<GraphOrderEdge> edges{ { 0, 0 }, { 5, 0 }, { 0, 9 } };
    EXPECT_EQ(stableTopologicalOrder(2, edges), (std::vector<std::size_t>{ 0, 1 }));
}

TEST(GraphOrderTest, AContentSlotThatSamplesATargetIsAnEdgeToo)
{
    // A content pass can read ANOTHER target's depth as its shadow map (detail::resolveShadowInput), and
    // that is the same dependency a program slot carries: it must record after the map's producer. The
    // slot records which target it resolved for exactly this reason — without the edge the order came
    // from the build order alone, so a consumer whose graph was created first shaded against the
    // previous frame's map, with no validation error at all (the image layouts agree).
    vine::graphics::RenderTargetPtr producer{ new vine::graphics::RenderTarget() };
    vine::graphics::RenderTargetPtr consumer{ new vine::graphics::RenderTarget() };

    VsgRendererState state;
    state.entryFor(consumer.get()).content_slots[SlotKey{}].sampled_target = producer.get();

    // The consumer is FIRST in the current order: exactly the case a creation-order seed cannot fix.
    const std::vector<vine::graphics::RenderTarget*> recorded_now{ consumer.get(), producer.get() };
    const auto                                       edges = collectOrderEdges(state, recorded_now);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges.front().consumer, 0u);
    EXPECT_EQ(edges.front().source, 1u);

    // And the ordering that edge produces puts the producer first.
    EXPECT_EQ(stableTopologicalOrder(2, edges), (std::vector<std::size_t>{ 1, 0 }));
}

TEST(GraphOrderTest, ADetachedOrUnrecordedSourceContributesNoEdge)
{
    vine::graphics::RenderTargetPtr producer{ new vine::graphics::RenderTarget() };
    vine::graphics::RenderTargetPtr elsewhere{ new vine::graphics::RenderTarget() };
    vine::graphics::RenderTargetPtr consumer{ new vine::graphics::RenderTarget() };

    VsgRendererState state;
    auto&            slot = state.entryFor(consumer.get()).content_slots[SlotKey{}];

    // A DETACHED slot is not recorded this frame, so it samples nothing and orders nothing.
    slot.sampled_target = producer.get();
    slot.detached       = true;
    EXPECT_TRUE(collectOrderEdges(state, { consumer.get(), producer.get() }).empty());

    // A source that is not itself recorded this frame is not waited for: there is nothing to order
    // against (this is also what keeps a released source from producing a dangling edge).
    slot.detached = false;
    EXPECT_TRUE(collectOrderEdges(state, { consumer.get(), elsewhere.get() }).empty());

    // Recorded and attached: the edge is there.
    EXPECT_EQ(collectOrderEdges(state, { consumer.get(), producer.get() }).size(), 1u);
}

TEST(GraphOrderTest, AProgramSlotAndADepthBorrowAreEdgesToo)
{
    // The other two rules the same collection carries, pinned where they are decided rather than only
    // through a whole frame: a program slot's sampled target, and a borrower's depth source.
    vine::graphics::RenderTargetPtr source{ new vine::graphics::RenderTarget() };
    vine::graphics::RenderTargetPtr program_consumer{ new vine::graphics::RenderTarget() };
    vine::graphics::RenderTargetPtr borrower{ new vine::graphics::RenderTarget() };

    VsgRendererState state;
    state.entryFor(program_consumer.get()).program_slots[SlotKey{}].source_target = source.get();
    state.entryFor(borrower.get()).depth_source                                  = source.get();

    const std::vector<vine::graphics::RenderTarget*> recorded_now{ program_consumer.get(), borrower.get(),
                                                                  source.get() };
    const auto                                       edges = collectOrderEdges(state, recorded_now);
    ASSERT_EQ(edges.size(), 2u);
    for (const auto& edge : edges) {
        EXPECT_EQ(edge.source, 2u) << "both consumers wait for the one producer";
        EXPECT_NE(edge.consumer, 2u);
    }
}
