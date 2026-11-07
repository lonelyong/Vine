#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include <vine/geom/planar/BSplineCurve.hpp>
#include <vine/geom/planar/BezierCurve.hpp>
#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/Curve.hpp>
#include <vine/geom/planar/CurveCurvature.hpp>
#include <vine/geom/planar/CurveCurveDistance.hpp>
#include <vine/geom/planar/CurveLength.hpp>
#include <vine/geom/planar/CurveProjection.hpp>
#include <vine/geom/planar/EllipseCurve.hpp>
#include <vine/geom/planar/HyperbolaCurve.hpp>
#include <vine/geom/planar/LineCurve.hpp>
#include <vine/geom/planar/OffsetCurve.hpp>
#include <vine/geom/planar/ParabolaCurve.hpp>
#include <vine/geom/planar/TrimmedCurve.hpp>

namespace
{

using vn::geom::planar::BSplineCurve;
using vn::geom::planar::BezierCurve;
using vn::geom::planar::CircleCurve;
using vn::geom::Continuity;
using vn::geom::planar::Curve;
using vn::geom::planar::EllipseCurve;
using vn::geom::planar::HyperbolaCurve;
using vn::geom::planar::LineCurve;
using vn::geom::planar::OffsetCurve;
using vn::geom::planar::ParabolaCurve;
using vn::geom::planar::TrimmedCurve;
using vn::geom::planar::offsetCurve;
using vn::geom::planar::trimCurve;

using vn::geom::planar::BSplineCurved;
using vn::geom::planar::BezierCurved;
using vn::geom::planar::CircleCurved;
using vn::geom::planar::EllipseCurved;
using vn::geom::planar::HyperbolaCurved;
using vn::geom::planar::LineCurved;
using vn::geom::planar::ParabolaCurved;

template <vn::math::FP T>
void expectNearPoint(const vn::math::Point2<T>& left, const vn::math::Point2<T>& right, T tolerance)
{
    EXPECT_NEAR(left.x, right.x, tolerance);
    EXPECT_NEAR(left.y, right.y, tolerance);
}

template <vn::math::FP T>
void expectNearVector(const vn::math::Vector2<T>& left, const vn::math::Vector2<T>& right, T tolerance)
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

    // Derivative order is a capability: one and two are always answerable and agree with the named entries, the third
    // is answerable exactly when the type reports room for it, and an order outside the capability answers empty.
    EXPECT_GE(curve.maxDerivativeOrder(), 2);
    for (const T u : samples) {
        const std::optional<vn::math::Vector2<T>> first  = curve.derivativeAt(u, 1);
        const std::optional<vn::math::Vector2<T>> second = curve.derivativeAt(u, 2);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        expectNearVector(*first, curve.derivative(u), tolerance);
        expectNearVector(*second, curve.secondDerivative(u), tolerance);
        EXPECT_FALSE(curve.derivativeAt(u, 0).has_value());
        EXPECT_FALSE(curve.derivativeAt(u, -1).has_value());
        EXPECT_EQ(curve.derivativeAt(u, 3).has_value(), curve.maxDerivativeOrder() >= 3);
        EXPECT_FALSE(curve.derivativeAt(u, curve.maxDerivativeOrder() + 1).has_value());
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
    const auto              quarter    = trimCurve(circle, T(0.3), T(2.1));
    const auto              full_turn  = trimCurve(circle, T(0), T(2) * static_cast<T>(vn::math::PI));
    // Covers the circle's whole range but ends past it, so its two ends differ: `isClosed()` must not claim it.
    const auto           overshooting = trimCurve(circle, T(0), T(2) * static_cast<T>(vn::math::PI) + T(0.3));
    // A trim of a trim: it must keep answering exactly the orders its own basis answers.
    const auto           re_trimmed   = trimCurve(TrimmedCurve<T>(full_turn.basis_curve, T(0.5), T(3.0)), T(1.0), T(2.0));
    const BezierCurve<T> bezier({ vn::math::Point2<T>(T(0), T(0)), vn::math::Point2<T>(T(1), T(2)),
                                  vn::math::Point2<T>(T(2), T(0)) });
    const BSplineCurve<T> spline(2, { vn::math::Point2<T>(T(0), T(0)), vn::math::Point2<T>(T(1), T(2)),
                                      vn::math::Point2<T>(T(2), T(0)) }, { T(0), T(1) }, { 3, 3 });
    const auto            offset_circle = offsetCurve(circle, T(0.5));

    expectInterfacePromises<T>(line);
    expectInterfacePromises<T>(circle);
    expectInterfacePromises<T>(ellipse);
    expectInterfacePromises<T>(hyperbola);
    expectInterfacePromises<T>(parabola);
    expectInterfacePromises<T>(quarter);
    expectInterfacePromises<T>(full_turn);
    expectInterfacePromises<T>(overshooting);
    expectInterfacePromises<T>(re_trimmed);
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
        // The third derivative is a capability, not a promise: a type that cannot answer it is skipped here, which is
        // exactly what `derivativeAt()` reports.
        const auto jerk = curve.derivativeAt(u, 3);
        ASSERT_TRUE(jerk.has_value());
        EXPECT_NEAR(jerk->x, (after_acceleration.x - before_acceleration.x) / (2.0 * h), tolerance);
        EXPECT_NEAR(jerk->y, (after_acceleration.y - before_acceleration.y) / (2.0 * h), tolerance);
    }
}

