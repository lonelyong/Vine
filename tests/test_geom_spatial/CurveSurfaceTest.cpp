#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <utility>

#include <vine/geom/spatial/BezierSurface.hpp>
#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/ConicalSurface.hpp>
#include <vine/geom/spatial/CurveSurfaceIntersection.hpp>
#include <vine/geom/spatial/CylindricalSurface.hpp>
#include <vine/geom/spatial/LineCurve.hpp>
#include <vine/geom/spatial/Plane.hpp>
#include <vine/geom/spatial/SphericalSurface.hpp>
#include <vine/geom/spatial/SurfaceParameterWindow.hpp>
#include <vine/geom/spatial/ToroidalSurface.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geom::spatial::BezierSurfaced;
using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::ConicalSurfaced;
using vn::geom::spatial::CurveSurfaceContact;
using vn::geom::spatial::CylindricalSurfaced;
using vn::geom::spatial::intersectCurveWithSurface;
using vn::geom::spatial::intersectCurveWithSurfaceByProjection;
using vn::geom::spatial::LineCurved;
using vn::geom::spatial::Planed;
using vn::geom::spatial::SphericalSurfaced;
using vn::geom::spatial::SurfaceParameterWindow;
using vn::geom::spatial::ToroidalSurfaced;
using vn::math::Point3d;
using vn::math::Vec3d;

/// A `signedValue()` overload is what makes a surface usable here: the search is a root find on it. This assertion
/// keeps the two in step - a new analytic surface has to provide one before it can be intersected.
static_assert(requires(const Planed& plane, const Point3d& point) { vn::geom::spatial::signedValue(plane, point); });
static_assert(requires(const SphericalSurfaced& ball, const Point3d& point) { vn::geom::spatial::signedValue(ball, point); });
static_assert(requires(const CylindricalSurfaced& tube, const Point3d& point) { vn::geom::spatial::signedValue(tube, point); });
static_assert(requires(const ConicalSurfaced& cone, const Point3d& point) { vn::geom::spatial::signedValue(cone, point); });
static_assert(requires(const ToroidalSurfaced& ring, const Point3d& point) { vn::geom::spatial::signedValue(ring, point); });

/// The same line-through-a-torus case at both precisions the library exports: the crossings are at x = +-2 and +-4,
/// so their count, their order and their positions are all checkable without a second formula.
template <vn::math::FP T>
void checkLineThroughTorus()
{
    const vn::geom::spatial::LineCurve<T>       line(vn::math::Point3<T>(T(-10), T(0), T(0)), vn::math::Vector3<T>(T(1), T(0), T(0)));
    const vn::geom::spatial::ToroidalSurface<T> ring(vn::math::Point3<T>(T(0), T(0), T(0)), vn::math::Vector3<T>(T(0), T(0), T(1)), T(3), T(1));

    const auto result = intersectCurveWithSurface<T>(line,
                                                     ring,
                                                     std::optional<std::pair<T, T>>{
                                                       { T(-20), T(20) }
    });
    ASSERT_EQ(result.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(result.hits.size(), 4U);

    // Ascending parameter, one entry per crossing: the tube is entered at -4, left at -2, entered again at 2, left
    // at 4 (u = x + 10 because the line starts at x = -10).
    const double expected_u[4] = { 6.0, 8.0, 12.0, 14.0 };
    const double tolerance     = T(1) == T(1.0f) ? 1e-4 : 1e-8;
    for (std::size_t hit = 0; hit < result.hits.size(); ++hit) {
        EXPECT_NEAR(static_cast<double>(result.hits[hit].curve_parameter), expected_u[hit], tolerance);
        EXPECT_NEAR(static_cast<double>(result.hits[hit].point.x), expected_u[hit] - 10.0, tolerance);
        // The reported point is the curve's own answer at the reported parameter, not a second computation.
        const auto on_curve = line.evaluate(result.hits[hit].curve_parameter);
        EXPECT_LT((on_curve - result.hits[hit].point).length(), T(1e-6));
    }
}

} // namespace

