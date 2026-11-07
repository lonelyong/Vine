#pragma once

#include <vine/geom/geom_global.hpp>

#include <optional>

#include <vine/math/Math.hpp>

#include "CurveTraits.hpp"
#include "RootFinding.hpp"

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief Narrows one sign change of @p value to a parameter, converting a geometric tolerance into a parameter one.
 *
 * @details
 * - The conversion is the point of this helper: a value tolerance says "the curve moved this much", and a parameter
 *   step of `du` moves it by about `|dP/du|`, so the parameter tolerance is `value_tolerance / speed`. The speed is
 *   taken at the middle of the bracket, which is where the root is expected.
 * - A vanishing speed cannot be divided by, so @p fallback_tolerance is used instead; the caller's scan step scaled
 *   down is the sensible choice, because it keeps the answer inside the bracket it came from.
 * - The narrowing itself is `vn::geom::detail::rootInBracket()`; this helper adds the unit conversion and nothing
 *   else. Keeping it out of the kernel is deliberate: the conversion needs the curve's speed, which is exactly the
 *   part `math` does not know about.
 * - It is dimension-free - it only calls `evaluate`'s derivative through @p curve and compares scalars - so the planar
 *   and space families share this one copy instead of carrying a textually identical pair.
 *
 * @param curve              The curve whose speed converts the two tolerances.
 * @param value              Scalar function whose sign changes inside the bracket.
 * @param previous           First parameter of the bracket (the sample before the change).
 * @param current            Second parameter of the bracket (the sample after the change).
 * @param value_tolerance    Geometric tolerance to reach, in the units of @p value.
 * @param fallback_tolerance Parameter tolerance used when the speed vanishes.
 * @return The root inside the bracket, or `std::nullopt` when the bracket does not straddle one.
 *
 * @tparam C        Concrete curve type.
 * @tparam Function Callable answering a scalar from a parameter.
 */
template <typename C, typename Function>
[[nodiscard]] std::optional<CurveScalar<C>> narrowCrossing(const C& curve, Function&& value, CurveScalar<C> previous,
                                                           CurveScalar<C> current, CurveScalar<C> value_tolerance,
                                                           CurveScalar<C> fallback_tolerance)
{
    using T = CurveScalar<C>;

    const T middle    = previous + (current - previous) * T(0.5);
    const T speed     = curve.derivative(middle).length();
    const T converted = (speed > vn::math::EPS<T>()) ? value_tolerance / speed : fallback_tolerance;
    return rootInBracket<T>(value, previous, current, (converted > T(0)) ? converted : fallback_tolerance);
}

VN_GEOM_DETAIL_NS_END
