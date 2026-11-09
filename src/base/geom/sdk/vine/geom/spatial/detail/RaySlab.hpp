#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/Ray.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief A ray with its reciprocals and sign bits precomputed, so a slab test needs no division.
 *
 * @details
 * - A query walks hundreds of boxes with the same direction; dividing once per query instead of once per box is the
 *   whole reason this type exists.
 * - `inverse_direction` is `1 / direction` per axis, with an infinite component where the direction is zero.
 *   The slab test never multiplies by it on such an axis: the parallel branch decides that axis first.
 * - Direction and its squared length are kept as well, because the slab test needs the raw components for the
 *   parallel test and for the "aimless ray" check.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct PreparedRay
{
    /// Ray origin.
    vn::math::Point3<T> origin{};

    /// Unit direction of travel; zero when the ray was constructed without one.
    vn::math::Vector3<T> direction{};

    /// `1 / direction` per axis, infinite where the component is zero.
    vn::math::Vector3<T> inverse_direction{};

    /// Per axis: true when the direction component is negative, so the lower bound is the far one.
    std::array<bool, 3> negative{ false, false, false };

    /// Squared length of `direction`; zero for an aimless ray.
    T direction_length2{ T(0) };

    /**
     * @brief Precomputes the reciprocal and sign table of one ray.
     *
     * @param ray The ray to prepare.
     * @return The prepared form; an aimless ray yields a zero `direction_length2` and infinite reciprocals.
     */
    static PreparedRay from(const Ray<T>& ray)
    {
        PreparedRay prepared;
        prepared.origin            = ray.origin;
        prepared.direction         = ray.direction;
        prepared.direction_length2 = ray.direction.length2();
        for (int axis = 0; axis < 3; ++axis) {
            const T component = axis == 0 ? ray.direction.x : (axis == 1 ? ray.direction.y : ray.direction.z);
            const T reciprocal = component == T(0) ? std::numeric_limits<T>::infinity() : T(1) / component;
            if (axis == 0) {
                prepared.inverse_direction.x = reciprocal;
            }
            else if (axis == 1) {
                prepared.inverse_direction.y = reciprocal;
            }
            else {
                prepared.inverse_direction.z = reciprocal;
            }
            prepared.negative[axis] = component < T(0);
        }
        return prepared;
    }
};

/**
 * @brief One ray crossing an axis-aligned box, before the normal is turned towards the ray.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct RayBoxInterval {
    /// Distance to the entry wall; negative when the origin is inside.
    T enter_t{ T(0) };

    /// Distance to the exit wall; never negative on a successful query.
    T exit_t{ T(0) };

    /// Axis of the entry wall (0 = X, 1 = Y, 2 = Z).
    int enter_axis{ -1 };

    /// Axis of the exit wall (0 = X, 1 = Y, 2 = Z).
    int exit_axis{ -1 };

    /// Outward normal of the entry wall on its own axis.
    T enter_sign{ T(0) };

    /// Outward normal of the exit wall on its own axis.
    T exit_sign{ T(0) };
};

/**
 * @brief The two distances at which a prepared ray crosses an axis-aligned box, and the outward wall of each crossing.
 *
 * @details
 * - This is the only slab implementation in the module: a box query prepares the ray (`PreparedRay::from(ray)`) and
 *   calls this, while the BVH traversal prepares once for the whole query.
 * - `enter_t` may be negative when the origin is inside; `exit_t` is never negative on success.
 *
 * @param ray      The prepared ray.
 * @param xmin     Lower bound along X.
 * @param ymin     Lower bound along Y.
 * @param zmin     Lower bound along Z.
 * @param xmax     Upper bound along X.
 * @param ymax     Upper bound along Y.
 * @param zmax     Upper bound along Z.
 * @param eps      Threshold that makes a ray aimless.
 * @param slab_eps Threshold for treating a direction component as parallel to its slab.
 * @param interval Receives both crossings on success; untouched otherwise.
 * @return `true` when the box lies in front of the origin or around it.
 */
template <vn::math::FP T>
[[nodiscard]] bool preparedRayBoxInterval(const PreparedRay<T>& ray,
                                          T                       xmin,
                                          T                       ymin,
                                          T                       zmin,
                                          T                       xmax,
                                          T                       ymax,
                                          T                       zmax,
                                          T                       eps,
                                          T                       slab_eps,
                                          RayBoxInterval<T>&      interval)
{
    if (!(ray.direction_length2 > eps * eps)) {
        return false; // an aimless ray crosses nothing
    }

    const T origin[3]    = { ray.origin.x, ray.origin.y, ray.origin.z };
    const T direction[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
    const T inverse[3]   = { ray.inverse_direction.x, ray.inverse_direction.y, ray.inverse_direction.z };
    const T lower[3]     = { xmin, ymin, zmin };
    const T upper[3]     = { xmax, ymax, zmax };

    T   enter_t    = -std::numeric_limits<T>::infinity();
    T   exit_t     = std::numeric_limits<T>::infinity();
    int enter_axis = -1;
    int exit_axis  = -1;
    T   enter_sign = T(0);
    T   exit_sign  = T(0);

    for (int axis = 0; axis < 3; ++axis) {
        if (!(std::abs(direction[axis]) > slab_eps)) {
            // Parallel to this pair of faces: inside the slab it never leaves it, outside it never enters.
            if (origin[axis] < lower[axis] - slab_eps || origin[axis] > upper[axis] + slab_eps) {
                return false;
            }
            continue;
        }

        T    near_t    = (lower[axis] - origin[axis]) * inverse[axis];
        T    far_t     = (upper[axis] - origin[axis]) * inverse[axis];
        T    near_sign = T(-1); // the min plane faces -axis
        T    far_sign  = T(1);  // the max plane faces +axis
        if (near_t > far_t) {
            std::swap(near_t, far_t);
            std::swap(near_sign, far_sign);
        }

        if (near_t > enter_t) {
            enter_t    = near_t;
            enter_axis = axis;
            enter_sign = near_sign;
        }
        if (far_t < exit_t) {
            exit_t    = far_t;
            exit_axis = axis;
            exit_sign = far_sign;
        }
        if (enter_t > exit_t) {
            return false;
        }
    }

    if (enter_axis < 0 || exit_axis < 0 || exit_t < T(0)) {
        return false; // no plane was crossed, or the box lies entirely behind the origin
    }

    interval.enter_t    = enter_t;
    interval.exit_t     = exit_t;
    interval.enter_axis = enter_axis;
    interval.exit_axis  = exit_axis;
    interval.enter_sign = enter_sign;
    interval.exit_sign  = exit_sign;
    return true;
}

} // namespace detail

VN_GEOM_SPATIAL_NS_END
