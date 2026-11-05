#pragma once

#include "geom3d_global.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Solid.hpp"
#include "SolidDetail.hpp"

VN_GEOM3D_NS_BEGIN

/**
 * @brief Right circular cone with its apex at `+half_height` and its base disk at `-half_height`.
 */
template <vn::math::FP T>
class ConeSolid final : public Solid<T> {
  public:
    /// Radius of the base disk.
    T base_radius{ T(0) };

    /// Half of the height; the apex sits at `+half_height` and the base at `-half_height`.
    T half_height{ T(0) };

  public:
    /// Constructs the degenerate cone, which `isValid()` refuses.
    ConeSolid() = default;

    /**
     * @brief Constructs a cone from its base radius and half height.
     *
     * @param r  Radius of the base disk.
     * @param hh Half of the height (the full height is `2 * hh`).
     */
    ConeSolid(T r, T hh)
      : base_radius(r)
      , half_height(hh)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

template <vn::math::FP T>
bool ConeSolid<T>::isValid() const noexcept
{
    return base_radius > T(0) && half_height > T(0);
}

template <vn::math::FP T>
bool ConeSolid<T>::hasVolume(T eps) const noexcept
{
    return base_radius > eps && half_height * T(2) > eps;
}

template <vn::math::FP T>
Aabb<T> ConeSolid<T>::bounds() const noexcept
{
    // The base disk is the widest circle the cone has, so its circumscribing box bounds the whole body.
    return Aabb<T>(vn::math::Point3<T>(-base_radius, -base_radius, -half_height), vn::math::Point3<T>(base_radius, base_radius, half_height));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> ConeSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const T height = half_height * T(2);
    const T slope  = base_radius / height; // radius per unit distance below the apex
    const T radial = std::hypot(query.x, query.y);

    const T radius_here = slope * (half_height - query.z);
    if (query.z >= -half_height && query.z <= half_height && radial <= radius_here) {
        return SolidProjection<T>{ query, T(0) };
    }

    // Candidate 1: the base disk, which is not on the meridian profile.
    const T    base_scale = (radial > base_radius) ? base_radius / radial : T(1);
    const auto base_point = vn::math::Point3<T>(query.x * base_scale, query.y * base_scale, -half_height);

    // Candidate 2: the lateral surface, whose nearest point lies in the query's own meridian plane. Projecting onto the
    // profile segment from the base rim (base_radius, -half_height) to the apex (0, +half_height) is therefore exact.
    const T delta_radial   = -base_radius;
    const T delta_z        = height;
    const T length_squared = delta_radial * delta_radial + delta_z * delta_z;
    T       parameter      = ((radial - base_radius) * delta_radial + (query.z + half_height) * delta_z) / length_squared;
    parameter              = std::clamp(parameter, T(0), T(1));

    const T projected_radial = base_radius + delta_radial * parameter;
    const T projected_z      = -half_height + delta_z * parameter;

    // A query on the axis has no direction of its own; any meridian answers, and +X keeps `|point - query|` equal to
    // the profile distance.
    const T    direction_x   = (radial > vn::math::EPS<T>()) ? query.x / radial : T(1);
    const T    direction_y   = (radial > vn::math::EPS<T>()) ? query.y / radial : T(0);
    const auto lateral_point = vn::math::Point3<T>(direction_x * projected_radial, direction_y * projected_radial, projected_z);

    const T base_distance    = (base_point - query).length();
    const T lateral_distance = (lateral_point - query).length();
    return (base_distance <= lateral_distance) ? SolidProjection<T>{ base_point, base_distance } : SolidProjection<T>{ lateral_point, lateral_distance };
}

template <vn::math::FP T>
std::vector<RayHit<T>> ConeSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const T height = half_height * T(2);
    const T slope  = base_radius / height; // radius at a unit distance below the apex

    const T a = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y - slope * slope * ray.direction.z * ray.direction.z;
    const T b = T(2) * (ray.origin.x * ray.direction.x + ray.origin.y * ray.direction.y + slope * slope * (half_height - ray.origin.z) * ray.direction.z);
    const T c = ray.origin.x * ray.origin.x + ray.origin.y * ray.origin.y - slope * slope * (half_height - ray.origin.z) * (half_height - ray.origin.z);

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point = ray.pointAt(distance);
            if (point.z >= -half_height - eps && point.z <= half_height + eps) {
                const auto normal = vn::math::Vector3<T>(point.x, point.y, slope * slope * (half_height - point.z));
                detail::solidAppendHit(hits, ray, distance, normal, eps);
            }
        }
    }

    if (std::abs(ray.direction.z) > eps) {
        const T    distance = (-half_height - ray.origin.z) / ray.direction.z;
        const auto point    = ray.pointAt(distance);
        if (point.x * point.x + point.y * point.y <= base_radius * base_radius + eps) {
            detail::solidAppendHit(hits, ray, distance, vn::math::Vector3<T>(T(0), T(0), T(-1)), eps);
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

using ConeSolidf        = ConeSolid<float>;
using ConeSolidd        = ConeSolid<double>;

VN_GEOM3D_NS_END
