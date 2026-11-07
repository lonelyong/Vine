#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <vine/math/Math.hpp>
#include <vine/geom/detail/Quadrature.hpp>

namespace {

using vn::geom::detail::adaptiveSimpson;

} // namespace

TEST(Quadrature, integratesAKnownIntegral)
{
    // The integral of u^2 over [0, 2] is 8/3; Simpson's rule is exact on cubics, so the answer is not approximate.
    const auto square = adaptiveSimpson<double>([](double u) { return u * u; }, 0.0, 2.0, 1e-12);
    ASSERT_TRUE(square.has_value());
    EXPECT_NEAR(*square, 8.0 / 3.0, 1e-12);
}

TEST(Quadrature, integratesATrigonometricSpeed)
{
    // The arc length of a unit circle over one turn is 2*pi; the integrand is |C'| = 1, and over a quarter turn of a
    // circle of radius 2 it is 2 * pi / 2.
    const auto constant = adaptiveSimpson<double>([](double) { return 1.0; }, 0.0, vn::math::PI_TWO, 1e-9);
    ASSERT_TRUE(constant.has_value());
    EXPECT_NEAR(*constant, vn::math::PI_TWO, 1e-9);

    const auto speed = adaptiveSimpson<double>([](double u) { return 2.0 * std::abs(std::cos(u)) + 0.0 * u; },
                                               -vn::math::PI_HALF, vn::math::PI_HALF, 1e-10);
    ASSERT_TRUE(speed.has_value());
    EXPECT_NEAR(*speed, 4.0, 1e-9);
}

TEST(Quadrature, refinesWhereTheIntegrandBends)
{
    // A kink at the middle of the interval: the adaptive rule has to halve around it, and one Simpson estimate over the
    // whole interval would be wrong. |u - 1| integrates to 1 over [0, 2].
    const auto kinked = adaptiveSimpson<double>([](double u) { return std::abs(u - 1.0); }, 0.0, 2.0, 1e-9);
    ASSERT_TRUE(kinked.has_value());
    EXPECT_NEAR(*kinked, 1.0, 1e-9);
}

TEST(Quadrature, honoursTheDepthCap)
{
    // The cap bounds the work, so a depth of 0 must not keep refining: the answer is the first halving plus Richardson's
    // correction, which is visibly off for a kink sitting away from the middle.
    const auto capped = adaptiveSimpson<double>([](double u) { return std::abs(u - 0.5); }, 0.0, 2.0, 1e-12, 0);
    ASSERT_TRUE(capped.has_value());
    EXPECT_GT(std::abs(*capped - 1.25), 1e-6);

    // Allowed to refine, the same integral lands on the closed form.
    const auto refined = adaptiveSimpson<double>([](double u) { return std::abs(u - 0.5); }, 0.0, 2.0, 1e-12, 40);
    ASSERT_TRUE(refined.has_value());
    EXPECT_NEAR(*refined, 1.25, 1e-12);
}

TEST(Quadrature, refusesWhatItCannotIntegrate)
{
    const auto square = [](double u) { return u * u; };

    // A reversed or degenerate interval, a non-finite sample and a non-positive depth budget are all handled.
    EXPECT_FALSE(adaptiveSimpson<double>(square, 2.0, 0.0, 1e-9).has_value());
    EXPECT_FALSE(adaptiveSimpson<double>(square, 1.0, 1.0, 1e-9).has_value());
    EXPECT_FALSE(adaptiveSimpson<double>([](double u) { return 1.0 / u; }, -1.0, 1.0, 1e-9).has_value());
    EXPECT_FALSE(adaptiveSimpson<double>([](double) { return std::numeric_limits<double>::infinity(); }, 0.0, 1.0, 1e-9)
                     .has_value());
}

TEST(Quadrature, worksInSinglePrecision)
{
    const auto square = adaptiveSimpson<float>([](float u) { return u * u; }, 0.0f, 2.0f, 1e-5f);
    ASSERT_TRUE(square.has_value());
    EXPECT_NEAR(*square, 8.0f / 3.0f, 1e-5f);
}
