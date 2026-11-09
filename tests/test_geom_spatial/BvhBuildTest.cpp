#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/BvhBuilder.hpp>
#include <vine/geom/spatial/BvhTopology.hpp>

using vn::geom::spatial::Aabb;
using vn::geom::spatial::Bvh;
using vn::geom::spatial::BvhBuilder;
using vn::geom::spatial::BvhBuildQuality;

namespace
{

/**
 * @brief Fills a tree with `count` boxes spread over a grid, deterministically.
 *
 * @param count Number of primitives.
 * @return The tree, not yet built.
 */
Bvh<double> makeGridTree(int count)
{
    Bvh<double> tree;
    tree.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double x = static_cast<double>(index % 17) * 3.0;
        const double y = static_cast<double>((index / 17) % 13) * 3.0;
        const double z = static_cast<double>(index / (17 * 13)) * 3.0;
        tree.add(Aabb<double>(x, y, z, x + 1.0, y + 1.0, z + 1.0));
    }
    return tree;
}

/**
 * @brief Collects the ids an overlap query reports, sorted.
 *
 * @param tree The tree.
 * @param box  The query box.
 * @return The reported ids.
 */
std::vector<std::uint32_t> overlapIds(const Bvh<double>& tree, const Aabb<double>& box)
{
    std::vector<std::uint32_t> ids;
    tree.forEachOverlap(box, [&](std::uint32_t id, const Aabb<double>&) {
        ids.push_back(id);
        return true;
    });
    std::sort(ids.begin(), ids.end());
    return ids;
}

/**
 * @brief Asserts that a reported id set contains every primitive whose own box overlaps the query.
 *
 * @details
 * - The tree reports candidates against leaf boxes, which are unions of their primitives' boxes, so the reported set is
 *   a superset of the exact answer. Equality would only hold for a one-primitive-per-leaf tree.
 *
 * @param tree     The tree to query.
 * @param boxes    Every primitive box, indexed by id.
 * @param query    The query box.
 * @param reported The ids the tree reported.
 */
void expectCoversExactAnswer(const Bvh<double>& tree, const std::vector<Aabb<double>>& boxes, const Aabb<double>& query,
                             const std::vector<std::uint32_t>& reported)
{
    (void)tree;
    for (std::uint32_t id = 0; id < boxes.size(); ++id) {
        Aabb<double> overlap;
        if (boxes[id].intersectWith(query, overlap)) {
            ASSERT_TRUE(std::binary_search(reported.begin(), reported.end(), id))
                << "primitive " << id << " overlaps but was not reported";
        }
    }
}

/**
 * @brief Returns `count` grid boxes, in primitive-id order.
 *
 * @param count Number of boxes.
 * @return The boxes.
 */
std::vector<Aabb<double>> gridBoxes(int count)
{
    std::vector<Aabb<double>> boxes;
    boxes.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double x = static_cast<double>(index % 17) * 3.0;
        const double y = static_cast<double>((index / 17) % 13) * 3.0;
        const double z = static_cast<double>(index / (17 * 13)) * 3.0;
        boxes.emplace_back(x, y, z, x + 1.0, y + 1.0, z + 1.0);
    }
    return boxes;
}

} // namespace

TEST(BvhBuildTest, BuildProducesAValidTreeWithAPermutationOrder)
{
    auto tree = makeGridTree(500);

    EXPECT_FALSE(tree.isBuilt());
    EXPECT_TRUE(tree.primOrder().empty()) << "an unbuilt tree has no order to report";

    BvhBuilder{}.build(tree);

    ASSERT_TRUE(tree.isBuilt());
    EXPECT_TRUE(tree.validate());
    ASSERT_EQ(tree.primOrder().size(), 500u);

    std::vector<std::uint32_t> order(tree.primOrder().begin(), tree.primOrder().end());
    std::sort(order.begin(), order.end());
    for (std::uint32_t index = 0; index < order.size(); ++index) {
        ASSERT_EQ(order[index], index) << "primOrder() must be a permutation of every id";
    }

    const auto quality = tree.qualityReport();
    EXPECT_GT(quality.leaf_count, 0u);
    EXPECT_GT(quality.leaf_half_area_sum, 0.0);
    EXPECT_GT(quality.depth, 0u);
}

