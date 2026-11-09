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
 * @brief Triaxial ellipsoid centred on the origin, axes along X / Y / Z.
 */
template <vn::math::FP T>
class EllipsoidSolid final : public Solid<T> {
  public:
    /// Semi-axis along X.
    T radius_x{ T(0) };

    /// Semi-axis along Y.
    T radius_y{ T(0) };

    /// Semi-axis along Z.
    T radius_z{ T(0) };

  public:
    /// Constructs the degenerate ellipsoid, which `isValid()` refuses.
    EllipsoidSolid() = default;

    /**
     * @brief Constructs an ellipsoid from its three semi-axes.
     *
     * @param rx Semi-axis along X.
     * @param ry Semi-axis along Y.
     * @param rz Semi-axis along Z.
     */
    EllipsoidSolid(T rx, T ry, T rz)
      : radius_x(rx)
      , radius_y(ry)
      , radius_z(rz)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

template <vn::math::FP T>
bool EllipsoidSolid<T>::isValid() const noexcept
{
    return radius_x > T(0) && radius_y > T(0) && radius_z > T(0);
}

template <vn::math::FP T>
bool EllipsoidSolid<T>::hasVolume(T eps) const noexcept
{
    return radius_x > eps && radius_y > eps && radius_z > eps;
}

template <vn::math::FP T>
Aabb<T> EllipsoidSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-radius_x, -radius_y, -radius_z), vn::math::Point3<T>(radius_x, radius_y, radius_z));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> EllipsoidSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    if (!isValid()) {
        return std::nullopt;
    }

    const T normalized =
        (query.x / radius_x) * (query.x / radius_x) + (query.y / radius_y) * (query.y / radius_y) + (query.z / radius_z) * (query.z / radius_z);
    if (normalized <= T(1)) {
        return SolidProjection<T>{ query, T(0) };
    }

    const T    lambda = detail::solidEllipsoidMultiplier(radius_x, radius_y, radius_z, query, tolerance);
    const auto point  = vn::math::Point3<T>(radius_x * radius_x * query.x / (radius_x * radius_x + lambda),
                                            radius_y * radius_y * query.y / (radius_y * radius_y + lambda),
                                            radius_z * radius_z * query.z / (radius_z * radius_z + lambda));
    return SolidProjection<T>{ point, (point - query).length() };
}

template <vn::math::FP T>
std::vector<RayHit<T>> EllipsoidSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid() || !(ray.direction.length2() > T(0))) {
        return hits;
    }

    const vn::math::Vector3<T> offset(ray.origin.x / radius_x, ray.origin.y / radius_y, ray.origin.z / radius_z);
    const vn::math::Vector3<T> direction(ray.direction.x / radius_x, ray.direction.y / radius_y, ray.direction.z / radius_z);
    const T                    a = direction.dot(direction);
    const T                    b = T(2) * offset.dot(direction);
    const T                    c = offset.dot(offset) - T(1);

    T low  = T(0);
    T high = T(0);
    if (detail::solidSolveQuadratic(a, b, c, low, high, eps) > 0) {
        for (const T distance : { low, high }) {
            const auto point  = ray.pointAt(distance);
            const auto normal = vn::math::Vector3<T>(point.x / (radius_x * radius_x), point.y / (radius_y * radius_y), point.z / (radius_z * radius_z));
            detail::solidAppendHit(hits, ray, distance, normal, eps);
        }
    }

    detail::solidSortHits(hits, eps);
    return hits;
}

using EllipsoidSolidf        = EllipsoidSolid<float>;
using EllipsoidSolidd        = EllipsoidSolid<double>;

VN_GEOM_SPATIAL_NS_END