/**
 * @brief Checks that every family algorithm accepts a curve through the interface alone.
 *
 * @details
 * - None of the six algorithms knows a concrete type, so each one is run on every swept type with a finite window.
 *   This is the counterpart of the contract sweep: the eleven-item interface is enough not only to state facts about
 *   a curve, but to measure it and relate it to another one.
 * - The window is supplied for every type so the same call shape applies to bounded and unbounded curves alike.
 */
template <vn::math::FP T>
void expectFamilyAlgorithmsAccept(const Curve<T>& curve, const std::pair<T, T>& range)
{
    // `closestPointOnCurve` deduces its scalar from the query, so its window must already be the optional: passing a
    // plain pair would be a conversion inside deduction, which fails.
    const std::optional<std::pair<T, T>> window{ range };

    const auto length = vn::geom::planar::arcLength(curve, window);
    ASSERT_TRUE(length);
    EXPECT_GE(*length, T(0));

    if (*length > T(0)) {
        const auto parameter = vn::geom::planar::parameterAtLength(curve, *length / T(2), window);
        ASSERT_TRUE(parameter);

        const auto half = vn::geom::planar::arcLength(curve, std::optional<std::pair<T, T>>{ { range.first, *parameter } });
        ASSERT_TRUE(half);
        const T tolerance = T(4096) * vn::math::EPS<T>() * ((*length > T(1)) ? *length : T(1));
        EXPECT_NEAR(*half, *length / T(2), tolerance);
    }

    const auto curvature = vn::geom::planar::signedCurvature(curve, (range.first + range.second) / T(2));
    ASSERT_TRUE(curvature);
    EXPECT_TRUE(std::isfinite(*curvature));

    const auto closest = vn::geom::planar::closestPointOnCurve(curve, vn::math::Point2<T>(T(1), T(1)), window);
    ASSERT_TRUE(closest);

    const CircleCurve<T> probe(vn::math::Point2<T>(T(10), T(0)), T(1));
    const auto           distance = vn::geom::planar::curveToCurveDistance(curve, probe, window);
    ASSERT_TRUE(distance);
    EXPECT_GE(distance->distance, T(0));
}

