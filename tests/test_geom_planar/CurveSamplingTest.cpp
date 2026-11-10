#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/CurveSampling.hpp>
#include <vine/geom/planar/LineCurve.hpp>

namespace
{

using vn::geom::planar::CircleCurved;
using vn::geom::planar::LineCurved;
using vn::geom::planar::MaxDeflectionSampler;
using vn::geom::planar::UniformLengthSampler;
using vn::geom::planar::UniformParameterSampler;
using vn::math::PI;
using vn::math::Point2d;
using vn::math::Vec2d;

TEST(CurveSamplingTest, SplitsAPlanarLineEvenlyInBothSpaces)
{
    const LineCurved line(Point2d(0.0, 0.0), Vec2d(3.0, 4.0));

    UniformParameterSampler<double> by_parameter;
    by_parameter.segments(4);
    by_parameter.curve(line);
    by_parameter.range(0.0, 2.0);
    ASSERT_TRUE(by_parameter.compute());
    ASSERT_EQ(by_parameter.pointCount(), 5u);

    UniformLengthSampler<double> by_length;
    by_length.step(2.5);
    by_length.curve(line);
    by_length.range(0.0, 2.0);
    ASSERT_TRUE(by_length.compute());
    ASSERT_EQ(by_length.pointCount(), 5u);

    for (std::size_t index = 0; index < by_length.pointCount(); ++index) {
        EXPECT_NEAR(by_length.polyline().parameters[index], by_parameter.polyline().parameters[index], 1e-9);
        EXPECT_NEAR((by_length.polyline().points[index] - by_parameter.polyline().points[index]).length(), 0.0, 1e-9);
    }
}

TEST(CurveSamplingTest, SplitsAPlanarCircleByArcLength)
{
    const CircleCurved circle(Point2d(2.0, 2.0), 2.0);

    // A quarter turn of arc length is pi/2 on a radius of 2, i.e. pi/4 of parameter.
    UniformLengthSampler<double> sampler;
    sampler.step(PI / 2.0);
    sampler.tolerance(1e-10);
    sampler.curve(circle);
    ASSERT_TRUE(sampler.compute());

    const auto& parameters = sampler.polyline().parameters;
    ASSERT_GT(parameters.size(), 2u);
    for (std::size_t index = 1; index < parameters.size(); ++index) {
        EXPECT_NEAR(parameters[index] - parameters[index - 1], PI / 4.0, 1e-6);
    }
    EXPECT_NEAR(parameters.back(), 2.0 * PI, 1e-12);
}

TEST(CurveSamplingTest, KeepsEveryPlanarChordWithinTheRequestedSagitta)
{
    const double       radius = 2.0;
    const CircleCurved circle(Point2d(0.0, 0.0), radius);

    const double deviation = 0.05;

    MaxDeflectionSampler<double> sampler;
    sampler.deviation(deviation);
    sampler.curve(circle);
    ASSERT_TRUE(sampler.compute());
    ASSERT_GE(sampler.pointCount(), 5u);

    const auto& points = sampler.polyline().points;
    for (std::size_t index = 1; index < points.size(); ++index) {
        const double half_chord = (points[index] - points[index - 1]).length() * 0.5;
        const double sagitta    = radius - std::sqrt(std::max(radius * radius - half_chord * half_chord, 0.0));
        EXPECT_LE(sagitta, deviation * (1.0 + 1e-9));
    }
}

TEST(CurveSamplingTest, UsesTheCurveOwnDomainWhenNoWindowIsGiven)
{
    const CircleCurved circle(Point2d(0.0, 0.0), 1.0);

    UniformParameterSampler<double> sampler;
    sampler.segments(8);
    sampler.curve(circle);
    ASSERT_TRUE(sampler.compute());

    const auto& parameters = sampler.polyline().parameters;
    EXPECT_DOUBLE_EQ(parameters.front(), 0.0);
    EXPECT_NEAR(parameters.back(), 2.0 * PI, 1e-12);

    // A closed curve's ends meet; the result keeps both points rather than dropping one silently.
    EXPECT_NEAR((sampler.polyline().points.front() - sampler.polyline().points.back()).length(), 0.0, 1e-12);
}

} // namespace
