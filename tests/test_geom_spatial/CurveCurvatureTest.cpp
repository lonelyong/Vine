#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/BezierCurve.hpp>
#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/CurveCurvature.hpp>
#include <vine/geom/spatial/EllipseCurve.hpp>
#include <vine/geom/spatial/LineCurve.hpp>

namespace
{

using vn::geom::spatial::BezierCurved;
using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::curvature;
using vn::geom::spatial::EllipseCurved;
using vn::geom::spatial::LineCurved;
using vn::math::Point3d;
using vn::math::Vec3d;

TEST(CurveCurvatureTest, ALineIsFlat)
{
    const LineCurved line(Point3d(1.0, 2.0, 3.0), Vec3d(3.0, 4.0, 0.0));

    for (const double u : { -2.0, 0.0, 7.5 }) {
        const auto answer = curvature(line, u);
        ASSERT_TRUE(answer);
        EXPECT_DOUBLE_EQ(*answer, 0.0);
    }
}

TEST(CurveCurvatureTest, ACircleTurnsAtItsRadius)
{
    const CircleCurved circle(Point3d(3.0, -1.0, 2.0), Vec3d(0.0, 0.0, 1.0), 2.0);

    for (const double u : { 0.0, 0.7, 3.9, 6.1 }) {
        const auto answer = curvature(circle, u);
        ASSERT_TRUE(answer);

        // A space curve has no canonical side: the curvature is the reciprocal of the radius and never negative.
        EXPECT_NEAR(*answer, 0.5, 1e-12);
        EXPECT_GE(*answer, 0.0);
    }

    const vn::geom::spatial::CircleCurvef float_circle(vn::math::Point3f(0.0f, 0.0f, 0.0f),
                                                vn::math::Vec3f(0.0f, 0.0f, 1.0f), 0.5f);
    const auto                 float_answer = curvature(float_circle, 1.0f);
    ASSERT_TRUE(float_answer);
    EXPECT_NEAR(*float_answer, 2.0f, 1e-5f);
}

TEST(CurveCurvatureTest, AnEllipseBendsUnevenly)
{
    const EllipseCurved ellipse(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), Vec3d(1.0, 0.0, 0.0), 3.0, 2.0);

    // At the end of the major axis the radius of curvature is b^2 / a; at the minor axis it is a^2 / b.
    const auto at_major = curvature(ellipse, 0.0);
    ASSERT_TRUE(at_major);
    EXPECT_NEAR(*at_major, 3.0 / 4.0, 1e-12);

    const auto at_minor = curvature(ellipse, vn::math::PI_HALF);
    ASSERT_TRUE(at_minor);
    EXPECT_NEAR(*at_minor, 2.0 / 9.0, 1e-12);
}

TEST(CurveCurvatureTest, ASingularParameterHasNoCurvature)
{
    // The first two poles coincide, so the velocity vanishes at u = 0; the curvature is undefined there.
    const BezierCurved cusp({ Point3d(0.0, 0.0, 0.0), Point3d(0.0, 0.0, 0.0), Point3d(1.0, 2.0, 0.0) });

    EXPECT_FALSE(curvature(cusp, 0.0));
    EXPECT_TRUE(curvature(cusp, 0.5));
}

TEST(CurveCurvatureTest, AnUnusableCurveHasNoCurvature)
{
    const LineCurved degenerate(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0));

    EXPECT_FALSE(curvature(degenerate, 0.0));
}

} // namespace
