#pragma once

#include "geometry_global.hpp"

#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

VN_GEOMETRY_NS_BEGIN

/**
 * @brief One ray hit: where it happened and which way the surface faces there.
 *
 * @details
 * - `t` is measured along the ray in the ray's own units: a `Ray` carries a unit direction, so `t` is a distance in
 *   the space the query was made in.
 * - `point` is `ray.pointAt(t)`, kept here so a caller does not compute it a second time.
 * - `normal` faces BACK towards the ray: its dot product with the ray's direction is never positive. A query result
 *   therefore describes the surface as the ray sees it, not as the winding order of a triangle happens to define it.
 *
 * @warning
 * - A hit is only meaningful when the query returned true; a false result leaves this value untouched.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct RayHit {
    /// Distance from the ray origin to the hit, in the ray's units.
    T t{ 0 };

    /// Hit point in the space the query was made in.
    vn::math::Point3<T> point;

    /// Surface normal at the hit, facing back towards the ray.
    vn::math::Vector3<T> normal;
};

using RayHitf = RayHit<float>;
using RayHitd = RayHit<double>;

VN_GEOMETRY_NS_END