template <vn::math::FP T>
void expectEveryTypeWorksWithTheFamilyAlgorithms()
{
    const LineCurve<T>      line(vn::math::Point2<T>(T(1), T(2)), vn::math::Vector2<T>(T(2), T(3)));
    const CircleCurve<T>    circle(vn::math::Point2<T>(T(1), T(-2)), T(2));
    const EllipseCurve<T>   ellipse(vn::math::Point2<T>(T(-1), T(1)), vn::math::Vector2<T>(T(0), T(3)), T(3), T(2));
    const HyperbolaCurve<T> hyperbola(vn::math::Point2<T>(T(1), T(2)), vn::math::Vector2<T>(T(2), T(0)), T(2), T(1));
    const ParabolaCurve<T>  parabola(vn::math::Point2<T>(T(0), T(1)), vn::math::Vector2<T>(T(1), T(1)), T(1));
    const auto              arc    = trimCurve(circle, T(0.3), T(2.1));
    const BezierCurve<T>    bezier({ vn::math::Point2<T>(T(0), T(0)), vn::math::Point2<T>(T(1), T(2)),
                                     vn::math::Point2<T>(T(2), T(0)) });
    const BSplineCurve<T>   spline(2, { vn::math::Point2<T>(T(0), T(0)), vn::math::Point2<T>(T(1), T(2)),
                                        vn::math::Point2<T>(T(2), T(0)) }, { T(0), T(1) }, { 3, 3 });
    const auto              offset = offsetCurve(circle, T(0.5));

    expectFamilyAlgorithmsAccept<T>(line, std::pair<T, T>{ T(-1), T(1) });
    expectFamilyAlgorithmsAccept<T>(circle, std::pair<T, T>{ T(0.3), T(5) });
    expectFamilyAlgorithmsAccept<T>(ellipse, std::pair<T, T>{ T(0.3), T(5) });
    expectFamilyAlgorithmsAccept<T>(hyperbola, std::pair<T, T>{ T(-1), T(1) });
    expectFamilyAlgorithmsAccept<T>(parabola, std::pair<T, T>{ T(-1), T(1) });
    expectFamilyAlgorithmsAccept<T>(arc, std::pair<T, T>{ T(0.4), T(2.0) });
    expectFamilyAlgorithmsAccept<T>(bezier, std::pair<T, T>{ T(0.1), T(0.9) });
    expectFamilyAlgorithmsAccept<T>(spline, std::pair<T, T>{ T(0.1), T(0.9) });
    expectFamilyAlgorithmsAccept<T>(offset, std::pair<T, T>{ T(0.3), T(5) });
}

TEST(CurveContractTest, EveryPlanarCurveWorksWithTheFamilyAlgorithmsInDouble)
{
    expectEveryTypeWorksWithTheFamilyAlgorithms<double>();
}

TEST(CurveContractTest, EveryPlanarCurveWorksWithTheFamilyAlgorithmsInFloat)
{
    expectEveryTypeWorksWithTheFamilyAlgorithms<float>();
}

TEST(CurveContractTest, EveryCurveDifferentiatesConsistently)
{
    const LineCurved      line({ 1.0, 2.0 }, { 2.0, 3.0 });
    const CircleCurved    circle({ 1.0, -2.0 }, 2.0);
    const EllipseCurved   ellipse({ -1.0, 1.0 }, { 0.0, 3.0 }, 3.0, 2.0);
    const HyperbolaCurved hyperbola({ 1.0, 2.0 }, { 2.0, 0.0 }, 2.0, 1.0);
    const ParabolaCurved  parabola({ 0.0, 1.0 }, { 1.0, 1.0 }, 1.0);
    const auto            arc = trimCurve(circle, 0.4, 2.7);
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

/**
 * @brief Verifies that the capability reports what each type can really answer, and that the answer survives erasure.
 *
 * @details
 * - The point of the capability is a runtime handle: a caller holding `const Curve<T>&` must be able to ask "how high
 *   can you differentiate" and get the concrete type's answer, and must get `std::nullopt` rather than a fabricated
 *   zero above it.
 * - The offset curve is the type that cannot answer a third: its own `O'''` needs the basis' fourth.
 */
TEST(CurveContractTest, TheDerivativeCapabilityReportsWhatEachTypeCanAnswer)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         offset = offsetCurve(circle, 0.5);
    EXPECT_EQ(offset.maxDerivativeOrder(), 2);
    EXPECT_TRUE(offset.derivativeAt(0.4, 2).has_value());
    EXPECT_FALSE(offset.derivativeAt(0.4, 3).has_value());

    const BezierCurved bezier({ { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } });
    EXPECT_EQ(bezier.maxDerivativeOrder(), 3);

    const std::optional<vn::math::Vec2d> third = bezier.derivativeAt(0.4, 3);
    ASSERT_TRUE(third.has_value());
    expectNearVector(*third, bezier.thirdDerivative(0.4), 0.0);

    const auto arc = trimCurve(circle, 0.2, 2.0);
    EXPECT_EQ(arc.maxDerivativeOrder(), 3);
    EXPECT_TRUE(arc.derivativeAt(1.0, 3).has_value());

    // Through the contract the concrete answer must survive: this is what a runtime basis needs.
    const Curve<double>& erased = bezier;
    EXPECT_EQ(erased.maxDerivativeOrder(), 3);
    ASSERT_TRUE(erased.derivativeAt(0.4, 3).has_value());
    EXPECT_FALSE(erased.derivativeAt(0.4, 4).has_value());
}

} // namespace
