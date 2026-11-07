#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <utility>

#include <vine/geom/spatial/BezierCurve.hpp>
#include <vine/geom/spatial/CircleCurve.hpp>
#include <vine/geom/spatial/CurveProjection.hpp>
#include <vine/geom/spatial/LineCurve.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

namespace
{

using vn::geom::spatial::BezierCurved;
using vn::geom::spatial::CircleCurved;
using vn::geom::spatial::closestPointOnCurve;
using vn::geom::spatial::LineCurved;
using vn::math::Point3d;
using vn::math::Vec3d;

/// The same line projection at both precisions the library exports.
template <vn::math::FP T>
void checkLineFootAtBothPrecisions()
{
    const vn::geom::spatial::LineCurve<T> line(vn::math::Point3<T>(T(0), T(0), T(0)), vn::math::Vector3<T>(T(1), T(0), T(0)));
    const auto                       projection = closestPointOnCurve(line,
                                                                      vn::math::Point3<T>(T(3), T(4), T(0)),
                                                                      std::optional<std::pair<T, T>>{
                                                                        { T(0), T(10) }
    });

    ASSERT_TRUE(projection.has_value());
    const double tolerance = (T(1) == T(1.0f)) ? 1e-4 : 1e-9;
    EXPECT_NEAR(static_cast<double>(projection->parameter), 3.0, tolerance);
    EXPECT_NEAR(static_cast<double>(projection->distance), 4.0, tolerance);
    EXPECT_NEAR(static_cast<double>(projection->point.x), 3.0, tolerance);
}

} // namespace

TEST(CurveProjection, aPointProjectsOntoALineCurveAtItsFoot)
{
    // The foot is the perpendicular projection; the parameter is the distance along the unit direction.
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto       projection = closestPointOnCurve(line,
                                                      Point3d(3.0, 4.0, 0.0),
                                                      std::optional<std::pair<double, double>>{
                                                        { 0.0, 10.0 }
    });

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->parameter, 3.0, 1e-9);
    EXPECT_NEAR(projection->distance, 4.0, 1e-9);
    EXPECT_LT((projection->point - Point3d(3.0, 0.0, 0.0)).length(), 1e-9);
    EXPECT_LT((line.evaluate(projection->parameter) - projection->point).length(), 1e-12);
}

TEST(CurveProjection, anUnboundedCurveNeedsAWindow)
{
    // A line's own domain is infinite: there is no finite interval to sample, and the query refuses rather than
    // inventing one. A reversed or zero-width window is refused the same way.
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));

    EXPECT_FALSE(closestPointOnCurve(line, Point3d(3.0, 4.0, 0.0)).has_value());
    EXPECT_FALSE(closestPointOnCurve(line,
                                     Point3d(3.0, 4.0, 0.0),
                                     std::optional<std::pair<double, double>>{
                                       { 10.0, 0.0 }
    })
                     .has_value());
    EXPECT_FALSE(closestPointOnCurve(line,
                                     Point3d(3.0, 4.0, 0.0),
                                     std::optional<std::pair<double, double>>{
                                       { 5.0, 5.0 }
    })
                     .has_value());
}

TEST(CurveProjection, aFootOutsideTheWindowFallsOnTheNearerEnd)
{
    // The perpendicular foot is behind the window, so the minimum is the window's start, where `f` has no root.
    const LineCurved line(Point3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0));
    const auto       projection = closestPointOnCurve(line,
                                                      Point3d(-3.0, 4.0, 0.0),
                                                      std::optional<std::pair<double, double>>{
                                                        { 0.0, 10.0 }
    });

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->parameter, 0.0, 1e-9);
    EXPECT_NEAR(projection->distance, 5.0, 1e-9);
}

TEST(CurveProjection, aPointProjectsOntoACircle)
{
    // The circle's domain is bounded and periodic, so it needs no window; its own frame decides which parameter the
    // nearest point has, so the parameter is checked against `evaluate()` rather than against a constant.
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const auto         projection = closestPointOnCurve(circle, Point3d(5.0, 0.0, 0.0));

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->distance, 3.0, 1e-7);
    EXPECT_LT((projection->point - Point3d(2.0, 0.0, 0.0)).length(), 1e-6);
    EXPECT_LT((circle.evaluate(projection->parameter) - projection->point).length(), 1e-12);
}

TEST(CurveProjection, theCentreOfACircleReportsOneOfItsEquidistantPoints)
{
    // Every point of the circle is a nearest point of its centre: one is reported, and the distance is the radius.
    const CircleCurved circle(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 1.0), 2.0);
    const auto         projection = closestPointOnCurve(circle, Point3d(0.0, 0.0, 0.0));

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->distance, 2.0, 1e-7);
    EXPECT_NEAR((projection->point - Point3d(0.0, 0.0, 0.0)).length(), 2.0, 1e-7);
}

TEST(CurveProjection, aPointProjectsOntoASplineCurve)
{
    // The quadratic Bezier B(u) = (2u, 2u(1 - u), 0); the nearest point of (1, 2, 0) is its apex at u = 1/2,
    // 1.5 away - the analytic answer, not a sampled approximation.
    const BezierCurved curve({ Point3d(0.0, 0.0, 0.0), Point3d(1.0, 1.0, 0.0), Point3d(2.0, 0.0, 0.0) });
    const auto         projection = closestPointOnCurve(curve, Point3d(1.0, 2.0, 0.0));

    ASSERT_TRUE(projection.has_value());
    EXPECT_NEAR(projection->parameter, 0.5, 1e-7);
    EXPECT_NEAR(projection->distance, 1.5, 1e-7);
    EXPECT_LT((projection->point - Point3d(1.0, 0.5, 0.0)).length(), 1e-7);
}

TEST(CurveProjection, unusableInputsAnswerNothing)
{
    // An aimless line and an empty spline are both refused by `isValid()`.
    const LineCurved aimless(Point3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 0.0));
    EXPECT_FALSE(closestPointOnCurve(aimless,
                                     Point3d(1.0, 1.0, 1.0),
                                     std::optional<std::pair<double, double>>{
                                       { 0.0, 10.0 }
    })
                     .has_value());

    const BezierCurved empty_curve;
    EXPECT_FALSE(closestPointOnCurve(empty_curve, Point3d(1.0, 1.0, 1.0)).has_value());
}

TEST(CurveProjection, bothPrecisionsFindTheSameFoot)
{
    checkLineFootAtBothPrecisions<double>();
    checkLineFootAtBothPrecisions<float>();
}
