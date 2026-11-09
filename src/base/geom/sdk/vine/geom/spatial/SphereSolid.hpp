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
 * @brief SphereSolid centred on the origin.
 */
template <vn::math::FP T>
class SphereSolid final : public Solid<T> {
  public:
    /// Radius.
    T radius{ T(0) };

  public:
    /// Constructs the degenerate sphere (radius 0), which `isValid()` refuses.
    SphereSolid() = default;

    /**
     * @brief Constructs a sphere of the given radius.
     * @param r Radius.
     */
    explicit SphereSolid(T r)
      : radius(r)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

template <vn::math::FP T>
bool SphereSolid<T>::isValid() const noexcept
{
    return radius > T(0);
}

template <vn::math::FP T>
bool SphereSolid<T>::hasVolume(T eps) const noexcept
{
    return radius > eps;
}

template <vn::math::FP T>
Aabb<T> SphereSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-radius, -radius, -radius), vn::math::Point3<T>(radius, radius, radius));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> SphereSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const auto offset   = vn::math::Vector3<T>(query.x, query.y, query.z);
    const T    distance = offset.length();
    if (distance <= radius) {
        return SolidProjection<T>{ query, T(0) };
    }

    const T scale = radius / distance;
    return SolidProjection<T>{ vn::math::Point3<T>(offset.x * scale, offset.y * scale, offset.z * scale), distance - radius };
}

template <vn::math::FP T>
std::vector<RayHit<T>> SphereSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const auto offset = vn::math::Vector3<T>(ray.origin.x, ray.origin.y, ray.origin.z);
    const T    a      = ray.direction.dot(ray.direction);
    const T    b      = T(2) * offset.dot(ray.direction);
    const T    c      = offset.dot(offset) - radius * radius;

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point  = ray.pointAt(distance);
            const auto normal = vn::math::Vector3<T>(point.x, point.y, point.z);
            detail::solidAppendHit(hits, ray, distance, normal, eps);
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

using SphereSolidf        = SphereSolid<float>;
using SphereSolidd        = SphereSolid<double>;

VN_GEOM_SPATIAL_NS_END
