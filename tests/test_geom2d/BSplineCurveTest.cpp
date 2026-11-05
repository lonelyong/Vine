#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom2d/BSplineCurve.hpp>
#include <vine/geom2d/CurveCurveIntersection.hpp>
#include <vine/geom2d/CurveProjection.hpp>
#include <vine/geom2d/LineCurve.hpp>

namespace
{

using vn::geom2d::BSplineCurved;
using vn::geom2d::BSplineCurvef;
using vn::geom2d::CurveCurveContact;
using vn::geom2d::LineCurved;
using vn::geom2d::closestPointOnCurve;
using vn::geom2d::intersectCurveWithCurve;

TEST(BSplineCurveTest, AClampedQuadraticIsItsBezier)
{
    const BSplineCurved spline(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 });

    EXPECT_TRUE(spline.isValid());
    EXPECT_FALSE(spline.isRational());
    EXPECT_NEAR(spline.evaluate(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(spline.evaluate(0.5).x, 1.0, 1e-12);
    EXPECT_NEAR(spline.evaluate(0.5).y, 1.0, 1e-12);
    EXPECT_NEAR(spline.evaluate(1.0).x, 2.0, 1e-12);

    // The same numbers the Bezier test pins, because a clamped knot vector with no interior knot is a Bezier.
    EXPECT_NEAR(spline.derivative(0.0).x, 2.0, 1e-12);
    EXPECT_NEAR(spline.derivative(0.0).y, 4.0, 1e-12);
    EXPECT_NEAR(spline.secondDerivative(0.25).y, -8.0, 1e-12);
}

TEST(BSplineCurveTest, FollowsItsKnots)
{
    const BSplineCurved polyline(1, { { 0.0, 0.0 }, { 2.0, 0.0 }, { 2.0, 2.0 } }, { 0.0, 1.0, 2.0 }, { 2, 1, 2 });

    EXPECT_TRUE(polyline.isValid());
    EXPECT_NEAR(polyline.evaluate(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(polyline.evaluate(0.5).x, 1.0, 1e-12);
    EXPECT_NEAR(polyline.evaluate(1.0).x, 2.0, 1e-12);
    EXPECT_NEAR(polyline.evaluate(1.0).y, 0.0, 1e-12);
    EXPECT_NEAR(polyline.evaluate(1.5).y, 1.0, 1e-12);
    EXPECT_NEAR(polyline.evaluate(2.0).y, 2.0, 1e-12);

    // Piecewise linear: the velocity is constant along each span and jumps at the join.
    EXPECT_NEAR(polyline.derivative(0.5).x, 2.0, 1e-12);
    EXPECT_NEAR(polyline.derivative(0.5).y, 0.0, 1e-12);
    EXPECT_NEAR(polyline.derivative(1.5).x, 0.0, 1e-12);
    EXPECT_NEAR(polyline.derivative(1.5).y, 2.0, 1e-12);
}

TEST(BSplineCurveTest, ContinuityComesFromTheMultiplicities)
{
    // Interior multiplicity 2 at degree 2 leaves a corner: C0. Multiplicity 1 leaves C1.
    const BSplineCurved corner(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, -1.0 }, { 3.0, 1.0 }, { 4.0, 0.0 } },
                               { 0.0, 1.0, 2.0 }, { 3, 2, 3 });
    const BSplineCurved smooth(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, -1.0 }, { 3.0, 0.0 } }, { 0.0, 1.0, 2.0 },
                               { 3, 1, 3 });

    EXPECT_TRUE(corner.isValid());
    EXPECT_TRUE(smooth.isValid());
    EXPECT_EQ(corner.continuity(), vn::math::Continuity::C0);
    EXPECT_EQ(smooth.continuity(), vn::math::Continuity::C1);
    EXPECT_FALSE(corner.isCN(1));
    EXPECT_TRUE(smooth.isCN(1));
}

TEST(BSplineCurveTest, ARationalQuadraticIsAQuarterCircle)
{
    const double       half_root_two = std::sqrt(0.5);
    const BSplineCurved quarter(2, { { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 } }, { 0.0, 1.0 }, { 3, 3 },
                                { 1.0, half_root_two, 1.0 });

    EXPECT_TRUE(quarter.isRational());
    EXPECT_TRUE(quarter.isValid());

    for (int index = 0; index <= 16; ++index) {
        const auto point = quarter.evaluate(static_cast<double>(index) / 16.0);
        EXPECT_NEAR(std::hypot(point.x, point.y), 1.0, 1e-11);
    }
}

TEST(BSplineCurveTest, ParametersOutsideTheDomainClampToTheEnds)
{
    const BSplineCurved spline(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 });

    EXPECT_NEAR(spline.evaluate(-5.0).x, spline.evaluate(0.0).x, 1e-12);
    EXPECT_NEAR(spline.evaluate(9.0).y, spline.evaluate(1.0).y, 1e-12);
    EXPECT_DOUBLE_EQ(spline.firstParameter(), 0.0);
    EXPECT_DOUBLE_EQ(spline.lastParameter(), 1.0);
}

