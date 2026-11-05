#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include <vine/geom3d/Obb.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Vector3.hpp>

using namespace vn::math;
using vn::geom3d::Obbd;

namespace {

std::vector<Vec3d> cornersOfBox(const Vec3d& half_extents, const Quatd& orientation, const Vec3d& center)
{
    std::vector<Vec3d> points;
    points.reserve(8);
    for (int i = 0; i < 8; ++i) {
        const Vec3d local((i & 1) ? half_extents.x : -half_extents.x,
                          (i & 2) ? half_extents.y : -half_extents.y,
                          (i & 4) ? half_extents.z : -half_extents.z);
        points.push_back(center + orientation * local);
    }
    return points;
}

std::array<double, 3> sortedExtents(const Vec3d& extents)
{
    std::array<double, 3> values{ extents.x, extents.y, extents.z };
    std::sort(values.begin(), values.end());
    return values;
}

void expectEncloses(const Obbd& box, const std::vector<Vec3d>& points)
{
    for (const auto& point : points) {
        EXPECT_TRUE(box.contains(Point3d(point.x, point.y, point.z)));
    }
}

} // namespace

TEST(ObbFit, pcaRecoversARotatedBox)
{
    const Quatd        rotation(PI / 6.0, Vec3d(0.0, 0.0, 1.0));
    const Vec3d        half_extents(3.0, 2.0, 1.0);
    const Vec3d        center(1.0, 2.0, 3.0);
    const std::vector<Vec3d> points = cornersOfBox(half_extents, rotation, center);

    const Obbd box = Obbd::computePca(points);

    EXPECT_NEAR(box.center().x, center.x, 1e-9);
    EXPECT_NEAR(box.center().y, center.y, 1e-9);
    EXPECT_NEAR(box.center().z, center.z, 1e-9);

    const auto extents = sortedExtents(box.halfExtents());
    EXPECT_NEAR(extents[0], 1.0, 1e-9);
    EXPECT_NEAR(extents[1], 2.0, 1e-9);
    EXPECT_NEAR(extents[2], 3.0, 1e-9);

    EXPECT_NEAR(box.orientation().length(), 1.0, 1e-12);
    expectEncloses(box, points);
}

TEST(ObbFit, pcaPutsTheLargestVarianceOnLocalX)
{
    const Vec3d              half_extents(5.0, 1.0, 1.0);
    const std::vector<Vec3d> points = cornersOfBox(half_extents, Quatd(), Vec3d());

    const Obbd box = Obbd::computePca(points);

    // Eigenvalues ascend, so the major axis becomes local X and the two minor axes follow.
    EXPECT_NEAR(box.halfExtents().x, 5.0, 1e-9);
    const auto minor = sortedExtents(box.halfExtents());
    EXPECT_NEAR(minor[0], 1.0, 1e-9);
    EXPECT_NEAR(minor[1], 1.0, 1e-9);
    expectEncloses(box, points);
}

TEST(ObbFit, pcaCollinearPointsKeepTheMajorAxis)
{
    const std::vector<Vec3d> points{ Vec3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0), Vec3d(2.0, 0.0, 0.0) };

    const Obbd box = Obbd::computePca(points);

    EXPECT_NEAR(box.center().x, 1.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().x, 1.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().y, 0.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().z, 0.0, 1e-9);
    expectEncloses(box, points);
}

TEST(ObbFit, pcaSinglePointIsAZeroBoxAtThatPoint)
{
    const std::vector<Vec3d> points{ Vec3d(2.0, -3.0, 4.0) };

    const Obbd box = Obbd::computePca(points);

    EXPECT_NEAR(box.center().x, 2.0, 1e-9);
    EXPECT_NEAR(box.center().y, -3.0, 1e-9);
    EXPECT_NEAR(box.center().z, 4.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().x, 0.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().y, 0.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().z, 0.0, 1e-9);
    EXPECT_TRUE(box.orientation().isIdentity()) << "a point carries no direction: the axes fall back to world";
    expectEncloses(box, points);
}

