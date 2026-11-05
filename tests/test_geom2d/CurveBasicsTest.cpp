#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include <vine/geom2d/CircleCurve.hpp>
#include <vine/geom2d/Curve.hpp>
#include <vine/geom2d/LineCurve.hpp>

namespace
{

using vn::geom2d::CircleCurved;
using vn::geom2d::CircleCurvef;
using vn::geom2d::Curve;
using vn::geom2d::LineCurved;

TEST(LineCurveTest, WalksAlongItsDirection)
{
    const LineCurved line({ 1.0, 2.0 }, { 2.0, 0.0 });

    EXPECT_DOUBLE_EQ(line.evaluate(3.0).x, 7.0);
    EXPECT_DOUBLE_EQ(line.evaluate(3.0).y, 2.0);
    EXPECT_DOUBLE_EQ(line.pointAt(-1.0).x, -1.0);
    EXPECT_DOUBLE_EQ(line.pointAt(-1.0).y, 2.0);
}

TEST(LineCurveTest, HasAConstantVelocity)
{
    const LineCurved line({ 0.0, 0.0 }, { 2.0, 3.0 });

    EXPECT_DOUBLE_EQ(line.derivative(-4.0).x, 2.0);
    EXPECT_DOUBLE_EQ(line.derivative(-4.0).y, 3.0);
    EXPECT_DOUBLE_EQ(line.secondDerivative(7.0).x, 0.0);
    EXPECT_DOUBLE_EQ(line.secondDerivative(7.0).y, 0.0);
}

TEST(LineCurveTest, IsUnboundedAndNeverClosed)
{
    const LineCurved line({ 0.0, 0.0 }, { 1.0, 0.0 });

    EXPECT_EQ(line.firstParameter(), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(line.lastParameter(), std::numeric_limits<double>::infinity());
    EXPECT_FALSE(line.isClosed());
    EXPECT_FALSE(line.isPeriodic());
    EXPECT_DOUBLE_EQ(line.period(), 0.0);
    EXPECT_EQ(line.continuity(), vn::math::Continuity::CN);
    EXPECT_TRUE(line.isCN(3));
}

TEST(LineCurveTest, AZeroDirectionIsNotALine)
{
    const LineCurved degenerate({ 4.0, 5.0 }, { 0.0, 0.0 });

    EXPECT_FALSE(degenerate.isValid());
    EXPECT_DOUBLE_EQ(degenerate.evaluate(2.0).x, 4.0);
    EXPECT_DOUBLE_EQ(degenerate.evaluate(2.0).y, 5.0);
}

TEST(LineCurveTest, ReversesByNegatingItsParameter)
{
    const LineCurved line({ 1.0, 1.0 }, { 1.0, 1.0 });

    EXPECT_DOUBLE_EQ(line.reversedParameter(2.5), -2.5);
    EXPECT_DOUBLE_EQ(line.reversedParameter(line.reversedParameter(2.5)), 2.5);
    EXPECT_DOUBLE_EQ(line.evaluate(line.reversedParameter(2.5)).x, line.evaluate(-2.5).x);
}

TEST(CircleCurveTest, WalksItsAngle)
{
    const CircleCurved circle({ 1.0, 1.0 }, 2.0);

    EXPECT_NEAR(circle.evaluate(0.0).x, 3.0, 1e-12);
    EXPECT_NEAR(circle.evaluate(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(circle.evaluate(vn::math::PI_HALF).x, 1.0, 1e-12);
    EXPECT_NEAR(circle.evaluate(vn::math::PI_HALF).y, 3.0, 1e-12);
    EXPECT_NEAR(circle.evaluate(vn::math::PI).x, -1.0, 1e-12);
    EXPECT_NEAR(circle.evaluate(vn::math::PI).y, 1.0, 1e-12);
}

TEST(CircleCurveTest, TurnsTowardsItsCentre)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);

    EXPECT_NEAR(circle.derivative(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(circle.derivative(0.0).y, 2.0, 1e-12);
    EXPECT_NEAR(circle.secondDerivative(0.0).x, -2.0, 1e-12);
    EXPECT_NEAR(circle.secondDerivative(0.0).y, 0.0, 1e-12);
}

TEST(CircleCurveTest, IsClosedPeriodicAndSmooth)
{
    const CircleCurved circle({ 0.0, 0.0 }, 1.0);

    EXPECT_DOUBLE_EQ(circle.firstParameter(), 0.0);
    EXPECT_NEAR(circle.lastParameter(), vn::math::PI_TWO, 1e-12);
    EXPECT_TRUE(circle.isClosed());
    EXPECT_TRUE(circle.isPeriodic());
    EXPECT_NEAR(circle.period(), vn::math::PI_TWO, 1e-12);
    EXPECT_EQ(circle.continuity(), vn::math::Continuity::CN);
    EXPECT_TRUE(circle.isCN(3));
    EXPECT_NEAR(circle.circumference(), vn::math::PI_TWO, 1e-12);
}

TEST(CircleCurveTest, ANonPositiveRadiusIsNotACircle)
{
    const CircleCurved degenerate({ 0.0, 0.0 }, 0.0);
    const CircleCurvef float_circle({ 0.0f, 0.0f }, 1.0f);

    EXPECT_FALSE(degenerate.isValid());
    EXPECT_TRUE(float_circle.isValid());
    EXPECT_NEAR(float_circle.circumference(), vn::math::PI_TWO, 1e-5);
}

TEST(CircleCurveTest, ReversesByReflectingInsideThePeriod)
{
    const CircleCurved circle({ 0.0, 0.0 }, 1.0);
    const double       u = 0.75;

    EXPECT_NEAR(circle.reversedParameter(u), vn::math::PI_TWO - u, 1e-12);
    EXPECT_NEAR(circle.reversedParameter(circle.reversedParameter(u)), u, 1e-12);
    EXPECT_NEAR(circle.evaluate(circle.reversedParameter(u)).x, circle.evaluate(u).x, 1e-12);
}

TEST(CurveTest, AnswersThroughTheBaseInterface)
{
    const LineCurved   line({ 0.0, 0.0 }, { 1.0, 0.0 });
    const CircleCurved circle({ 0.0, 0.0 }, 1.0);

    const Curve<double>& as_line   = line;
    const Curve<double>& as_circle = circle;

    EXPECT_DOUBLE_EQ(as_line.evaluate(2.0).x, 2.0);
    EXPECT_TRUE(as_line.isValid());
    EXPECT_FALSE(as_line.isClosed());
    EXPECT_TRUE(as_circle.isClosed());
    EXPECT_TRUE(as_circle.isCN(1));

    std::unique_ptr<const Curve<double>> owned = std::make_unique<CircleCurved>(circle);
    EXPECT_TRUE(owned->isPeriodic());
}

} // namespace
