#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/Bvh.hpp>
#include <vine/geom/spatial/BvhBuilder.hpp>
#include <vine/geom/spatial/detail/BvhBuildJob.hpp>

using vn::geom::spatial::Aabb;
using vn::geom::spatial::Bvh;
using vn::geom::spatial::BvhBuilder;

namespace
{

/**
 * @brief The dumbest legal build: one leaf covering every primitive.
 *
 * @details
 * - Written to prove the point of the split: the tree's traversal and its invariants must not care how good the
 *   construction was. If this passes, the structure depends on the contract and nothing else.
 */
struct StickBuild
{
    template <typename T, template <typename> class Volume>
    std::uint32_t operator()(vn::geom::spatial::detail::BuildJob<T, Volume>& job) const
    {
        const auto node = job.appendNode();
        job.writeLeaf(node, 0, static_cast<std::uint32_t>(job.order.size()));
        job.depth = 0;
        return job.node_count;
    }
};

/**
 * @brief A build that breaks the layout contract by pointing its right child at the wrong slot.
 */
struct BrokenChildBuild
{
    template <typename T, template <typename> class Volume>
    std::uint32_t operator()(vn::geom::spatial::detail::BuildJob<T, Volume>& job) const
    {
        const auto count = static_cast<std::uint32_t>(job.order.size());
        if (count < 2) {
            return StickBuild{}(job);
        }
        const auto root = job.appendNode();
        job.appendNode();
        job.depth              = 1;
        job.node_bounds[root]  = job.rangeBounds(0, count);
        job.nodes[root].count  = 0;
        job.nodes[root].offset = 99; // nowhere near a valid slot
        return job.node_count;
    }
};

/**
 * @brief A build that forgets to make the order a permutation.
 */
struct BrokenOrderBuild
{
    template <typename T, template <typename> class Volume>
    std::uint32_t operator()(vn::geom::spatial::detail::BuildJob<T, Volume>& job) const
    {
        for (auto& id : job.order) {
            id = 0; // every slot the same primitive: not a permutation
        }
        return StickBuild{}(job);
    }
};

/// A tree over thirty-two primitives, enough for a real split.
Bvh<double> makeTree()
{
    Bvh<double> tree;
    for (int index = 0; index < 32; ++index) {
        const double x = static_cast<double>(index) * 2.0;
        tree.add(Aabb<double>(x, 0, 0, x + 1.0, 1, 1));
    }
    return tree;
}

/// Collects the ids an overlap query reports, sorted.
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

} // namespace

TEST(BvhBuilderContractTest, ADegenerateBuildStillYieldsACorrectTree)
{
    auto custom = makeTree();
    BvhBuilder{}.buildWith(custom, StickBuild{});

    ASSERT_TRUE(custom.validate());
    EXPECT_EQ(custom.qualityReport().depth, 0u);
    EXPECT_EQ(custom.qualityReport().leaf_count, 1u);

    auto reference = makeTree();
    BvhBuilder{}.build(reference);

    // A single leaf holds every primitive, so it reports all of them for any query that reaches it: that is what
    // "conservative" means, and it is exactly why the caller still runs its own exact test.
    const Aabb<double> query(0, 0, 0, 20, 1, 1);
    const auto         reported = overlapIds(custom, query);
    for (std::uint32_t id = 0; id < 12; ++id) {
        ASSERT_TRUE(std::binary_search(reported.begin(), reported.end(), id))
            << "the degenerate build missed primitive " << id;
    }
    EXPECT_LE(overlapIds(reference, query).size(), reported.size());
}

TEST(BvhBuilderContractTest, ValidateRejectsABrokenRightChild)
{
    auto tree = makeTree();
    BvhBuilder{}.buildWith(tree, BrokenChildBuild{});

    EXPECT_FALSE(tree.validate());
}

TEST(BvhBuilderContractTest, ValidateRejectsABrokenOrder)
{
    auto tree = makeTree();
    BvhBuilder{}.buildWith(tree, BrokenOrderBuild{});

    EXPECT_FALSE(tree.validate());
}

TEST(BvhBuilderContractTest, TheBuilderIsIndependentOfTheTreeItFilled)
{
    const BvhBuilder builder = BvhBuilder{}.leafSize(2).maxDepth(32).quality(vn::geom::spatial::BvhBuildQuality::kFast);

    auto first = makeTree();
    BvhBuilder{}.build(first);
    const auto first_quality = first.qualityReport();

    // A builder copied earlier must be able to build another tree with its own settings.
    BvhBuilder copy = builder;
    auto        second = makeTree();
    copy.build(second);

    EXPECT_TRUE(second.validate());
    EXPECT_GT(first_quality.depth, 0u);
    EXPECT_LE(second.qualityReport().depth, 32u);
    EXPECT_EQ(builder.leafSize(), 2u);
    EXPECT_EQ(builder.maxDepth(), 32u);
}
