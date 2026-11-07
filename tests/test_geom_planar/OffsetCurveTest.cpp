#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom/planar/BSplineCurve.hpp>
#include <vine/geom/planar/BezierCurve.hpp>
#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/CurveProjection.hpp>
#include <vine/geom/planar/EllipseCurve.hpp>
#include <vine/geom/planar/LineCurve.hpp>
#include <vine/geom/planar/OffsetCurve.hpp>
#include <vine/geom/planar/TrimmedCurve.hpp>

namespace
{

using vn::geom::planar::BSplineCurved;
using vn::geom::planar::BezierCurved;
using vn::geom::planar::CircleCurved;
using vn::geom::planar::EllipseCurved;
using vn::geom::planar::LineCurved;
using vn::geom::planar::OffsetCurve;
using vn::geom::planar::closestPointOnCurve;
using vn::geom::planar::offsetCurve;
using vn::geom::planar::trimCurve;

TEST(OffsetCurveTest, ALineOffsetsToItsLeft)
{
    const LineCurved line({ 0.0, 0.0 }, { 1.0, 0.0 });
    const auto       offset = offsetCurve(line, 1.0);

    // Left of +X is +Y.
    EXPECT_NEAR(offset.normalAt(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(offset.normalAt(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(offset.evaluate(2.0).x, 2.0, 1e-12);
    EXPECT_NEAR(offset.evaluate(2.0).y, 1.0, 1e-12);

    EXPECT_NEAR(offset.derivative(-3.0).x, 1.0, 1e-12);
    EXPECT_NEAR(offset.derivative(-3.0).y, 0.0, 1e-12);
    EXPECT_NEAR(offset.secondDerivative(5.0).x, 0.0, 1e-12);
    EXPECT_NEAR(offset.secondDerivative(5.0).y, 0.0, 1e-12);
}

TEST(OffsetCurveTest, ACounterClockwiseCircleOffsetsTowardsItsCentre)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         inner = offsetCurve(circle, 1.0);

    // The left normal of a counter-clockwise circle points inward, so a positive distance shrinks it.
    for (int index = 0; index <= 16; ++index) {
        const double u     = vn::math::PI_TWO * static_cast<double>(index) / 16.0;
        const auto   point = inner.evaluate(u);
        EXPECT_NEAR(std::hypot(point.x, point.y), 1.0, 1e-12);
    }

    // A circle of radius 1 has the acceleration -(point - centre).
    const auto acceleration = inner.secondDerivative(0.75);
    const auto point        = inner.evaluate(0.75);
    EXPECT_NEAR(acceleration.x, -point.x, 1e-12);
    EXPECT_NEAR(acceleration.y, -point.y, 1e-12);
    EXPECT_TRUE(inner.isClosed());
    EXPECT_TRUE(inner.isPeriodic());
    EXPECT_NEAR(inner.period(), vn::math::PI_TWO, 1e-12);
}

TEST(OffsetCurveTest, TheSecondDerivativeMatchesAFiniteDifferenceOfTheDerivative)
{
    // An ellipse has a varying curvature, so this exercises the whole normal-rate chain, not just a circle's.
    const EllipseCurved ellipse({ 1.0, -2.0 }, { 1.0, 1.0 }, 3.0, 2.0);
    const auto          offset = offsetCurve(ellipse, 0.4);
    const double        h      = 1e-5;

    for (const double u : { 0.3, 1.1, 2.4, 4.0, 5.6 }) {
        const auto symbolic = offset.secondDerivative(u);
        const auto before   = offset.derivative(u - h);
        const auto after    = offset.derivative(u + h);
        EXPECT_NEAR(symbolic.x, (after.x - before.x) / (2.0 * h), 1e-4);
        EXPECT_NEAR(symbolic.y, (after.y - before.y) / (2.0 * h), 1e-4);
    }
}

TEST(OffsetCurveTest, TheSplineCurvesAnswerAThirdDerivative)
{
    const BezierCurved  bezier({ { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } });
    const BSplineCurved spline(3, { { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } }, { 0.0, 1.0 }, { 4, 4 });
    const double        h = 1e-5;

    for (const double u : { 0.25, 0.5, 0.75 }) {
        const auto bezier_jerk = bezier.thirdDerivative(u);
        EXPECT_NEAR(bezier_jerk.x,
                    (bezier.secondDerivative(u + h).x - bezier.secondDerivative(u - h).x) / (2.0 * h), 1e-3);

        const auto spline_jerk = spline.thirdDerivative(u);
        EXPECT_NEAR(spline_jerk.y,
                    (spline.secondDerivative(u + h).y - spline.secondDerivative(u - h).y) / (2.0 * h), 1e-3);
    }
}

TEST(OffsetCurveTest, ASplineBasisOffsetsWithAnExactSecondDerivative)
{
    const BezierCurved bezier({ { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } });
    const auto         offset = offsetCurve(bezier, -0.5);
    const double       h      = 1e-5;

    for (const double u : { 0.2, 0.5, 0.8 }) {
        const auto symbolic = offset.secondDerivative(u);
        const auto before   = offset.derivative(u - h);
        const auto after    = offset.derivative(u + h);
        EXPECT_NEAR(symbolic.x, (after.x - before.x) / (2.0 * h), 1e-4);
        EXPECT_NEAR(symbolic.y, (after.y - before.y) / (2.0 * h), 1e-4);
    }
}

TEST(OffsetCurveTest, ReversingTheBasisNeedsTheNegatedDistanceToTraceTheSameCurve)
{
    const LineCurved forward({ 0.0, 0.0 }, { 1.0, 0.0 });
    const LineCurved backward({ 0.0, 0.0 }, { -1.0, 0.0 });
    const auto       left     = offsetCurve(forward, 1.0);
    const auto       reversed = offsetCurve(backward, -1.0);
    const double     u        = 2.0;

    // The same geometric line, walked the other way: y = 1 at the mirrored parameter.
    const auto from_forward  = left.evaluate(forward.reversedParameter(u));
    const auto from_backward = reversed.evaluate(u);
    EXPECT_NEAR(from_forward.x, from_backward.x, 1e-12);
    EXPECT_NEAR(from_forward.y, from_backward.y, 1e-12);

    EXPECT_NEAR(left.reversedParameter(u), forward.reversedParameter(u), 1e-12);
}

TEST(OffsetCurveTest, AZeroDistanceIsTheBasis)
{
    const auto arc  = trimCurve(CircleCurved({ 1.0, 2.0 }, 3.0), 0.0, vn::math::PI_HALF);
    const auto zero = offsetCurve(arc, 0.0);

    for (const double u : { 0.0, 0.5, vn::math::PI_HALF }) {
        EXPECT_NEAR(zero.evaluate(u).x, arc.evaluate(u).x, 1e-12);
        EXPECT_NEAR(zero.evaluate(u).y, arc.evaluate(u).y, 1e-12);
        EXPECT_NEAR(zero.derivative(u).x, arc.derivative(u).x, 1e-12);
        EXPECT_NEAR(zero.secondDerivative(u).x, arc.secondDerivative(u).x, 1e-12);
    }
}

TEST(OffsetCurveTest, ASingularBasisParameterKeepsTheAnswersFinite)
{
    // The first two poles coincide, so the velocity vanishes at u = 0 and the normal is undefined there.
    const BezierCurved cusp({ { 0.0, 0.0 }, { 0.0, 0.0 }, { 1.0, 2.0 } });
    const auto         offset = offsetCurve(cusp, 0.5);

    const auto point = offset.evaluate(0.0);
    EXPECT_DOUBLE_EQ(point.x, 0.0);
    EXPECT_DOUBLE_EQ(point.y, 0.0);
    EXPECT_DOUBLE_EQ(offset.derivative(0.0).x, 0.0);
    EXPECT_TRUE(std::isfinite(offset.secondDerivative(0.0).y));
}

TEST(OffsetCurveTest, TheFamilyAlgorithmsAcceptAnOffset)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         inner = offsetCurve(circle, 1.0);

    const auto projection = closestPointOnCurve(inner, vn::math::Point2<double>{ 0.0, 0.0 });
    ASSERT_TRUE(projection);
    EXPECT_NEAR(projection->distance, 1.0, 1e-6);
}

} // namespace
