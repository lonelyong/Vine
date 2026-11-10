#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/BvhBuilder.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/RayHit.hpp>

using vn::geom::spatial::Aabb;
using vn::geom::spatial::Bvh;
using vn::geom::spatial::BvhBuilder;
using vn::geom::spatial::Ray;
using vn::geom::spatial::RayHit;

namespace
{

/// Primitive count large enough to give the tree real depth, small enough to keep the suite quick.
constexpr int kPrimitiveCount = 2000;

/// Queries per shape compared against the brute-force answer.
constexpr int kQueryCount = 400;

/**
 * @brief Fills a tree with deterministic pseudo-random boxes.
 *
 * @details
 * - A fixed seed, so a failure is reproducible from the test name alone.
 * - Boxes vary in size by two orders of magnitude, which is what makes a tree worth having and a bad split visible.
 *
 * @return The built tree, plus the boxes in primitive-id order for the brute-force answer.
 */
std::pair<Bvh<double>, std::vector<Aabb<double>>> buildRandomTree()
{
    std::mt19937                    random(20261107);
    std::uniform_real_distribution<double> position(-100.0, 100.0);
    std::uniform_real_distribution<double> extent(0.1, 10.0);

    Bvh<double>              tree;
    std::vector<Aabb<double>> boxes;
    boxes.reserve(kPrimitiveCount);
    tree.reserve(kPrimitiveCount);

    for (int index = 0; index < kPrimitiveCount; ++index) {
        const double x = position(random);
        const double y = position(random);
        const double z = position(random);
        const double sx = extent(random);
        const double sy = extent(random);
        const double sz = extent(random);
        boxes.emplace_back(x, y, z, x + sx, y + sy, z + sz);
        tree.add(boxes.back());
    }
    BvhBuilder{}.build(tree);
    return { std::move(tree), std::move(boxes) };
}

/**
 * @brief Collects the ids a query reports, sorted, so two traversals can be compared as sets.
 *
 * @tparam Query Callable taking an `std::vector<vn::geom::spatial::Bvh<double>::PrimId>&`.
 * @param query  The query to run.
 * @return The reported ids, sorted ascending.
 */
template <typename Query>
std::vector<std::uint32_t> collectIds(Query query)
{
    std::vector<std::uint32_t> ids;
    query(ids);
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

TEST(BvhQueryTest, OverlapVisitsEveryPrimitiveThatOverlaps)
{
    const auto [tree, boxes] = buildRandomTree();
    ASSERT_TRUE(tree.validate());

    std::mt19937                           random(7);
    std::uniform_real_distribution<double> position(-100.0, 100.0);
    std::uniform_real_distribution<double> extent(1.0, 30.0);

    std::size_t reported = 0;
    for (int query = 0; query < kQueryCount; ++query) {
        const double x = position(random);
        const double y = position(random);
        const double z = position(random);
        const Aabb<double> box(x, y, z, x + extent(random), y + extent(random), z + extent(random));

        const auto visited = collectIds([&](std::vector<std::uint32_t>& ids) {
            tree.forEachOverlap(box, [&](std::uint32_t id, const Aabb<double>&) {
                ids.push_back(id);
                return true;
            });
        });

        std::vector<std::uint32_t> expected;
        for (std::uint32_t id = 0; id < boxes.size(); ++id) {
            Aabb<double> overlap;
            if (boxes[id].intersectWith(box, overlap)) {
                expected.push_back(id);
            }
        }

        // The leaf volumes are conservative, so the traversal may report more than the exact answer but never less.
        std::vector<std::uint32_t> missing;
        std::set_difference(expected.begin(), expected.end(), visited.begin(), visited.end(),
                            std::back_inserter(missing));
        ASSERT_TRUE(missing.empty()) << "query " << query << " missed " << missing.size() << " primitives";
        reported += visited.size();
    }
    EXPECT_GT(reported, 0u);
}

TEST(BvhQueryTest, RayVisitsEveryPrimitiveTheRayEnters)
{
    const auto [tree, boxes] = buildRandomTree();
    ASSERT_TRUE(tree.validate());

    std::mt19937                           random(11);
    std::uniform_real_distribution<double> position(-120.0, 120.0);
    std::uniform_real_distribution<double> component(-1.0, 1.0);

    std::size_t reported = 0;
    for (int query = 0; query < kQueryCount; ++query) {
        const Ray<double> ray(vn::math::Point3<double>(position(random), position(random), position(random)),
                              vn::math::Vector3<double>(component(random), component(random), component(random)));
        if (ray.direction.length2() == 0.0) {
            continue;
        }

        const auto visited = collectIds([&](std::vector<std::uint32_t>& ids) {
            tree.forEachRay(ray, [&](std::uint32_t id, const Aabb<double>&, double&) {
                ids.push_back(id);
                return true;
            });
        });

        std::vector<std::uint32_t> expected;
        for (std::uint32_t id = 0; id < boxes.size(); ++id) {
            RayHit<double> hit;
            if (boxes[id].intersectWith(ray, hit)) {
                expected.push_back(id);
            }
        }

        std::vector<std::uint32_t> missing;
        std::set_difference(expected.begin(), expected.end(), visited.begin(), visited.end(),
                            std::back_inserter(missing));
        ASSERT_TRUE(missing.empty()) << "ray " << query << " missed " << missing.size() << " primitives";
        reported += visited.size();
    }
    EXPECT_GT(reported, 0u);
}

TEST(BvhQueryTest, ClosestHitFindsTheSameDistanceAsBruteForce)
{
    const auto [tree, boxes] = buildRandomTree();
    ASSERT_TRUE(tree.validate());

    std::mt19937                           random(13);
    std::uniform_real_distribution<double> position(-120.0, 120.0);

    for (int query = 0; query < kQueryCount; ++query) {
        const Ray<double> ray(vn::math::Point3<double>(position(random), position(random), position(random)),
                              vn::math::Vector3<double>(1.0, 0.3, -0.2));

        double nearest = std::numeric_limits<double>::infinity();
        tree.forEachRay(
            ray,
            [&](std::uint32_t, const Aabb<double>& box, double& limit) {
                RayHit<double> hit;
                if (box.intersectWith(ray, hit) && hit.t < limit) {
                    limit  = hit.t;
                    nearest = hit.t;
                }
                return true;
            },
            nearest);

        double expected = std::numeric_limits<double>::infinity();
        for (const auto& box : boxes) {
            RayHit<double> hit;
            if (box.intersectWith(ray, hit) && hit.t < expected) {
                expected = hit.t;
            }
        }

        // Pruning is allowed to skip work, never to skip the nearest primitive.
        ASSERT_EQ(std::isfinite(nearest), std::isfinite(expected)) << "ray " << query;
        if (std::isfinite(expected)) {
            EXPECT_NEAR(nearest, expected, 1e-9) << "ray " << query;
        }
    }
}

TEST(BvhQueryTest, VisitCanStopTheTraversalEarly)
{
    const auto [tree, boxes] = buildRandomTree();
    ASSERT_TRUE(tree.validate());

    int calls = 0;
    tree.forEachOverlap(Aabb<double>(-1000, -1000, -1000, 1000, 1000, 1000),
                        [&](std::uint32_t, const Aabb<double>&) {
                            ++calls;
                            return calls < 3;
                        });
    EXPECT_EQ(calls, 3) << "the visitor's false must end the walk, not be ignored";
}

TEST(BvhQueryTest, ASealedTreeReportsTheSamePrimitivesItDidBefore)
{
    auto [tree, boxes] = buildRandomTree();
    ASSERT_TRUE(tree.validate());
    const std::size_t   prim_count = tree.primCount();
    const std::size_t   leaf_count = tree.qualityReport().leaf_count;
    const Aabb<double>  query(-10.0, -10.0, -10.0, 10.0, 10.0, 10.0);

    const auto before = collectIds([&](std::vector<std::uint32_t>& ids) {
        tree.forEachOverlap(query, [&](std::uint32_t id, const Aabb<double>&) {
            ids.push_back(id);
            return true;
        });
    });
    ASSERT_FALSE(before.empty()) << "the query has to report something for this to compare anything";

    tree.seal();

    ASSERT_TRUE(tree.isSealed());
    EXPECT_EQ(tree.primCount(), prim_count) << "the primitive order keeps the count the volumes stated";
    EXPECT_TRUE(tree.isBuilt());
    EXPECT_TRUE(tree.validate()) << "validate() checks the tree's own combinatorics, which sealing does not touch";
    EXPECT_EQ(tree.qualityReport().leaf_count, leaf_count) << "the leaves and their boxes are the RESULT, not scratch";

    const auto after = collectIds([&](std::vector<std::uint32_t>& ids) {
        tree.forEachOverlap(query, [&](std::uint32_t id, const Aabb<double>& box) {
            // What replaces the per-primitive box is the leaf's: a SUPERSET of it, which is why the ids cannot change.
            EXPECT_LE(box.xmin, boxes[id].xmin);
            EXPECT_LE(box.ymin, boxes[id].ymin);
            EXPECT_LE(box.zmin, boxes[id].zmin);
            EXPECT_GE(box.xmax, boxes[id].xmax);
            EXPECT_GE(box.ymax, boxes[id].ymax);
            EXPECT_GE(box.zmax, boxes[id].zmax);
            ids.push_back(id);
            return true;
        });
    });
    EXPECT_EQ(after, before);
}

TEST(BvhQueryTest, ASealedTreeStillAnswersRayQueriesWithTheSamePrimitives)
{
    auto [tree, boxes] = buildRandomTree();
    ASSERT_TRUE(tree.validate());

    const Ray<double> ray(vn::math::Point3<double>(-120.0, -40.0, -30.0), vn::math::Vector3<double>(1.0, 0.3, -0.2));
    const auto        walk = [&](const Bvh<double>& source) {
        return collectIds([&](std::vector<std::uint32_t>& ids) {
            source.forEachRay(ray, [&](std::uint32_t id, const Aabb<double>&, double&) {
                ids.push_back(id);
                return true;
            });
        });
    };

    const auto before = walk(tree);
    tree.seal();
    EXPECT_EQ(walk(tree), before) << "the ray walk culls with leaf boxes, so dropping the primitive ones changes no id";
}

TEST(BvhQueryTest, ASealedTreeRefusesEveryOperationThatNeedsTheVolumes)
{
    auto [tree, boxes] = buildRandomTree();
    tree.seal();
    ASSERT_TRUE(tree.isSealed());

    const Aabb<double> box(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    EXPECT_THROW((void)tree.add(box), std::logic_error);
    EXPECT_THROW(tree.addRange(1u, [&](std::size_t) { return box; }), std::logic_error);
    EXPECT_THROW(tree.update(0u, box), std::logic_error);
    EXPECT_THROW(tree.refit(), std::logic_error);
    EXPECT_THROW(tree.reserve(16u), std::logic_error);
    EXPECT_THROW((void)tree.restoreTopology(tree.topology()), std::logic_error);
    EXPECT_THROW(BvhBuilder{}.build(tree), std::logic_error)
        << "a build partitions the per-primitive volumes a sealed tree no longer holds, so it cannot silently no-op";
}

TEST(BvhQueryTest, ClearingASealedTreeMakesItMutableAgain)
{
    auto [tree, boxes] = buildRandomTree();
    tree.seal();
    ASSERT_TRUE(tree.isSealed());

    tree.clear();
    EXPECT_FALSE(tree.isSealed()) << "the sealing went with the tree it described";
    EXPECT_EQ(tree.primCount(), 0u);
    EXPECT_NO_THROW((void)tree.add(Aabb<double>(0.0, 0.0, 0.0, 1.0, 1.0, 1.0)));
}

TEST(BvhQueryTest, SealingATreeThatWasNeverBuiltKeepsItsPrimitivesMutable)
{
    Bvh<double> tree;
    (void)tree.add(Aabb<double>(0.0, 0.0, 0.0, 1.0, 1.0, 1.0));

    tree.seal();

    EXPECT_FALSE(tree.isSealed()) << "there is no result to keep, so nothing is dropped";
    EXPECT_EQ(tree.primCount(), 1u);
    EXPECT_NO_THROW(BvhBuilder{}.build(tree));
    EXPECT_TRUE(tree.isBuilt());
}
