#include <gtest/gtest.h>

#include <type_traits>

#include <vine/geometry/Obb.hpp>

using namespace vn::math;
using vn::geometry::Obbd;
using vn::geometry::Obbf;

TEST(Obb, DefaultAndCanonicalizedConstruction)
{
    const Obbf box;
    EXPECT_EQ(box.center(), Point3f(0.0f, 0.0f, 0.0f));
    EXPECT_EQ(box.halfExtents(), Vec3f(0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(box.orientation().isIdentity());

    const Quatf rotation(2.0f, 0.0f, 0.0f, 2.0f);
    const Obbf canonicalized(Point3f(1.0f, 2.0f, 3.0f), Vec3f(-1.0f, 2.0f, -3.0f), rotation);
    EXPECT_EQ(canonicalized.halfExtents(), Vec3f(1.0f, 2.0f, 3.0f));
    EXPECT_NEAR(canonicalized.orientation().length(), 1.0f, 1e-6f);
}

TEST(Obb, ContainsUsesLocalCoordinates)
{
    const Obbf box(Point3f(), Vec3f(1.0f, 2.0f, 3.0f), Quatf(static_cast<float>(PI) / 2.0f, Vec3f(0.0f, 0.0f, 1.0f)));

    EXPECT_FALSE(box.contains(Point3f(0.0f, 1.5f, 0.0f)));
    EXPECT_TRUE(box.contains(Point3f(1.5f, 0.0f, 0.0f)));
    EXPECT_TRUE(box.contains(Point3f(-2.0f, 1.0f, 3.0f)));
}

TEST(Obb, CornersAndAabbEncloseRotatedBox)
{
    const Obbf box(Point3f(2.0f, 3.0f, 4.0f), Vec3f(1.0f, 2.0f, 0.5f), Quatf(static_cast<float>(PI) / 2.0f, Vec3f(0.0f, 0.0f, 1.0f)));
    const auto corners = box.corners();
    const auto bounds = box.toAabb();

    EXPECT_NEAR(corners[0].x, 4.0f, 1e-6f);
    EXPECT_NEAR(corners[0].y, 2.0f, 1e-6f);
    EXPECT_NEAR(corners[0].z, 3.5f, 1e-6f);
    EXPECT_NEAR(corners[7].x, 0.0f, 1e-6f);
    EXPECT_NEAR(corners[7].y, 4.0f, 1e-6f);
    EXPECT_NEAR(corners[7].z, 4.5f, 1e-6f);
    EXPECT_NEAR(bounds.min().x, 0.0f, 1e-6f);
    EXPECT_NEAR(bounds.min().y, 2.0f, 1e-6f);
    EXPECT_NEAR(bounds.min().z, 3.5f, 1e-6f);
    EXPECT_NEAR(bounds.max().x, 4.0f, 1e-6f);
    EXPECT_NEAR(bounds.max().y, 4.0f, 1e-6f);
    EXPECT_NEAR(bounds.max().z, 4.5f, 1e-6f);
}

TEST(Obb, IntersectsIncludingTouchingAndRejectsSeparatedBoxes)
{
    const Obbf a(Point3f(), Vec3f(1.0f, 1.0f, 1.0f));
    const Obbf touching(Point3f(2.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f));
    const Obbf separated(Point3f(2.01f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f));
    const Obbf rotated(Point3f(0.0f, 0.0f, 0.0f), Vec3f(0.5f, 1.0f, 0.5f), Quatf(static_cast<float>(PI) / 4.0f, Vec3f(0.0f, 0.0f, 1.0f)));

    EXPECT_TRUE(a.intersects(touching));
    EXPECT_FALSE(a.intersects(separated));
    EXPECT_TRUE(a.intersects(rotated));
    EXPECT_TRUE(rotated.intersects(a));
}

TEST(Obb, DoublePrecisionTypeIsAvailable)
{
    static_assert(std::is_same_v<Obbd::value_type, double>);

    Obbd box(Point3d(1.0, 2.0, 3.0), Vec3d(0.5, 1.0, 2.0));
    EXPECT_TRUE(box.contains(Point3d(1.5, 2.0, 3.0)));
}

TEST(Obb, GeometryOwnsTheOrientedBoxType)
{
    static_assert(std::is_same_v<Obbf, vn::geometry::Obb<float>>);
    static_assert(std::is_same_v<Obbd, vn::geometry::Obb<double>>);
}
