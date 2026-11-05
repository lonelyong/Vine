#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include <vine/geom2d/ArcCurve.hpp>
#include <vine/geom2d/BSplineCurve.hpp>
#include <vine/geom2d/BezierCurve.hpp>
#include <vine/geom2d/CircleCurve.hpp>
#include <vine/geom2d/Curve.hpp>
#include <vine/geom2d/EllipseCurve.hpp>
#include <vine/geom2d/HyperbolaCurve.hpp>
#include <vine/geom2d/LineCurve.hpp>
#include <vine/geom2d/OffsetCurve.hpp>
#include <vine/geom2d/ParabolaCurve.hpp>

namespace
{

using vn::geom2d::ArcCurve;
using vn::geom2d::BSplineCurve;
using vn::geom2d::BezierCurve;
using vn::geom2d::CircleCurve;
using vn::geom2d::Continuity;
using vn::geom2d::Curve;
using vn::geom2d::EllipseCurve;
using vn::geom2d::HyperbolaCurve;
using vn::geom2d::LineCurve;
using vn::geom2d::OffsetCurve;
using vn::geom2d::ParabolaCurve;

using vn::geom2d::ArcCurved;
using vn::geom2d::BSplineCurved;
using vn::geom2d::BezierCurved;
using vn::geom2d::CircleCurved;
using vn::geom2d::EllipseCurved;
using vn::geom2d::HyperbolaCurved;
using vn::geom2d::LineCurved;
using vn::geom2d::ParabolaCurved;

template <vn::math::FP T>
void expectNearPoint(const vn::math::Point2<T>& left, const vn::math::Point2<T>& right, T tolerance)
{
    EXPECT_NEAR(left.x, right.x, tolerance);
    EXPECT_NEAR(left.y, right.y, tolerance);
}

/**
 * @brief Checks the promises every planar curve makes, whatever its parameterisation.
 *
 * @details
 * - The domain is ordered, reversal is an involution that keeps the point, a closed curve meets its own ends and a
 *   periodic one repeats with its own period; a curve that is not periodic answers a period of 0.
 * - Unbounded curves are sampled at finite parameters, because their own endpoints are infinite and averaging them
 *   would produce a NaN.
 */
template <vn::math::FP T>
void expectInterfacePromises(const Curve<T>& curve)
{
    ASSERT_TRUE(curve.isValid());

    const Continuity continuity = curve.continuity();
    EXPECT_TRUE(curve.isCN(0));
    EXPECT_FALSE(curve.isCN(-1));
    EXPECT_EQ(curve.isCN(3), continuity == Continuity::CN);

    T samples[3];
    if (std::isfinite(curve.firstParameter()) && std::isfinite(curve.lastParameter())) {
        EXPECT_LE(curve.firstParameter(), curve.lastParameter());
        samples[0] = curve.firstParameter();
        samples[1] = (curve.firstParameter() + curve.lastParameter()) / T(2);
        samples[2] = curve.lastParameter();
    } else {
        samples[0] = T(-1);
        samples[1] = T(0);
        samples[2] = T(1);
    }

    const T tolerance = T(512) * vn::math::EPS<T>();
    for (const T u : samples) {
        EXPECT_NEAR(curve.reversedParameter(curve.reversedParameter(u)), u, tolerance);
    }

    if (std::isfinite(curve.firstParameter()) && std::isfinite(curve.lastParameter())) {
        EXPECT_NEAR(curve.reversedParameter(curve.firstParameter()), curve.lastParameter(), tolerance);
        EXPECT_NEAR(curve.reversedParameter(curve.lastParameter()), curve.firstParameter(), tolerance);
    }

    if (curve.isClosed()) {
        expectNearPoint(curve.evaluate(curve.firstParameter()), curve.evaluate(curve.lastParameter()), tolerance);
    }

    if (curve.isPeriodic()) {
        EXPECT_GT(curve.period(), T(0));
        for (const T u : samples) {
            expectNearPoint(curve.evaluate(u + curve.period()), curve.evaluate(u), tolerance);
        }
    } else {
        EXPECT_EQ(curve.period(), T(0));
    }
}

template <vn::math::FP T>
void expectEveryPlanarCurveKeepsItsPromises()
{
    const LineCurve<T>      line(vn::math::Point2<T>(T(1), T(2)), vn::math::Vector2<T>(T(2), T(3)));
    const CircleCurve<T>    circle(vn::math::Point2<T>(T(1), T(-2)), T(2));
    const EllipseCurve<T>   ellipse(vn::math::Point2<T>(T(-1), T(1)), vn::math::Vector2<T>(T(0), T(3)), T(3), T(2));
    const HyperbolaCurve<T> hyperbola(vn::math::Point2<T>(T(1), T(2)), vn::math::Vector2<T>(T(2), T(0)), T(2), T(1));
    const ParabolaCurve<T>  parabola(vn::math::Point2<T>(T(0), T(1)), vn::math::Vector2<T>(T(1), T(1)), T(1));
    const ArcCurve<T>       quarter(circle, T(0.3), T(2.1));
    const ArcCurve<T>       full_turn(circle, T(0), T(2) * static_cast<T>(vn::math::PI));
    const BezierCurve<T>    bezier({ vn::math::Point2<T>(T(0), T(0)), vn::math::Point2<T>(T(1), T(2)),
                                     vn::math::Point2<T>(T(2), T(0)) });
    const BSplineCurve<T>   spline(2, { vn::math::Point2<T>(T(0), T(0)), vn::math::Point2<T>(T(1), T(2)),
                                        vn::math::Point2<T>(T(2), T(0)) }, { T(0), T(1) }, { 3, 3 });
    const OffsetCurve<CircleCurve<T>> offset_circle(circle, T(0.5));

    expectInterfacePromises<T>(line);
    expectInterfacePromises<T>(circle);
    expectInterfacePromises<T>(ellipse);
    expectInterfacePromises<T>(hyperbola);
    expectInterfacePromises<T>(parabola);
    expectInterfacePromises<T>(quarter);
    expectInterfacePromises<T>(full_turn);
    expectInterfacePromises<T>(bezier);
    expectInterfacePromises<T>(spline);
    expectInterfacePromises<T>(offset_circle);
}

TEST(CurveContractTest, EveryPlanarCurveKeepsItsPromisesInDouble)
{
    expectEveryPlanarCurveKeepsItsPromises<double>();
}

TEST(CurveContractTest, EveryPlanarCurveKeepsItsPromisesInFloat)
{
    expectEveryPlanarCurveKeepsItsPromises<float>();
}

/**
 * @brief Checks that each derivative is the derivative of the one before it, by central difference.
 *
 * @details
 * - The sample points stay away from the ends of the window, where a central difference would reach outside the domain
 *   (and where a clamped spline answers its end point instead of continuing).
 * - Double only: in float the step that keeps the truncation error small loses most of the result's digits, so the
 *   float sweep covers the interface promises instead.
 * - This is what pins `thirdDerivative()` on the concrete types: it is not on `Curve<T>`, so the offset curve is the
 *   only code that would otherwise exercise it.
 */
template <typename C>
void expectDerivativeChain(const C& curve, double from, double to)
{
    const double h         = (to - from) * 1e-3;
    // The step is small enough for the truncation error to stay below this, and large enough that the subtraction keeps
    // its digits; on curves whose derivatives grow (a hyperbola towards its ends) the residual is relative, not absolute.
    const double tolerance = 1e-4;

    for (const double fraction : { 0.25, 0.5, 0.75 }) {
        const double u = from + (to - from) * fraction;

        const auto before_point = curve.evaluate(u - h);
        const auto after_point  = curve.evaluate(u + h);
        const auto velocity     = curve.derivative(u);
        EXPECT_NEAR(velocity.x, (after_point.x - before_point.x) / (2.0 * h), tolerance);
        EXPECT_NEAR(velocity.y, (after_point.y - before_point.y) / (2.0 * h), tolerance);

        const auto before_velocity = curve.derivative(u - h);
        const auto after_velocity  = curve.derivative(u + h);
        const auto acceleration    = curve.secondDerivative(u);
        EXPECT_NEAR(acceleration.x, (after_velocity.x - before_velocity.x) / (2.0 * h), tolerance);
        EXPECT_NEAR(acceleration.y, (after_velocity.y - before_velocity.y) / (2.0 * h), tolerance);

        const auto before_acceleration = curve.secondDerivative(u - h);
        const auto after_acceleration  = curve.secondDerivative(u + h);
        const auto jerk                = curve.thirdDerivative(u);
        EXPECT_NEAR(jerk.x, (after_acceleration.x - before_acceleration.x) / (2.0 * h), tolerance);
        EXPECT_NEAR(jerk.y, (after_acceleration.y - before_acceleration.y) / (2.0 * h), tolerance);
    }
}

TEST(CurveContractTest, EveryCurveDifferentiatesConsistently)
{
    const LineCurved      line({ 1.0, 2.0 }, { 2.0, 3.0 });
    const CircleCurved    circle({ 1.0, -2.0 }, 2.0);
    const EllipseCurved   ellipse({ -1.0, 1.0 }, { 0.0, 3.0 }, 3.0, 2.0);
    const HyperbolaCurved hyperbola({ 1.0, 2.0 }, { 2.0, 0.0 }, 2.0, 1.0);
    const ParabolaCurved  parabola({ 0.0, 1.0 }, { 1.0, 1.0 }, 1.0);
    const ArcCurved       arc(circle, 0.4, 2.7);
    const BezierCurved    bezier({ { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } });
    const BSplineCurved   spline(3, { { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } }, { 0.0, 1.0 },
                                { 4, 4 });

    // The rational forms answer through Leibniz' rule instead of differences of the poles.
    const BezierCurved  rational_bezier({ { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 }, { -1.0, 0.5 } },
                                       { 1.0, std::sqrt(0.5), 1.0, 1.2 });
    const BSplineCurved rational_spline(3, { { 1.0, 0.0 }, { 1.0, 1.0 }, { 0.0, 1.0 }, { -1.0, 0.5 } }, { 0.0, 1.0 },
                                       { 4, 4 }, { 1.0, std::sqrt(0.5), 1.0, 1.2 });

    expectDerivativeChain(line, -2.0, 2.0);
    expectDerivativeChain(circle, 0.2, 6.0);
    expectDerivativeChain(ellipse, 0.2, 6.0);
    expectDerivativeChain(hyperbola, -2.0, 2.0);
    expectDerivativeChain(parabola, -2.0, 2.0);
    expectDerivativeChain(arc, 0.6, 2.5);
    expectDerivativeChain(bezier, 0.05, 0.95);
    expectDerivativeChain(spline, 0.05, 0.95);
    expectDerivativeChain(rational_bezier, 0.05, 0.95);
    expectDerivativeChain(rational_spline, 0.05, 0.95);
}

} // namespace
