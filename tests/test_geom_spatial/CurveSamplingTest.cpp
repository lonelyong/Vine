#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/BezierCurve.hpp>
#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/CurveLength.hpp>
#include <vine/geom/spatial/CurveSampling.hpp>
#include <vine/geom/spatial/LineCurve.hpp>

namespace
{

using vn::geom::spatial::arcLength;
using vn::geom::spatial::BezierCurved;
using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::LineCurved;
using vn::geom::spatial::MaxDeflectionSampler;
using vn::geom::spatial::UniformLengthSampler;
using vn::geom::spatial::UniformParameterSampler;
using vn::math::PI;
using vn::math::Point3d;
using vn::math::Vec3d;

/// The sagitta a chord from @p a to @p b really cuts on a circle of the given radius.
double circleSagitta(double radius, const Point3d& a, const Point3d& b)
{
    const double half_chord = (b - a).length() * 0.5;
    const double distance   = std::sqrt(std::max(radius * radius - half_chord * half_chord, 0.0));
    return radius - distance;
}

TEST(CurveSamplingTest, SplitsAnUnboundedLineEvenlyInBothSpaces)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(3.0, 4.0, 0.0));

    UniformParameterSampler<double> by_parameter;
    by_parameter.segments(4);
    by_parameter.curve(line);
    by_parameter.range(0.0, 2.0);
    ASSERT_TRUE(by_parameter.compute());
    ASSERT_EQ(by_parameter.pointCount(), 5u);

    // The direction is not unit length: one unit of the parameter is five units of distance.
    for (std::size_t index = 1; index < by_parameter.pointCount(); ++index) {
        const double chord = (by_parameter.polyline().points[index] - by_parameter.polyline().points[index - 1]).length();
        EXPECT_NEAR(chord, 2.5, 1e-12);
    }

    // The same four chords described as an arc length: on a line the two rules name the same points.
    UniformLengthSampler<double> by_length;
    by_length.step(2.5);
    by_length.curve(line);
    by_length.range(0.0, 2.0);
    ASSERT_TRUE(by_length.compute());
    ASSERT_EQ(by_length.pointCount(), 5u);

    for (std::size_t index = 0; index < by_length.pointCount(); ++index) {
        EXPECT_NEAR(by_length.polyline().parameters[index], by_parameter.polyline().parameters[index], 1e-9);
    }
}

TEST(CurveSamplingTest, SplitsACircleByAngleAndByArcLength)
{
    const CircleCurved circle(Point3d(1.0, 1.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    UniformParameterSampler<double> by_parameter;
    by_parameter.segments(8);
    by_parameter.curve(circle);
    ASSERT_TRUE(by_parameter.compute());
    ASSERT_EQ(by_parameter.pointCount(), 9u);

    for (std::size_t index = 1; index < by_parameter.pointCount(); ++index) {
        EXPECT_NEAR(by_parameter.polyline().parameters[index] - by_parameter.polyline().parameters[index - 1],
                    PI / 4.0, 1e-12);
    }

    // A quarter turn of arc length is pi/2 on a radius of 2, i.e. pi/4 of parameter.
    UniformLengthSampler<double> by_length;
    by_length.step(PI / 2.0);
    by_length.tolerance(1e-10);
    by_length.curve(circle);
    ASSERT_TRUE(by_length.compute());

    const auto& parameters = by_length.polyline().parameters;
    ASSERT_GT(parameters.size(), 2u);
    for (std::size_t index = 1; index < parameters.size(); ++index) {
        EXPECT_NEAR(parameters[index] - parameters[index - 1], PI / 4.0, 1e-6);
    }
    EXPECT_NEAR(parameters.back(), 2.0 * PI, 1e-12);
}

TEST(CurveSamplingTest, KeepsEveryCircleChordWithinTheRequestedSagitta)
{
    const double       radius = 2.0;
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), radius);

    const double deviation = 0.05;

    MaxDeflectionSampler<double> sampler;
    sampler.deviation(deviation);
    sampler.curve(circle);
    ASSERT_TRUE(sampler.compute());
    ASSERT_GE(sampler.pointCount(), 5u);

    const auto& points = sampler.polyline().points;
    for (std::size_t index = 1; index < points.size(); ++index) {
        EXPECT_LE(circleSagitta(radius, points[index - 1], points[index]), deviation * (1.0 + 1e-9));
    }
}

