#pragma once

#include <vine/geom/geom_global.hpp>

#include <optional>
#include <type_traits>

#include <vine/geom/detail/CurveTraits.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief Returns the signed curvature of a planar curve at one parameter, positive where the curve turns left.
 *
 * @details
 * - `k(u) = (C'(u) x C''(u)) / |C'(u)|^3`, where `x` is the two-dimensional cross product
 *   `v.x * w.y - v.y * w.x`. The sign follows the direction of travel: the centre of curvature lies on the left
 *   normal when the answer is positive, which is the same left side `OffsetCurve` offsets to.
 * - `|k|` is the geometric curvature, the reciprocal of the radius of curvature; the sign only says which way the
 *   curve turns, so a mirrored traversal negates it.
 * - The curve's parameterisation does not matter: the speed is divided out, so the answer describes the geometry.
 * - Only `derivative()`, `secondDerivative()` and `isValid()` are used, so any `Curve<T>` works.
 *
 * @param curve The curve to measure; must satisfy `isValid()`.
 * @param u     Parameter in the curve's own units.
 * @return The signed curvature, or `std::nullopt` when the curve is invalid or its speed vanishes at @p u, where the
 *         curvature is undefined (a cusp, or a degenerate parameterisation).
 *
 * @note
 * - A straight line answers exactly 0 and a circle answers `1 / |radius|` at every parameter.
 * - A parabola bending towards its own left answers a positive value; the sign is a property of the traversal, not of
 *   the point set.
 *
 * @tparam C Concrete curve type; must derive from `Curve<C::value_type>`.
 */
template <typename C>
[[nodiscard]] std::optional<geom::detail::CurveScalar<C>> signedCurvature(const C& curve,
                                                                         geom::detail::CurveScalar<C> u)
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
    const T    cross        = velocity.x * acceleration.y - velocity.y * acceleration.x;
    return cross / (speed * speed * speed);
}

VN_GEOM_PLANAR_NS_END