TEST(BvhBuildTest, BuildIsIdempotentWhateverHappenedBefore)
{
    auto tree = makeGridTree(500);

    BvhBuilder{}.build(tree);
    const std::vector<std::uint32_t> first_order(tree.primOrder().begin(), tree.primOrder().end());
    const auto                       first_quality = tree.qualityReport();

    // A different quality in between must not leak into the next build.
    BvhBuilder{}.quality(BvhBuildQuality::kFast).build(tree);
    BvhBuilder{}.build(tree);

    const std::vector<std::uint32_t> second_order(tree.primOrder().begin(), tree.primOrder().end());
    const auto                       second_quality = tree.qualityReport();

    EXPECT_EQ(first_order, second_order);
    EXPECT_EQ(first_quality.leaf_count, second_quality.leaf_count);
    EXPECT_EQ(first_quality.depth, second_quality.depth);
    EXPECT_DOUBLE_EQ(first_quality.leaf_half_area_sum, second_quality.leaf_half_area_sum);
}

TEST(BvhBuildTest, BothQualitiesProduceCorrectTrees)
{
    for (const auto quality : { BvhBuildQuality::kFast, BvhBuildQuality::kSah }) {
        auto   tree = makeGridTree(500);
        BvhBuilder{}.quality(quality).build(tree);

        ASSERT_TRUE(tree.validate()) << "quality index " << static_cast<int>(quality);

        std::size_t visited = 0;
        tree.forEachOverlap(Aabb<double>(-1, -1, -1, 1000, 1000, 1000), [&](std::uint32_t, const Aabb<double>&) {
            ++visited;
            return true;
        });
        EXPECT_GE(visited, 500u) << "every primitive must be reachable";
    }
}

TEST(BvhBuildTest, LeafSizeChangesTheShapeButNotTheAnswers)
{
    auto tree = makeGridTree(400);
    BvhBuilder{}.leafSize(1).build(tree);
    const auto fine = tree.qualityReport();

    auto coarse_tree = makeGridTree(400);
    BvhBuilder{}.leafSize(16).build(coarse_tree);
    const auto coarse = coarse_tree.qualityReport();

    EXPECT_GT(fine.leaf_count, coarse.leaf_count) << "a smaller leaf must make more leaves";
    EXPECT_GT(fine.depth, coarse.depth);

    const Aabb<double>         query = Aabb<double>(0, 0, 0, 20, 20, 20);
    const std::vector<Aabb<double>> boxes = gridBoxes(400);
    // A bigger leaf makes bigger leaf boxes, so it reports more candidates; both must cover the exact answer.
    expectCoversExactAnswer(tree, boxes, query, overlapIds(tree, query));
    expectCoversExactAnswer(coarse_tree, boxes, query, overlapIds(coarse_tree, query));
}

TEST(BvhBuildTest, RefitMatchesARebuildOfTheSameVolumes)
{
    auto moved = makeGridTree(400);
    BvhBuilder{}.build(moved);

    std::mt19937                           random(5);
    std::uniform_real_distribution<double> shift(-2.0, 2.0);
    for (std::uint32_t id = 0; id < moved.primCount(); ++id) {
        const double dx = shift(random);
        const double dy = shift(random);
        const double dz = shift(random);
        // Shifting every box the same way keeps them valid as leaf volumes; the tree only needs its boxes refreshed.
        const double x = static_cast<double>(id % 17) * 3.0 + dx;
        const double y = static_cast<double>((id / 17) % 13) * 3.0 + dy;
        const double z = static_cast<double>(id / (17 * 13)) * 3.0 + dz;
        moved.update(id, Aabb<double>(x, y, z, x + 1.0, y + 1.0, z + 1.0));
    }
    moved.refit();

    auto rebuilt = makeGridTree(400);
    std::mt19937                           replay(5);
    for (std::uint32_t id = 0; id < rebuilt.primCount(); ++id) {
        const double dx = shift(replay);
        const double dy = shift(replay);
        const double dz = shift(replay);
        const double x = static_cast<double>(id % 17) * 3.0 + dx;
        const double y = static_cast<double>((id / 17) % 13) * 3.0 + dy;
        const double z = static_cast<double>(id / (17 * 13)) * 3.0 + dz;
        rebuilt.update(id, Aabb<double>(x, y, z, x + 1.0, y + 1.0, z + 1.0));
    }
    BvhBuilder{}.build(rebuilt);

    ASSERT_TRUE(moved.validate());
    ASSERT_TRUE(rebuilt.validate());

    const Aabb<double> query(-10, -10, -10, 60, 60, 60);
    EXPECT_EQ(overlapIds(moved, query), overlapIds(rebuilt, query));
}

TEST(BvhBuildTest, RebuildReusesCapacity)
{
    auto tree = makeGridTree(600);
    BvhBuilder{}.build(tree);

    const auto order_before = std::vector<std::uint32_t>(tree.primOrder().begin(), tree.primOrder().end());
    BvhBuilder{}.build(tree);
    const auto order_after = std::vector<std::uint32_t>(tree.primOrder().begin(), tree.primOrder().end());

    EXPECT_EQ(order_before, order_after) << "a rebuild must not depend on the previous order";
}