TEST(CurveSamplingTest, EstimatesTheSagittaAtEachChordMidpointOfASpline)
{
    const BezierCurved spline({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 3.0, 0.0), Point3d(3.0, -1.0, 1.0),
                                Point3d(4.0, 2.0, 2.0) });

    const double deviation = 0.05;

    MaxDeflectionSampler<double> sampler;
    sampler.deviation(deviation);
    sampler.range(0.0, 1.0);
    sampler.curve(spline);
    ASSERT_TRUE(sampler.compute());

    const auto& polyline = sampler.polyline();
    ASSERT_GT(polyline.points.size(), 2u);
    EXPECT_DOUBLE_EQ(polyline.parameters.front(), 0.0);
    EXPECT_DOUBLE_EQ(polyline.parameters.back(), 1.0);

    for (std::size_t index = 1; index < polyline.points.size(); ++index) {
        EXPECT_GT(polyline.parameters[index], polyline.parameters[index - 1]);

        const double middle       = (polyline.parameters[index] + polyline.parameters[index - 1]) * 0.5;
        const auto   chord_middle = polyline.points[index - 1]
                              + (polyline.points[index] - polyline.points[index - 1]) * 0.5;
        EXPECT_LE((spline.evaluate(middle) - chord_middle).length(), deviation * (1.0 + 1e-9));
    }
}

TEST(CurveSamplingTest, RefinesWhenTheSagittaBoundTightens)
{
    const BezierCurved spline({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 3.0, 0.0), Point3d(3.0, -1.0, 1.0),
                                Point3d(4.0, 2.0, 2.0) });

    MaxDeflectionSampler<double> coarse;
    coarse.deviation(0.2);
    coarse.range(0.0, 1.0);
    coarse.curve(spline);
    ASSERT_TRUE(coarse.compute());

    MaxDeflectionSampler<double> fine;
    fine.deviation(0.01);
    fine.range(0.0, 1.0);
    fine.curve(spline);
    ASSERT_TRUE(fine.compute());

    EXPECT_LT(coarse.pointCount(), fine.pointCount());

    // A finer polyline is also a longer one: it hugs the curve more closely.
    const auto length = arcLength(spline);
    ASSERT_TRUE(length);

    double coarse_length = 0.0;
    for (std::size_t index = 1; index < coarse.pointCount(); ++index) {
        coarse_length += (coarse.polyline().points[index] - coarse.polyline().points[index - 1]).length();
    }

    double fine_length = 0.0;
    for (std::size_t index = 1; index < fine.pointCount(); ++index) {
        fine_length += (fine.polyline().points[index] - fine.polyline().points[index - 1]).length();
    }

    EXPECT_LE(coarse_length, fine_length + 1e-12);
    EXPECT_LE(fine_length, *length + 1e-9);
}

TEST(CurveSamplingTest, RefusesWhatItCannotSample)
{
    const LineCurved   line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const CircleCurved unusable(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 0.0);
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    {
        UniformParameterSampler<double> sampler;
        EXPECT_FALSE(sampler.compute()); // no curve bound
        EXPECT_EQ(sampler.pointCount(), 0u);
    }
    {
        UniformParameterSampler<double> sampler;
        sampler.curve(unusable);
        EXPECT_FALSE(sampler.compute()); // the curve itself refuses
    }
    {
        UniformParameterSampler<double> sampler;
        sampler.curve(line);
        EXPECT_FALSE(sampler.compute()); // unbounded and not narrowed
    }
    {
        UniformParameterSampler<double> sampler;
        sampler.segments(0);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute()); // zero chords
    }
    {
        UniformLengthSampler<double> sampler;
        sampler.step(0.0);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute()); // a step of zero never advances
    }
    {
        UniformLengthSampler<double> sampler;
        sampler.step(-1.0);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute());
    }
    {
        MaxDeflectionSampler<double> sampler;
        sampler.deviation(0.0);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute()); // zero deflection would need infinitely many points
    }
    {
        MaxDeflectionSampler<double> sampler;
        sampler.deviation(0.1);
        sampler.maxPoints(1);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute()); // a budget of one point cannot hold a chord
    }
    {
        UniformParameterSampler<double> sampler;
        sampler.range(1.0, 1.0);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute()); // an empty window
    }
    {
        UniformParameterSampler<double> sampler;
        sampler.range(1.0, 0.5);
        sampler.curve(circle);
        EXPECT_FALSE(sampler.compute()); // a reversed window is not silently swapped
    }
}

