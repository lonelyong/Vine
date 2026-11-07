#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom/planar/BSplineCurve.hpp>
#include <vine/geom/planar/BezierCurve.hpp>
#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/CurveCurvature.hpp>
#include <vine/geom/planar/EllipseCurve.hpp>
#include <vine/geom/planar/LineCurve.hpp>
#include <vine/geom/planar/ParabolaCurve.hpp>

namespace
{

using vn::geom::planar::BezierCurved;
using vn::geom::planar::CircleCurved;
using vn::geom::planar::CircleCurvef;
using vn::geom::planar::EllipseCurved;
using vn::geom::planar::LineCurved;
using vn::geom::planar::ParabolaCurved;
using vn::geom::planar::signedCurvature;

TEST(CurveCurvatureTest, ALineIsFlat)
{
    const LineCurved line({ 1.0, 2.0 }, { 3.0, 4.0 });

    for (const double u : { -2.0, 0.0, 7.5 }) {
        const auto curvature = signedCurvature(line, u);
        ASSERT_TRUE(curvature);
        EXPECT_DOUBLE_EQ(*curvature, 0.0);
    }
}

TEST(CurveCurvatureTest, ACircleTurnsAtItsRadius)
{
    const CircleCurved circle({ 3.0, -1.0 }, 2.0);

    for (const double u : { 0.0, 0.7, 3.9, 6.1 }) {
        const auto curvature = signedCurvature(circle, u);
        ASSERT_TRUE(curvature);

        // Counter-clockwise travel puts the centre of curvature on the left, so the sign is positive.
        EXPECT_NEAR(*curvature, 0.5, 1e-12);
    }

    const CircleCurvef float_circle({ 0.0f, 0.0f }, 0.5f);
    const auto        float_curvature = signedCurvature(float_circle, 1.0f);
    ASSERT_TRUE(float_curvature);
    EXPECT_NEAR(*float_curvature, 2.0f, 1e-5f);
}

TEST(CurveCurvatureTest, AnEllipseBendsUnevenly)
{
    const EllipseCurved ellipse({ 0.0, 0.0 }, { 1.0, 0.0 }, 3.0, 2.0);

    // At the end of the major axis the radius of curvature is b^2 / a; at the minor axis it is a^2 / b.
    const auto at_major = signedCurvature(ellipse, 0.0);
    ASSERT_TRUE(at_major);
    EXPECT_NEAR(*at_major, 3.0 / 4.0, 1e-12);

    const auto at_minor = signedCurvature(ellipse, vn::math::PI_HALF);
    ASSERT_TRUE(at_minor);
    EXPECT_NEAR(*at_minor, 2.0 / 9.0, 1e-12);
}

TEST(CurveCurvatureTest, AParabolaBendsToItsOwnRightHere)
{
    // P(u) = (u^2 / 4, u): at the vertex the velocity is +Y and the curve bends towards +X, which is the right side.
    const ParabolaCurved parabola({ 0.0, 0.0 }, { 1.0, 0.0 }, 1.0);

    const auto vertex = signedCurvature(parabola, 0.0);
    ASSERT_TRUE(vertex);
    EXPECT_NEAR(*vertex, -0.5, 1e-12);

    const auto at_two = signedCurvature(parabola, 2.0);
    ASSERT_TRUE(at_two);
    EXPECT_NEAR(*at_two, -0.5 / (2.0 * std::sqrt(2.0)), 1e-12);
}

TEST(CurveCurvatureTest, ASingularParameterHasNoCurvature)
{
    // The first two poles coincide, so the velocity vanishes at u = 0; the curvature is undefined there.
    const BezierCurved cusp({ { 0.0, 0.0 }, { 0.0, 0.0 }, { 1.0, 2.0 } });

    EXPECT_FALSE(signedCurvature(cusp, 0.0));
    EXPECT_TRUE(signedCurvature(cusp, 0.5));
}

TEST(CurveCurvatureTest, AnUnusableCurveHasNoCurvature)
{
    const LineCurved degenerate({ 0.0, 0.0 }, { 0.0, 0.0 });

    EXPECT_FALSE(signedCurvature(degenerate, 0.0));
}

TEST(CurveCurvatureTest, AStraightSplineIsFlat)
{
    const vn::geom::planar::BSplineCurved spline(1, { { 0.0, 0.0 }, { 3.0, 4.0 } }, { 0.0, 1.0 }, { 2, 2 });

    for (const double u : { 0.1, 0.5, 0.9 }) {
        const auto answer = signedCurvature(spline, u);
        ASSERT_TRUE(answer);
        EXPECT_NEAR(*answer, 0.0, 1e-12);
    }
}

TEST(CurveCurvatureTest, ARationalSplineKeepsTheCircleCurvature)
{
    const BezierCurved quarter({ { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 } }, { 1.0, std::sqrt(0.5), 1.0 });

    for (const double u : { 0.2, 0.5, 0.8 }) {
        const auto answer = signedCurvature(quarter, u);
        ASSERT_TRUE(answer);
        EXPECT_NEAR(*answer, 1.0, 1e-9);
    }
}

TEST(CurveCurvatureTest, AgreesWithTheTurningRateOfTheTangent)
{
    const EllipseCurved ellipse({ 1.0, -2.0 }, { 1.0, 1.0 }, 3.0, 2.0);
    const double        h = 1e-6;

    for (const double u : { 0.4, 1.3, 2.9, 4.4 }) {
        const auto before  = ellipse.derivative(u - h);
        const auto after   = ellipse.derivative(u + h);
        const auto angle   = [](const auto& v) { return std::atan2(v.y, v.x); };
        const double rate  = (angle(after) - angle(before)) / (2.0 * h);
        const double speed = ellipse.derivative(u).length();

        const auto answer = signedCurvature(ellipse, u);
        ASSERT_TRUE(answer);
        EXPECT_NEAR(*answer, rate / speed, 1e-5);
    }
}

} // namespace
