/**
 * @brief The pass-dependency graph and its schedule (see `.ai/design/vsg-reimplementation.md` D2.1).
 *
 * Device-free by construction: the graph is pure data over the pass scopes the recorder collected.
 *
 * What these cases pin, and why each one is a picture rather than a formality:
 *
 *   * the ORDER the passes run in is a rendering decision - the host announces passes in registration
 *     order and stacks them by their announced order, so a scheduler that reorders them without a
 *     dependency to justify it silently re-stacks the picture;
 *   * a CYCLE is a declaration no order can satisfy, and the three possible answers (run them anyway /
 *     drop the frame / skip the cycle and say so) look completely different on screen - "run them anyway"
 *     samples an attachment nobody wrote, and "drop the frame" turns a bug in two auxiliary passes into a
 *     black picture;
 *   * a pass that only READS a skipped one still runs: its input was not produced this frame, which is
 *     exactly the situation the contract already has for "nothing produced this input", and stranding it
 *     would let one cycle swallow the whole frame.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/vsg/core/AllocationGate.hpp>
#include <vine/vsg/core/FrameGraph.hpp>

using vn::vsg::core::CollectedPass;
using vn::vsg::core::FrameGraph;
using vn::vsg::core::FrameSchedule;

namespace
{

/// @brief Builds the pass list the graph is reset with (only the fields a schedule reads).
std::vector<CollectedPass> passes(std::initializer_list<std::pair<std::uint32_t, int>> described)
{
    std::vector<CollectedPass> list;
    for (const auto& [id, order] : described)
    {
        CollectedPass pass;
        pass.pass  = id;
        pass.order = order;
        list.push_back(pass);
    }
    return list;
}

/// @brief The scheduled nodes, as a vector of node indices.
std::vector<std::uint32_t> orderOf(const FrameSchedule& schedule)
{
    return schedule.order;
}

}  // namespace

TEST(FrameGraphTest, PassesWithoutDependenciesKeepTheAnnouncedOrderAndTheirCallOrderOnATie)
{
    const std::vector<CollectedPass> list = passes({ { 1, 10 }, { 2, -5 }, { 3, 10 } });

    FrameGraph graph;
    graph.reset(list);
    const FrameSchedule& schedule = graph.schedule();

    // Node indices are the positions in the pass list: -5 runs first, then the two 10s in call order.
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 1, 0, 2 }));
    EXPECT_TRUE(schedule.valid());
    EXPECT_EQ(graph.edgeCount(), 0u);
}

TEST(FrameGraphTest, RebuildingAndSchedulingAFrameAsksForNoMemory)
{
    // THE STEADY-STATE RULE FOR THIS TYPE, and the defect that put it here: the first version built its
    // Tarjan tables, its two stacks and its ready-queue as LOCALS, and cleared the adjacency table with
    // `assign` - which throws away the capacity every edge then asks for again. Measured as ~27 allocations
    // per frame for a one-pass pipeline, and invisible to a heap-byte reading because a frame frees what it
    // takes (`core::AllocationGate`'s counting half is what showed it; see its file note).
    //
    // The graph's own guard, next to the code: a frame with a real DEPENDENCY is the interesting case,
    // because an edge is what re-allocates when an adjacency row loses its capacity.
    if (!vn::vsg::core::AllocationGate::countsAvailable())
    {
        GTEST_SKIP() << "this binary does not instrument the allocator";
    }

    const std::vector<CollectedPass> list = passes({ { 1, -10 }, { 2, 0 }, { 3, 20 } });
    FrameGraph                       graph;

    const auto build = [&] {
        graph.reset(list);
        graph.addEdge(0, 1);
        graph.addEdge(1, 2);
        (void)graph.schedule();
    };
    build();  // warm up: the first frame is allowed to grow the tables

    vn::vsg::core::AllocationGate gate;
    gate.begin();
    build();
    gate.end();

    EXPECT_EQ(gate.allocations(), 0u)
        << "a frame that reuses the graph must not ask for memory (counted " << gate.allocations() << ")";
    EXPECT_TRUE(graph.schedule().valid());
}

TEST(FrameGraphTest, AProducerRunsBeforeItsConsumerWhateverTheAnnouncedOrder)
{
    const std::vector<CollectedPass> list = passes({ { 1, 10 }, { 2, -5 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 1);  // the first-announced pass produces what the second one reads

    const FrameSchedule& schedule = graph.schedule();
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 0, 1 }));
    EXPECT_EQ(graph.edgeCount(), 1u);
}

TEST(FrameGraphTest, BothBranchesOfADiamondRunBeforeTheJoin)
{
    const std::vector<CollectedPass> list = passes({ { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 1);
    graph.addEdge(0, 2);
    graph.addEdge(1, 3);
    graph.addEdge(2, 3);

    const FrameSchedule& schedule = graph.schedule();
    ASSERT_EQ(schedule.order.size(), 4u);
    EXPECT_EQ(schedule.order.front(), 0u);
    EXPECT_EQ(schedule.order.back(), 3u);
    EXPECT_EQ(schedule.order[1], 1u);  // the two middle passes in call order
    EXPECT_EQ(schedule.order[2], 2u);
}

TEST(FrameGraphTest, ARepeatedDependencyIsOneEdge)
{
    const std::vector<CollectedPass> list = passes({ { 1, 0 }, { 2, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 1);
    graph.addEdge(0, 1);

    (void)graph.schedule();
    EXPECT_EQ(graph.edgeCount(), 1u);
    EXPECT_EQ(orderOf(graph.schedule()), (std::vector<std::uint32_t>{ 0, 1 }));
}

TEST(FrameGraphTest, ACycleIsSkippedAndItsReaderStillRuns)
{
    // 0 <-> 1 are a cycle; 2 is independent; 3 reads what 1 writes (so it must wait for a pass that will
    // not run this frame).
    const std::vector<CollectedPass> list = passes({ { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 1);
    graph.addEdge(1, 0);
    graph.addEdge(1, 3);

    const FrameSchedule& schedule = graph.schedule();

    // The whole component is left out, and the frame still has an order for everything else.
    EXPECT_FALSE(schedule.valid());
    ASSERT_EQ(schedule.cycleCount(), 1u);
    EXPECT_EQ(schedule.skipped, (std::vector<std::uint32_t>{ 0, 1 }));
    ASSERT_EQ(schedule.cycles.size(), 1u);
    EXPECT_EQ(schedule.cycles[0], (std::vector<std::uint32_t>{ 0, 1 }));
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 2, 3 }));

    // Every node is accounted for exactly once: ordered, skipped, or excluded by the caller.
    EXPECT_EQ(schedule.order.size() + schedule.skipped.size() + graph.excludedCount(), graph.nodeCount());
}

TEST(FrameGraphTest, TwoIndependentCyclesAreTwoProblems)
{
    const std::vector<CollectedPass> list = passes({ { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 1);
    graph.addEdge(1, 0);
    graph.addEdge(2, 3);
    graph.addEdge(3, 2);
    // node 4 is independent and must still run

    const FrameSchedule& schedule = graph.schedule();
    EXPECT_EQ(schedule.cycleCount(), 2u);
    EXPECT_EQ(schedule.skipped, (std::vector<std::uint32_t>{ 0, 1, 2, 3 }));
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 4 }));
}

TEST(FrameGraphTest, ASelfEdgeIsACycleAllByItself)
{
    const std::vector<CollectedPass> list = passes({ { 1, 0 }, { 2, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 0);  // "this pass must run before itself": only a cyclic declaration means that

    const FrameSchedule& schedule = graph.schedule();
    EXPECT_EQ(schedule.cycleCount(), 1u);
    EXPECT_EQ(schedule.skipped, (std::vector<std::uint32_t>{ 0 }));
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 1 }));
}

TEST(FrameGraphTest, AnExcludedNodeNeitherOrdersNorBlocksAnything)
{
    const std::vector<CollectedPass> list = passes({ { 1, 0 }, { 2, 0 }, { 3, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 1);
    graph.addEdge(1, 2);

    // The caller could not serve node 1 this frame (an unknown target, say): its pass does not run, and the
    // pass that reads it is not stranded - the same situation as "nothing produced this input".
    graph.exclude(1);
    const FrameSchedule& schedule = graph.schedule();

    EXPECT_EQ(graph.excludedCount(), 1u);
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 0, 2 }));
    EXPECT_TRUE(schedule.valid());
    EXPECT_TRUE(schedule.skipped.empty());
}

TEST(FrameGraphTest, AnEdgeToANodeOutsideTheGraphIsIgnored)
{
    const std::vector<CollectedPass> list = passes({ { 1, 0 } });

    FrameGraph graph;
    graph.reset(list);
    graph.addEdge(0, 7);  // no such node
    graph.addEdge(7, 0);

    const FrameSchedule& schedule = graph.schedule();
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 0 }));
    EXPECT_EQ(graph.edgeCount(), 0u);
}

TEST(FrameGraphTest, ResettingTheGraphForgetsTheFrameThatWasThere)
{
    FrameGraph graph;
    graph.reset(passes({ { 1, 0 }, { 2, 0 } }));
    graph.addEdge(0, 1);
    (void)graph.schedule();

    graph.reset(passes({ { 3, 0 } }));
    const FrameSchedule& schedule = graph.schedule();
    EXPECT_EQ(graph.nodeCount(), 1u);
    EXPECT_EQ(graph.edgeCount(), 0u);
    EXPECT_EQ(orderOf(schedule), (std::vector<std::uint32_t>{ 0 }));
}
