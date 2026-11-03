#include <gtest/gtest.h>

#include <numbers>

#include <vine/geometry/Aabb.hpp>
#include <vine/geometry/Obb.hpp>
#include <vine/geometry/Plane.hpp>
#include <vine/geometry/Ray.hpp>

namespace {

using vn::geometry::Aabbd;
using vn::geometry::Obbd;
using vn::geometry::Plane;
using vn::geometry::RayHitd;
using vn::geometry::Rayd;
using vn::math::Point3d;
using vn::math::Vec3d;

constexpr double kTolerance = 1e-9;

/// The unit box at the origin, the shape every box case below is written against.
Aabbd unitBox()
{
    return Aabbd(Point3d(-1.0, -1.0, -1.0), Point3d(1.0, 1.0, 1.0));
}

/// A ray from a point along a direction; the direction is normalized by the point's constructor.
Rayd rayFrom(double ox, double oy, double oz, double dx, double dy, double dz)
{
    return Rayd(Point3d(ox, oy, oz), Vec3d(dx, dy, dz));
}

} // namespace

TEST(RayTest, theDirectionIsStoredAsAUnitVector)
{
    const Rayd ray = rayFrom(0.0, 0.0, 5.0, 0.0, 0.0, -10.0);

    EXPECT_DOUBLE_EQ(ray.direction.z, -1.0);
    EXPECT_DOUBLE_EQ(ray.direction.length(), 1.0);

    const auto point = ray.pointAt(4.0);
    EXPECT_DOUBLE_EQ(point.x, 0.0);
    EXPECT_DOUBLE_EQ(point.z, 1.0);
}

TEST(RayTest, anAimlessRayIsZeroLengthAndHitsNothing)
{
    Rayd ray;
    EXPECT_DOUBLE_EQ(ray.direction.length(), 0.0);

    RayHitd hit;
    EXPECT_FALSE(unitBox().intersectWith(ray, hit));
    EXPECT_FALSE(Plane<double>(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0)).intersectWith(ray, hit));
    EXPECT_FALSE(vn::geometry::intersectTriangle(ray, Point3d(0.0, 0.0, 0.0), Point3d(1.0, 0.0, 0.0), Point3d(0.0, 1.0, 0.0), hit));
}

TEST(RayTest, theDistanceToAPointStopsAtTheOrigin)
{
    const Rayd ray = rayFrom(0.0, 0.0, 0.0, 0.0, 0.0, 1.0);

    EXPECT_DOUBLE_EQ(ray.distanceToPoint(Point3d(0.0, 0.0, 3.0)), 0.0) << "the point lies on the ray";
    EXPECT_DOUBLE_EQ(ray.distanceToPoint(Point3d(2.0, 0.0, 4.0)), 2.0);
    // Behind the origin the ray does not exist: the distance is measured to the origin itself.
    EXPECT_DOUBLE_EQ(ray.distanceToPoint(Point3d(0.0, 0.0, -5.0)), 5.0);
}

TEST(RayTest, aBoxHitFromOutsideReportsTheEnteringFace)
{
    RayHitd hit;
    ASSERT_TRUE(unitBox().intersectWith(rayFrom(0.0, 0.0, 5.0, 0.0, 0.0, -1.0), hit));

    EXPECT_DOUBLE_EQ(hit.t, 4.0);
    EXPECT_DOUBLE_EQ(hit.point.z, 1.0);
    EXPECT_DOUBLE_EQ(hit.normal.x, 0.0);
    EXPECT_DOUBLE_EQ(hit.normal.y, 0.0);
    EXPECT_DOUBLE_EQ(hit.normal.z, 1.0);
}

TEST(RayTest, aRayLeavingTheBoxOrPointingAwayIsAMiss)
{
    RayHitd hit;
    EXPECT_FALSE(unitBox().intersectWith(rayFrom(0.0, 0.0, 5.0, 0.0, 0.0, 1.0), hit)) << "the box is behind the origin";
    EXPECT_FALSE(unitBox().intersectWith(rayFrom(5.0, 0.0, 0.0, 0.0, 1.0, 0.0), hit)) << "parallel to a slab and outside it";
}

TEST(RayTest, aRayThatStartsInsideReportsTheWallItSees)
{
    RayHitd hit;
    ASSERT_TRUE(unitBox().intersectWith(rayFrom(0.0, 0.0, 0.0, 0.0, 0.0, 1.0), hit));

    EXPECT_DOUBLE_EQ(hit.t, 0.0);
    // The far wall is met from within, so the normal faces BACK towards the ray: every hit keeps that invariant.
    EXPECT_DOUBLE_EQ(hit.normal.z, -1.0);
}

TEST(RayTest, aTinyButRealComponentStillEntersTheBox)
{
    // The x component is small yet non-zero, and the origin sits just outside the x slab: treating it as "parallel"
    // would reject a ray that does cross the box.
    RayHitd hit;
    ASSERT_TRUE(unitBox().intersectWith(rayFrom(1.0 + 1e-10, 0.0, 5.0, -1e-9, 0.0, -1.0), hit));
    EXPECT_NEAR(hit.point.x, 1.0, 1e-6);
}

