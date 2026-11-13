#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>

#include <vine/graphics/RenderCommand.hpp>

#include "DrawListBake.hpp"

using vn::geom::spatial::Aabbd;
using vn::graphics::RenderCommand;
using vn::graphics::detail::makeSortKey;
using vn::graphics::detail::SortKey;
using vn::math::Mat4d;
using vn::math::Point3d;

namespace
{

/** @brief A key by intent (field order: draw order, layer, transparency, depth, tie). */
SortKey key(std::int32_t order, std::uint32_t layer, bool transparent, std::int64_t depth, std::uint64_t tie)
{
    return SortKey{ order, layer, transparent, depth, tie };
}

} // namespace

/*
 * @brief T9: the sort key is a TOTAL order - any permutation of the same commands answers the same list.
 *
 * This is what a stable sort does NOT give: with equal keys, the result would be the input order, so two producers (or
 * two machines) that walk the same content in a different order would draw it differently. The tie field is what
 * makes the order a property of the commands instead (see SortKey).
 */
TEST(SelectTest, TheSortKeyIsATotalOrder)
{
    const std::vector<SortKey> keys{
        key(0, 0, false, 5, 1),
        key(0, 0, false, 10, 2),
        key(0, 0, true, 10, 3),
        key(0, 0, true, 5, 4),
        key(0, 1, false, 1, 5),
        key(1, 0, false, 1, 6),
        key(0, 0, false, 5, 7), // same place in the order as key 0, a different command
        key(-1, 3, true, 0, 8),
    };

    std::vector<SortKey> expected = keys;
    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(std::adjacent_find(expected.begin(), expected.end()) == expected.end(), true)
        << "no two keys compare equal: the tie field is unique per command";

    std::mt19937 rng(20261113U);
    for (int round = 0; round < 64; ++round) {
        std::vector<SortKey> shuffled = keys;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::sort(shuffled.begin(), shuffled.end());
        EXPECT_EQ(shuffled, expected) << "round " << round << ": a permutation is not a different picture";
    }
}

/*
 * @brief The field order the key documents: draw order, then layer, then the segment, then depth, then the tie.
 */
TEST(SelectTest, TheKeyReadsDrawOrderThenLayerThenSegmentThenDepth)
{
    EXPECT_LT(key(0, 9, true, 100, 1), key(1, 0, false, 0, 2)) << "the declared draw order dominates everything";
    EXPECT_LT(key(0, 0, true, 100, 1), key(0, 1, false, 0, 2)) << "the layer dominates the segment and the depth";
    EXPECT_LT(key(0, 0, false, 100, 1), key(0, 0, true, 0, 2)) << "opaque is drawn before transparent";
    EXPECT_LT(key(0, 0, false, 5, 2), key(0, 0, false, 6, 1)) << "opaque front-to-back (closer first)";
    EXPECT_LT(key(0, 0, true, 6, 2), key(0, 0, true, 5, 1)) << "transparent back-to-front (farther first)";
    EXPECT_LT(key(0, 0, false, 5, 1), key(0, 0, false, 5, 2)) << "the tie breaks two otherwise equal commands";
    EXPECT_FALSE(key(0, 0, false, 5, 1) < key(0, 0, false, 5, 1)) << "a key is not less than itself";
}

/*
 * @brief The representative point is the world box CENTRE, not the node's origin.
 *
 * A CAD part that keeps its original coordinates has its local origin far from the geometry, and a key taken there
 * orders the part by a point that is not on it - the defect `.ai/bugs/graphics-transparent-sort-uses-origin.md`
 * records. Here the NEAR object's node origin is farther than the far object's, so an origin-based key would answer
 * the opposite of this expectation.
 */
TEST(SelectTest, TheKeySortsByTheBoxCentreNotTheNodeOrigin)
{
    const Mat4d view; // identity: the camera sits at the origin and looks down its own -Z

    RenderCommand near_command;
    near_command.model_matrix = vn::math::translate(0.0, 0.0, -100.0); // its origin is the FARTHEST point in the test
    RenderCommand far_command;
    far_command.model_matrix = vn::math::translate(0.0, 0.0, 0.0);

    const Aabbd near_box(Point3d(-1.0, -1.0, -6.0), Point3d(1.0, 1.0, -4.0)); // centre z = -5
    const Aabbd far_box(Point3d(-1.0, -1.0, -11.0), Point3d(1.0, 1.0, -9.0)); // centre z = -10

    const SortKey near_key = makeSortKey(near_command, 1U, near_box, view, 1U);
    const SortKey far_key  = makeSortKey(far_command, 1U, far_box, view, 2U);

    EXPECT_LT(near_key.depth, far_key.depth) << "camera-space depth grows away from the eye";
    EXPECT_LT(near_key, far_key) << "the box centre decides; the node origin does not";
}

/*
 * @brief A row with no box falls back to its own origin: an approximation, but a defined one.
 */
TEST(SelectTest, ACommandWithNoBoxFallsBackToItsOrigin)
{
    const Mat4d view;

    RenderCommand command;
    command.model_matrix = vn::math::translate(0.0, 0.0, -7.0);

    const SortKey keyed = makeSortKey(command, 1U, Aabbd::empty(), view, 1U);
    EXPECT_EQ(keyed.depth, static_cast<std::int64_t>(std::llround(7.0 * SortKey::kDepthStep))) << "origin z = -7 is 7 in front";
}
