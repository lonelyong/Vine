#include <gtest/gtest.h>

#include <vine/geom3d/LineCurve.hpp>
#include <vine/geom3d/Plane.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

using namespace vn::math;
using vn::geom3d::LineCurved;
using vn::geom3d::Planed;

TEST(LineCurve, closestPointProjectsOntoTheLineCurve)
{
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const Point3d projected = line.closestPoint(Point3d(3.0, 4.0, 0.0));
    EXPECT_DOUBLE_EQ(projected.x, 3.0);
    EXPECT_DOUBLE_EQ(projected.y, 0.0);
    EXPECT_DOUBLE_EQ(projected.z, 0.0);
}

TEST(LineCurve, skewLineCurvesReportTheClosestPair)
{
    const LineCurved l1(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const LineCurved l2(Point3d(0.0, 1.0, 1.0), Vec3d(0.0, 0.0, 1.0));

    const auto hit = l1.intersectWith(l2);
    EXPECT_FALSE(hit.is_parallel);
    EXPECT_FALSE(hit.is_intersected);
    EXPECT_DOUBLE_EQ(hit.params.distance, 1.0);
    EXPECT_DOUBLE_EQ(hit.params.pt1.x, 0.0);
    EXPECT_DOUBLE_EQ(hit.params.pt2.y, 1.0);
    EXPECT_DOUBLE_EQ(hit.params.pt2.z, 0.0);
}

TEST(LineCurve, parallelAndCollinearFlags)
{
    const LineCurved l1(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto parallel = l1.intersectWith(LineCurved(Point3d(0.0, 2.0, 0.0), Vec3d(1.0, 0.0, 0.0)));
    EXPECT_TRUE(parallel.is_parallel);
    EXPECT_FALSE(parallel.is_collinear);
    EXPECT_FALSE(parallel.is_intersected);
    EXPECT_DOUBLE_EQ(parallel.params.distance, 2.0);
    EXPECT_DOUBLE_EQ(parallel.params.pt1.x, 0.0);
    EXPECT_DOUBLE_EQ(parallel.params.pt2.y, 2.0);

    const auto collinear = l1.intersectWith(LineCurved(Point3d(5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0)));
    EXPECT_TRUE(collinear.is_parallel);
    EXPECT_TRUE(collinear.is_collinear);
    EXPECT_TRUE(collinear.is_intersected);
}

TEST(Plane, lineCrossesThePlaneAtOnePoint)
{
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const LineCurved  line(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0));

    Point3d hit;
    ASSERT_TRUE(plane.intersectWith(line, hit));
    EXPECT_DOUBLE_EQ(hit.x, 1.0);
    EXPECT_DOUBLE_EQ(hit.y, 2.0);
    EXPECT_DOUBLE_EQ(hit.z, 0.0);
}

TEST(Plane, parallelLineCurveHasNoIntersection)
{
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const LineCurved  line(Point3d(1.0, 2.0, 5.0), Vec3d(1.0, 0.0, 0.0));

    Point3d hit;
    EXPECT_FALSE(plane.intersectWith(line, hit));
}

TEST(Plane, twoPlanesMeetAlongALineCurve)
{
    const Planed plane_z(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Planed plane_x(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    // The output line is overwritten by the call; the input only has to be a valid line.
    LineCurved intersection(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    ASSERT_TRUE(plane_z.intersectWith(plane_x, intersection));

    // x = 0 and z = 0: the line runs along Y through the world origin.
    EXPECT_DOUBLE_EQ(intersection.origin.x, 0.0);
    EXPECT_DOUBLE_EQ(intersection.origin.z, 0.0);
    EXPECT_DOUBLE_EQ(intersection.direction.x, 0.0);
    EXPECT_DOUBLE_EQ(intersection.direction.z, 0.0);
    EXPECT_NE(intersection.direction.y, 0.0);

    // A parallel pair has no intersection line.
    const Planed parallel(Point3d(0.0, 0.0, 3.0), Vec3d(0.0, 0.0, 1.0));
    EXPECT_FALSE(plane_z.intersectWith(parallel, intersection));
}

TEST(Plane, coincidentPlanesReportNoIntersectionLineCurve)
{
    // Two descriptions of the same plane intersect in a whole plane, and a plane has no result type here: the entry
    // answers `false` instead of inventing a line to stand for it (geometry-intersection.md N1).
    const Planed plane_z(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Planed same_plane(Point3d(4.0, -1.0, 0.0), Vec3d(0.0, 0.0, 5.0));

    LineCurved leftover(Point3d(7.0, 7.0, 7.0), Vec3d(0.0, 1.0, 0.0));
    EXPECT_FALSE(plane_z.intersectWith(same_plane, leftover));

    // The output was not touched, so its previous contents are still there - that is the contract of a `false` result.
    EXPECT_DOUBLE_EQ(leftover.origin.x, 7.0);
    EXPECT_DOUBLE_EQ(leftover.direction.y, 1.0);
}
