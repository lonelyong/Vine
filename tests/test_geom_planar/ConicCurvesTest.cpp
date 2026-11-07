#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <vine/math/Math.hpp>

#include <vine/geom/planar/ConicCurve.hpp>
#include <vine/geom/planar/Curve.hpp>
#include <vine/geom/planar/EllipseCurve.hpp>
#include <vine/geom/planar/HyperbolaCurve.hpp>
#include <vine/geom/planar/ParabolaCurve.hpp>

namespace
{

using vn::geom::planar::ConicCurve;
using vn::geom::planar::Curve;
using vn::geom::planar::EllipseCurved;
using vn::geom::planar::HyperbolaCurved;
using vn::geom::planar::HyperbolaCurvef;
using vn::geom::planar::ParabolaCurved;
using vn::geom::planar::ParabolaCurvef;

TEST(HyperbolaCurveTest, WalksTheBranchAlongItsTransverseAxis)
{
    const HyperbolaCurved branch({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    EXPECT_NEAR(branch.evaluate(0.0).x, 2.0, 1e-12);
    EXPECT_NEAR(branch.evaluate(0.0).y, 0.0, 1e-12);
    EXPECT_NEAR(branch.evaluate(1.0).x, 2.0 * std::cosh(1.0), 1e-12);
    EXPECT_NEAR(branch.evaluate(1.0).y, std::sinh(1.0), 1e-12);
    EXPECT_NEAR(branch.pointAt(-1.0).x, branch.evaluate(1.0).x, 1e-12);
}

TEST(HyperbolaCurveTest, AcceleratesAwayFromItsCentre)
{
    const HyperbolaCurved branch({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    EXPECT_NEAR(branch.derivative(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(branch.derivative(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(branch.secondDerivative(0.0).x, 2.0, 1e-12);
    EXPECT_NEAR(branch.secondDerivative(0.0).y, 0.0, 1e-12);
}

TEST(HyperbolaCurveTest, IsUnboundedAndSmooth)
{
    const HyperbolaCurved branch({ 1.0, 1.0 }, { 0.0, 1.0 }, 2.0, 1.0);

    EXPECT_EQ(branch.firstParameter(), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(branch.lastParameter(), std::numeric_limits<double>::infinity());
    EXPECT_FALSE(branch.isClosed());
    EXPECT_FALSE(branch.isPeriodic());
    EXPECT_DOUBLE_EQ(branch.period(), 0.0);
    EXPECT_EQ(branch.continuity(), vn::geom::Continuity::CN);
    EXPECT_TRUE(branch.isCN(2));

    // The transverse axis is stored normalised, so the branch starts along +Y here.
    EXPECT_NEAR(branch.evaluate(0.0).x, 1.0, 1e-12);
    EXPECT_NEAR(branch.evaluate(0.0).y, 3.0, 1e-12);
}

TEST(HyperbolaCurveTest, AUsableBranchNeedsBothAxesAndADirection)
{
    const HyperbolaCurved no_direction({ 0.0, 0.0 }, { 0.0, 0.0 }, 2.0, 1.0);
    const HyperbolaCurved no_minor_axis({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 0.0);
    const HyperbolaCurvef float_branch({ 0.0f, 0.0f }, { 1.0f, 0.0f }, 2.0f, 1.0f);

    EXPECT_FALSE(no_direction.isValid());
    EXPECT_FALSE(no_minor_axis.isValid());
    EXPECT_TRUE(float_branch.isValid());
    EXPECT_NEAR(float_branch.evaluate(0.0f).x, 2.0f, 1e-5f);

    EXPECT_NEAR(no_direction.evaluate(1.0).x, 0.0, 1e-12);
    EXPECT_NEAR(no_direction.evaluate(1.0).y, 0.0, 1e-12);
}

TEST(HyperbolaCurveTest, ReversesByNegatingItsParameter)
{
    const HyperbolaCurved branch({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);

    EXPECT_DOUBLE_EQ(branch.reversedParameter(1.5), -1.5);
    EXPECT_DOUBLE_EQ(branch.reversedParameter(branch.reversedParameter(1.5)), 1.5);
}

TEST(ParabolaCurveTest, WalksFromItsVertex)
{
    const ParabolaCurved parabola({ 0.0, 0.0 }, { 1.0, 0.0 }, 1.0);

    EXPECT_NEAR(parabola.evaluate(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(parabola.evaluate(0.0).y, 0.0, 1e-12);
    EXPECT_NEAR(parabola.evaluate(2.0).x, 1.0, 1e-12);
    EXPECT_NEAR(parabola.evaluate(2.0).y, 2.0, 1e-12);
    EXPECT_NEAR(parabola.evaluate(-2.0).x, parabola.evaluate(2.0).x, 1e-12);
    EXPECT_NEAR(parabola.evaluate(-2.0).y, -parabola.evaluate(2.0).y, 1e-12);
}

TEST(ParabolaCurveTest, HasAConstantAcceleration)
{
    const ParabolaCurved parabola({ 0.0, 0.0 }, { 1.0, 0.0 }, 1.0);

    EXPECT_NEAR(parabola.derivative(0.0).x, 0.0, 1e-12);
    EXPECT_NEAR(parabola.derivative(0.0).y, 1.0, 1e-12);
    EXPECT_NEAR(parabola.derivative(2.0).x, 1.0, 1e-12);
    EXPECT_NEAR(parabola.derivative(2.0).y, 1.0, 1e-12);
    EXPECT_NEAR(parabola.secondDerivative(-7.0).x, 0.5, 1e-12);
    EXPECT_NEAR(parabola.secondDerivative(-7.0).y, 0.0, 1e-12);
}

TEST(ParabolaCurveTest, IsUnboundedAndSmooth)
{
    const ParabolaCurved parabola({ 0.0, 0.0 }, { 0.0, 1.0 }, 1.0);

    EXPECT_EQ(parabola.firstParameter(), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(parabola.lastParameter(), std::numeric_limits<double>::infinity());
    EXPECT_FALSE(parabola.isClosed());
    EXPECT_FALSE(parabola.isPeriodic());
    EXPECT_DOUBLE_EQ(parabola.period(), 0.0);
    EXPECT_EQ(parabola.continuity(), vn::geom::Continuity::CN);

    // The axis is stored normalised: with +Y as the axis, the curve opens along +Y and bends towards -X.
    EXPECT_NEAR(parabola.evaluate(2.0).x, -2.0, 1e-12);
    EXPECT_NEAR(parabola.evaluate(2.0).y, 1.0, 1e-12);
}

TEST(ParabolaCurveTest, AUsableParabolaNeedsAFocalLengthAndADirection)
{
    const ParabolaCurved no_focal_length({ 0.0, 0.0 }, { 1.0, 0.0 }, 0.0);
    const ParabolaCurved no_direction({ 0.0, 0.0 }, { 0.0, 0.0 }, 1.0);
    const ParabolaCurvef float_parabola({ 0.0f, 0.0f }, { 1.0f, 0.0f }, 1.0f);

    EXPECT_FALSE(no_focal_length.isValid());
    EXPECT_FALSE(no_direction.isValid());
    EXPECT_TRUE(float_parabola.isValid());
    EXPECT_NEAR(float_parabola.evaluate(2.0f).x, 1.0f, 1e-5f);

    EXPECT_NEAR(no_direction.evaluate(2.0).x, 0.0, 1e-12);
    EXPECT_NEAR(no_direction.evaluate(2.0).y, 0.0, 1e-12);
}

TEST(ParabolaCurveTest, ReversesByNegatingItsParameter)
{
    const ParabolaCurved parabola({ 0.0, 0.0 }, { 1.0, 0.0 }, 1.0);

    EXPECT_DOUBLE_EQ(parabola.reversedParameter(0.75), -0.75);
    EXPECT_DOUBLE_EQ(parabola.reversedParameter(parabola.reversedParameter(0.75)), 0.75);
}

TEST(ParabolaCurveTest, AZeroFocalLengthAnswersTheVertexInsteadOfANaN)
{
    const ParabolaCurved degenerate({ 1.0, 2.0 }, { 1.0, 0.0 }, 0.0);

    EXPECT_FALSE(degenerate.isValid());

    // u = 0 would divide zero by zero; every parameter answers the vertex instead.
    for (const double u : { -2.0, 0.0, 3.0 }) {
        const auto point = degenerate.evaluate(u);
        EXPECT_DOUBLE_EQ(point.x, 1.0);
        EXPECT_DOUBLE_EQ(point.y, 2.0);
        EXPECT_DOUBLE_EQ(degenerate.derivative(u).x, 0.0);
        EXPECT_DOUBLE_EQ(degenerate.secondDerivative(u).y, 0.0);
    }
}

TEST(ConicCurveTest, EveryConicSharesTheSameFrameConventions)
{
    const EllipseCurved   ellipse({ 1.0, 2.0 }, { 0.0, 4.0 }, 3.0, 2.0);
    const HyperbolaCurved hyperbola({ 1.0, 2.0 }, { 0.0, 4.0 }, 3.0, 2.0);
    const ParabolaCurved  parabola({ 3.0, 4.0 }, { 0.0, 4.0 }, 2.0);

    EXPECT_NEAR(ellipse.x_direction.y, 1.0, 1e-12);
    EXPECT_NEAR(ellipse.yAxis().x, -1.0, 1e-12);
    EXPECT_NEAR(hyperbola.x_direction.y, 1.0, 1e-12);
    EXPECT_NEAR(hyperbola.yAxis().x, -1.0, 1e-12);
    EXPECT_NEAR(parabola.x_direction.y, 1.0, 1e-12);
    EXPECT_NEAR(parabola.yAxis().x, -1.0, 1e-12);

    // The vertex of a parabola is the inherited `center`.
    EXPECT_NEAR(parabola.pointAt(0.0).x, parabola.center.x, 1e-12);
    EXPECT_NEAR(parabola.pointAt(0.0).y, parabola.center.y, 1e-12);

    const ConicCurve<double>& as_conic = hyperbola;
    EXPECT_EQ(as_conic.continuity(), vn::geom::Continuity::CN);
}

} // namespace
