#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom2d/BezierCurve.hpp>
#include <vine/geom2d/CurveCurveIntersection.hpp>
#include <vine/geom2d/CurveProjection.hpp>
#include <vine/geom2d/LineCurve.hpp>

namespace
{

using vn::geom2d::BezierCurved;
using vn::geom2d::BezierCurvef;
using vn::geom2d::CurveCurveContact;
using vn::geom2d::LineCurved;
using vn::geom2d::closestPointOnCurve;
using vn::geom2d::intersectCurveWithCurve;

TEST(BezierCurveTest, WalksFromItsFirstPoleToItsLast)
{
    const BezierCurved arc({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } });

    EXPECT_EQ(arc.degree(), 2);
    EXPECT_FALSE(arc.isRational());

    EXPECT_NEAR(arc.evaluate(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(arc.evaluate(0.0).y, 0.0, 1e-12);
    EXPECT_NEAR(arc.evaluate(0.5).x, 1.0, 1e-12);
    EXPECT_NEAR(arc.evaluate(0.5).y, 1.0, 1e-12);
    EXPECT_NEAR(arc.evaluate(1.0).x, 2.0, 1e-12);
    EXPECT_NEAR(arc.evaluate(1.0).y, 0.0, 1e-12);
}

TEST(BezierCurveTest, DifferentiatesIntoItsDifferences)
{
    const BezierCurved arc({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } });

    // B'(0) = 2 * (P1 - P0) = (2, 4); B'(1/2) = (2, 0); B'' is 2 * (P2 - 2 P1 + P0) = (0, -8).
    EXPECT_NEAR(arc.derivative(0.0).x, 2.0, 1e-12);
    EXPECT_NEAR(arc.derivative(0.0).y, 4.0, 1e-12);
    EXPECT_NEAR(arc.derivative(0.5).x, 2.0, 1e-12);
    EXPECT_NEAR(arc.derivative(0.5).y, 0.0, 1e-12);
    EXPECT_NEAR(arc.secondDerivative(0.25).x, 0.0, 1e-12);
    EXPECT_NEAR(arc.secondDerivative(0.25).y, -8.0, 1e-12);
}

TEST(BezierCurveTest, ARationalQuadraticIsAQuarterCircle)
{
    const double       half_root_two = std::sqrt(0.5);
    const BezierCurved quarter({ { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 } },
                               { 1.0, half_root_two, 1.0 });

    EXPECT_TRUE(quarter.isRational());
    EXPECT_TRUE(quarter.isValid());

    EXPECT_NEAR(quarter.evaluate(0.0).x, 1.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(0.0).y, 0.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(1.0).x, 0.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(1.0).y, 1.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(0.5).x, std::sqrt(0.5), 1e-12);
    EXPECT_NEAR(quarter.evaluate(0.5).y, std::sqrt(0.5), 1e-12);

    // Every point sits on the unit circle, which is what the weights are for.
    for (int index = 0; index <= 16; ++index) {
        const auto point = quarter.evaluate(static_cast<double>(index) / 16.0);
        EXPECT_NEAR(std::hypot(point.x, point.y), 1.0, 1e-12);
    }
}

TEST(BezierCurveTest, ACurveWithoutTwoPolesIsNotUsable)
{
    const BezierCurved empty;
    const BezierCurved single({ { 4.0, 5.0 } });
    const BezierCurved mismatched_weights({ { 0.0, 0.0 }, { 1.0, 0.0 } }, { 1.0 });
    const BezierCurved zero_weight({ { 0.0, 0.0 }, { 1.0, 0.0 } }, { 1.0, 0.0 });

    EXPECT_FALSE(empty.isValid());
    EXPECT_FALSE(single.isValid());
    EXPECT_FALSE(mismatched_weights.isValid());
    EXPECT_FALSE(zero_weight.isValid());

    // The degenerate answers are finite: the origin for no poles, the only pole otherwise.
    EXPECT_DOUBLE_EQ(empty.evaluate(0.5).x, 0.0);
    EXPECT_DOUBLE_EQ(empty.evaluate(0.5).y, 0.0);
    EXPECT_DOUBLE_EQ(single.evaluate(0.5).x, 4.0);
    EXPECT_DOUBLE_EQ(single.evaluate(0.5).y, 5.0);
    EXPECT_DOUBLE_EQ(single.derivative(0.5).x, 0.0);
    EXPECT_DOUBLE_EQ(single.secondDerivative(0.5).y, 0.0);
}

TEST(BezierCurveTest, IsClosedWhenItsEndPolesMeet)
{
    const BezierCurved open({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } });
    const BezierCurved closed({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 0.0, 0.0 } });

    EXPECT_FALSE(open.isClosed());
    EXPECT_TRUE(closed.isClosed());
    EXPECT_FALSE(open.isPeriodic());
    EXPECT_DOUBLE_EQ(open.period(), 0.0);
    EXPECT_EQ(open.continuity(), vn::math::Continuity::CN);
}

