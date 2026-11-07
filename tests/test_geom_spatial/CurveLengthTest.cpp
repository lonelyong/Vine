#include <gtest/gtest.h>

#include <cmath>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/BezierCurve.hpp>
#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/CurveLength.hpp>
#include <vine/geom/spatial/LineCurve.hpp>

namespace
{

using vn::geom::spatial::arcLength;
using vn::geom::spatial::BezierCurved;
using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::LineCurved;
using vn::geom::spatial::parameterAtLength;
using vn::math::Point3d;
using vn::math::Vec3d;

TEST(CurveLengthTest, MeasuresALineByItsSpeed)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(3.0, 4.0, 0.0));

    // The direction is not unit length: two units of the parameter move five units of distance.
    const auto length = arcLength(line, std::pair{ 0.0, 2.0 });
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 10.0, 1e-12);
}

TEST(CurveLengthTest, MeasuresACircleByItsRadius)
{
    const CircleCurved circle(Point3d(1.0, 1.0, 1.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    // Without a window the curve's own domain is used, which is one whole turn.
    const auto full = arcLength(circle);
    ASSERT_TRUE(full);
    EXPECT_NEAR(*full, 4.0 * vn::math::PI, 1e-9);

    const auto half = arcLength(circle, std::pair{ vn::math::PI_HALF, vn::math::PI });
    ASSERT_TRUE(half);
    EXPECT_NEAR(*half, 2.0 * vn::math::PI_HALF, 1e-9);
}

TEST(CurveLengthTest, ASplineMatchesADensePolyline)
{
    const BezierCurved bezier({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 3.0, 0.0), Point3d(3.0, -1.0, 1.0),
                                Point3d(4.0, 2.0, 2.0) });

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
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    EXPECT_FALSE(arcLength(line));
    EXPECT_FALSE(arcLength(line, std::pair{ 2.0, 1.0 }));
    EXPECT_FALSE(arcLength(line, std::pair{ 1.0, 1.0 }));
    EXPECT_FALSE(arcLength(LineCurved(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0)), std::pair{ 0.0, 1.0 }));
}

TEST(CurveLengthTest, WorksInFloat)
{
    const vn::geom::spatial::CircleCurvef circle(vn::math::Point3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                                          2.0f);

    const auto length = arcLength(circle);
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 4.0f * vn::math::PI, 1e-4f);
}

TEST(CurveLengthTest, InvertsItself)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(3.0, 4.0, 0.0));

    // The speed is five units per parameter, so five units of length are one unit of parameter.
    const auto parameter = parameterAtLength(line, 5.0, std::pair{ 0.0, 2.0 });
    ASSERT_TRUE(parameter);
    EXPECT_NEAR(*parameter, 1.0, 1e-9);

    const auto length = arcLength(line, std::pair{ 0.0, *parameter });
    ASSERT_TRUE(length);
    EXPECT_NEAR(*length, 5.0, 1e-9);
}

TEST(CurveLengthTest, FindsAnInteriorParameterOnACircle)
{
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    // A quarter of the circumference is the parameter pi/2 on a circle of radius 2.
    const auto parameter = parameterAtLength(circle, vn::math::PI);
    ASSERT_TRUE(parameter);
    EXPECT_NEAR(*parameter, vn::math::PI_HALF, 1e-9);
}

TEST(CurveLengthTest, TheInverseRefusesLengthsOutsideTheWindow)
{
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    EXPECT_FALSE(parameterAtLength(circle, -1.0));
    EXPECT_FALSE(parameterAtLength(circle, 100.0));

    const auto start = parameterAtLength(circle, 0.0);
    ASSERT_TRUE(start);
    EXPECT_DOUBLE_EQ(*start, 0.0);
}

} // namespace