TEST(CurveSurface, aLineCrossesAPlaneWhereTheAnalyticAnswerSays)
{
    // The analytic entry point is the reference: the root find has to land on the same point for the same input.
    const LineCurved line(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const Planed     plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));

    Point3d analytic;
    ASSERT_TRUE(plane.intersectWith(line, analytic));

    const auto result = intersectCurveWithSurface(line,
                                                  plane,
                                                  std::optional<std::pair<double, double>>{
                                                    { 0.0, 10.0 }
    });
    ASSERT_EQ(result.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(result.hits.size(), 1U);
    EXPECT_NEAR(result.hits.front().curve_parameter, 5.0, 1e-7);
    EXPECT_LT((result.hits.front().point - analytic).length(), 1e-7);
}

TEST(CurveSurface, anUnboundedCurveNeedsAWindow)
{
    // A line's own domain is infinite: the search refuses instead of inventing a range to sample.
    const LineCurved line(Point3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const Planed     plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));

    EXPECT_EQ(intersectCurveWithSurface(line, plane).contact, CurveSurfaceContact::None);

    // A window that does not contain the crossing reports nothing - this is a window, not a hint.
    const auto outside = intersectCurveWithSurface(line,
                                                   plane,
                                                   std::optional<std::pair<double, double>>{
                                                     { 20.0, 30.0 }
    });
    EXPECT_EQ(outside.contact, CurveSurfaceContact::None);
    EXPECT_TRUE(outside.hits.empty());
}

TEST(CurveSurface, aCircleCrossesAnOffCentreSphereTwice)
{
    // The circle has radius 4 in the z = 0 plane around the origin; the sphere sits at (3, 0, 0) with radius 3, so
    // the two meet where 25 - 24 cos(theta) = 9, i.e. cos(theta) = 2/3: exactly two crossings. The circle's bounds are
    // closed and periodic, so no window is needed.
    const CircleCurved      circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 4.0);
    const SphericalSurfaced ball(Point3d(3.0, 0.0, 0.0), 3.0);

    const auto result = intersectCurveWithSurface(circle, ball);
    ASSERT_EQ(result.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(result.hits.size(), 2U);

    // cos(u) = 2/3 has two solutions in [0, 2*pi), and the hits come back in ascending parameter order.
    const double crossing = std::acos(2.0 / 3.0);
    EXPECT_NEAR(result.hits[0].curve_parameter, crossing, 1e-6);
    EXPECT_NEAR(result.hits[1].curve_parameter, 2.0 * vn::math::PI - crossing, 1e-6);

    for (const auto& hit : result.hits) {
        EXPECT_NEAR((hit.point - Point3d(3.0, 0.0, 0.0)).length(), 3.0, 1e-6);
        EXPECT_DOUBLE_EQ(hit.point.z, 0.0);
    }
}

TEST(CurveSurface, aLineThroughATorusMeetsItFourTimes)
{
    checkLineThroughTorus<double>();
    checkLineThroughTorus<float>();
}

TEST(CurveSurface, aCurveLyingOnTheSurfaceIsCoincidentNotAListOfPoints)
{
    // A line inside a plane, and a circle on a sphere of the same radius: both share infinitely many points with the
    // surface, so the answer is the classification, and no hits are listed.
    const LineCurved in_plane(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 1.0, 0.0));
    const Planed     plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));

    const auto line_result = intersectCurveWithSurface(in_plane,
                                                       plane,
                                                       std::optional<std::pair<double, double>>{
                                                         { 0.0, 10.0 }
    });
    EXPECT_EQ(line_result.contact, CurveSurfaceContact::Coincident);
    EXPECT_TRUE(line_result.hits.empty());

    const CircleCurved      equator(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 2.0);
    const auto              circle_result = intersectCurveWithSurface(equator, ball);
    EXPECT_EQ(circle_result.contact, CurveSurfaceContact::Coincident);
    EXPECT_TRUE(circle_result.hits.empty());
}

TEST(CurveSurface, aTouchWithoutCrossingIsReportedAsNoCrossing)
{
    // The plane is tangent to the sphere and the line skims it: `signedValue()` never changes sign, so the search finds
    // no crossing. That is the documented behaviour (geometry-intersection.md P4), not an accident.
    const LineCurved line(Point3d(-5.0, 0.0, 1.0), Vec3d(1.0, 0.0, 0.0));
    const Planed     plane(Point3d(0.0, 0.0, 1.5), Vec3d(0.0, 0.0, 1.0));

    const auto result = intersectCurveWithSurface(line,
                                                  plane,
                                                  std::optional<std::pair<double, double>>{
                                                    { 0.0, 10.0 }
    });
    EXPECT_EQ(result.contact, CurveSurfaceContact::None);
}

