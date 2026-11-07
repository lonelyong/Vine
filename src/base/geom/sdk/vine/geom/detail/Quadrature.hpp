#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <concepts>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief Integrates a one-variable function over an interval by adaptive Simpson quadrature.
 *
 * @details
 * - Simpson's rule on one interval is `(b - a) / 6 * (f(a) + 4 f(mid) + f(b))`. Each interval is halved while the two
 *   half estimates disagree with the whole one by more than `15 * tolerance`, which is Richardson's error estimate for
 *   this rule, and the halves are added back with the correction `delta / 15`.
 * - `max_depth` caps the halving per interval, so a function with a kink (an unbounded or non-smooth speed) cannot
 *   recurse without bound; a negative depth is treated as 0, and 0 still does the first halving of the whole interval
 *   (two Simpson estimates plus the correction) before it stops.
 * - No allocation and no retained state: @p function is only ever called, `6 + 2^max_depth` times at worst.
 * - This kernel deliberately knows nothing about curves: it takes a scalar function and an interval, which is why it
 *   lives in `math` rather than in a geometry module (`geometry-intersection.md` 2.7 states the same rule for the root
 *   finder).
 *
 * @param function   Integrand answering `T(T)`; must be finite wherever it is sampled.
 * @param lower      Lower end of the interval.
 * @param upper      Upper end of the interval; must not precede @p lower.
 * @param tolerance  Absolute error to aim for; a non-positive value falls back to `EPS<T>()`.
 * @param max_depth  Halving cap per interval; negative values are treated as 0, which still halves the interval once.
 * @return The integral, or `std::nullopt` when the interval is reversed, a sample is not finite, or the result is not.
 *
 * @warning
 * - The tolerance is on the **integral**, not on the interval width; it does not move the ends.
 * - A function that is infinite somewhere inside the interval (a speed at an unbounded parameter) has no finite answer
 *   here: the rule propagates the non-finite sample instead of producing a plausible-looking number.
 *
 * @tparam T        Floating-point scalar.
 * @tparam Function Callable answering `T(T)`.
 */
template <vn::math::FP T, typename Function>
    requires std::invocable<Function&, T>
[[nodiscard]] std::optional<T> adaptiveSimpson(Function&& function, T lower, T upper, T tolerance, int max_depth = 20)
{
    if (!(upper > lower)) {
        return std::nullopt;
    }

    const T   target      = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    const int depth_limit = (max_depth < 0) ? 0 : ((max_depth > 40) ? 40 : max_depth);

    const auto integrate = [&](auto&& self, T a, T b, T fa, T fm, T fb, T whole, T error, int depth) -> T {
        const T middle       = (a + b) / T(2);
        const T left_middle  = (a + middle) / T(2);
        const T right_middle = (middle + b) / T(2);
        const T f_left       = function(left_middle);
        const T f_right      = function(right_middle);

        const T left  = (middle - a) / T(6) * (fa + T(4) * f_left + fm);
        const T right = (b - middle) / T(6) * (fm + T(4) * f_right + fb);
        const T delta = left + right - whole;
        if (depth <= 0 || std::abs(delta) <= T(15) * error) {
            return left + right + delta / T(15);
        }

        return self(self, a, middle, fa, f_left, fm, left, error / T(2), depth - 1)
             + self(self, middle, b, fm, f_right, fb, right, error / T(2), depth - 1);
    };

    const T middle = (lower + upper) / T(2);
    const T fa     = function(lower);
    const T fm     = function(middle);
    const T fb     = function(upper);
    if (!std::isfinite(fa) || !std::isfinite(fm) || !std::isfinite(fb)) {
        return std::nullopt;
    }

    const T whole      = (upper - lower) / T(6) * (fa + T(4) * fm + fb);
    const T integral   = integrate(integrate, lower, upper, fa, fm, fb, whole, target, depth_limit);
    if (!std::isfinite(integral)) {
        return std::nullopt;
    }

    return integral;
}

VN_GEOM_DETAIL_NS_END
