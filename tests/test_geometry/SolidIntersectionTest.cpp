#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <vine/geometry/Primitive.hpp>
#include <vine/geometry/Ray.hpp>
#include <vine/geometry/RayHit.hpp>
#include <vine/geometry/Solid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geometry::BoxSolidd;
using vn::geometry::ConeSolidd;
using vn::geometry::CylinderSolidd;
using vn::geometry::EllipsoidSolidd;
using vn::geometry::Primitive;
using vn::geometry::Rayd;
using vn::geometry::SphereSolidd;
using vn::math::Point3d;
using vn::math::Vec3d;

/// Every reported normal faces back towards the ray, exactly as `RayHit` defines.
template <typename Hits, typename Ray>
void expectNormalsFaceTheRay(const Hits& hits, const Ray& ray)
{
    for (const auto& hit : hits) {
        EXPECT_LE(hit.normal.dot(ray.direction), 1e-6) << "a normal points away from the ray";
    }
}

/// The same sphere crossing at both precisions the library exports.
template <vn::math::FP T>
void checkSphereAtBothPrecisions()
{
    const vn::geometry::SphereSolid<T>              sphere{ T(1) };
    const vn::geometry::Ray<T>                 ray(vn::math::Point3<T>(T(-5), T(0), T(0)), vn::math::Vector3<T>(T(1), T(0), T(0)));
    const std::vector<vn::geometry::RayHit<T>> hits = sphere.intersectWith(ray);

    ASSERT_EQ(hits.size(), 2U);
    const double tolerance = (T(1) == T(1.0f)) ? 1e-4 : 1e-9;
    EXPECT_NEAR(static_cast<double>(hits[0].t), 4.0, tolerance);
    EXPECT_NEAR(static_cast<double>(hits[1].t), 6.0, tolerance);
}

} // namespace

