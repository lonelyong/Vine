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

} // namespace