TEST(BSplineCurveTest, AnInconsistentKnotVectorIsNotUsable)
{
    const BSplineCurved empty;
    // Three poles at degree 2 need a flat knot vector of length 6: 3 + 3 + 1 = 7 is wrong.
    const BSplineCurved wrong_size(2, { { 0.0, 0.0 }, { 1.0, 0.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 2, 2 });
    // An interior multiplicity of degree + 1 breaks the curve in two.
    const BSplineCurved broken(2, { { 0.0, 0.0 }, { 1.0, 1.0 }, { 2.0, 0.0 }, { 3.0, 1.0 } }, { 0.0, 1.0, 2.0 },
                               { 3, 3, 3 });
    const BSplineCurved zero_weight(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 },
                                    { 1.0, 0.0, 1.0 });

    EXPECT_FALSE(empty.isValid());
    EXPECT_FALSE(wrong_size.isValid());
    EXPECT_FALSE(broken.isValid());
    EXPECT_FALSE(zero_weight.isValid());

    // The degenerate answers stay finite: the origin without poles, the first pole otherwise.
    EXPECT_DOUBLE_EQ(empty.evaluate(0.5).x, 0.0);
    EXPECT_DOUBLE_EQ(wrong_size.evaluate(0.5).x, 0.0);
    EXPECT_DOUBLE_EQ(wrong_size.derivative(0.5).x, 0.0);
    EXPECT_DOUBLE_EQ(wrong_size.secondDerivative(0.5).y, 0.0);
}

TEST(BSplineCurveTest, IsClosedWhenItsEndsMeet)
{
    const BSplineCurved open(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 });
    const BSplineCurved closed(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 0.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 });

    EXPECT_FALSE(open.isClosed());
    EXPECT_TRUE(closed.isClosed());
    EXPECT_FALSE(open.isPeriodic());
    EXPECT_DOUBLE_EQ(open.period(), 0.0);
}

TEST(BSplineCurveTest, ReversesInsideItsKnotDomain)
{
    const BSplineCurved spline(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 });

    EXPECT_NEAR(spline.reversedParameter(0.25), 0.75, 1e-12);
    EXPECT_NEAR(spline.reversedParameter(spline.reversedParameter(0.25)), 0.25, 1e-12);
    EXPECT_NEAR(spline.reversedParameter(spline.firstParameter()), spline.lastParameter(), 1e-12);
}

TEST(BSplineCurveTest, BoundsAreThePoleBox)
{
    const BSplineCurved spline(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, -1.0 } }, { 0.0, 1.0 }, { 3, 3 });
    const auto         box = spline.bounds();

    EXPECT_NEAR(box.xmin, 0.0, 1e-12);
    EXPECT_NEAR(box.ymin, -1.0, 1e-12);
    EXPECT_NEAR(box.xmax, 2.0, 1e-12);
    EXPECT_NEAR(box.ymax, 2.0, 1e-12);
}

TEST(BSplineCurveTest, TheFamilyAlgorithmsWorkWithoutKnowingTheType)
{
    const BSplineCurved spline(2, { { 0.0, 0.0 }, { 1.0, 2.0 }, { 2.0, 0.0 } }, { 0.0, 1.0 }, { 3, 3 });

    const auto projection = closestPointOnCurve(spline, vn::math::Point2<double>{ 1.0, 3.0 });
    ASSERT_TRUE(projection);
    EXPECT_NEAR(projection->point.y, 1.0, 1e-6);
    EXPECT_NEAR(projection->distance, 2.0, 1e-6);

    const LineCurved across({ 0.0, 0.0 }, { 1.0, 0.0 });
    const auto       crossings = intersectCurveWithCurve(
        spline, across, std::optional<std::pair<double, double>>{ { 0.0, 1.0 } },
        std::optional<std::pair<double, double>>{ { -10.0, 10.0 } });
    ASSERT_EQ(crossings.contact, CurveCurveContact::Isolated);
    EXPECT_EQ(crossings.hits.size(), 2u);
}

TEST(BSplineCurveTest, BothPrecisionsWalkTheSameCurve)
{
    const BSplineCurvef narrow(2, { { 0.0f, 0.0f }, { 1.0f, 2.0f }, { 2.0f, 0.0f } }, { 0.0f, 1.0f }, { 3, 3 });

    EXPECT_TRUE(narrow.isValid());
    EXPECT_NEAR(narrow.evaluate(0.5f).x, 1.0f, 1e-6f);
    EXPECT_NEAR(narrow.evaluate(0.5f).y, 1.0f, 1e-6f);
}

} // namespace
