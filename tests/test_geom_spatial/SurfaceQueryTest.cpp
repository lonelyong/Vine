#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <vine/geom/spatial/BezierSurface.hpp>
#include <vine/geom/spatial/CylindricalSurface.hpp>
#include <vine/geom/spatial/Plane.hpp>
#include <vine/geom/spatial/RaySurfaceIntersection.hpp>
#include <vine/geom/spatial/SphericalSurface.hpp>
#include <vine/geom/spatial/SurfaceProjection.hpp>
#include <vine/geom/spatial/ToroidalSurface.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geom::spatial::BezierSurfaced;
using vn::geom::spatial::closestPointOnSurface;
using vn::geom::spatial::CylindricalSurfaced;
using vn::geom::spatial::intersectRayWithSurface;
using vn::geom::spatial::Planed;
using vn::geom::spatial::Planef;
using vn::geom::spatial::Rayd;
using vn::geom::spatial::Rayf;
using vn::geom::spatial::RayHitd;
using vn::geom::spatial::SphericalSurfaced;
using vn::geom::spatial::SurfaceParameterWindow;
using vn::geom::spatial::ToroidalSurfaced;
using vn::math::Point3d;
using vn::math::Vec3d;

/// The distances in these cases are exact integers, so a projection that only lands near the minimum is visible.
constexpr double kPlaneTolerance = 1e-8;

/// The same ray through a sphere at both precisions the library exports.
template <vn::math::FP T>
void checkSphereRayAtBothPrecisions()
{
    const vn::geom::spatial::SphericalSurface<T> ball(vn::math::Point3<T>(T(0), T(0), T(0)), T(1));
    const vn::geom::spatial::Ray<T>              ray(vn::math::Point3<T>(T(-5), T(0), T(0)), vn::math::Vector3<T>(T(1), T(0), T(0)));

    const auto result = intersectRayWithSurface<T>(ray, ball);
    ASSERT_FALSE(result.coincident);
    ASSERT_EQ(result.hits.size(), 2U);

    const T tolerance = (T(1) == T(1.0f)) ? T(1e-3) : T(1e-9);
    EXPECT_NEAR(static_cast<double>(result.hits[0].hit.t), 4.0, static_cast<double>(tolerance));
    EXPECT_NEAR(static_cast<double>(result.hits[1].hit.t), 6.0, static_cast<double>(tolerance));
    for (const auto& hit : result.hits) {
        EXPECT_NEAR(static_cast<double>((hit.hit.point - ball.centre).length()), 1.0, static_cast<double>(tolerance));
        EXPECT_LE(hit.hit.normal.dot(ray.direction), T(0));
    }
}

} // namespace

TEST(SurfaceProjection, aPointProjectsOntoAPlaneAtItsFoot)
{
    // The plane sits at z = 3 whatever the length of its normal; the query is 5 above it, and its foot is the point
    // with the same x / y.
    const Planed plane(Point3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 2.0));
    const auto   projection = closestPointOnSurface(plane, Point3d(4.0, 6.0, 8.0), SurfaceParameterWindow<double>{ -20.0, 20.0, -20.0, 20.0 });

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->distance, 5.0, kPlaneTolerance);
    EXPECT_NEAR(projection->point.z, 3.0, kPlaneTolerance);
    EXPECT_NEAR(projection->point.x, 4.0, kPlaneTolerance);
    EXPECT_NEAR(projection->point.y, 6.0, kPlaneTolerance);
    EXPECT_LT((plane.evaluate(projection->u, projection->v) - projection->point).length(), 1e-12);
}

