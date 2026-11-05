#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom2d/ArcCurve.hpp>
#include <vine/geom2d/CircleCurve.hpp>
#include <vine/geom2d/CurveProjection.hpp>
#include <vine/geom2d/EllipseCurve.hpp>
#include <vine/geom2d/HyperbolaCurve.hpp>
#include <vine/geom2d/LineCurve.hpp>

namespace
{

using vn::geom2d::ArcCurved;
using vn::geom2d::CircleCurved;
using vn::geom2d::CircleCurvef;
using vn::geom2d::EllipseCurved;
using vn::geom2d::HyperbolaCurved;
using vn::geom2d::LineCurved;
using vn::geom2d::closestPointOnCurve;

TEST(CurveProjectionTest, ALineHasAnExactFoot)
{
    const LineCurved line({ 0.0, 1.0 }, { 2.0, 0.0 });

    const auto projection = closestPointOnCurve(line, vn::math::Point2<double>{ 3.0, 4.0 },
                                                std::optional<std::pair<double, double>>{ { -10.0, 10.0 } });
    ASSERT_TRUE(projection);

    EXPECT_NEAR(projection->point.x, 3.0, 1e-12);
    EXPECT_NEAR(projection->point.y, 1.0, 1e-12);
    EXPECT_NEAR(projection->parameter, 1.5, 1e-12);
    EXPECT_NEAR(projection->distance, 3.0, 1e-12);

    // The reported point is the curve evaluated at the reported parameter.
    EXPECT_NEAR(projection->point.x, line.evaluate(projection->parameter).x, 1e-12);
    EXPECT_NEAR(projection->point.y, line.evaluate(projection->parameter).y, 1e-12);
}

TEST(CurveProjectionTest, AnUnboundedCurveNeedsAWindow)
{
    const LineCurved line({ 0.0, 0.0 }, { 1.0, 0.0 });

    EXPECT_FALSE(closestPointOnCurve(line, vn::math::Point2<double>{ 3.0, 4.0 }));
}

TEST(CurveProjectionTest, AFootOutsideTheWindowFallsOnTheNearerEnd)
{
    const LineCurved line({ 0.0, 0.0 }, { 1.0, 0.0 });

    const auto projection = closestPointOnCurve(line, vn::math::Point2<double>{ 5.0, 0.0 },
                                                std::optional<std::pair<double, double>>{ { -1.0, 1.0 } });
    ASSERT_TRUE(projection);

    EXPECT_NEAR(projection->parameter, 1.0, 1e-12);
    EXPECT_NEAR(projection->point.x, 1.0, 1e-12);
    EXPECT_NEAR(projection->distance, 4.0, 1e-12);
}

TEST(CurveProjectionTest, APointProjectsOntoACircle)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);

    const auto projection = closestPointOnCurve(circle, vn::math::Point2<double>{ 3.0, 0.0 });
    ASSERT_TRUE(projection);

    EXPECT_NEAR(projection->point.x, 2.0, 1e-12);
    EXPECT_NEAR(projection->point.y, 0.0, 1e-12);
    EXPECT_NEAR(projection->parameter, 0.0, 1e-9);
    EXPECT_NEAR(projection->distance, 1.0, 1e-12);
}

TEST(CurveProjectionTest, APointProjectsOntoAnEllipse)
{
    const EllipseCurved ellipse({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    const auto projection = closestPointOnCurve(ellipse, vn::math::Point2<double>{ 5.0, 0.0 });
    ASSERT_TRUE(projection);

    EXPECT_NEAR(projection->point.x, 2.0, 1e-9);
    EXPECT_NEAR(projection->point.y, 0.0, 1e-9);
    EXPECT_NEAR(projection->parameter, 0.0, 1e-6);
    EXPECT_NEAR(projection->distance, 3.0, 1e-9);
}

TEST(CurveProjectionTest, TheCentreOfACircleReportsOneOfItsEquidistantPoints)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);

    const auto projection = closestPointOnCurve(circle, vn::math::Point2<double>{ 0.0, 0.0 });
    ASSERT_TRUE(projection);

    // Every point of the circle is a nearest point of its centre: one of them is reported, and the distance is the
    // radius.
    EXPECT_NEAR(projection->distance, 2.0, 1e-7);
    EXPECT_NEAR(projection->point.distanceTo(circle.center), 2.0, 1e-7);
}

TEST(CurveProjectionTest, APointProjectsOntoAnArc)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(circle, 0.0, vn::math::PI_HALF);

    const auto projection = closestPointOnCurve(quarter, vn::math::Point2<double>{ 3.0, 0.0 });
    ASSERT_TRUE(projection);

    EXPECT_NEAR(projection->point.x, 2.0, 1e-12);
    EXPECT_NEAR(projection->point.y, 0.0, 1e-12);
    EXPECT_NEAR(projection->parameter, 0.0, 1e-9);
    EXPECT_NEAR(projection->distance, 1.0, 1e-12);
}

TEST(CurveProjectionTest, AnUnboundedConicProjectsInsideItsWindow)
{
    const HyperbolaCurved branch({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    // The nearest point of (3, 0) on x = 2 cosh(u), y = sinh(u) is where 5 cosh(u) = 6, at distance sqrt(0.8);
    // no sample lands on it, so the answer comes from the refined root.
    const auto projection = closestPointOnCurve(branch, vn::math::Point2<double>{ 3.0, 0.0 },
                                                std::optional<std::pair<double, double>>{ { -2.0, 2.0 } });
    ASSERT_TRUE(projection);

    EXPECT_NEAR(projection->point.x, 2.4, 1e-7);
    EXPECT_NEAR(std::abs(projection->point.y), 0.6633249580712264, 1e-7);
    EXPECT_NEAR(projection->distance, 0.8944271909999159, 1e-7);
}

TEST(CurveProjectionTest, AnUnusableCurveAnswersNothing)
{
    const LineCurved degenerate({ 0.0, 0.0 }, { 0.0, 0.0 });

    EXPECT_FALSE(closestPointOnCurve(degenerate, vn::math::Point2<double>{ 1.0, 1.0 },
                                     std::optional<std::pair<double, double>>{ { -1.0, 1.0 } }));
}

TEST(CurveProjectionTest, BothPrecisionsFindTheSameFoot)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const CircleCurvef float_circle({ 0.0f, 0.0f }, 2.0f);

    const auto precise = closestPointOnCurve(circle, vn::math::Point2<double>{ 3.0, 0.0 });
    const auto narrow  = closestPointOnCurve(float_circle, vn::math::Point2<float>{ 3.0f, 0.0f });

    ASSERT_TRUE(precise);
    ASSERT_TRUE(narrow);
    EXPECT_NEAR(narrow->point.x, static_cast<float>(precise->point.x), 1e-5f);
    EXPECT_NEAR(narrow->distance, static_cast<float>(precise->distance), 1e-5f);
}

} // namespace
