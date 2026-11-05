#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom2d/ArcCurve.hpp>
#include <vine/geom2d/CircleCurve.hpp>
#include <vine/geom2d/CurveCurveIntersection.hpp>
#include <vine/geom2d/LineCurve.hpp>

namespace
{

using vn::geom2d::ArcCurved;
using vn::geom2d::CircleCurved;
using vn::geom2d::CircleCurvef;
using vn::geom2d::CurveCurveContact;
using vn::geom2d::LineCurved;
using vn::geom2d::LineCurvef;
using vn::geom2d::intersectCurveWithCurve;

const std::optional<std::pair<double, double>> kLineWindow{ { -10.0, 10.0 } };

TEST(CurveIntersectionTest, TwoLinesCrossOnce)
{
    const LineCurved across({ 0.0, 0.0 }, { 1.0, 0.0 });
    const LineCurved upright({ 2.0, -1.0 }, { 0.0, 1.0 });

    const auto result = intersectCurveWithCurve(across, upright, kLineWindow, kLineWindow);
    ASSERT_EQ(result.contact, CurveCurveContact::Isolated);
    ASSERT_EQ(result.hits.size(), 1u);

    EXPECT_NEAR(result.hits[0].point.x, 2.0, 1e-9);
    EXPECT_NEAR(result.hits[0].point.y, 0.0, 1e-9);
    EXPECT_NEAR(result.hits[0].first_parameter, 2.0, 1e-9);
    EXPECT_NEAR(result.hits[0].second_parameter, 1.0, 1e-9);
}

TEST(CurveIntersectionTest, ParallelLinesDoNotCross)
{
    const LineCurved across({ 0.0, 0.0 }, { 1.0, 0.0 });
    const LineCurved above({ 0.0, 1.0 }, { 1.0, 0.0 });

    const auto result = intersectCurveWithCurve(across, above, kLineWindow, kLineWindow);

    EXPECT_EQ(result.contact, CurveCurveContact::None);
    EXPECT_TRUE(result.hits.empty());
}

TEST(CurveIntersectionTest, ALineCrossesACircleTwice)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const LineCurved   across({ 0.0, 0.0 }, { 1.0, 0.0 });

    const auto result = intersectCurveWithCurve(across, circle, kLineWindow);
    ASSERT_EQ(result.contact, CurveCurveContact::Isolated);
    ASSERT_EQ(result.hits.size(), 2u);

    // Hits ascend by the first curve's parameter, so the left crossing comes first.
    EXPECT_NEAR(result.hits[0].point.x, -2.0, 1e-9);
    EXPECT_NEAR(result.hits[0].point.y, 0.0, 1e-9);
    EXPECT_NEAR(result.hits[0].first_parameter, -2.0, 1e-9);
    EXPECT_NEAR(result.hits[1].point.x, 2.0, 1e-9);
    EXPECT_NEAR(result.hits[1].point.y, 0.0, 1e-9);

    // The second parameter is the circle's own angle.
    EXPECT_NEAR(std::abs(result.hits[0].second_parameter), vn::math::PI, 1e-6);
    EXPECT_NEAR(result.hits[1].second_parameter, 0.0, 1e-6);
}

TEST(CurveIntersectionTest, TwoCirclesCrossTwice)
{
    const CircleCurved left({ 0.0, 0.0 }, 2.0);
    const CircleCurved right({ 3.0, 0.0 }, 2.0);

    const auto result = intersectCurveWithCurve(left, right);
    ASSERT_EQ(result.contact, CurveCurveContact::Isolated);
    ASSERT_EQ(result.hits.size(), 2u);

    const double height = std::sqrt(4.0 - 2.25);
    EXPECT_NEAR(result.hits[0].point.x, 1.5, 1e-6);
    EXPECT_NEAR(std::abs(result.hits[0].point.y), height, 1e-6);
    EXPECT_NEAR(result.hits[1].point.x, 1.5, 1e-6);
    EXPECT_NEAR(std::abs(result.hits[1].point.y), height, 1e-6);
}

TEST(CurveIntersectionTest, AnArcIsClippedByItsOwnWindow)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(circle, 0.0, vn::math::PI_HALF);
    const LineCurved   across({ 0.0, 0.0 }, { 1.0, 0.0 });

    const auto result = intersectCurveWithCurve(quarter, across, std::nullopt, kLineWindow);
    ASSERT_EQ(result.contact, CurveCurveContact::Isolated);
    ASSERT_EQ(result.hits.size(), 1u);

    EXPECT_NEAR(result.hits[0].point.x, 2.0, 1e-9);
    EXPECT_NEAR(result.hits[0].point.y, 0.0, 1e-9);
}

TEST(CurveIntersectionTest, OverlappingArcsAreCoincident)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    first(circle, 0.0, vn::math::PI);
    const ArcCurved    second(circle, vn::math::PI_HALF, 3.0 * vn::math::PI_HALF);

    const auto result = intersectCurveWithCurve(first, second);

    EXPECT_EQ(result.contact, CurveCurveContact::Coincident);
    EXPECT_TRUE(result.hits.empty());
}

TEST(CurveIntersectionTest, IdenticalCirclesAreCoincident)
{
    const CircleCurved circle({ 1.0, 2.0 }, 2.0);

    const auto result = intersectCurveWithCurve(circle, circle);

    EXPECT_EQ(result.contact, CurveCurveContact::Coincident);
    EXPECT_TRUE(result.hits.empty());
}

TEST(CurveIntersectionTest, ACurveThatCannotBeSearchedAnswersNothing)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const LineCurved   unbounded({ 0.0, 0.0 }, { 1.0, 0.0 });
    const LineCurved   degenerate({ 0.0, 0.0 }, { 0.0, 0.0 });

    // An unbounded curve needs a window from the caller.
    EXPECT_EQ(intersectCurveWithCurve(unbounded, circle).contact, CurveCurveContact::None);

    EXPECT_EQ(intersectCurveWithCurve(degenerate, circle, kLineWindow).contact, CurveCurveContact::None);
}

TEST(CurveIntersectionTest, BothPrecisionsFindTheSameCrossings)
{
    const CircleCurvef circle({ 0.0f, 0.0f }, 2.0f);
    const LineCurvef   across({ 0.0f, 0.0f }, { 1.0f, 0.0f });

    const auto result =
        intersectCurveWithCurve(across, circle, std::optional<std::pair<float, float>>{ { -10.0f, 10.0f } });
    ASSERT_EQ(result.contact, CurveCurveContact::Isolated);
    ASSERT_EQ(result.hits.size(), 2u);

    EXPECT_NEAR(result.hits[0].point.x, -2.0f, 1e-4f);
    EXPECT_NEAR(result.hits[1].point.x, 2.0f, 1e-4f);
}

} // namespace
