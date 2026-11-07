#include <gtest/gtest.h>

#include <vector>

#include <vine/geom/detail/BSplineBasis.hpp>

namespace
{

using vn::geom::detail::basisValue;
using vn::geom::detail::knotAt;
using vn::geom::detail::knotContinuity;
using vn::geom::detail::knotSpan;

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

TEST(BSplineBasisTest, TheBasisFunctionsPartitionUnity)
{
    // Degree 3, clamped, three simple interior knots: flat knots 0,0,0,0,1,2,3,4,4,4,4 - seven basis functions.
    const std::vector<double> knots{ 0.0, 1.0, 2.0, 3.0, 4.0 };
    const std::vector<int>    multiplicities{ 4, 1, 1, 1, 4 };
    const std::size_t         pole_count = 7;

    for (int step = 0; step <= 80; ++step) {
        const double u   = 4.0 * static_cast<double>(step) / 80.0;
        double       sum = 0.0;
        for (std::size_t index = 0; index < pole_count; ++index) {
            const double value = basisValue<double>(knots, multiplicities, index, 3, 0, u);
            EXPECT_GE(value, -1e-12);
            sum += value;
        }
        EXPECT_NEAR(sum, 1.0, 1e-12);
    }
}

TEST(BSplineBasisTest, TheSpanHoldsTheParameter)
{
    const std::vector<double> knots{ 0.0, 1.0, 2.0, 3.0, 4.0 };
    const std::vector<int>    multiplicities{ 4, 1, 1, 1, 4 };
    const std::size_t         pole_count = 7;

    for (int step = 0; step <= 80; ++step) {
        const double      u    = 4.0 * static_cast<double>(step) / 80.0;
        const std::size_t span = knotSpan<double>(knots, multiplicities, 3, u);

        EXPECT_GE(span, 3u);
        EXPECT_LE(span, pole_count - 1);
        EXPECT_LE(knotAt<double>(knots, multiplicities, span), u);
        EXPECT_LE(u, knotAt<double>(knots, multiplicities, span + 1));
    }
}

TEST(BSplineBasisTest, TheDerivativeMatchesAFiniteDifference)
{
    const std::vector<double> knots{ 0.0, 1.0, 2.0, 3.0, 4.0 };
    const std::vector<int>    multiplicities{ 4, 1, 1, 1, 4 };
    const double              h = 1e-6;

    // Away from the knots, where the derivative of the basis functions is smooth.
    for (const double u : { 0.35, 1.4, 2.55, 3.3 }) {
        for (std::size_t index = 0; index < 7; ++index) {
            const double symbolic =
                basisValue<double>(knots, multiplicities, index, 3, 1, u);
            const double difference = (basisValue<double>(knots, multiplicities, index, 3, 0, u + h)
                                       - basisValue<double>(knots, multiplicities, index, 3, 0, u - h))
                                    / (2.0 * h);
            EXPECT_NEAR(symbolic, difference, 1e-5);
        }
    }
}

TEST(BSplineBasisTest, TheContinuityComesFromTheInternalMultiplicity)
{
    const std::vector<int> simple{ 4, 1, 4 };
    const std::vector<int> doubled{ 4, 2, 4 };
    const std::vector<int> tripled{ 4, 3, 4 };
    const std::vector<int> single_span{ 4, 4 };

    EXPECT_EQ(knotContinuity(3, simple), vn::geom::Continuity::C2);
    EXPECT_EQ(knotContinuity(3, doubled), vn::geom::Continuity::C1);
    EXPECT_EQ(knotContinuity(3, tripled), vn::geom::Continuity::C0);
    EXPECT_EQ(knotContinuity(3, single_span), vn::geom::Continuity::CN);
}

} // namespace
