#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Math.hpp>

#include <vine/geom2d/ArcCurve.hpp>
#include <vine/geom2d/CircleCurve.hpp>
#include <vine/geom2d/Curve.hpp>

namespace
{

using vn::geom2d::ArcCurved;
using vn::geom2d::ArcCurvef;
using vn::geom2d::CircleCurved;
using vn::geom2d::CircleCurvef;
using vn::geom2d::Curve;

TEST(ArcCurveTest, WalksTheCircleBetweenItsAngles)
{
    const CircleCurved quarter_circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(quarter_circle, 0.0, vn::math::PI_HALF);

    EXPECT_NEAR(quarter.evaluate(0.0).x, 2.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(0.0).y, 0.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(vn::math::PI_HALF).x, 0.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(vn::math::PI_HALF).y, 2.0, 1e-12);

    EXPECT_NEAR(quarter.derivative(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(quarter.derivative(0.0).y, 2.0, 1e-12);
    EXPECT_NEAR(quarter.secondDerivative(0.0).x, -2.0, 1e-12);
    EXPECT_NEAR(quarter.secondDerivative(0.0).y, 0.0, 1e-12);

    EXPECT_DOUBLE_EQ(quarter.firstParameter(), 0.0);
    EXPECT_NEAR(quarter.lastParameter(), vn::math::PI_HALF, 1e-12);
}

TEST(ArcCurveTest, AQuarterTurnIsAPieceWithTwoEnds)
{
    const CircleCurved quarter_circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(quarter_circle, 0.0, vn::math::PI_HALF);

    EXPECT_FALSE(quarter.isClosed());
    EXPECT_FALSE(quarter.isPeriodic());
    EXPECT_DOUBLE_EQ(quarter.period(), 0.0);
    EXPECT_EQ(quarter.continuity(), vn::math::Continuity::CN);
    EXPECT_TRUE(quarter.isValid());
}

TEST(ArcCurveTest, AFullTurnIsClosedAndPeriodic)
{
    const CircleCurved full_circle({ 1.0, 1.0 }, 2.0);
    const ArcCurved    full_turn(full_circle, 0.0, vn::math::PI_TWO);

    EXPECT_TRUE(full_turn.isClosed());
    EXPECT_TRUE(full_turn.isPeriodic());
    EXPECT_NEAR(full_turn.period(), vn::math::PI_TWO, 1e-12);
    EXPECT_NEAR(full_turn.evaluate(vn::math::PI_TWO).x, full_turn.evaluate(0.0).x, 1e-12);
    EXPECT_NEAR(full_turn.evaluate(vn::math::PI_TWO).y, full_turn.evaluate(0.0).y, 1e-12);
}

TEST(ArcCurveTest, ReversesInsideItsOwnInterval)
{
    const CircleCurved quarter_circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(quarter_circle, 0.0, vn::math::PI_HALF);
    const double       u = 0.75;

    EXPECT_NEAR(quarter.reversedParameter(u), vn::math::PI_HALF - u, 1e-12);
    EXPECT_NEAR(quarter.reversedParameter(quarter.reversedParameter(u)), u, 1e-12);
    EXPECT_NEAR(quarter.reversedParameter(quarter.firstParameter()), quarter.lastParameter(), 1e-12);
    EXPECT_NEAR(quarter.reversedParameter(quarter.lastParameter()), quarter.firstParameter(), 1e-12);

    // Reversal walks the piece the other way, so the point moves to its mirror about the middle of the interval.
    EXPECT_NEAR(quarter.evaluate(quarter.reversedParameter(u)).x, quarter.evaluate(u).y, 1e-12);
    EXPECT_NEAR(quarter.evaluate(quarter.reversedParameter(u)).y, quarter.evaluate(u).x, 1e-12);
}

TEST(ArcCurveTest, AReversedIntervalIsNotAnArc)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    reversed(circle, vn::math::PI, 0.0);

    EXPECT_FALSE(reversed.isValid());
    EXPECT_NEAR(reversed.evaluate(0.0).x, 2.0, 1e-12);
    EXPECT_NEAR(reversed.evaluate(0.0).y, 0.0, 1e-12);
}

TEST(ArcCurveTest, ADegenerateCircleIsNotAnArc)
{
    const CircleCurved degenerate_circle({ 0.0, 0.0 }, 0.0);
    const ArcCurved    arc(degenerate_circle, 0.0, vn::math::PI_HALF);

    EXPECT_FALSE(arc.isValid());
}

TEST(ArcCurveTest, AnswersOutsideTheArcWithTheCircle)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(circle, 0.0, vn::math::PI_HALF);
    const double       outside = -vn::math::PI_QUARTER;

    EXPECT_NEAR(std::hypot(quarter.evaluate(outside).x, quarter.evaluate(outside).y), 2.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(outside).x, circle.pointAt(outside).x, 1e-12);
}

TEST(ArcCurveTest, AnswersThroughTheBaseInterface)
{
    const CircleCurvef quarter_circle({ 0.0f, 0.0f }, 2.0f);
    const ArcCurvef    quarter(quarter_circle, 0.0f, vn::math::PI_HALF);
    const Curve<float>& as_curve = quarter;

    EXPECT_TRUE(as_curve.isValid());
    EXPECT_FALSE(as_curve.isClosed());
    EXPECT_NEAR(as_curve.evaluate(vn::math::PI_HALF).y, 2.0f, 1e-5f);
}

} // namespace
