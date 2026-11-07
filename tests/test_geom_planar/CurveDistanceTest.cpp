#include <gtest/gtest.h>

#include <cmath>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom/planar/BezierCurve.hpp>
#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/CurveCurveDistance.hpp>
#include <vine/geom/planar/LineCurve.hpp>
#include <vine/geom/planar/OffsetCurve.hpp>

namespace
{

using vn::geom::planar::CircleCurved;
using vn::geom::planar::CircleCurvef;
using vn::geom::planar::LineCurved;
using vn::geom::planar::curveToCurveDistance;

TEST(CurveDistanceTest, ParallelLinesKeepTheirSeparation)
{
    const LineCurved lower({ 0.0, 0.0 }, { 1.0, 0.0 });
    const LineCurved upper({ 0.0, 3.0 }, { 1.0, 0.0 });

    const auto closest = curveToCurveDistance(lower, upper, std::pair{ 0.0, 10.0 }, std::pair{ 0.0, 10.0 });
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 3.0, 1e-9);
    EXPECT_NEAR(closest->first_point.y, 0.0, 1e-9);
    EXPECT_NEAR(closest->second_point.y, 3.0, 1e-9);
}

TEST(CurveDistanceTest, ALineToACircleIsTheGap)
{
    const LineCurved   line({ -10.0, 0.0 }, { 1.0, 0.0 });
    const CircleCurved circle({ 0.0, 3.0 }, 1.0);

    const auto closest = curveToCurveDistance(line, circle, std::pair{ -10.0, 10.0 });
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 2.0, 1e-7);
    EXPECT_NEAR(closest->first_point.x, 0.0, 1e-7);
    EXPECT_NEAR(closest->first_point.y, 0.0, 1e-9);
    EXPECT_NEAR(closest->second_point.x, 0.0, 1e-7);
    EXPECT_NEAR(closest->second_point.y, 2.0, 1e-7);

    // The reported pair is consistent with the parameters and the distance.
    EXPECT_NEAR((line.evaluate(closest->first_parameter) - closest->first_point).length(), 0.0, 1e-12);
    EXPECT_NEAR((circle.evaluate(closest->second_parameter) - closest->second_point).length(), 0.0, 1e-12);
    EXPECT_NEAR((closest->second_point - closest->first_point).length(), closest->distance, 1e-12);
}

TEST(CurveDistanceTest, TwoCirclesAnswerTheGapBetweenThem)
{
    const CircleCurved left({ 0.0, 0.0 }, 2.0);
    const CircleCurved right({ 5.0, 0.0 }, 2.0);

    const auto closest = curveToCurveDistance(left, right);
    ASSERT_TRUE(closest);

    // Two radii leave one unit between the near sides.
    EXPECT_NEAR(closest->distance, 1.0, 1e-7);
    EXPECT_NEAR(closest->first_point.x, 2.0, 1e-7);
    EXPECT_NEAR(closest->second_point.x, 3.0, 1e-7);
}

TEST(CurveDistanceTest, CrossingCurvesAreAtDistanceZero)
{
    const CircleCurved left({ 0.0, 0.0 }, 2.0);
    const CircleCurved right({ 3.0, 0.0 }, 2.0);

    const auto closest = curveToCurveDistance(left, right);
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 0.0, 1e-6);
}

TEST(CurveDistanceTest, WorksInFloat)
{
    const CircleCurvef left({ 0.0f, 0.0f }, 2.0f);
    const CircleCurvef right({ 5.0f, 0.0f }, 2.0f);

    const auto closest = curveToCurveDistance(left, right);
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 1.0f, 1e-3f);
}

TEST(CurveDistanceTest, ASplineToALineIsItsClosestRise)
{
    // P(u) = (2u, 4u(1-u)) touches y = 0 at both ends and rises to 1 in between, so the distance to y = -1 is 1.
    const vn::geom::planar::BezierCurved curve({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } });
    const LineCurved            line({ 0.0, -1.0 }, { 1.0, 0.0 });

    const auto closest = curveToCurveDistance(curve, line, std::pair{ 0.0, 1.0 }, std::pair{ -10.0, 10.0 });
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 1.0, 1e-7);
}

TEST(CurveDistanceTest, AWindowCanPushTheClosestPairToItsEnd)
{
    const LineCurved   line({ 0.0, 0.0 }, { 1.0, 0.0 });
    const CircleCurved circle({ 5.0, 3.0 }, 1.0);

    // With room the line reaches x = 5 and the gap is 2; stopping it at x = 1 moves the answer to the end point.
    const auto roomy = curveToCurveDistance(line, circle, std::pair{ 0.0, 10.0 });
    ASSERT_TRUE(roomy);
    EXPECT_NEAR(roomy->distance, 2.0, 1e-7);

    const auto truncated = curveToCurveDistance(line, circle, std::pair{ 0.0, 1.0 });
    ASSERT_TRUE(truncated);
    EXPECT_NEAR(truncated->first_point.x, 1.0, 1e-7);
    EXPECT_NEAR(truncated->distance, 4.0, 1e-7);
}

TEST(CurveDistanceTest, AnOffsetCurveTakesPartWithoutSpecialCasing)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         inner = vn::geom::planar::offsetCurve(circle, 0.5);
    const CircleCurved other({ 4.0, 0.0 }, 0.5);

    // Radii 1.5 and 0.5 with centres four apart leave two units between the near sides.
    const auto closest = curveToCurveDistance(inner, other);
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 2.0, 1e-7);
}

TEST(CurveDistanceTest, RefusesAWindowItCannotUse)
{
    const LineCurved   line({ 0.0, 0.0 }, { 1.0, 0.0 });
    const CircleCurved circle({ 0.0, 3.0 }, 1.0);

    // An unbounded curve needs a finite window.
    EXPECT_FALSE(curveToCurveDistance(line, circle));
    EXPECT_FALSE(curveToCurveDistance(line, circle, std::pair{ 2.0, 1.0 }));

    // An unusable curve has no distance.
    const LineCurved degenerate({ 0.0, 0.0 }, { 0.0, 0.0 });
    EXPECT_FALSE(curveToCurveDistance(degenerate, circle, std::pair{ 0.0, 1.0 }));
}

} // namespace