TEST(CurveSamplingTest, KeepsTheArraysParallelAndInsideTheWindow)
{
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 3.0);

    UniformParameterSampler<double> sampler;
    sampler.segments(6);
    sampler.curve(circle);
    sampler.range(0.5, 2.0);
    ASSERT_TRUE(sampler.compute());

    const auto& polyline = sampler.polyline();
    ASSERT_EQ(polyline.points.size(), polyline.parameters.size());
    ASSERT_EQ(sampler.pointCount(), 7u);
    EXPECT_DOUBLE_EQ(polyline.parameters.front(), 0.5);
    EXPECT_DOUBLE_EQ(polyline.parameters.back(), 2.0);

    for (std::size_t index = 0; index < polyline.points.size(); ++index) {
        const auto expected = circle.evaluate(polyline.parameters[index]);
        EXPECT_NEAR((polyline.points[index] - expected).length(), 0.0, 1e-12);
    }

    // Both precisions describe the same shape.
    const vn::geom::spatial::CircleCurvef narrow_circle(vn::math::Point3f(0.0f, 0.0f, 0.0f),
                                                        vn::math::Vec3f(0.0f, 0.0f, 1.0f), 3.0f);

    UniformParameterSampler<float> narrow;
    narrow.segments(6);
    narrow.curve(narrow_circle);
    narrow.range(0.5f, 2.0f);
    ASSERT_TRUE(narrow.compute());
    EXPECT_EQ(narrow.pointCount(), sampler.pointCount());
}

TEST(CurveSamplingTest, DropsThePreviousResultWhenTheCurveChanges)
{
    const CircleCurved smaller(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);
    const CircleCurved larger(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 5.0);

    MaxDeflectionSampler<double> sampler;
    sampler.deviation(0.05);
    sampler.curve(smaller);
    ASSERT_TRUE(sampler.compute());
    const std::size_t small_count = sampler.pointCount();
    ASSERT_GT(small_count, 0u);

    // Rebinding the curve without computing must not hand back the previous curve's points.
    sampler.curve(larger);
    EXPECT_EQ(sampler.pointCount(), 0u);
    EXPECT_TRUE(sampler.polyline().points.empty());

    // A failed run clears too.
    sampler.deviation(-1.0);
    EXPECT_EQ(sampler.pointCount(), 0u);

    sampler.deviation(0.05);
    ASSERT_TRUE(sampler.compute());
    EXPECT_GT(sampler.pointCount(), small_count);
    EXPECT_NEAR((sampler.polyline().points.front() - larger.evaluate(0.0)).length(), 0.0, 1e-12);
}

TEST(CurveSamplingTest, ReusesOneConfigurationAcrossCurves)
{
    const CircleCurved first(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);
    const CircleCurved second(Point3d(10.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    UniformLengthSampler<double> sampler;
    sampler.step(PI / 4.0);
    sampler.tolerance(1e-10);

    sampler.curve(first);
    ASSERT_TRUE(sampler.compute());
    const std::size_t count = sampler.pointCount();

    sampler.curve(second);
    ASSERT_TRUE(sampler.compute());
    EXPECT_EQ(sampler.pointCount(), count);

    // The second run's points belong to the second curve: same shape, ten units along X.
    const auto& points = sampler.polyline().points;
    ASSERT_FALSE(points.empty());
    EXPECT_NEAR((points.front() - second.evaluate(0.0)).length(), 0.0, 1e-12);
    EXPECT_NEAR(points.front().x, 11.0, 1e-12);
}

TEST(CurveSamplingTest, StopsAtThePointBudgetInsteadOfCoarsening)
{
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 1.0);

    MaxDeflectionSampler<double> sampler;
    sampler.deviation(0.01);
    sampler.curve(circle);
    ASSERT_TRUE(sampler.compute());
    const auto needed = sampler.pointCount();
    ASSERT_GT(needed, 2u);

    sampler.maxPoints(static_cast<int>(needed));
    EXPECT_TRUE(sampler.compute());

    sampler.maxPoints(static_cast<int>(needed) - 1);
    EXPECT_FALSE(sampler.compute());
    EXPECT_EQ(sampler.pointCount(), 0u);
}

} // namespace
