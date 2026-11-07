#include <gtest/gtest.h>

#include <cmath>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/CurveCurveDistance.hpp>
#include <vine/geom/spatial/LineCurve.hpp>

namespace
{

using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::CurveCurveDistance;
using vn::geom::spatial::curveToCurveDistance;
using vn::geom::spatial::LineCurved;
using vn::math::Point3d;
using vn::math::Vec3d;

TEST(CurveCurveDistanceTest, ParallelLinesKeepTheirSeparation)
{
    const LineCurved lower(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const LineCurved upper(Point3d(0.0, 3.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto closest = curveToCurveDistance(lower, upper, std::pair{ 0.0, 10.0 }, std::pair{ 0.0, 10.0 });
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 3.0, 1e-9);
    EXPECT_NEAR(closest->first_point.y, 0.0, 1e-9);
    EXPECT_NEAR(closest->second_point.y, 3.0, 1e-9);
}

TEST(CurveCurveDistanceTest, ALineToACircleIsTheGap)
{
    const LineCurved   line(Point3d(-10.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const CircleCurved circle(Point3d(0.0, 3.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    const auto closest = curveToCurveDistance(line, circle, std::pair{ -10.0, 10.0 });
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 2.0, 1e-7);
    EXPECT_NEAR(closest->first_point.y, 0.0, 1e-9);
    EXPECT_NEAR(closest->second_point.y, 2.0, 1e-7);

    // The reported pair is consistent with the parameters and the distance.
    EXPECT_NEAR((line.evaluate(closest->first_parameter) - closest->first_point).length(), 0.0, 1e-12);
    EXPECT_NEAR((circle.evaluate(closest->second_parameter) - closest->second_point).length(), 0.0, 1e-12);
    EXPECT_NEAR((closest->second_point - closest->first_point).length(), closest->distance, 1e-12);
}

TEST(CurveCurveDistanceTest, TwoCirclesAnswerTheGapBetweenThem)
{
    const CircleCurved left(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const CircleCurved right(Point3d(5.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    const auto closest = curveToCurveDistance(left, right);
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 1.0, 1e-7);
    EXPECT_NEAR(closest->first_point.x, 2.0, 1e-7);
    EXPECT_NEAR(closest->second_point.x, 3.0, 1e-7);
}

TEST(CurveCurveDistanceTest, CrossingCurvesAreAtDistanceZero)
{
    const CircleCurved left(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const CircleCurved right(Point3d(3.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    const auto closest = curveToCurveDistance(left, right);
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 0.0, 1e-6);
}

TEST(CurveCurveDistanceTest, WorksInFloat)
{
    const vn::geom::spatial::CircleCurvef left(vn::math::Point3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                                        2.0f);
    const vn::geom::spatial::CircleCurvef right(vn::math::Point3f(5.0f, 0.0f, 0.0f), vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                                         2.0f);

    const auto closest = curveToCurveDistance(left, right);
    ASSERT_TRUE(closest);
    EXPECT_NEAR(closest->distance, 1.0f, 1e-3f);
}

TEST(CurveCurveDistanceTest, RefusesAWindowItCannotUse)
{
    const LineCurved   line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const CircleCurved circle(Point3d(0.0, 3.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    EXPECT_FALSE(curveToCurveDistance(line, circle));
    EXPECT_FALSE(curveToCurveDistance(line, circle, std::pair{ 2.0, 1.0 }));

    const LineCurved degenerate(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0));
    EXPECT_FALSE(curveToCurveDistance(degenerate, circle, std::pair{ 0.0, 1.0 }));
}

} // namespace
