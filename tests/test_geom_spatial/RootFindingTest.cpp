#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <vine/geom/detail/RootFinding.hpp>

namespace {

using vn::geom::detail::parameterReaching;
using vn::geom::detail::rootInBracket;

double quadratic(double u)
{
    return u * u - 2.0;
}

} // namespace

TEST(RootFinding, narrowsABracketToTheRoot)
{
    const auto root = rootInBracket<double>(quadratic, 0.0, 2.0, 1e-12);
    ASSERT_TRUE(root.has_value());
    EXPECT_NEAR(*root, std::sqrt(2.0), 1e-11);

    // A root that sits exactly on an end of the bracket is answered as that end, without iterating.
    const auto at_lower = rootInBracket<double>([](double u) { return u - 3.0; }, 3.0, 5.0, 1e-12);
    ASSERT_TRUE(at_lower.has_value());
    EXPECT_DOUBLE_EQ(*at_lower, 3.0);
}

TEST(RootFinding, reportsABracketThatDoesNotStraddleARoot)
{
    // Both ends positive: from inside the bracket there is no way to know whether the root is elsewhere.
    EXPECT_FALSE(rootInBracket<double>(quadratic, 2.0, 3.0, 1e-12).has_value());

    // A reversed or degenerate bracket, a non-positive tolerance and an empty iteration budget are refused.
    EXPECT_FALSE(rootInBracket<double>(quadratic, 2.0, 0.0, 1e-12).has_value());
    EXPECT_FALSE(rootInBracket<double>(quadratic, 1.0, 1.0, 1e-12).has_value());
    EXPECT_FALSE(rootInBracket<double>(quadratic, 0.0, 2.0, 0.0).has_value());
    EXPECT_FALSE(rootInBracket<double>(quadratic, 0.0, 2.0, 1e-12, 0).has_value());
}

TEST(RootFinding, aNanFromTheFunctionIsReportedInsteadOfCompared)
{
    // Every comparison with NaN is false, so a bracketing test would silently answer "no root": the search refuses.
    const auto root = rootInBracket<double>([](double u) { return std::isnan(u) ? u : u - 1.0; }, 0.0, 2.0, 1e-12);
    EXPECT_TRUE(root.has_value());
    EXPECT_NEAR(*root, 1.0, 1e-11);

    const auto nan_at_end = rootInBracket<double>([](double) { return std::numeric_limits<double>::quiet_NaN(); }, 0.0,
                                                2.0, 1e-12);
    EXPECT_FALSE(nan_at_end.has_value());
}

TEST(RootFinding, keepsConvergingOnASkewedBracket)
{
    // False position alone creeps towards the small end of a cubic bracket; the bisection fallback is what bounds the
    // iteration count, so this case is the one that would hang or leave a wide bracket.
    const auto root = rootInBracket<double>([](double u) { return u * u * u; }, -1.0, 8.0, 1e-10);
    ASSERT_TRUE(root.has_value());
    EXPECT_NEAR(*root, 0.0, 1e-9);

    // A tight iteration budget still answers something inside the final bracket rather than nothing.
    const auto capped = rootInBracket<double>(quadratic, 0.0, 2.0, 0.0, 0);
    EXPECT_FALSE(capped.has_value());
    const auto few = rootInBracket<double>(quadratic, 0.0, 2.0, 1e-12, 2);
    ASSERT_TRUE(few.has_value());
    EXPECT_NEAR(*few, std::sqrt(2.0), 0.5);
}

TEST(RootFinding, worksInSinglePrecision)
{
    const auto root = rootInBracket<float>([](float u) { return u * u - 2.0f; }, 0.0f, 2.0f, 1e-6f);
    ASSERT_TRUE(root.has_value());
    EXPECT_NEAR(*root, std::sqrt(2.0f), 1e-4f);
}

TEST(RootFinding, theMonotoneRefinerReachesATargetWithASlopeHint)
{
    // value(u) = u^2 rising over [0, 3]: reaching 2 needs u = sqrt(2), and the slope hint is 2u.
    const auto value = [](double u) { return std::optional<double>(u * u); };
    const auto slope = [](double u) { return 2.0 * u; };

    const auto reached = parameterReaching<double>(value, slope, 0.0, 3.0, 2.0, 1e-12);
    ASSERT_TRUE(reached.has_value());
    EXPECT_NEAR(*reached, std::sqrt(2.0), 1e-11);

    // A target at the lower end is answered at once, and one outside the bracket still narrows into it rather than
    // failing: the bracket is the caller's promise, so the search only reports the best parameter it reached.
    const auto at_lower = parameterReaching<double>(value, slope, 0.0, 3.0, 0.0, 1e-12);
    ASSERT_TRUE(at_lower.has_value());
    EXPECT_NEAR(*at_lower, 0.0, 1e-6);
}

TEST(RootFinding, theMonotoneRefinerFallsBackWhenTheSlopeIsUseless)
{
    const auto value = [](double u) { return std::optional<double>(u); };

    // A zero or negative slope cannot give a Newton step, so the search bisects; it still converges on a linear target.
    const auto flat = parameterReaching<double>(value, [](double) { return 0.0; }, 0.0, 4.0, 1.0, 1e-12);
    ASSERT_TRUE(flat.has_value());
    EXPECT_NEAR(*flat, 1.0, 1e-6);

    // A failed evaluation ends the search instead of being read as a value.
    const auto failing = parameterReaching<double>([](double) { return std::optional<double>(); },
                                                   [](double) { return 1.0; }, 0.0, 4.0, 1.0, 1e-12);
    EXPECT_FALSE(failing.has_value());

    // Degenerate requests are refused: a reversed bracket, a non-positive tolerance, an empty iteration budget.
    EXPECT_FALSE(parameterReaching<double>(value, [](double) { return 1.0; }, 4.0, 0.0, 1.0, 1e-12).has_value());
    EXPECT_FALSE(parameterReaching<double>(value, [](double) { return 1.0; }, 0.0, 4.0, 1.0, 0.0).has_value());
    EXPECT_FALSE(parameterReaching<double>(value, [](double) { return 1.0; }, 0.0, 4.0, 1.0, 1e-12, 0).has_value());
}