TEST(SurfaceProjection, anUnboundedDirectionNeedsAWindow)
{
    // A plane's own domain is infinite, so there is no rectangle to sample and the query refuses instead of inventing
    // one. A window that is empty or reversed is refused the same way.
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));

    EXPECT_FALSE(closestPointOnSurface(plane, Point3d(1.0, 2.0, 3.0)).has_value());
    EXPECT_FALSE(closestPointOnSurface(plane, Point3d(1.0, 2.0, 3.0), SurfaceParameterWindow<double>{ 5.0, 5.0, -1.0, 1.0 }).has_value());
    EXPECT_FALSE(closestPointOnSurface(plane, Point3d(1.0, 2.0, 3.0), SurfaceParameterWindow<double>{ 10.0, -10.0, -1.0, 1.0 }).has_value());
}

TEST(SurfaceProjection, aPointProjectsOntoASphereAlongItsRadius)
{
    // The sphere's domain is bounded, so it needs no window; the nearest point is on the line to the centre.
    const SphericalSurfaced ball(Point3d(1.0, 0.0, 0.0), 2.0);
    const auto              projection = closestPointOnSurface(ball, Point3d(1.0, 0.0, 5.0));

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->distance, 3.0, 1e-8);
    EXPECT_LT((projection->point - Point3d(1.0, 0.0, 2.0)).length(), 1e-8);
}

TEST(SurfaceProjection, aPointProjectsOntoACylinderThroughItsAxis)
{
    // A cylinder is unbounded along its axis, so the height has to be narrowed by the caller.
    const CylindricalSurfaced tube(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const auto projection = closestPointOnSurface(tube, Point3d(5.0, 0.0, 3.0), SurfaceParameterWindow<double>{ -vn::math::PI, vn::math::PI, -10.0, 10.0 });

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->distance, 3.0, 1e-7);
    EXPECT_LT((projection->point - Point3d(2.0, 0.0, 3.0)).length(), 1e-6);
}

TEST(SurfaceProjection, anInfiniteEndTakesTheSurfacesOwnBound)
{
    // -infinity / +infinity mean "leave that side natural": the cylinder's azimuth is bounded by the surface itself,
    // only its height has to be narrowed.
    const CylindricalSurfaced tube(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const auto                projection =
        closestPointOnSurface(tube,
                              Point3d(5.0, 0.0, 3.0),
                              SurfaceParameterWindow<double>{ -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), -10.0, 10.0 });

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->distance, 3.0, 1e-7);
    EXPECT_LT((projection->point - Point3d(2.0, 0.0, 3.0)).length(), 1e-6);
}

TEST(SurfaceProjection, theNearestPointOnATorusIsTheBestLocalMinimum)
{
    // A torus has a local minimum on each side of the tube; the grid has to start the refinement near the global one.
    const ToroidalSurfaced ring(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 3.0, 1.0);

    const auto above_tube = closestPointOnSurface(ring, Point3d(3.0, 0.0, 5.0));
    ASSERT_TRUE(above_tube.has_value());
    EXPECT_NEAR(above_tube->distance, 4.0, 1e-6);
    EXPECT_LT((above_tube->point - Point3d(3.0, 0.0, 1.0)).length(), 1e-6);

    // On the axis the nearest point is the inner equator, sqrt(10) - 1 away.
    const auto on_axis = closestPointOnSurface(ring, Point3d(0.0, 0.0, 1.0));
    ASSERT_TRUE(on_axis.has_value());
    EXPECT_NEAR(on_axis->distance, std::sqrt(10.0) - 1.0, 1e-6);
}

TEST(SurfaceProjection, bothPrecisionsFindTheSameNearestPoint)
{
    const vn::geom::spatial::SphericalSurface<float> ball(vn::math::Point3<float>(0, 0, 0), 1.0f);
    const auto                                  projection = closestPointOnSurface(ball, vn::math::Point3<float>(0, 0, 3));

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(static_cast<double>(projection->distance), 2.0, 1e-4);
    EXPECT_NEAR(static_cast<double>(projection->point.z), 1.0, 1e-4);
}

