#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>

#include <vine/geom/planar/BezierCurve.hpp>
#include <vine/geom/planar/CircleCurve.hpp>
#include <vine/geom/planar/CurveLength.hpp>
#include <vine/geom/planar/CurveProjection.hpp>
#include <vine/geom/planar/OffsetCurve.hpp>
#include <vine/geom/planar/TrimmedCurve.hpp>

namespace
{

using vn::geom::planar::BezierCurved;
using vn::geom::planar::CircleCurved;
using vn::geom::planar::CircleCurvef;
using vn::geom::planar::TrimmedCurved;
using vn::geom::planar::TrimmedCurvef;
using vn::geom::planar::arcLength;
using vn::geom::planar::closestPointOnCurve;
using vn::geom::planar::offsetCurve;
using vn::geom::planar::trimCurve;

TEST(TrimmedCurveTest, WalksTheCircleBetweenItsAngles)
{
    const CircleCurved circle({ 1.0, -2.0 }, 2.0);
    const auto         quarter = trimCurve(circle, 0.0, vn::math::PI_HALF);

    EXPECT_NEAR(quarter.evaluate(0.0).x, 3.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(0.0).y, -2.0, 1e-12);

    const double diagonal = std::sqrt(2.0);
    EXPECT_NEAR(quarter.evaluate(vn::math::PI_QUARTER).x, 1.0 + diagonal, 1e-12);
    EXPECT_NEAR(quarter.evaluate(vn::math::PI_QUARTER).y, -2.0 + diagonal, 1e-12);

    EXPECT_NEAR(quarter.evaluate(vn::math::PI_HALF).x, 1.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(vn::math::PI_HALF).y, 0.0, 1e-12);

    // Trimming does not reparameterise: the piece keeps the circle's speed.
    EXPECT_NEAR(quarter.derivative(0.7).length(), 2.0, 1e-12);
}

TEST(TrimmedCurveTest, AQuarterTurnIsAPieceWithTwoEnds)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         quarter = trimCurve(circle, 0.0, vn::math::PI_HALF);

    EXPECT_TRUE(quarter.isValid());
    EXPECT_FALSE(quarter.isClosed());
    EXPECT_FALSE(quarter.isPeriodic());
    EXPECT_EQ(quarter.period(), 0.0);
    EXPECT_NEAR(quarter.firstParameter(), 0.0, 1e-12);
    EXPECT_NEAR(quarter.lastParameter(), vn::math::PI_HALF, 1e-12);
}

TEST(TrimmedCurveTest, AFullTurnIsClosedAndPeriodic)
{
    const CircleCurved circle({ 1.0, 2.0 }, 3.0);
    const auto         full_turn = trimCurve(circle, 0.0, vn::math::PI_TWO);

    EXPECT_TRUE(full_turn.isClosed());
    EXPECT_TRUE(full_turn.isPeriodic());
    EXPECT_NEAR(full_turn.period(), vn::math::PI_TWO, 1e-12);
}

TEST(TrimmedCurveTest, ARangeThatOvershootsTheCircleIsNotClosed)
{
    const CircleCurved circle({ 0.0, 0.0 }, 1.0);
    const auto       overshooting = trimCurve(circle, 0.0, vn::math::PI_TWO + 0.3);

    // It covers the circle's whole range but ends at a different point, so it is a piece, not a closed curve.
    EXPECT_FALSE(overshooting.isClosed());
    EXPECT_TRUE(overshooting.isPeriodic());
}

TEST(TrimmedCurveTest, ReversesInsideItsOwnInterval)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         quarter = trimCurve(circle, 0.0, vn::math::PI_HALF);

    EXPECT_NEAR(quarter.reversedParameter(0.0), vn::math::PI_HALF, 1e-12);
    EXPECT_NEAR(quarter.reversedParameter(vn::math::PI_HALF), 0.0, 1e-12);
    EXPECT_NEAR(quarter.reversedParameter(0.3), vn::math::PI_HALF - 0.3, 1e-12);
    EXPECT_NEAR(quarter.reversedParameter(quarter.reversedParameter(0.7)), 0.7, 1e-12);
}

TEST(TrimmedCurveTest, AReversedIntervalIsNotAPiece)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         reversed = trimCurve(circle, vn::math::PI, 0.0);

    EXPECT_FALSE(reversed.isValid());
    // The queries still answer the basis rather than reordering the interval.
    EXPECT_NEAR(reversed.evaluate(0.0).x, 2.0, 1e-12);
}

TEST(TrimmedCurveTest, ADegenerateCircleIsNotAPiece)
{
    const CircleCurved degenerate({ 1.0, -1.0 }, 0.0);
    const auto         trim = trimCurve(degenerate, 0.0, vn::math::PI_HALF);

    EXPECT_FALSE(trim.isValid());
    EXPECT_NEAR(trim.evaluate(0.4).x, 1.0, 1e-12);
    EXPECT_NEAR(trim.evaluate(0.4).y, -1.0, 1e-12);
}