TEST(CurveSurface, unusableInputsAnswerNothingInsteadOfNaN)
{
    // A degenerate surface and a degenerate curve are both reported by `isValid()`; the search asks before it samples.
    const LineCurved        line(Point3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const SphericalSurfaced collapsed(Point3d(0.0, 0.0, 0.0), 0.0);
    const auto              window = std::optional<std::pair<double, double>>{
        { 0.0, 10.0 }
    };

    EXPECT_EQ(intersectCurveWithSurface(line, collapsed, window).contact, CurveSurfaceContact::None);

    const LineCurved aimless(Point3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, 0.0));
    const Planed     plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    EXPECT_EQ(intersectCurveWithSurface(aimless, plane, window).contact, CurveSurfaceContact::None);
}

TEST(CurveSurfaceProjection, aLineCrossesASplinePatchWhereTheRayDoes)
{
    // A patch has no implicit form, so this is the entry that must work: the crossing is where the same geometry's
    // ray entry puts it, t = 4 along a unit direction.
    const BezierSurfaced patch({ Point3d(0.0, 0.0, 1.0), Point3d(1.0, 0.0, 1.0), Point3d(0.0, 1.0, 1.0), Point3d(1.0, 1.0, 1.0) }, 2, 2);
    const LineCurved     line(Point3d(0.25, 0.25, 5.0), Vec3d(0.0, 0.0, -1.0));
    const auto           window = std::optional<std::pair<double, double>>{
        { 0.0, 10.0 }
    };

    const auto result = intersectCurveWithSurfaceByProjection(line, patch, window);
    ASSERT_EQ(result.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(result.hits.size(), 1U);
    EXPECT_NEAR(result.hits.front().curve_parameter, 4.0, 1e-6);
    EXPECT_LT((result.hits.front().point - Point3d(0.25, 0.25, 1.0)).length(), 1e-6);
}

TEST(CurveSurfaceProjection, theAnalyticAndProjectedSearchesAgreeOnAPlane)
{
    // The plane has both an implicit form and a parameterisation: the two entries must land on the same crossing.
    const LineCurved line(Point3d(1.0, 2.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const Planed     plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    const auto       window = std::optional<std::pair<double, double>>{
        { 0.0, 10.0 }
    };

    const auto analytic  = intersectCurveWithSurface(line, plane, window);
    const auto projected = intersectCurveWithSurfaceByProjection(line, plane, window, SurfaceParameterWindow<double>{ -10.0, 10.0, -10.0, 10.0 });

    ASSERT_EQ(projected.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(projected.hits.size(), analytic.hits.size());
    EXPECT_NEAR(projected.hits.front().curve_parameter, analytic.hits.front().curve_parameter, 1e-6);
    EXPECT_LT((projected.hits.front().point - analytic.hits.front().point).length(), 1e-6);
}

TEST(CurveSurfaceProjection, aTrimmedSheetOnlyAnswersInsideItsWindow)
{
    // The sphere's back half alone: the line still crosses it at x = -1 (t = 4), and the crossing at x = +1 is
    // outside the surface window, so it is not reported.
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 1.0);
    const LineCurved        line(Point3d(-5.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto              window = std::optional<std::pair<double, double>>{
        { 0.0, 10.0 }
    };

    const auto result = intersectCurveWithSurfaceByProjection(line,
                                                              ball,
                                                              window,
                                                              SurfaceParameterWindow<double>{ vn::math::PI_HALF, 3.0 * vn::math::PI_HALF, 0.0, vn::math::PI });
    ASSERT_EQ(result.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(result.hits.size(), 1U);
    EXPECT_NEAR(result.hits.front().curve_parameter, 4.0, 1e-5);
}

TEST(CurveSurfaceProjection, aLineLyingOnAPatchIsCoincidentNotAListOfPoints)
{
    const BezierSurfaced patch({ Point3d(0.0, 0.0, 1.0), Point3d(1.0, 0.0, 1.0), Point3d(0.0, 1.0, 1.0), Point3d(1.0, 1.0, 1.0) }, 2, 2);
    const LineCurved     line(Point3d(0.1, 0.5, 1.0), Vec3d(1.0, 0.0, 0.0));
    const auto           window = std::optional<std::pair<double, double>>{
        { 0.0, 0.8 }
    };

    const auto result = intersectCurveWithSurfaceByProjection(line, patch, window);
    EXPECT_EQ(result.contact, CurveSurfaceContact::Coincident);
    EXPECT_TRUE(result.hits.empty());
}

TEST(CurveSurfaceProjection, unusableInputsAnswerNothing)
{
    // An empty patch is reported by `isValid()`; a plane's natural domain is infinite, so without a surface window
    // there is nothing to project onto and the search refuses instead of sampling an invented rectangle.
    const LineCurved line(Point3d(0.25, 0.25, 5.0), Vec3d(0.0, 0.0, -1.0));
    const auto       window = std::optional<std::pair<double, double>>{
        { 0.0, 10.0 }
    };

    const BezierSurfaced empty_patch;
    EXPECT_EQ(intersectCurveWithSurfaceByProjection(line, empty_patch, window).contact, CurveSurfaceContact::None);

    const Planed plane(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0));
    EXPECT_EQ(intersectCurveWithSurfaceByProjection(line, plane, window).contact, CurveSurfaceContact::None);
}

TEST(CurveSurfaceProjection, aCrossingAtASingularParameterLineIsNotReported)
{
    // A line down the sphere's axis: every point outside the sphere projects onto a pole, where the parameterisation has
    // no normal, so the projected sign is unknown there and the crossings are not reported. This is the documented gap
    // (geometry-intersection.md P9); the analytic entry answers the same geometry exactly, which is why it exists.
    const SphericalSurfaced ball(Point3d(0.0, 0.0, 0.0), 1.0);
    const LineCurved        line(Point3d(0.0, 0.0, 5.0), Vec3d(0.0, 0.0, -1.0));
    const auto              window = std::optional<std::pair<double, double>>{
        { 0.0, 10.0 }
    };

    EXPECT_EQ(intersectCurveWithSurfaceByProjection(line, ball, window).contact, CurveSurfaceContact::None);

    const auto analytic = intersectCurveWithSurface(line, ball, window);
    ASSERT_EQ(analytic.contact, CurveSurfaceContact::Isolated);
    EXPECT_EQ(analytic.hits.size(), 2U);
}

namespace
{

/// The same patch crossing at both precisions the library exports.
template <vn::math::FP T>
void checkPatchCrossingAtBothPrecisions()
{
    const vn::geom::spatial::BezierSurface<T> patch({ vn::math::Point3<T>(T(0), T(0), T(1)),
                                                 vn::math::Point3<T>(T(1), T(0), T(1)),
                                                 vn::math::Point3<T>(T(0), T(1), T(1)),
                                                 vn::math::Point3<T>(T(1), T(1), T(1)) },
                                               2,
                                               2);
    const vn::geom::spatial::LineCurve<T>     line(vn::math::Point3<T>(T(0.25), T(0.25), T(5)), vn::math::Vector3<T>(T(0), T(0), T(-1)));
    const std::optional<std::pair<T, T>> curve_window = std::pair<T, T>{ T(0), T(10) };

    const auto result = intersectCurveWithSurfaceByProjection(line, patch, curve_window, vn::geom::spatial::naturalSurfaceWindow<T>(), T(1e-5));
    ASSERT_EQ(result.contact, CurveSurfaceContact::Isolated);
    ASSERT_EQ(result.hits.size(), 1U);

    const double tolerance = (T(1) == T(1.0f)) ? 1e-3 : 1e-6;
    EXPECT_NEAR(static_cast<double>(result.hits.front().curve_parameter), 4.0, tolerance);
}

} // namespace

TEST(CurveSurfaceProjection, bothPrecisionsFindTheSamePatchCrossing)
{
    checkPatchCrossingAtBothPrecisions<double>();
    checkPatchCrossingAtBothPrecisions<float>();
}
