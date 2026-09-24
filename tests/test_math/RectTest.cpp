#include <gtest/gtest.h>

#include <span>
#include <type_traits>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Rect3.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

using namespace vn::math;

TEST(Rect2, minMax)
{
    // Construct with (xmin, ymin, xmax, ymax).
    Rect2d r(-3.0, 3.0, 2.0, 10.0);

    auto mn = r.min();
    auto mx = r.max();

    EXPECT_DOUBLE_EQ(mn.x, -3.0);
    EXPECT_DOUBLE_EQ(mn.y,  3.0);
    EXPECT_DOUBLE_EQ(mx.x,  2.0);
    EXPECT_DOUBLE_EQ(mx.y, 10.0);

    EXPECT_DOUBLE_EQ(r.width(),  5.0);
    EXPECT_DOUBLE_EQ(r.height(), 7.0);

    auto c = r.center();
    EXPECT_DOUBLE_EQ(c.x, -0.5);
    EXPECT_DOUBLE_EQ(c.y,  6.5);
}

TEST(Rect2, intersectWith)
{
    Rect2d a(0.0, 0.0, 10.0, 10.0);
    Rect2d b(5.0, -2.0, 15.0, 3.0);
    Rect2d i;
    bool   ok = a.intersectWith(b, i);

    EXPECT_TRUE(ok);
    auto mn = i.min();
    auto mx = i.max();
    EXPECT_DOUBLE_EQ(mn.x, 5.0);
    EXPECT_DOUBLE_EQ(mn.y, 0.0);
    EXPECT_DOUBLE_EQ(mx.x, 10.0);
    EXPECT_DOUBLE_EQ(mx.y, 3.0);

    // disjoint → returns false, out = gap rect (normalized)
    Rect2d c(20.0, 20.0, 22.0, 22.0);
    ok = a.intersectWith(c, i);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(i.isNeg());
    EXPECT_DOUBLE_EQ(i.min().x, 10.0);
    EXPECT_DOUBLE_EQ(i.max().x, 20.0);
}

TEST(Rect3, expandBy)
{
    Rect3d r1;

    r1.expandBy(Point3d(1, 10, 10));
    r1.expandBy(Point3d(-1, 15, -6));

    Rect3d r2(-6.0, -6.0, -6.0, 4.0, 4.0, 4.0);
    r1.expandBy(r2);

    auto mn = r1.min();
    auto mx = r1.max();

    EXPECT_DOUBLE_EQ(mn.x, -6.0);
    EXPECT_DOUBLE_EQ(mn.y, -6.0);
    EXPECT_DOUBLE_EQ(mn.z, -6.0);
    EXPECT_DOUBLE_EQ(mx.x,  4.0);
    EXPECT_DOUBLE_EQ(mx.y, 15.0);
    EXPECT_DOUBLE_EQ(mx.z, 10.0);
}

TEST(Rect3, intersectWith)
{
    Rect3d a(0.0, 0.0, 0.0, 10.0, 10.0, 10.0);
    Rect3d b(5.0, -2.0, 3.0, 15.0, 3.0, 13.0);
    Rect3d i;
    bool   ok = a.intersectWith(b, i);

    EXPECT_TRUE(ok);
    auto mn = i.min();
    auto mx = i.max();
    EXPECT_DOUBLE_EQ(mn.x, 5.0);
    EXPECT_DOUBLE_EQ(mn.y, 0.0);
    EXPECT_DOUBLE_EQ(mn.z, 3.0);
    EXPECT_DOUBLE_EQ(mx.x, 10.0);
    EXPECT_DOUBLE_EQ(mx.y, 3.0);
    EXPECT_DOUBLE_EQ(mx.z, 10.0);

    // disjoint → returns false, out = gap box (normalized)
    Rect3d c(20.0, 20.0, 20.0, 22.0, 22.0, 22.0);
    ok = a.intersectWith(c, i);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(i.isNeg());
    EXPECT_DOUBLE_EQ(i.min().x, 10.0);
    EXPECT_DOUBLE_EQ(i.max().x, 20.0);
}

TEST(Rect3, computeEnclosesPoints)
{
    std::vector<Vec3d> pts{ Vec3d(-1.0, 2.0, 3.0), Vec3d(4.0, -5.0, 6.0), Vec3d(7.0, 8.0, -9.0) };

    Rect3d box = Rect3d::compute(pts);
    EXPECT_DOUBLE_EQ(box.min().x, -1.0);
    EXPECT_DOUBLE_EQ(box.min().y, -5.0);
    EXPECT_DOUBLE_EQ(box.min().z, -9.0);
    EXPECT_DOUBLE_EQ(box.max().x, 7.0);
    EXPECT_DOUBLE_EQ(box.max().y, 8.0);
    EXPECT_DOUBLE_EQ(box.max().z, 6.0);
    EXPECT_FALSE(box.isNeg());

    // Empty input yields the default (zero) box.
    Rect3d empty = Rect3d::compute(std::span<const Vec3d>{});
    EXPECT_DOUBLE_EQ(empty.min().x, 0.0);
    EXPECT_DOUBLE_EQ(empty.max().z, 0.0);
}

TEST(Rect3, AabbAliasIsSameType)
{
    static_assert(std::is_same_v<Aabb<double>, Rect3<double>>);
    static_assert(std::is_same_v<Aabb<float>, Rect3<float>>);
    static_assert(std::is_same_v<Aabbd, Rect3<double>>);
    static_assert(std::is_same_v<Aabbf, Rect3<float>>);
    static_assert(std::is_same_v<Aabbi, Rect3<int32_t>>);
    static_assert(std::is_same_v<Aabbd, Aabb<double>>);

    Aabb<double> box(-1.0, -2.0, -3.0, 4.0, 5.0, 6.0);
    EXPECT_DOUBLE_EQ(box.center().x, 1.5);
    EXPECT_DOUBLE_EQ(box.center().y, 1.5);
    EXPECT_DOUBLE_EQ(box.center().z, 1.5);
}

TEST(Rect3, emptySentinelExpand)
{
    // Default-constructed box is a zero box at the origin (valid).
    Rect3d zero;
    EXPECT_TRUE(zero.isValid());
    EXPECT_FALSE(zero.isEmpty());

    // empty() starts an accumulation; it is invalid until expanded.
    Aabbd box = Aabbd::empty();
    EXPECT_TRUE(box.isEmpty());
    EXPECT_FALSE(box.isValid());

    // expandBy of a single vector turns the sentinel into that point box.
    box.expandBy(Vec3d(2.0, -3.0, 5.0));
    EXPECT_FALSE(box.isEmpty());
    EXPECT_TRUE(box.isValid());
    EXPECT_DOUBLE_EQ(box.min().x, 2.0);
    EXPECT_DOUBLE_EQ(box.max().y, -3.0);

    box.expandBy(Vec3d(-1.0, 0.0, 9.0));
    EXPECT_DOUBLE_EQ(box.min().x, -1.0);
    EXPECT_DOUBLE_EQ(box.min().y, -3.0);
    EXPECT_DOUBLE_EQ(box.max().z, 9.0);

    // Merging an empty box into a valid one is a no-op.
    Aabbd merged = box;
    merged.expandBy(Aabbd::empty());
    EXPECT_EQ(merged, box);
}
