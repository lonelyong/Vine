#include <gtest/gtest.h>

#include <vector>

#include <vine/math/spline/BSplineBasis.hpp>

namespace
{

using vn::math::spline::knotSpan;

TEST(BSplineBasisTest, FindsTheSpanOfAPiecewiseLinearKnotVector)
{
    // Degree 1, clamped at both ends, one interior knot: the flat knot vector is 0, 0, 1, 2, 2.
    const std::vector<double> knots{ 0.0, 1.0, 2.0 };
    const std::vector<int>    multiplicities{ 2, 1, 2 };

    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 1, 0.0), 1u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 1, 0.5), 1u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 1, 1.0), 2u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 1, 1.5), 2u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 1, 2.0), 2u);
}

TEST(BSplineBasisTest, FindsTheSpanOfAQuadraticWithTwoInteriorKnots)
{
    // Degree 2, clamped, two simple interior knots: the flat knot vector is 0, 0, 0, 1, 2, 3, 3, 3.
    const std::vector<double> knots{ 0.0, 1.0, 2.0, 3.0 };
    const std::vector<int>    multiplicities{ 3, 1, 1, 3 };

    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 2, 0.0), 2u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 2, 0.5), 2u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 2, 1.5), 3u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 2, 2.5), 4u);
    EXPECT_EQ(knotSpan<double>(knots, multiplicities, 2, 3.0), 4u);
}

TEST(BSplineBasisTest, TheSpanSearchTerminatesOnAOneApartBracket)
{
    // The regression: `u` in the last span of a two-span bracket used to loop forever, because the
    // midpoint stayed equal to the lower bound.
    const std::vector<float> knots{ 0.0f, 1.0f, 2.0f };
    const std::vector<int>   multiplicities{ 2, 1, 2 };

    EXPECT_EQ(knotSpan<float>(knots, multiplicities, 1, 1.75f), 2u);
}

} // namespace