TEST(RaySurface, aRayCrossesAPlaneLikeTheAnalyticEntry)
{
    // The plane's own entry is the reference: the family-level search has to land on the same crossing.
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Rayd   ray(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const auto   window = SurfaceParameterWindow<double>{ -10.0, 10.0, -10.0, 10.0 };

    RayHitd analytic;
    ASSERT_TRUE(plane.intersectWith(ray, analytic));

    const auto result = intersectRayWithSurface(ray, plane, window);
    ASSERT_FALSE(result.coincident);
    ASSERT_EQ(result.hits.size(), 1U);
    EXPECT_NEAR(result.hits.front().hit.t, analytic.t, 1e-8);
    EXPECT_LT((result.hits.front().hit.point - analytic.point).length(), 1e-8);
    EXPECT_LE(result.hits.front().hit.normal.dot(ray.direction), 0.0);
}

TEST(RaySurface, aRayThroughASphereIsHitTwice)
{
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 1.0);
    const Rayd              ray(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto result = intersectRayWithSurface(ray, ball);
    ASSERT_FALSE(result.coincident);
    ASSERT_EQ(result.hits.size(), 2U);
    EXPECT_NEAR(result.hits[0].hit.t, 4.0, 1e-8);
    EXPECT_NEAR(result.hits[1].hit.t, 6.0, 1e-8);

    for (const auto& hit : result.hits) {
        EXPECT_NEAR((hit.hit.point - Point3d(0.0, 0.0, 0.0)).length(), 1.0, 1e-8);
        EXPECT_LT((ball.evaluate(hit.u, hit.v) - hit.hit.point).length(), 1e-8);
        EXPECT_LE(hit.hit.normal.dot(ray.direction), 0.0);
    }
}

TEST(RaySurface, aRayThroughATorusIsHitFourTimes)
{
    // The same geometry as the curve x surface case: the tube is entered and left twice, so four crossings at
    // t = 6, 8, 12 and 14.
    const ToroidalSurfaced ring(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 3.0, 1.0);
    const Rayd             ray(Point3d(-10.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto result = intersectRayWithSurface(ray, ring);
    ASSERT_FALSE(result.coincident);
    ASSERT_EQ(result.hits.size(), 4U);

    const double expected[4] = { 6.0, 8.0, 12.0, 14.0 };
    for (std::size_t hit = 0; hit < result.hits.size(); ++hit) {
        EXPECT_NEAR(result.hits[hit].hit.t, expected[hit], 1e-7);
        EXPECT_NEAR(result.hits[hit].hit.point.x, expected[hit] - 10.0, 1e-7);
    }
}

TEST(RaySurface, aRayFromInsideASphereLeavesOnceWithAnInwardNormal)
{
    // The entering crossing is behind the origin and is not a ray hit; the normal of the remaining one faces back
    // towards the ray, which puts it on the inside of the sphere.
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 2.0);
    const Rayd              ray(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto result = intersectRayWithSurface(ray, ball);
    ASSERT_FALSE(result.coincident);
    ASSERT_EQ(result.hits.size(), 1U);
    EXPECT_NEAR(result.hits.front().hit.t, 2.0, 1e-8);
    EXPECT_LE(result.hits.front().hit.normal.dot(ray.direction), 0.0);
    EXPECT_NEAR(result.hits.front().hit.normal.x, -1.0, 1e-7);
}

TEST(RaySurface, aHitBehindTheOriginIsNotACrossing)
{
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 1.0);
    const Rayd              ray(Point3d(5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    EXPECT_TRUE(intersectRayWithSurface(ray, ball).hits.empty());
}

TEST(RaySurface, aRayLyingInAPlaneIsCoincidentNotAListOfPoints)
{
    // Every point of the ray is on the plane, so the intersection is a stretch, not a point set.
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Rayd   ray(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto   window = SurfaceParameterWindow<double>{ -10.0, 10.0, -10.0, 10.0 };

    const auto result = intersectRayWithSurface(ray, plane, window);
    EXPECT_TRUE(result.coincident);
    EXPECT_TRUE(result.hits.empty());
}

TEST(RaySurface, theWindowIsAConstraintNotAHint)
{
    // The sphere's back half alone: the ray still crosses it once, at azimuth pi, and the front crossing is outside
    // the window, so it is not reported.
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 1.0);
    const Rayd              ray(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    const auto back_half = intersectRayWithSurface(ray, ball, SurfaceParameterWindow<double>{ vn::math::PI_HALF, 3.0 * vn::math::PI_HALF, 0.0, vn::math::PI });
    ASSERT_FALSE(back_half.coincident);
    ASSERT_EQ(back_half.hits.size(), 1U);
    EXPECT_NEAR(back_half.hits.front().hit.t, 4.0, 1e-7);
}

TEST(RaySurface, anUnboundedDirectionNeedsAWindow)
{
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const Rayd   ray(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0));

    const auto result = intersectRayWithSurface(ray, plane);
    EXPECT_FALSE(result.coincident);
    EXPECT_TRUE(result.hits.empty());
}

TEST(RaySurface, unusableInputsAnswerNothing)
{
    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const auto   window = SurfaceParameterWindow<double>{ -10.0, 10.0, -10.0, 10.0 };

    const Rayd aimless(Point3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, 0.0));
    EXPECT_TRUE(intersectRayWithSurface(aimless, plane, window).hits.empty());

    const SphericalSurfaced collapsed(Point3d(0.0, 0.0, 0.0), 0.0);
    EXPECT_TRUE(intersectRayWithSurface(Rayd(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0)), collapsed).hits.empty());

    const auto reversed = SurfaceParameterWindow<double>{ 10.0, -10.0, -10.0, 10.0 };
    EXPECT_TRUE(intersectRayWithSurface(Rayd(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0)), plane, reversed).hits.empty());
}

TEST(RaySurface, aSplinePatchIsHitThroughItsOwnParameters)
{
    // A bilinear patch on z = 1 over the unit square: the hit is where the ray is, and u / v report that place.
    const BezierSurfaced patch({ Point3d(0.0, 0.0, 1.0), Point3d(1.0, 0.0, 1.0), Point3d(0.0, 1.0, 1.0), Point3d(1.0, 1.0, 1.0) }, 2, 2);
    const Rayd           ray(Point3d(0.25, 0.25, 5.0), Vec3d(0.0, 0.0, -1.0));

    const auto result = intersectRayWithSurface(ray, patch);
    ASSERT_FALSE(result.coincident);
    ASSERT_EQ(result.hits.size(), 1U);
    EXPECT_NEAR(result.hits.front().hit.t, 4.0, 1e-7);
    EXPECT_LT((result.hits.front().hit.point - Point3d(0.25, 0.25, 1.0)).length(), 1e-7);
    EXPECT_NEAR(result.hits.front().u, 0.25, 1e-7);
    EXPECT_NEAR(result.hits.front().v, 0.25, 1e-7);
    EXPECT_LE(result.hits.front().hit.normal.dot(ray.direction), 0.0);
}

TEST(RaySurface, bothPrecisionsAgreeOnASphere)
{
    checkSphereRayAtBothPrecisions<double>();
    checkSphereRayAtBothPrecisions<float>();
}

TEST(RaySurface, aCoincidentRayIsAlsoSeenInFloat)
{
    const vn::geom::spatial::Planef plane(vn::math::Point3<float>(0, 0, 0), vn::math::Vector3<float>(0, 0, 1));
    const Rayf                 ray(vn::math::Point3<float>(-5, 0, 0), vn::math::Vector3<float>(1, 0, 0));

    const auto result = intersectRayWithSurface(ray, plane, SurfaceParameterWindow<float>{ -10.0f, 10.0f, -10.0f, 10.0f });
    EXPECT_TRUE(result.coincident);
    EXPECT_TRUE(result.hits.empty());
}
