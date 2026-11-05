#pragma once

#include "geom3d_global.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Ray.hpp"
#include "RayHit.hpp"

VN_GEOM3D_NS_BEGIN

namespace detail
{

/**
 * @brief Solves `sum_i (a_i * q_i / (a_i^2 + lambda))^2 = 1` for the ellipsoid's nearest point.
 *
 * @details
 * - The Lagrange condition of "nearest point on `sum (p_i / a_i)^2 = 1`" gives `p_i = a_i^2 q_i / (a_i^2 + lambda)`;
 *   substituting it into the constraint leaves this one-variable, strictly decreasing equation, whose root is unique
 *   for a query outside the ellipsoid.
 * - The bracket is grown by doubling and then halved, which is slower than Newton but cannot leave the root and needs
 *   no derivative.
 *
 * @param radius_x First semi-axis.
 * @param radius_y Second semi-axis.
 * @param radius_z Third semi-axis.
 * @param query    Query point, known to be outside the ellipsoid.
 * @param tolerance Relative bracket width at which the search stops.
 * @return The Lagrange multiplier `lambda >= 0`.
 */
template <vn::math::FP T>
[[nodiscard]] T solidEllipsoidMultiplier(T radius_x, T radius_y, T radius_z, const vn::math::Point3<T>& query, T tolerance)
{
    const T numerator_x = radius_x * query.x;
    const T numerator_y = radius_y * query.y;
    const T numerator_z = radius_z * query.z;
    const T square_x    = radius_x * radius_x;
    const T square_y    = radius_y * radius_y;
    const T square_z    = radius_z * radius_z;

    const auto residual = [&](T lambda) {
        const T nx = numerator_x / (square_x + lambda);
        const T ny = numerator_y / (square_y + lambda);
        const T nz = numerator_z / (square_z + lambda);
        return nx * nx + ny * ny + nz * nz - T(1);
    };

    T high = T(1);
    while (residual(high) > T(0)) {
        high *= T(2);
    }

    T low = T(0);
    for (int iteration = 0; iteration < 256; ++iteration) {
        if (high - low <= tolerance * std::max(T(1), high)) {
            break;
        }
        const T middle = low + (high - low) * T(0.5);
        if (residual(middle) > T(0)) {
            low = middle;
        }
        else {
            high = middle;
        }
    }

    return low + (high - low) * T(0.5);
}

/**
 * @brief Appends one crossing if it is on the forward ray, with the normal turned back towards the ray.
 *
 * @param hits     Collected crossings.
 * @param ray      The ray being cast.
 * @param distance Distance along the ray; a negative one is behind the origin and is dropped.
 * @param outward  Outward normal at the crossing, before the orientation fix; may be zero at a singular point.
 * @param eps      Numerical vanishing threshold.
 */
template <vn::math::FP T>
void solidAppendHit(std::vector<RayHit<T>>& hits, const Ray<T>& ray, T distance, const vn::math::Vector3<T>& outward, T eps)
{
    if (!std::isfinite(distance) || distance < -eps) {
        return;
    }

    RayHit<T> hit{};
    hit.t     = (distance < T(0)) ? T(0) : distance;
    hit.point = ray.pointAt(hit.t);

    const T length = outward.length();
    hit.normal     = length > eps ? outward / length : vn::math::Vector3<T>(T(0), T(0), T(0));
    if (hit.normal.dot(ray.direction) > T(0)) {
        hit.normal = -hit.normal;
    }
    hits.push_back(hit);
}

/**
 * @brief Sorts the crossings by distance and merges the ones closer together than @p eps.
 *
 * @param hits Collected crossings.
 * @param eps  Distance within which two crossings are the same crossing.
 */
template <vn::math::FP T>
void solidSortHits(std::vector<RayHit<T>>& hits, T eps)
{
    if (hits.size() < 2) {
        return;
    }

    std::sort(hits.begin(), hits.end(), [](const RayHit<T>& left, const RayHit<T>& right) { return left.t < right.t; });
    hits.erase(std::unique(hits.begin(), hits.end(), [eps](const RayHit<T>& left, const RayHit<T>& right) { return std::abs(left.t - right.t) <= eps; }),
               hits.end());
}

/**
 * @brief Solves `a t^2 + b t + c = 0`, answering the root count (0, 1 or 2).
 *
 * @param a   Quadratic coefficient; a vanishing one degrades to the linear equation.
 * @param b   Linear coefficient.
 * @param c   Constant term.
 * @param low  Receives the smaller root.
 * @param high Receives the larger root.
 * @param eps Numerical vanishing threshold for @p a and @p b.
 * @return The number of roots written.
 */
template <vn::math::FP T>
int solidSolveQuadratic(T a, T b, T c, T& low, T& high, T eps)
{
    if (!(std::abs(a) > eps)) {
        if (!(std::abs(b) > eps)) {
            return 0;
        }
        low = high = -c / b;
        return 1;
    }

    const T discriminant = b * b - 4 * a * c;
    if (discriminant < T(0)) {
        return 0;
    }

    const T root = std::sqrt(discriminant);
    low          = (-b - root) / (2 * a);
    high         = (-b + root) / (2 * a);
    if (low > high) {
        std::swap(low, high);
    }
    return 2;
}

/**
 * @brief Appends the crossings of a ray with an axis-aligned box at the origin, by the slab method.
 *
 * @param hits   Collected crossings.
 * @param ray    The ray being cast.
 * @param half_x Half extent along X.
 * @param half_y Half extent along Y.
 * @param half_z Half extent along Z.
 * @param eps    Numerical vanishing threshold for a parallel slab.
 */
template <vn::math::FP T>
void solidAppendBoxHits(std::vector<RayHit<T>>& hits, const Ray<T>& ray, T half_x, T half_y, T half_z, T eps)
{
    detail::RayBoxInterval<T> interval;
    if (!detail::rayBoxInterval(ray, -half_x, -half_y, -half_z, half_x, half_y, half_z, eps, eps, interval)) {
        return;
    }

    // One face normal per axis; `sign` picks which of the pair.
    const auto axis_normal = [](int axis, T sign) { return vn::math::Vector3<T>(axis == 0 ? sign : T(0), axis == 1 ? sign : T(0), axis == 2 ? sign : T(0)); };

    solidAppendHit(hits, ray, interval.enter_t, axis_normal(interval.enter_axis, interval.enter_sign), eps);
    solidAppendHit(hits, ray, interval.exit_t, axis_normal(interval.exit_axis, interval.exit_sign), eps);
}

} // namespace detail

VN_GEOM3D_NS_END
