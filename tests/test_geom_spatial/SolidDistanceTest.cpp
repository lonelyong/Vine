#include <gtest/gtest.h>

#include <cmath>

#include <vine/models/Primitive.hpp>
#include <vine/models/Box.hpp>
#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/geom/spatial/ConeSolid.hpp>
#include <vine/geom/spatial/CylinderSolid.hpp>
#include <vine/geom/spatial/EllipsoidSolid.hpp>
#include <vine/geom/spatial/SphereSolid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geom::spatial::BoxSolidd;
using vn::geom::spatial::ConeSolidd;
using vn::geom::spatial::CylinderSolidd;
using vn::geom::spatial::EllipsoidSolidd;
using vn::models::Primitive;
using vn::geom::spatial::SphereSolidd;
using vn::math::Point3d;
using vn::math::Point3f;
using vn::math::Vec3d;

/// The nearest point is the query itself inside a solid, and the distance is zero there.
template <typename Projection>
void expectInside(const Projection& projection, const Point3d& query)
{
    ASSERT_TRUE(projection.has_value());
    EXPECT_DOUBLE_EQ(projection->distance, 0.0);
    EXPECT_LT((projection->point - query).length(), 1e-12);
}

/// The same sphere projection at both precisions the library exports.
template <vn::math::FP T>
void checkSphereProjectionAtBothPrecisions()
{
    const vn::geom::spatial::SphereSolid<T> sphere{ T(1) };
    const auto                    projection = sphere.closestPoint(vn::math::Point3<T>(T(5), T(0), T(0)));

    ASSERT_TRUE(projection.has_value());
    const double tolerance = (T(1) == T(1.0f)) ? 1e-4 : 1e-9;
    EXPECT_NEAR(static_cast<double>(projection->distance), 4.0, tolerance);
    EXPECT_NEAR(static_cast<double>(projection->point.x), 1.0, tolerance);
}

} // namespace

TEST(SolidDistance, aBoxClampsToItsFacesAndCorners)
{
    const BoxSolidd box{ 1.0, 1.0, 1.0 };

    const auto face = box.closestPoint(Point3d(3.0, 0.0, 0.0));
    ASSERT_TRUE(face.has_value());
    EXPECT_LT((face->point - Point3d(1.0, 0.0, 0.0)).length(), 1e-9);
    EXPECT_NEAR(face->distance, 2.0, 1e-9);

    const auto corner = box.closestPoint(Point3d(3.0, 3.0, 3.0));
    ASSERT_TRUE(corner.has_value());
    EXPECT_LT((corner->point - Point3d(1.0, 1.0, 1.0)).length(), 1e-9);
    EXPECT_NEAR(corner->distance, std::sqrt(12.0), 1e-9);

    expectInside(box.closestPoint(Point3d(0.5, 0.5, 0.5)), Point3d(0.5, 0.5, 0.5));
}

TEST(SolidDistance, aSphereProjectsAlongItsRadius)
{
    const SphereSolidd sphere{ 1.0 };

    const auto outside = sphere.closestPoint(Point3d(5.0, 0.0, 0.0));
    ASSERT_TRUE(outside.has_value());
    EXPECT_LT((outside->point - Point3d(1.0, 0.0, 0.0)).length(), 1e-9);
    EXPECT_NEAR(outside->distance, 4.0, 1e-9);

    expectInside(sphere.closestPoint(Point3d(0.0, 0.0, 0.0)), Point3d(0.0, 0.0, 0.0));
}

TEST(SolidDistance, anEllipsoidProjectsOntoItsSurfaceAlongTheNormal)
{
    // On the axes the projection is the semi-axis itself; off the axes the nearest point satisfies the Lagrange
    // condition, i.e. the segment to the query is parallel to the surface normal.
    const EllipsoidSolidd ellipsoid{ 1.0, 2.0, 3.0 };

    const auto along_x = ellipsoid.closestPoint(Point3d(5.0, 0.0, 0.0));
    ASSERT_TRUE(along_x.has_value());
    EXPECT_LT((along_x->point - Point3d(1.0, 0.0, 0.0)).length(), 1e-7);
    EXPECT_NEAR(along_x->distance, 4.0, 1e-7);

    const auto along_y = ellipsoid.closestPoint(Point3d(0.0, 5.0, 0.0));
    ASSERT_TRUE(along_y.has_value());
    EXPECT_LT((along_y->point - Point3d(0.0, 2.0, 0.0)).length(), 1e-7);
    EXPECT_NEAR(along_y->distance, 3.0, 1e-7);

    const auto along_z = ellipsoid.closestPoint(Point3d(0.0, 0.0, 5.0));
    ASSERT_TRUE(along_z.has_value());
    EXPECT_LT((along_z->point - Point3d(0.0, 0.0, 3.0)).length(), 1e-7);
    EXPECT_NEAR(along_z->distance, 2.0, 1e-7);

    const Point3d query(4.0, 5.0, 6.0);
    const auto    off_axis = ellipsoid.closestPoint(query);
    ASSERT_TRUE(off_axis.has_value());

    const Point3d point = off_axis->point;
    EXPECT_NEAR((point.x / 1.0) * (point.x / 1.0) + (point.y / 2.0) * (point.y / 2.0) + (point.z / 3.0) * (point.z / 3.0), 1.0, 1e-7);
    EXPECT_NEAR(off_axis->distance, (point - query).length(), 1e-9);

    const Vec3d normal(point.x, point.y / 4.0, point.z / 9.0);
    const Vec3d to_query = query - point;
    EXPECT_GT(normal.dot(to_query), 0.0);
    EXPECT_LT((normal.cross(to_query)).length(), 1e-6 * to_query.length() * normal.length());

    expectInside(ellipsoid.closestPoint(Point3d(0.0, 0.0, 0.0)), Point3d(0.0, 0.0, 0.0));
}