TEST(SolidIntersection, aRayCrossesABoxTwice)
{
    // A box is a slab: entering at x = -1 and leaving at x = +1; both normals face back towards the ray.
    const BoxSolidd box{ 1.0, 1.0, 1.0 };
    const Rayd ray(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto hits = box.intersectWith(ray);

    ASSERT_EQ(hits.size(), 2U);
    EXPECT_NEAR(hits[0].t, 4.0, 1e-6);
    EXPECT_NEAR(hits[1].t, 6.0, 1e-6);
    EXPECT_LT((hits[0].point - Point3d(-1.0, 0.0, 0.0)).length(), 1e-6);
    EXPECT_LT((hits[1].point - Point3d(1.0, 0.0, 0.0)).length(), 1e-6);
    EXPECT_LT((hits[0].normal - Vec3d(-1.0, 0.0, 0.0)).length(), 1e-6);
    EXPECT_LT((hits[1].normal - Vec3d(-1.0, 0.0, 0.0)).length(), 1e-6);
    expectNormalsFaceTheRay(hits, ray);
}

TEST(SolidIntersection, aRayFromInsideASolidReportsOnlyTheExit)
{
    // The entering crossing is behind the origin, so only the boundary ahead of it is a hit.
    const BoxSolidd box{ 1.0, 1.0, 1.0 };
    const Rayd ray(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto hits = box.intersectWith(ray);

    ASSERT_EQ(hits.size(), 1U);
    EXPECT_NEAR(hits[0].t, 1.0, 1e-6);
    EXPECT_LT((hits[0].normal - Vec3d(-1.0, 0.0, 0.0)).length(), 1e-6);
}

TEST(SolidIntersection, aRayCrossesASphereTwiceAndMissesItBeside)
{
    const SphereSolidd sphere = SphereSolidd{ 1.0 };
    const Rayd    through(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto    hits = sphere.intersectWith(through);
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_NEAR(hits[0].t, 4.0, 1e-6);
    EXPECT_NEAR(hits[1].t, 6.0, 1e-6);
    expectNormalsFaceTheRay(hits, through);

    const Rayd beside(Point3d(-5.0, 3.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    EXPECT_TRUE(sphere.intersectWith(beside).empty());
}

TEST(SolidIntersection, aTangentialRayTouchesASphereOnce)
{
    // y = 1 is exactly the radius: the quadratic has a double root at x = 0, and the normal is perpendicular to the
    // ray, so it is neither flipped nor lost.
    const SphereSolidd sphere = SphereSolidd{ 1.0 };
    const Rayd    tangent(Point3d(-5.0, 1.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto    hits = sphere.intersectWith(tangent);

    ASSERT_EQ(hits.size(), 1U);
    EXPECT_NEAR(hits[0].t, 5.0, 1e-6);
    EXPECT_LT((hits[0].normal - Vec3d(0.0, 1.0, 0.0)).length(), 1e-6);
}

TEST(SolidIntersection, anEllipsoidUsesItsThreeSemiAxes)
{
    // Along X the crossings are at the X semi-axis, along Y at the Y one: the scaled quadratic does the work.
    const EllipsoidSolidd ellipsoid{ 1.0, 2.0, 3.0 };

    const Rayd along_x(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto hits_x = ellipsoid.intersectWith(along_x);
    ASSERT_EQ(hits_x.size(), 2U);
    EXPECT_NEAR(hits_x[0].t, 4.0, 1e-6);
    EXPECT_NEAR(hits_x[1].t, 6.0, 1e-6);

    const Rayd along_y(Point3d(0.0, -5.0, 0.0), Vec3d(0.0, 1.0, 0.0));
    const auto hits_y = ellipsoid.intersectWith(along_y);
    ASSERT_EQ(hits_y.size(), 2U);
    EXPECT_NEAR(hits_y[0].t, 3.0, 1e-6);
    EXPECT_NEAR(hits_y[1].t, 7.0, 1e-6);
}

TEST(SolidIntersection, aCylinderIsHitOnItsSideAndOnItsCaps)
{
    const CylinderSolidd cylinder{ 1.0, 1.0 };

    // Across the axis at mid height: two side crossings at the radius.
    const Rayd across(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto side = cylinder.intersectWith(across);
    ASSERT_EQ(side.size(), 2U);
    EXPECT_NEAR(side[0].t, 4.0, 1e-6);
    EXPECT_NEAR(side[1].t, 6.0, 1e-6);
    EXPECT_NEAR(std::abs(side[0].normal.z), 0.0, 1e-6);
    expectNormalsFaceTheRay(side, across);

    // Down the axis: the two caps at z = +/- 1.
    const Rayd down(Point3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const auto caps = cylinder.intersectWith(down);
    ASSERT_EQ(caps.size(), 2U);
    EXPECT_NEAR(caps[0].t, 4.0, 1e-6);
    EXPECT_NEAR(caps[1].t, 6.0, 1e-6);
    EXPECT_NEAR(caps[0].normal.z, 1.0, 1e-6);
    EXPECT_NEAR(caps[1].normal.z, 1.0, 1e-6);
}

TEST(SolidIntersection, aConeIsHitOnItsSideAndItsBase)
{
    // At mid height the cone's radius is half the base radius; a horizontal ray therefore meets the side at
    // x = -0.5 and x = +0.5.
    const ConeSolidd cone = ConeSolidd{ 1.0, 1.0 };
    const Rayd  across(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto  side = cone.intersectWith(across);
    ASSERT_EQ(side.size(), 2U);
    EXPECT_NEAR(side[0].t, 4.5, 1e-6);
    EXPECT_NEAR(side[1].t, 5.5, 1e-6);

    // The side normal is (x, y, k^2 (top - z)) normalised; with r = 1, h = 2 that is a z component of
    // 0.25 / sqrt(0.25^2 + 0.5^2) = 1 / sqrt(5).
    const double expected_axial = 1.0 / std::sqrt(5.0);
    EXPECT_NEAR(std::abs(side[0].normal.z), expected_axial, 1e-6);
    expectNormalsFaceTheRay(side, across);

    // Off the axis, a vertical ray meets the base and then the side.
    const Rayd upward(Point3d(0.3, 0.0, -5.0), Vec3d(0.0, 0.0, 1.0));
    const auto both = cone.intersectWith(upward);
    ASSERT_EQ(both.size(), 2U);
    EXPECT_NEAR(both[0].t, 4.0, 1e-6);
    EXPECT_NEAR(both[0].normal.z, -1.0, 1e-6);
    EXPECT_NEAR(both[1].t, 5.4, 1e-6);
    EXPECT_LT(both[1].normal.z, 0.0);
    expectNormalsFaceTheRay(both, upward);
}

TEST(SolidIntersection, unusableInputsAnswerNothing)
{
    const SphereSolidd sphere = SphereSolidd{ 1.0 };

    const Rayd aimless(Point3d(-5.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0));
    EXPECT_TRUE(sphere.intersectWith(aimless).empty());

    const SphereSolidd collapsed{ 0.0 };
    const Rayd    ray(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    EXPECT_TRUE(collapsed.intersectWith(ray).empty());
}

TEST(SolidIntersection, aModelReachesTheSameHitsThroughItsSolid)
{
    // The modeling type has no ray query of its own: its analytic solid answers.
    const vn::geometry::Sphere sphere(1.0f);

    const auto hits = sphere.solid().intersectWith(vn::geometry::Rayf(vn::math::Point3f(-5.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f)));
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_NEAR(hits[0].t, 4.0f, 1e-5f);
    EXPECT_NEAR(hits[1].t, 6.0f, 1e-5f);

    // A degenerate model still carries its kind; it is the geometry that refuses the query.
    const vn::geometry::Cylinder collapsed(0.0f, 2.0f);
    EXPECT_FALSE(collapsed.isValid());
    const auto refused = collapsed.solid().intersectWith(vn::geometry::Rayf(vn::math::Point3f(-5.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(refused.empty());
}

TEST(SolidIntersection, bothPrecisionsAgreeOnASphere)
{
    checkSphereAtBothPrecisions<double>();
    checkSphereAtBothPrecisions<float>();
}
