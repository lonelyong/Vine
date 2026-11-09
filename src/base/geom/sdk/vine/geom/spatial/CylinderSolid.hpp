#pragma once

#include <vine/geom/geom_global.hpp>

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

#include <vine/geom/spatial/detail/SolidDetail.hpp>

#include "Aabb.hpp"
#include "Solid.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief Right circular cylinder around +Z, spanning `[-half_height, +half_height]`.
 */
template <vn::math::FP T>
class CylinderSolid final : public Solid<T> {
  public:
    /// Radius.
    T radius{ T(0) };

    /// Half of the height; the end caps sit at `-half_height` and `+half_height`.
    T half_height{ T(0) };

  public:
    /// Constructs the degenerate cylinder, which `isValid()` refuses.
    CylinderSolid() = default;

    /**
     * @brief Constructs a cylinder from its radius and half height.
     *
     * @param r           Radius.
     * @param hh          Half of the height (the full height is `2 * hh`).
     */
    CylinderSolid(T r, T hh)
      : radius(r)
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
bool CylinderSolid<T>::isValid() const noexcept
{
    return radius > T(0) && half_height > T(0);
}

template <vn::math::FP T>
bool CylinderSolid<T>::hasVolume(T eps) const noexcept
{
    return radius > eps && half_height * T(2) > eps;
}

template <vn::math::FP T>
Aabb<T> CylinderSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-radius, -radius, -half_height), vn::math::Point3<T>(radius, radius, half_height));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> CylinderSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const T radial = std::hypot(query.x, query.y);
    if (std::abs(query.z) <= half_height && radial <= radius) {
        return SolidProjection<T>{ query, T(0) };
    }

    const T    scale = (radial > radius) ? radius / radial : T(1);
    const auto point = vn::math::Point3<T>(query.x * scale, query.y * scale, std::clamp(query.z, -half_height, half_height));
    return SolidProjection<T>{ point, (point - query).length() };
}

template <vn::math::FP T>
std::vector<RayHit<T>> CylinderSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const T a = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y;
    const T b = T(2) * (ray.origin.x * ray.direction.x + ray.origin.y * ray.direction.y);
    const T c = ray.origin.x * ray.origin.x + ray.origin.y * ray.origin.y - radius * radius;

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point = ray.pointAt(distance);
            if (std::abs(point.z) <= half_height + eps) {
                detail::solidAppendHit(hits, ray, distance, vn::math::Vector3<T>(point.x, point.y, T(0)), eps);
            }
        }
    }

    if (std::abs(ray.direction.z) > eps) {
        for (const T end : { -half_height, half_height }) {
            const T    distance = (end - ray.origin.z) / ray.direction.z;
            const auto point    = ray.pointAt(distance);
            if (point.x * point.x + point.y * point.y <= radius * radius + eps) {
                detail::solidAppendHit(hits, ray, distance, vn::math::Vector3<T>(T(0), T(0), end > T(0) ? T(1) : T(-1)), eps);
            }
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

using CylinderSolidf        = CylinderSolid<float>;
using CylinderSolidd        = CylinderSolid<double>;

VN_GEOM_SPATIAL_NS_END
