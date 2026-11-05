#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Types.hpp>

#include <vine/geom2d/ArcCurve.hpp>
#include <vine/geom2d/CircleCurve.hpp>
#include <vine/geom2d/EllipseCurve.hpp>

namespace
{

using vn::geom2d::ArcCurved;
using vn::geom2d::CircleCurved;
using vn::geom2d::CircleCurvef;
using vn::geom2d::EllipseCurved;

/**
 * @brief Checks that the box contains the curve over its interval, allowing the rounding of the support values.
 */
template <vn::math::FP T, typename C>
void expectEveryPointInside(const C& curve, const vn::math::Rect2<T>& box, T from, T to)
{
    const T slack = T(64) * vn::math::EPS<T>();
    for (int index = 0; index <= 64; ++index) {
        const T    u     = from + (to - from) * static_cast<T>(index) / T(64);
        const auto point = curve.evaluate(u);

        EXPECT_GE(point.x, box.xmin - slack);
        EXPECT_LE(point.x, box.xmax + slack);
        EXPECT_GE(point.y, box.ymin - slack);
        EXPECT_LE(point.y, box.ymax + slack);
    }
}

TEST(BoundsTest, ACircleIsItsCentreGrownByItsRadius)
{
    const CircleCurved circle({ 1.0, 2.0 }, 2.0);
    const auto         box = circle.bounds();

    EXPECT_NEAR(box.xmin, -1.0, 1e-12);
    EXPECT_NEAR(box.ymin, 0.0, 1e-12);
    EXPECT_NEAR(box.xmax, 3.0, 1e-12);
    EXPECT_NEAR(box.ymax, 4.0, 1e-12);

    expectEveryPointInside(circle, box, circle.firstParameter(), circle.lastParameter());
}

TEST(BoundsTest, ANegativeRadiusAnswersTheSameBox)
{
    const CircleCurved circle({ 1.0, 2.0 }, -2.0);
    const auto         box = circle.bounds();

    EXPECT_FALSE(circle.isValid());
    EXPECT_NEAR(box.width(), 4.0, 1e-12);
    EXPECT_NEAR(box.height(), 4.0, 1e-12);
}

TEST(BoundsTest, AZeroRadiusIsAPointBox)
{
    const CircleCurved circle({ 1.0, 2.0 }, 0.0);
    const auto         box = circle.bounds();

    EXPECT_TRUE(box.isZero());
    EXPECT_NEAR(box.xmin, 1.0, 1e-12);
    EXPECT_NEAR(box.ymin, 2.0, 1e-12);
}

TEST(BoundsTest, AnAxisAlignedEllipseIsItsSemiAxes)
{
    const EllipseCurved ellipse({ 0.0, 0.0 }, { 1.0, 0.0 }, 2.0, 1.0);
    const auto          box = ellipse.bounds();

    EXPECT_NEAR(box.xmin, -2.0, 1e-12);
    EXPECT_NEAR(box.ymin, -1.0, 1e-12);
    EXPECT_NEAR(box.xmax, 2.0, 1e-12);
    EXPECT_NEAR(box.ymax, 1.0, 1e-12);
}

TEST(BoundsTest, ARotatedEllipseSwapsItsSemiAxes)
{
    const EllipseCurved ellipse({ 3.0, 4.0 }, { 0.0, 1.0 }, 2.0, 1.0);
    const auto          box = ellipse.bounds();

    // The major axis lies along +Y, so the major radius sizes the height and the minor radius the width.
    EXPECT_NEAR(box.width(), 2.0, 1e-12);
    EXPECT_NEAR(box.height(), 4.0, 1e-12);
    EXPECT_NEAR(box.center().x, 3.0, 1e-12);
    EXPECT_NEAR(box.center().y, 4.0, 1e-12);
}

TEST(BoundsTest, ADiagonalEllipseUsesItsSupportFunction)
{
    const double        s       = std::sqrt(0.5);
    const EllipseCurved ellipse({ 0.0, 0.0 }, { s, s }, 2.0, 1.0);
    const auto          box = ellipse.bounds();
    const double        half = std::sqrt(2.5);

    EXPECT_NEAR(box.xmax, half, 1e-12);
    EXPECT_NEAR(box.ymax, half, 1e-12);
    EXPECT_NEAR(box.xmin, -half, 1e-12);
    EXPECT_NEAR(box.ymin, -half, 1e-12);
}

TEST(BoundsTest, AnArcKeepsOnlyTheExtremaInsideItsInterval)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    upper(circle, vn::math::PI_QUARTER, 3.0 * vn::math::PI_QUARTER);
    const auto         box       = upper.bounds();
    const double       reach     = 2.0 * std::sqrt(0.5);

    // The quarter turn inside the interval is the top of the circle; the ends give the horizontal extent.
    EXPECT_NEAR(box.ymax, 2.0, 1e-12);
    EXPECT_NEAR(box.ymin, reach, 1e-12);
    EXPECT_NEAR(box.xmin, -reach, 1e-12);
    EXPECT_NEAR(box.xmax, reach, 1e-12);

    expectEveryPointInside(upper, box, upper.firstParameter(), upper.lastParameter());
}

TEST(BoundsTest, AReversedArcAnswersTheSameBox)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    forward(circle, vn::math::PI_QUARTER, 3.0 * vn::math::PI_QUARTER);
    const ArcCurved    backward(circle, 3.0 * vn::math::PI_QUARTER, vn::math::PI_QUARTER);

    EXPECT_FALSE(backward.isValid());

    const auto forward_box  = forward.bounds();
    const auto backward_box = backward.bounds();
    EXPECT_NEAR(backward_box.xmin, forward_box.xmin, 1e-12);
    EXPECT_NEAR(backward_box.ymin, forward_box.ymin, 1e-12);
    EXPECT_NEAR(backward_box.xmax, forward_box.xmax, 1e-12);
    EXPECT_NEAR(backward_box.ymax, forward_box.ymax, 1e-12);
}

TEST(BoundsTest, AQuarterArcIsTheQuarterBox)
{
    const CircleCurved circle({ 0.0, 0.0 }, 2.0);
    const ArcCurved    quarter(circle, 0.0, vn::math::PI_HALF);
    const auto         box = quarter.bounds();

    EXPECT_NEAR(box.xmin, 0.0, 1e-12);
    EXPECT_NEAR(box.ymin, 0.0, 1e-12);
    EXPECT_NEAR(box.xmax, 2.0, 1e-12);
    EXPECT_NEAR(box.ymax, 2.0, 1e-12);
}

TEST(BoundsTest, BothPrecisionsAnswerTheSameCircleBox)
{
    const CircleCurvef narrow({ 1.0f, 2.0f }, 2.0f);
    const auto         box = narrow.bounds();

    EXPECT_NEAR(box.width(), 4.0f, 1e-5f);
    EXPECT_NEAR(box.height(), 4.0f, 1e-5f);
    EXPECT_NEAR(box.center().x, 1.0f, 1e-5f);

    expectEveryPointInside(narrow, box, narrow.firstParameter(), narrow.lastParameter());
}

} // namespace
