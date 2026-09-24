#pragma once
#include "graphics_global.hpp"

#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::math::Vec3d;

/**
 * @brief Ray struct for picking and collision detection.
 *
 * Represents a ray in 3D space defined by an origin point and a direction.
 */
struct VN_GRAPHICS_API Ray {
    /** Ray origin point. */
    Vec3d origin;

    /** Ray direction (unit vector). */
    Vec3d direction;

    /** @brief Default constructor (creates zero ray). */
    Ray() = default;

    /** @brief Constructs a ray from origin and direction.
     *
     * @param o Ray origin.
     * @param d Ray direction (will be normalized).
     */
    Ray(const Vec3d& o, const Vec3d& d)
      : origin(o), direction(d.normalized())
    {}

    /** @brief Gets a point along the ray.
     *
     * @param t Distance along the ray from origin.
     * @return Point at origin + t * direction.
     */
    Vec3d pointAt(double t) const
    {
        return origin + direction * t;
    }

    /** @brief Computes the closest distance from a point to the ray.
     *
     * @param point The point to test.
     * @return Distance from point to ray.
     */
    double distanceToPoint(const Vec3d& point) const;
};

VN_GRAPHICS_NS_END