TEST(TrimmedCurveTest, AnswersOutsideTheTrimWithTheBasis)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         quarter = trimCurve(circle, 0.0, vn::math::PI_HALF);

    // Trimming removes a range; it does not cut the curve short.
    EXPECT_NEAR(quarter.evaluate(vn::math::PI).x, -2.0, 1e-12);
    EXPECT_NEAR(quarter.evaluate(vn::math::PI).y, 0.0, 1e-12);
}

TEST(TrimmedCurveTest, AnswersThroughTheBaseInterface)
{
    const CircleCurvef circle({ 0.0f, 0.0f }, 2.0f);
    const auto         quarter = trimCurve(circle, 0.0f, static_cast<float>(vn::math::PI_HALF));

    const vn::geom::planar::Curve<float>& erased = quarter;
    EXPECT_TRUE(erased.isValid());
    EXPECT_NEAR(erased.evaluate(0.0f).x, 2.0f, 1e-5f);
    EXPECT_EQ(erased.maxDerivativeOrder(), 3);
    EXPECT_TRUE(erased.derivativeAt(0.5f, 3).has_value());
}

TEST(TrimmedCurveTest, TrimsACurveThatIsNotACircle)
{
    // The point of the generic trim: any basis, not only a circle.
    const BezierCurved bezier({ { 0.0, 0.0 }, { 1.0, 3.0 }, { 3.0, -1.0 }, { 4.0, 2.0 } });
    const auto         piece = trimCurve(bezier, 0.25, 0.75);

    EXPECT_TRUE(piece.isValid());
    EXPECT_FALSE(piece.isClosed());
    EXPECT_NEAR(piece.evaluate(0.25).x, bezier.evaluate(0.25).x, 1e-12);
    EXPECT_NEAR(piece.evaluate(0.9).y, bezier.evaluate(0.9).y, 1e-12);

    // A quarter of the curve is shorter than the whole one, and the family algorithms accept the piece.
    const auto whole_length = arcLength(bezier);
    const auto piece_length = arcLength(piece);
    ASSERT_TRUE(whole_length);
    ASSERT_TRUE(piece_length);
    EXPECT_LT(*piece_length, *whole_length);
    EXPECT_GT(*piece_length, 0.0);

    const auto projection = closestPointOnCurve(piece, vn::math::Point2<double>{ 2.0, 0.0 });
    ASSERT_TRUE(projection);
    EXPECT_TRUE(projection->parameter >= 0.25 && projection->parameter <= 0.75);
}

TEST(TrimmedCurveTest, ForwardsTheBasisDerivativeCapability)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);

    // A trim of a circle answers three, because a circle does.
    const auto over_circle = trimCurve(circle, 0.0, 1.0);
    EXPECT_EQ(over_circle.maxDerivativeOrder(), 3);
    EXPECT_TRUE(over_circle.derivativeAt(0.5, 3).has_value());

    // A trim of an offset answers two, because an offset does; nothing above it is invented.
    const auto over_offset = trimCurve(offsetCurve(circle, 0.5), 0.5, 3.0);
    EXPECT_EQ(over_offset.maxDerivativeOrder(), 2);
    EXPECT_TRUE(over_offset.derivativeAt(1.0, 2).has_value());
    EXPECT_FALSE(over_offset.derivativeAt(1.0, 3).has_value());
}

TEST(TrimmedCurveTest, AMissingBasisIsAReportedMistake)
{
    const TrimmedCurved trim(nullptr, 0.0, 1.0);

    EXPECT_FALSE(trim.isValid());
    EXPECT_FALSE(trim.isClosed());
    EXPECT_FALSE(trim.isPeriodic());
    EXPECT_EQ(trim.period(), 0.0);
    EXPECT_EQ(trim.evaluate(0.5).x, 0.0);
    EXPECT_EQ(trim.evaluate(0.5).y, 0.0);
    EXPECT_EQ(trim.derivative(0.5).length(), 0.0);
    EXPECT_FALSE(trim.derivativeAt(0.5, 1).has_value());
    EXPECT_EQ(trim.maxDerivativeOrder(), 2);
}

TEST(TrimmedCurveTest, ATrimOfATrimKeepsTheInnerRange)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const auto         inner = trimCurve(circle, 0.5, 3.0);
    const auto         outer = trimCurve(TrimmedCurved(inner.basis_curve, 1.0, 2.0), 1.2, 1.8);

    // The outer trim reflects inside its own interval, not inside the basis or the inner trim.
    EXPECT_NEAR(outer.reversedParameter(1.2), 1.8, 1e-12);
    EXPECT_TRUE(outer.isValid());
}

} // namespace
