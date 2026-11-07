#pragma once

#include <vine/geom/geom_global.hpp>

#include <optional>
#include <type_traits>

#include <vine/geom/detail/CurveTraits.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief Returns the curvature of a space curve at one parameter.
 *
 * @details
 * - `k(u) = |C'(u) x C''(u)| / |C'(u)|^3`, the length of the acceleration's component orthogonal to the velocity,
 *   divided by the speed cubed. It is the reciprocal of the radius of curvature and is never negative.
 * - Unlike the planar `signedCurvature()`, a space curve has no canonical "left" side, so the sign is not defined:
 *   there is no orientation that could decide it. The planar module reports a signed value because a plane does
 *   provide one.
 * - The curve's parameterisation does not matter: the speed is divided out, so the answer describes the geometry.
 * - Only `derivative()`, `secondDerivative()` and `isValid()` are used, so any `Curve<T>` works.
 *
 * @param curve The curve to measure; must satisfy `isValid()`.
 * @param u     Parameter in the curve's own units.
 * @return The curvature, or `std::nullopt` when the curve is invalid or its speed vanishes at @p u, where the
 *         curvature is undefined (a cusp, or a degenerate parameterisation).
 *
 * @note
 * - A straight line answers exactly 0 and a circle answers `1 / |radius|` at every parameter.
 * - A planar curve embedded in 3D reports the absolute value of what `geom::planar::signedCurvature()` reports for it.
 *
 * @tparam C Concrete curve type; must derive from `Curve<C::value_type>`.
 */
template <typename C>
[[nodiscard]] std::optional<geom::detail::CurveScalar<C>> curvature(const C& curve, geom::detail::CurveScalar<C> u)
    requires std::is_base_of_v<Curve<geom::detail::CurveScalar<C>>, C>
{
    using T = geom::detail::CurveScalar<C>;

    if (!curve.isValid()) {
        return std::nullopt;
    }

    const auto velocity = curve.derivative(u);
    const T    speed    = velocity.length();
    if (!(speed > vn::math::EPS<T>())) {
        return std::nullopt;
    }

    const auto acceleration = curve.secondDerivative(u);
    return velocity.cross(acceleration).length() / (speed * speed * speed);
}

VN_GEOM_SPATIAL_NS_END
