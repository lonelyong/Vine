#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <vine/math/Math.hpp>

#include "CurveTraits.hpp"

VN_GEOM_DETAIL_NS_BEGIN

/**
 * @brief Refines a parameter pair towards a local minimum of the distance with a bounded Gauss-Newton iteration.
 *
 * @details
 * - It minimises `|C1(u) - C2(v)|^2` with the Jacobian `[C1'(u), -C2'(v)]`: the step solves
 *   `(J^T J) [du, dv] = -J^T r`. A singular normal matrix means the tangents are parallel (a face-to-face pair), where
 *   the two-parameter step is meaningless; the current pair is returned.
 * - Steps are clamped to a quarter of each window, so a bad step cannot leave the search range.
 * - It is dimension-free - it only subtracts points, takes dot products and reads two derivatives - so both families
 *   share this one refinement instead of carrying a textually identical pair.
 *
 * @param first        First curve.
 * @param second       Second curve.
 * @param first_range  Search window on @p first.
 * @param second_range Search window on @p second.
 * @param start_u      Starting parameter on @p first.
 * @param start_v      Starting parameter on @p second.
 * @param tolerance    Geometric tolerance at which the iteration stops early.
 * @return The refined pair; the starting pair when the geometry is degenerate.
 *
 * @tparam T  Floating-point scalar.
 * @tparam C1 Concrete type of @p first.
 * @tparam C2 Concrete type of @p second.
 */
template <vn::math::FP T, typename C1, typename C2>
[[nodiscard]] std::pair<T, T> refineCurvePair(const C1& first, const C2& second, const std::pair<T, T>& first_range,
                                              const std::pair<T, T>& second_range, T start_u, T start_v, T tolerance)
{
    const T max_step_u = (first_range.second - first_range.first) * T(0.25);
    const T max_step_v = (second_range.second - second_range.first) * T(0.25);

    T u = start_u;
    T v = start_v;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const auto offset = first.evaluate(u) - second.evaluate(v);
        if (offset.length() <= tolerance) {
            return { u, v };
        }

        const auto a = first.derivative(u);
        const auto b = second.derivative(v);

        const T gradient_u = a.dot(offset);
        const T gradient_v = -b.dot(offset);
        const T normal_uu  = a.dot(a);
        const T normal_uv  = -a.dot(b);
        const T normal_vv  = b.dot(b);

        const T determinant = normal_uu * normal_vv - normal_uv * normal_uv;
        if (!(std::abs(determinant) > vn::math::EPS<T>())) {
            return { u, v };
        }

        T step_u = (-gradient_u * normal_vv + gradient_v * normal_uv) / determinant;
        T step_v = (-normal_uu * gradient_v + normal_uv * gradient_u) / determinant;
        step_u   = std::clamp(step_u, -max_step_u, max_step_u);
        step_v   = std::clamp(step_v, -max_step_v, max_step_v);

        u = std::clamp(u + step_u, first_range.first, first_range.second);
        v = std::clamp(v + step_v, second_range.first, second_range.second);
    }

    return { u, v };
}

VN_GEOM_DETAIL_NS_END