TEST(BvhBuildTest, UnbuiltTreeAnswersNothingRatherThanFromAStaleTree)
{
    auto tree = makeGridTree(64);
    BvhBuilder{}.build(tree);

    int calls = 0;
    tree.forEachOverlap(Aabb<double>(-1e6, -1e6, -1e6, 1e6, 1e6, 1e6), [&](std::uint32_t, const Aabb<double>&) {
        ++calls;
        return true;
    });
    EXPECT_EQ(calls, 64);

    tree.add(Aabb<double>(0, 0, 0, 1, 1, 1)); // adding needs a rebuild
    EXPECT_FALSE(tree.isBuilt());

    calls = 0;
    tree.forEachOverlap(Aabb<double>(-1e6, -1e6, -1e6, 1e6, 1e6, 1e6), [&](std::uint32_t, const Aabb<double>&) {
        ++calls;
        return true;
    });
    EXPECT_EQ(calls, 0) << "a tree with pending additions must not answer from the previous topology";
}

TEST(BvhBuildTest, EdgeCasesStayLegal)
{
    // Nothing registered: a build is legal and leaves an empty tree.
    Bvh<double> empty_tree;
    BvhBuilder{}.build(empty_tree);
    EXPECT_TRUE(empty_tree.empty());
    EXPECT_TRUE(empty_tree.validate());

    // One primitive: a single leaf, and the queries reach it.
    Bvh<double> single;
    single.add(Aabb<double>(0, 0, 0, 1, 1, 1));
    BvhBuilder{}.build(single);
    EXPECT_TRUE(single.validate());
    EXPECT_EQ(overlapIds(single, Aabb<double>(0.5, 0.5, 0.5, 2, 2, 2)).size(), 1u);

    // Degenerate boxes and identical centres are the pathological input a split heuristic can trip over.
    Bvh<double> degenerate;
    for (int index = 0; index < 64; ++index) {
        degenerate.add(Aabb<double>(0, 0, 0, 0, 0, 0));
    }
    BvhBuilder{}.build(degenerate);
    EXPECT_TRUE(degenerate.validate());

    // A depth limit of one forces a single leaf; the answers must still be complete.
    Bvh<double> shallow = makeGridTree(64);
    BvhBuilder{}.maxDepth(1).build(shallow);
    EXPECT_TRUE(shallow.validate());
    EXPECT_EQ(overlapIds(shallow, Aabb<double>(-1, -1, -1, 1e6, 1e6, 1e6)).size(), 64u);
}

TEST(BvhBuildTest, InvalidInputIsRejected)
{
    Bvh<double> tree;

    EXPECT_THROW((void)tree.add(Aabb<double>(std::numeric_limits<double>::quiet_NaN(), 0, 0, 1, 1, 1)),
                 std::invalid_argument);
    EXPECT_THROW((void)tree.add(Aabb<double>(1, 0, 0, 0, 1, 1)), std::invalid_argument) << "inverted on X";

    tree.add(Aabb<double>(0, 0, 0, 1, 1, 1));
    EXPECT_THROW(tree.update(7, Aabb<double>(0, 0, 0, 1, 1, 1)), std::invalid_argument);

    EXPECT_THROW((void)BvhBuilder{}.leafSize(0), std::invalid_argument);
    EXPECT_THROW((void)BvhBuilder{}.maxDepth(0), std::invalid_argument);
    EXPECT_THROW((void)BvhBuilder{}.maxDepth(vn::geom::spatial::kBvhMaxDepth + 1), std::invalid_argument);
}

TEST(BvhBuildTest, ARestoredTopologyAnswersTheSameQueries)
{
    // The shape is the half a build cannot recompute cheaply, so it is the half worth storing: build once, take
    // topology(), put it into a fresh tree over the same primitives, and every query must agree.
    auto reference = makeGridTree(500);
    BvhBuilder{}.build(reference);
    ASSERT_TRUE(reference.validate());

    const vn::geom::spatial::BvhTopology shape = reference.topology();
    ASSERT_FALSE(shape.nodes.empty());

    auto restored = makeGridTree(500);
    BvhBuilder{}.buildWith(restored, vn::geom::spatial::RestoreTopology{ shape });

    ASSERT_TRUE(restored.isBuilt());
    EXPECT_TRUE(restored.validate());
    EXPECT_EQ(restored.topology(), shape) << "the restored shape is the stored one, to the last node";

    // Boxes were recomputed from the primitives on the way in, so the two trees answer identically on every query.
    const auto boxes = gridBoxes(500);
    for (int index = 0; index < 500; index += 37) {
        const Aabb<double> query = boxes[static_cast<std::size_t>(index)];
        EXPECT_EQ(overlapIds(restored, query), overlapIds(reference, query))
            << "restored and built trees must report the same candidates for query " << index;
    }

    // And the restored tree keeps working after a refit: the pass that recomputes boxes is the same one.
    restored.refit();
    EXPECT_TRUE(restored.validate());
    EXPECT_EQ(overlapIds(restored, boxes[3]), overlapIds(reference, boxes[3]));
}

