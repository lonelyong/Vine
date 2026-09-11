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

#include "VsgBackendUtility.hpp"

using vine::vsg::detail::GraphOrderEdge;
using vine::vsg::detail::stableTopologicalOrder;

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
