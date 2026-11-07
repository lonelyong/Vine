#include <gtest/gtest.h>

#include <cmath>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>

#include <vine/geom/planar/TrimmedCurve.hpp>
#include <vine/geom/planar/BezierCurve.hpp>
#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/CurveLength.hpp>
#include <vine/geom/planar/LineCurve.hpp>
#include <vine/geom/planar/OffsetCurve.hpp>

namespace
{

using vn::geom::planar::BezierCurved;
using vn::geom::planar::CircleCurved;
using vn::geom::planar::CircleCurvef;
using vn::geom::planar::LineCurved;
using vn::geom::planar::arcLength;
using vn::geom::planar::parameterAtLength;

TEST(CurveLengthTest, MeasuresALineByItsSpeed)
{
    const LineCurved line({ 0.0, 0.0 }, { 3.0, 4.0 });

    // The direction is not unit length: two units of the parameter move five units of distance.
    const auto length = arcLength(line, std::pair{ 0.0, 2.0 });
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 10.0, 1e-12);
}

TEST(CurveLengthTest, MeasuresAnArcByItsRadius)
{
    const CircleCurved circle({ 1.0, 1.0 }, 2.0);

    // Without a window the curve's own domain is used, which is one whole turn.
    const auto full = arcLength(circle);
    ASSERT_TRUE(full);
    EXPECT_NEAR(*full, 4.0 * vn::math::PI, 1e-9);

    const auto half = arcLength(circle, std::pair{ vn::math::PI_HALF, vn::math::PI });
    ASSERT_TRUE(half);
    EXPECT_NEAR(*half, 2.0 * vn::math::PI_HALF, 1e-9);
}

TEST(CurveLengthTest, MeasuresTheArcTypesOwnInterval)
{
    const auto arc = trimCurve(CircleCurved({ 0.0, 0.0 }, 3.0), 0.5, 0.5 + vn::math::PI_HALF);

    const auto length = arcLength(arc);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 3.0 * vn::math::PI_HALF, 1e-9);
}

TEST(CurveLengthTest, ASplineMatchesADensePolyline)
{
    const BezierCurved bezier({ { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } });

    double polyline = 0.0;
    int    samples  = 20000;
    auto   previous = bezier.evaluate(0.0);
    for (int index = 1; index <= samples; ++index) {
        const auto point = bezier.evaluate(static_cast<double>(index) / static_cast<double>(samples));
        polyline += (point - previous).length();
        previous = point;
    }

    const auto length = arcLength(bezier, std::nullopt, 1e-10);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, polyline, 1e-6);
}

TEST(CurveLengthTest, RefusesAWindowItCannotUse)
{
    const LineCurved line({ 0.0, 0.0 }, { 1.0, 0.0 });

    // An unbounded curve needs a finite window; the function never invents one.
    EXPECT_FALSE(arcLength(line));
    EXPECT_FALSE(arcLength(line, std::pair{ 2.0, 1.0 }));
    EXPECT_FALSE(arcLength(line, std::pair{ 1.0, 1.0 }));

    // An unusable curve has no length either.
    EXPECT_FALSE(arcLength(LineCurved({ 0.0, 0.0 }, { 0.0, 0.0 }), std::pair{ 0.0, 1.0 }));
}

TEST(CurveLengthTest, MeasuresInFloat)
{
    const CircleCurvef circle({ 0.0f, 0.0f }, 2.0f);

    const auto length = arcLength(circle);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 4.0f * vn::math::PI, 1e-4f);
}

TEST(CurveLengthTest, InvertsItself)
{
    const LineCurved line({ 0.0, 0.0 }, { 3.0, 4.0 });

    // A quarter of the ten-unit window is the parameter 0.5, because the speed is five units per parameter.
    const auto parameter = parameterAtLength(line, 5.0, std::pair{ 0.0, 2.0 });
    ASSERT_TRUE(parameter);
    EXPECT_NEAR(*parameter, 1.0, 1e-9);

    const auto length = arcLength(line, std::pair{ 0.0, *parameter });
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 5.0, 1e-9);
}

TEST(CurveLengthTest, TheInverseRefusesLengthsOutsideTheWindow)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);

    EXPECT_FALSE(parameterAtLength(circle, -1.0));
    EXPECT_FALSE(parameterAtLength(circle, 100.0));

    const auto start = parameterAtLength(circle, 0.0);
    ASSERT_TRUE(start);
    EXPECT_DOUBLE_EQ(*start, 0.0);
}

TEST(CurveLengthTest, FindsAnInteriorParameterOnACircle)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);

    // A quarter of the circumference is the parameter pi/2 on a circle of radius 2.
    const auto parameter = parameterAtLength(circle, vn::math::PI);
    ASSERT_TRUE(parameter);
    EXPECT_NEAR(*parameter, vn::math::PI_HALF, 1e-9);
}

TEST(CurveLengthTest, InvertsInFloat)
{
    const CircleCurvef circle({ 0.0f, 0.0f }, 2.0f);

    const auto parameter = parameterAtLength(circle, 1.0f);
    ASSERT_TRUE(parameter);
    EXPECT_NEAR(*parameter, 0.5f, 1e-4f);
}

TEST(CurveLengthTest, AnOffsetCurveKeepsTheCircleLength)
{
    const auto inner = vn::geom::planar::offsetCurve(CircleCurved({ 0.0, 0.0 }, 2.0), 1.0);

    // The left normal of a counter-clockwise circle points inward, so the offset circle has radius 1.
    const auto length = arcLength(inner);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 2.0 * vn::math::PI, 1e-9);
}

TEST(CurveLengthTest, ARationalSplineMeasuresTheQuarterCircle)
{
    const BezierCurved quarter({ { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 } },
                               { 1.0, std::sqrt(0.5), 1.0 });

    const auto length = arcLength(quarter);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, vn::math::PI_HALF, 1e-9);
}

TEST(CurveLengthTest, ACuspStillHasAFiniteLength)
{
    // The first two poles coincide, so the curve starts at a cusp; the quadrature must stay bounded there.
    const BezierCurved cusp({ { 0.0, 0.0 }, { 0.0, 0.0 }, { 1.0, 2.0 } });

    double polyline = 0.0;
    int    samples  = 20000;
    auto   previous = cusp.evaluate(0.0);
    for (int index = 1; index <= samples; ++index) {
        const auto point = cusp.evaluate(static_cast<double>(index) / static_cast<double>(samples));
        polyline += (point - previous).length();
        previous = point;
    }

    const auto length = arcLength(cusp, std::nullopt, 1e-10);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, polyline, 1e-6);
}

} // namespace