TEST(ObbFit, pcaCoincidentVerticesKeepIdentityOrientation)
{
    // One coordinate repeated is the same degenerate case as a single vertex, not a different one.
    const std::vector<Vec3d> points{ Vec3d(2.0, -3.0, 4.0), Vec3d(2.0, -3.0, 4.0), Vec3d(2.0, -3.0, 4.0) };

    const Obbd box = Obbd::computePca(points);

    EXPECT_NEAR(box.center().x, 2.0, 1e-9);
    EXPECT_NEAR(box.center().y, -3.0, 1e-9);
    EXPECT_NEAR(box.center().z, 4.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().x, 0.0, 1e-9);
    EXPECT_TRUE(box.orientation().isIdentity());
    expectEncloses(box, points);
}

TEST(ObbFit, pcaEmptyInputReturnsTheDefaultBox)
{
    const Obbd box = Obbd::computePca(std::vector<Vec3d>{});

    EXPECT_DOUBLE_EQ(box.center().x, 0.0);
    EXPECT_DOUBLE_EQ(box.halfExtents().x, 0.0);
    EXPECT_TRUE(box.orientation().isIdentity());
}

TEST(ObbFit, fixedOrientationRecoversAnObliqueBox)
{
    const Vec3d x_axis = Vec3d(1.0, 1.0, 0.0).normalized();
    const Vec3d y_axis = Vec3d(-1.0, 1.0, 0.0).normalized();
    const Vec3d z_axis = x_axis.cross(y_axis);

    const Vec3d half_extents(3.0, 2.0, 1.0);
    const Vec3d center(1.0, 2.0, 3.0);

    std::vector<Vec3d> points;
    points.reserve(8);
    for (int i = 0; i < 8; ++i) {
        points.push_back(center + x_axis * ((i & 1) ? half_extents.x : -half_extents.x) +
                         y_axis * ((i & 2) ? half_extents.y : -half_extents.y) +
                         z_axis * ((i & 4) ? half_extents.z : -half_extents.z));
    }

    // Non-unit X and a Y that is not perpendicular: both are repaired by the fit.
    const Obbd box = Obbd::computeFixedOrientation(points, x_axis * 4.0, y_axis + x_axis * 0.5);

    EXPECT_NEAR(box.center().x, center.x, 1e-9);
    EXPECT_NEAR(box.center().y, center.y, 1e-9);
    EXPECT_NEAR(box.center().z, center.z, 1e-9);
    EXPECT_NEAR(box.halfExtents().x, 3.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().y, 2.0, 1e-9);
    EXPECT_NEAR(box.halfExtents().z, 1.0, 1e-9);
    expectEncloses(box, points);
}

TEST(ObbFit, fixedOrientationWithAParallelYStillEnclosesEveryPoint)
{
    const std::vector<Vec3d> points = cornersOfBox(Vec3d(2.0, 3.0, 4.0), Quatd(), Vec3d(5.0, 5.0, 5.0));

    // y parallel to x is degenerate: the fit keeps X and picks another axis, but it must still bound the points.
    const Obbd box = Obbd::computeFixedOrientation(points, Vec3d(1.0, 0.0, 0.0), Vec3d(2.0, 0.0, 0.0));

    expectEncloses(box, points);
    EXPECT_NEAR(box.orientation().length(), 1.0, 1e-12);
}

TEST(ObbFit, fixedOrientationEmptyInputReturnsTheDefaultBox)
{
    const Obbd box = Obbd::computeFixedOrientation(std::vector<Vec3d>{}, Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0));

    EXPECT_DOUBLE_EQ(box.center().x, 0.0);
    EXPECT_DOUBLE_EQ(box.halfExtents().x, 0.0);
    EXPECT_TRUE(box.orientation().isIdentity());
}