TEST(RayTest, anUnrotatedObbAgreesWithTheAxisAlignedBox)
{
    const Obbd box(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 1.0, 1.0));
    const Rayd ray = rayFrom(0.0, 0.0, 5.0, 0.0, 0.0, -1.0);

    RayHitd box_hit;
    RayHitd aabb_hit;
    ASSERT_TRUE(box.intersectWith(ray, box_hit));
    ASSERT_TRUE(unitBox().intersectWith(ray, aabb_hit));

    EXPECT_NEAR(box_hit.t, aabb_hit.t, kTolerance);
    EXPECT_NEAR(box_hit.point.z, aabb_hit.point.z, kTolerance);
    EXPECT_NEAR(box_hit.normal.z, aabb_hit.normal.z, kTolerance);
}

TEST(RayTest, aRotatedObbKeepsItsDistanceAlongTheUnitDirection)
{
    // Half-extent 2 on local X, rotated +90 degrees about Z, so local X lies along world +Y.
    const Obbd box(Point3d(0.0, 0.0, 0.0), Vec3d(2.0, 1.0, 1.0), vn::math::Quaternion<double>(std::numbers::pi / 2.0, Vec3d(0.0, 0.0, 1.0)));

    RayHitd hit;
    ASSERT_TRUE(box.intersectWith(rayFrom(0.0, 5.0, 0.0, 0.0, -1.0, 0.0), hit));

    EXPECT_NEAR(hit.t, 3.0, kTolerance) << "the 2-unit half extent lies on world Y after the rotation";
    EXPECT_NEAR(hit.point.y, 2.0, kTolerance);
    EXPECT_NEAR(hit.normal.y, 1.0, kTolerance);
}

TEST(RayTest, thePlaneReportsItsNormalFacingBackTowardsTheRay)
{
    const Plane<double> plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));

    RayHitd hit;
    ASSERT_TRUE(plane.intersectWith(rayFrom(0.0, 0.0, 5.0, 0.0, 0.0, -1.0), hit));

    EXPECT_DOUBLE_EQ(hit.t, 5.0);
    EXPECT_DOUBLE_EQ(hit.point.z, 0.0);
    EXPECT_DOUBLE_EQ(hit.normal.z, 1.0);
}

TEST(RayTest, aParallelPlaneOrOneBehindTheOriginIsAMiss)
{
    const Plane<double> plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));

    RayHitd hit;
    EXPECT_FALSE(plane.intersectWith(rayFrom(0.0, 0.0, 5.0, 1.0, 0.0, 0.0), hit)) << "parallel to the plane";
    EXPECT_FALSE(plane.intersectWith(rayFrom(0.0, 0.0, 5.0, 0.0, 0.0, 1.0), hit)) << "the plane is behind the origin";
}

TEST(RayTest, aTriangleIsHitFromBothSides)
{
    const Point3d a(0.0, 0.0, 0.0);
    const Point3d b(1.0, 0.0, 0.0);
    const Point3d c(0.0, 1.0, 0.0);

    RayHitd front;
    ASSERT_TRUE(vn::geometry::intersectTriangle(rayFrom(0.2, 0.2, 5.0, 0.0, 0.0, -1.0), a, b, c, front));
    EXPECT_DOUBLE_EQ(front.t, 5.0);
    EXPECT_DOUBLE_EQ(front.normal.z, 1.0);

    RayHitd back;
    ASSERT_TRUE(vn::geometry::intersectTriangle(rayFrom(0.2, 0.2, -5.0, 0.0, 0.0, 1.0), a, b, c, back));
    EXPECT_DOUBLE_EQ(back.t, 5.0);
    EXPECT_DOUBLE_EQ(back.normal.z, -1.0) << "the normal faces back towards the ray";
}

TEST(RayTest, aTriangleMissIsOutsideOrDegenerate)
{
    const Point3d a(0.0, 0.0, 0.0);
    const Point3d b(1.0, 0.0, 0.0);
    const Point3d c(0.0, 1.0, 0.0);

    RayHitd hit;
    EXPECT_FALSE(vn::geometry::intersectTriangle(rayFrom(0.8, 0.8, 5.0, 0.0, 0.0, -1.0), a, b, c, hit)) << "outside the triangle";
    EXPECT_FALSE(vn::geometry::intersectTriangle(rayFrom(0.2, 0.2, 5.0, 0.0, 0.0, -1.0), a, a, a, hit)) << "degenerate triangle";
}

TEST(RayTest, aSmallTriangleIsStillHit)
{
    // The determinant shrinks with the triangle's area, so an absolute tolerance would call a small face unfavourable
    // to a ray that hits it squarely.
    constexpr double kSize = 1e-5;
    const Point3d    a(0.0, 0.0, 0.0);
    const Point3d    b(kSize, 0.0, 0.0);
    const Point3d    c(0.0, kSize, 0.0);

    RayHitd hit;
    ASSERT_TRUE(vn::geometry::intersectTriangle(rayFrom(kSize * 0.25, kSize * 0.25, 1.0, 0.0, 0.0, -1.0), a, b, c, hit));
    EXPECT_NEAR(hit.point.z, 0.0, 1e-12);
}
