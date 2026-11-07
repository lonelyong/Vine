#include <gtest/gtest.h>

#include <vine/math/Math.hpp>

#include <vine/geom/planar/Curve.hpp>
#include <vine/geom/planar/EllipseCurve.hpp>

namespace
{

using vn::geom::planar::Curve;
using vn::geom::planar::EllipseCurved;
using vn::geom::planar::EllipseCurvef;

TEST(EllipseCurveTest, WalksItsEccentricAngle)
{
    const EllipseCurved ellipse({ 1.0, 2.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    EXPECT_NEAR(ellipse.evaluate(0.0).x, 3.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(0.0).y, 2.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(vn::math::PI_HALF).x, 1.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(vn::math::PI_HALF).y, 3.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(vn::math::PI).x, -1.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(vn::math::PI).y, 2.0, 1e-12);
    EXPECT_NEAR(ellipse.pointAt(vn::math::PI_HALF).y, ellipse.evaluate(vn::math::PI_HALF).y, 1e-12);
}

TEST(EllipseCurveTest, TurnsTowardsItsCentre)
{
    const EllipseCurved ellipse({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    EXPECT_NEAR(ellipse.derivative(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(ellipse.derivative(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(ellipse.derivative(vn::math::PI_HALF).x, -2.0, 1e-12);
    EXPECT_NEAR(ellipse.derivative(vn::math::PI_HALF).y, 0.0, 1e-12);

    const auto acceleration = ellipse.secondDerivative(0.0);
    EXPECT_NEAR(acceleration.x, -2.0, 1e-12);
    EXPECT_NEAR(acceleration.y, 0.0, 1e-12);
}

TEST(EllipseCurveTest, RotatesWithItsMajorDirection)
{
    const EllipseCurved ellipse({ 0.0, 0.0 }, { 0.0, 5.0 }, 2.0, 1.0);

    EXPECT_NEAR(ellipse.x_direction.x, 0.0, 1e-12);
    EXPECT_NEAR(ellipse.x_direction.y, 1.0, 1e-12);
    EXPECT_NEAR(ellipse.yAxis().x, -1.0, 1e-12);
    EXPECT_NEAR(ellipse.yAxis().y, 0.0, 1e-12);

    EXPECT_NEAR(ellipse.evaluate(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(0.0).y, 2.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(vn::math::PI_HALF).x, -1.0, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(vn::math::PI_HALF).y, 0.0, 1e-12);
}

TEST(EllipseCurveTest, IsClosedPeriodicAndSmooth)
{
    const EllipseCurved ellipse({ 3.0, 4.0 }, { 1.0, 0.0 }, 2.0, 3.0);

    EXPECT_DOUBLE_EQ(ellipse.firstParameter(), 0.0);
    EXPECT_NEAR(ellipse.lastParameter(), vn::math::PI_TWO, 1e-12);
    EXPECT_TRUE(ellipse.isClosed());
    EXPECT_TRUE(ellipse.isPeriodic());
    EXPECT_NEAR(ellipse.period(), vn::math::PI_TWO, 1e-12);
    EXPECT_EQ(ellipse.continuity(), vn::geom::Continuity::CN);
    EXPECT_TRUE(ellipse.isCN(3));
}

TEST(EllipseCurveTest, AZeroAxisIsNotAnEllipse)
{
    const EllipseCurved no_direction({ 4.0, 5.0 }, { 0.0, 0.0 }, 2.0, 1.0);
    const EllipseCurved no_minor_axis({ 4.0, 5.0 }, { 1.0, 0.0 }, 2.0, 0.0);
    const EllipseCurvef float_ellipse({ 0.0f, 0.0f }, { 1.0f, 0.0f }, 2.0f, 1.0f);

    EXPECT_FALSE(no_direction.isValid());
    EXPECT_FALSE(no_minor_axis.isValid());
    EXPECT_TRUE(float_ellipse.isValid());
    EXPECT_NEAR(float_ellipse.evaluate(0.0f).x, 2.0f, 1e-5f);

    EXPECT_DOUBLE_EQ(no_direction.evaluate(0.0).x, 4.0);
    EXPECT_DOUBLE_EQ(no_direction.evaluate(0.0).y, 5.0);
}

TEST(EllipseCurveTest, ReversesByReflectingInsideThePeriod)
{
    const EllipseCurved ellipse({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);
    const double       u = 0.75;

    EXPECT_NEAR(ellipse.reversedParameter(u), vn::math::PI_TWO - u, 1e-12);
    EXPECT_NEAR(ellipse.reversedParameter(ellipse.reversedParameter(u)), u, 1e-12);
    EXPECT_NEAR(ellipse.reversedParameter(ellipse.firstParameter()), ellipse.lastParameter(), 1e-12);
    EXPECT_NEAR(ellipse.reversedParameter(ellipse.lastParameter()), ellipse.firstParameter(), 1e-12);

    // Reversal walks the ellipse the other way, so the point moves to its mirror about the major axis.
    EXPECT_NEAR(ellipse.evaluate(ellipse.reversedParameter(u)).x, ellipse.evaluate(u).x, 1e-12);
    EXPECT_NEAR(ellipse.evaluate(ellipse.reversedParameter(u)).y, -ellipse.evaluate(u).y, 1e-12);
}

TEST(EllipseCurveTest, AnswersThroughTheBaseInterface)
{
    const EllipseCurved  ellipse({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);
    const Curve<double>& as_curve = ellipse;

    EXPECT_NEAR(as_curve.evaluate(vn::math::PI_HALF).y, 1.0, 1e-12);
    EXPECT_TRUE(as_curve.isValid());
    EXPECT_TRUE(as_curve.isClosed());
    EXPECT_TRUE(as_curve.isCN(2));
}

} // namespace