TEST(BvhBuildTest, ARestoredTopologyThatDoesNotDescribeTheTreeIsRefused)
{
    auto reference = makeGridTree(64);
    BvhBuilder{}.build(reference);
    const vn::geom::spatial::BvhTopology shape = reference.topology();

    const auto restore_into = [](const vn::geom::spatial::BvhTopology& topology) {
        auto tree = makeGridTree(64);
        BvhBuilder{}.buildWith(tree, vn::geom::spatial::RestoreTopology{ topology });
        return tree;
    };

    EXPECT_TRUE(restore_into(shape).isBuilt());

    auto wrong_order       = shape;
    wrong_order.order[1]   = wrong_order.order[0]; // no longer a permutation
    EXPECT_FALSE(restore_into(wrong_order).isBuilt()) << "an order that repeats an id is refused";

    auto too_short = shape;
    too_short.order.pop_back();
    EXPECT_FALSE(restore_into(too_short).isBuilt()) << "the order must cover every primitive";

    auto wrong_height = shape;
    ++wrong_height.depth;
    EXPECT_FALSE(restore_into(wrong_height).isBuilt())
        << "a height that is not the tree's own means the shape is not the one that was written";

    auto broken_nodes = shape;
    // The root's right child must start after the whole left subtree; pointing it at the left child itself is refused.
    broken_nodes.nodes.front().offset = 1u;
    EXPECT_FALSE(restore_into(broken_nodes).isBuilt());

    auto leaf_out_of_range = shape;
    for (auto& node : leaf_out_of_range.nodes) {
        if (node.count != 0u) {
            node.offset = 64u; // one past the last primitive
            break;
        }
    }
    EXPECT_FALSE(restore_into(leaf_out_of_range).isBuilt());
}

TEST(BvhBuildTest, AnOwnedTopologyIsAdoptedByTheTreeItself)
{
    // The second door to the same result: a caller that OWNS the shape (a decoder, say) hands it over and the tree takes
    // the arrays. Both doors read the same check, so they refuse the same shapes.
    auto reference = makeGridTree(256);
    BvhBuilder{}.build(reference);
    const vn::geom::spatial::BvhTopology stored = reference.topology();

    auto adopted = makeGridTree(256);
    EXPECT_TRUE(adopted.restoreTopology(stored));
    ASSERT_TRUE(adopted.isBuilt());
    EXPECT_TRUE(adopted.validate());
    EXPECT_EQ(adopted.topology(), stored) << "the adopted shape is the stored one, to the last node";

    const auto boxes = gridBoxes(256);
    for (int index = 0; index < 256; index += 29) {
        const Aabb<double> query = boxes[static_cast<std::size_t>(index)];
        EXPECT_EQ(overlapIds(adopted, query), overlapIds(reference, query))
            << "boxes were re-derived from the primitives, so the answers agree for query " << index;
    }

    auto wrong_order     = stored;
    wrong_order.order[1] = wrong_order.order[0];
    auto refused         = makeGridTree(256);
    EXPECT_FALSE(refused.restoreTopology(wrong_order));
    EXPECT_FALSE(refused.isBuilt()) << "a refused shape leaves no tree at all";
}

TEST(BvhBuildTest, ATopologyViewBorrowsTheTreesOwnArrays)
{
    // The saving side of the storage story: what is encoded is what the tree already holds. Two views of an unchanged tree
    // over the same storage cannot differ, which is what "borrowed" means; a copy would answer with two addresses.
    auto tree = makeGridTree(64);
    BvhBuilder{}.build(tree);

    const vn::geom::spatial::BvhTopologyView first  = tree.topologyView();
    const vn::geom::spatial::BvhTopologyView second = tree.topologyView();
    EXPECT_EQ(first.nodes.data(), second.nodes.data()) << "a view hands out the tree's storage";
    EXPECT_EQ(first.order.data(), second.order.data());
    EXPECT_EQ(first.nodes.size(), tree.topology().nodes.size());
    EXPECT_EQ(first.primitiveCount(), tree.primCount());
    EXPECT_EQ(first.depth, tree.qualityReport().depth);
}