TEST(SolidDistance, aCylinderClampsRadiallyAndAxially)
{
    const CylinderSolidd cylinder{ 1.0, 1.0 };

    const auto side = cylinder.closestPoint(Point3d(5.0, 0.0, 0.5));
    ASSERT_TRUE(side.has_value());
    EXPECT_LT((side->point - Point3d(1.0, 0.0, 0.5)).length(), 1e-9);
    EXPECT_NEAR(side->distance, 4.0, 1e-9);

    const auto cap = cylinder.closestPoint(Point3d(0.0, 0.0, 5.0));
    ASSERT_TRUE(cap.has_value());
    EXPECT_LT((cap->point - Point3d(0.0, 0.0, 1.0)).length(), 1e-9);
    EXPECT_NEAR(cap->distance, 4.0, 1e-9);

    const auto rim = cylinder.closestPoint(Point3d(5.0, 0.0, 5.0));
    ASSERT_TRUE(rim.has_value());
    EXPECT_LT((rim->point - Point3d(1.0, 0.0, 1.0)).length(), 1e-9);
    EXPECT_NEAR(rim->distance, std::sqrt(32.0), 1e-9);

    expectInside(cylinder.closestPoint(Point3d(0.0, 0.0, 0.0)), Point3d(0.0, 0.0, 0.0));
}

TEST(SolidDistance, aConeChoosesBetweenItsBaseAndItsSide)
{
    const ConeSolidd cone{ 1.0, 1.0 }; // base radius 1 at z = -1, apex at z = +1

    // Far from the axis at mid height the base rim is nearest.
    const auto beside = cone.closestPoint(Point3d(5.0, 0.0, 0.0));
    ASSERT_TRUE(beside.has_value());
    EXPECT_LT((beside->point - Point3d(1.0, 0.0, -1.0)).length(), 1e-7);
    EXPECT_NEAR(beside->distance, std::sqrt(17.0), 1e-7);

    // Above the apex the nearest point is the apex itself (on the axis, so any meridian answers).
    const auto above = cone.closestPoint(Point3d(0.0, 0.0, 5.0));
    ASSERT_TRUE(above.has_value());
    EXPECT_LT((above->point - Point3d(0.0, 0.0, 1.0)).length(), 1e-7);
    EXPECT_NEAR(above->distance, 4.0, 1e-7);

    // Below the base the base disk wins over the rim.
    const auto below = cone.closestPoint(Point3d(0.0, 0.0, -5.0));
    ASSERT_TRUE(below.has_value());
    EXPECT_LT((below->point - Point3d(0.0, 0.0, -1.0)).length(), 1e-7);
    EXPECT_NEAR(below->distance, 4.0, 1e-7);

    expectInside(cone.closestPoint(Point3d(0.0, 0.0, 0.0)), Point3d(0.0, 0.0, 0.0));
}

TEST(SolidDistance, anUnusableSolidAnswersNothing)
{
    const BoxSolidd    degenerate_box{ 1.0, 0.0, 1.0 };
    const SphereSolidd degenerate_sphere{ 0.0 };

    EXPECT_FALSE(degenerate_box.closestPoint(Point3d(1.0, 1.0, 1.0)).has_value());
    EXPECT_FALSE(degenerate_sphere.closestPoint(Point3d(1.0, 1.0, 1.0)).has_value());
}

TEST(SolidDistance, aModelReachesTheSameAnswerThroughItsSolid)
{
    // The modeling type has no distance query of its own: its analytic solid answers.
    const vn::models::Box box(2.0f, 2.0f, 2.0f);

    const auto face = box.solid().closestPoint(Point3f(3.0f, 0.0f, 0.0f));
    ASSERT_TRUE(face.has_value());
    EXPECT_LT((face->point - Point3f(1.0f, 0.0f, 0.0f)).length(), 1e-6f);
    EXPECT_NEAR(face->distance, 2.0f, 1e-6f);
}

TEST(SolidDistance, bothPrecisionsFindTheSameProjection)
{
    checkSphereProjectionAtBothPrecisions<double>();
    checkSphereProjectionAtBothPrecisions<float>();
}
