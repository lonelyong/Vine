#pragma once

#include "geometry_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Returns the unit in-plane axis a frame starts its parameterisation from.
 *
 * @details
 * - The preferred direction is projected into the plane orthogonal to @p plane_normal and normalised, so a caller may
 *   pass any vector - a plane normal, an axis direction, an already in-plane direction - without orthonormalising it
 *   first.
 * - When the projection vanishes (the preferred direction is parallel to the normal, or was left zero) the world axis
 *   least aligned with the normal is used instead. The rule is deterministic, so the same frame always yields the same
 *   axis and no state has to be stored to remember it.
 * - This is the one definition of that rule: the conic frame, the plane's derived axes and the axis-surface frame all
 *   call it, so they cannot drift apart.
 *
 * @param preferred_axis Direction the parameterisation should start from; a zero vector asks for the fallback.
 * @param plane_normal   Normal of the plane the axis must lie in; a zero vector leaves the plane undefined.
 * @return A unit vector in the plane; `(1, 0, 0)` when @p plane_normal is degenerate.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
[[nodiscard]] vn::math::Vector3<T> inPlaneAxis(const vn::math::Vector3<T>& preferred_axis,
                                               const vn::math::Vector3<T>& plane_normal)
{
    using Vector = vn::math::Vector3<T>;

    if (plane_normal.length() <= vn::math::EPS<T>()) {
        return Vector(T(1), T(0), T(0));
    }

    const Vector unit_normal = plane_normal.normalized();
    const Vector projected   = preferred_axis - unit_normal * unit_normal.dot(preferred_axis);
    if (projected.length() > vn::math::EPS<T>()) {
        return projected.normalized();
    }

    const Vector  candidates[3]  = { Vector(T(1), T(0), T(0)), Vector(T(0), T(1), T(0)), Vector(T(0), T(0), T(1)) };
    const Vector* least_aligned = &candidates[0];
    for (const auto& axis : candidates) {
        if (std::abs(unit_normal.dot(axis)) < std::abs(unit_normal.dot(*least_aligned))) {
            least_aligned = &axis;
        }
    }

    return (*least_aligned - unit_normal * unit_normal.dot(*least_aligned)).normalized();
}

VN_GEOMETRY_NS_END