TEST(BezierCurveTest, ReversesByMirroringTheParameter)
{
    const BezierCurved arc({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } });

    EXPECT_NEAR(arc.reversedParameter(0.25), 0.75, 1e-12);
    EXPECT_NEAR(arc.reversedParameter(arc.reversedParameter(0.25)), 0.25, 1e-12);
    EXPECT_NEAR(arc.reversedParameter(arc.firstParameter()), arc.lastParameter(), 1e-12);

    // The reversed curve is this one with its poles reversed: same x, mirrored y about the chord.
    EXPECT_NEAR(arc.evaluate(arc.reversedParameter(0.25)).x, arc.evaluate(0.75).x, 1e-12);
}

TEST(BezierCurveTest, BoundsAreThePoleBox)
{
    const BezierCurved arc({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, -1.0 } });
    const auto         box = arc.bounds();

    EXPECT_NEAR(box.xmin, 0.0, 1e-12);
    EXPECT_NEAR(box.ymin, -1.0, 1e-12);
    EXPECT_NEAR(box.xmax, 2.0, 1e-12);
    EXPECT_NEAR(box.ymax, 2.0, 1e-12);
}

TEST(BezierCurveTest, TheFamilyAlgorithmsWorkWithoutKnowingTheType)
{
    const BezierCurved arc({ { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } });

    // Nearest point of (1, 3): the apex at u = 1/2, two units away.
    const auto projection = closestPointOnCurve(arc, vn::math::Point2<double>{ 1.0, 3.0 });
    ASSERT_TRUE(projection);
    EXPECT_NEAR(projection->point.x, 1.0, 1e-6);
    EXPECT_NEAR(projection->point.y, 1.0, 1e-6);
    EXPECT_NEAR(projection->distance, 2.0, 1e-6);

    // The x axis meets the curve at its two end points and nowhere else.
    const LineCurved across({ 0.0, 0.0 }, { 1.0, 0.0 });
    const auto       crossings = intersectCurveWithCurve(
        arc, across, std::optional<std::pair<double, double>>{ { 0.0, 1.0 } },
        std::optional<std::pair<double, double>>{ { -10.0, 10.0 } });
    ASSERT_EQ(crossings.contact, CurveCurveContact::Isolated);
    EXPECT_EQ(crossings.hits.size(), 2u);
}

TEST(BezierCurveTest, BothPrecisionsWalkTheSameCurve)
{
    const BezierCurvef narrow({ { 0.0f, 0.0f }, { 1.0f, 2.0f }, { 2.0f, 0.0f } });

    EXPECT_TRUE(narrow.isValid());
    EXPECT_NEAR(narrow.evaluate(0.5f).x, 1.0f, 1e-6f);
    EXPECT_NEAR(narrow.evaluate(0.5f).y, 1.0f, 1e-6f);
}

} // namespace
