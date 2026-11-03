#include <gtest/gtest.h>

#include <vine/geometry/Line.hpp>
#include <vine/geometry/Plane.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

using namespace vn::math;
using vn::geometry::Lined;
using vn::geometry::Planed;

TEST(Line, closestPointProjectsOntoTheLine)
{
    const Lined line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const Point3d projected = line.closestPoint(Point3d(3.0, 4.0, 0.0));
    EXPECT_DOUBLE_EQ(projected.x, 3.0);
    EXPECT_DOUBLE_EQ(projected.y, 0.0);
    EXPECT_DOUBLE_EQ(projected.z, 0.0);
}

TEST(Line, skewLinesReportTheClosestPair)
{
    const Lined l1(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const Lined l2(Point3d(0.0, 1.0, 1.0), Vec3d(0.0, 0.0, 1.0));

    const auto hit = l1.intersectWith(l2);
    EXPECT_FALSE(hit.is_parallel);
    EXPECT_FALSE(hit.is_intersected);
    EXPECT_DOUBLE_EQ(hit.params.distance, 1.0);
    EXPECT_DOUBLE_EQ(hit.params.pt1.x, 0.0);
    EXPECT_DOUBLE_EQ(hit.params.pt2.y, 1.0);
    EXPECT_DOUBLE_EQ(hit.params.pt2.z, 0.0);
}

TEST(Line, parallelAndCollinearFlags)
{
    const Lined l1(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto parallel = l1.intersectWith(Lined(Point3d(0.0, 2.0, 0.0), Vec3d(1.0, 0.0, 0.0)));
    EXPECT_TRUE(parallel.is_parallel);
    EXPECT_FALSE(parallel.is_collinear);
    EXPECT_FALSE(parallel.is_intersected);
    EXPECT_DOUBLE_EQ(parallel.params.distance, 2.0);
    EXPECT_DOUBLE_EQ(parallel.params.pt1.x, 0.0);
    EXPECT_DOUBLE_EQ(parallel.params.pt2.y, 2.0);

    const auto collinear = l1.intersectWith(Lined(Point3d(5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0)));
    EXPECT_TRUE(collinear.is_parallel);
    EXPECT_TRUE(collinear.is_collinear);
    EXPECT_TRUE(collinear.is_intersected);
}

TEST(Plane, lineCrossesThePlaneAtOnePoint)
{
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Lined  line(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0));

    Point3d hit;
    ASSERT_TRUE(plane.intersectWith(line, hit));
    EXPECT_DOUBLE_EQ(hit.x, 1.0);
    EXPECT_DOUBLE_EQ(hit.y, 2.0);
    EXPECT_DOUBLE_EQ(hit.z, 0.0);
}

TEST(Plane, parallelLineHasNoIntersection)
{
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Lined  line(Point3d(1.0, 2.0, 5.0), Vec3d(1.0, 0.0, 0.0));

    Point3d hit;
    EXPECT_FALSE(plane.intersectWith(line, hit));
}

TEST(Plane, twoPlanesMeetAlongALine)
{
    const Planed plane_z(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Planed plane_x(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    // The output line is overwritten by the call; the input only has to be a valid line.
    Lined intersection(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
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
