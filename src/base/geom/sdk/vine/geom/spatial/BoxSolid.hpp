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

#include "Aabb.hpp"
#include "Solid.hpp"
#include "SolidDetail.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief Axis-aligned box centred on the origin.
 *
 * @details
 * - The three extents are stored **halved**, because every formula in this space is written that way and the field
 *   names say which half each one is.
 */
template <vn::math::FP T>
class BoxSolid final : public Solid<T> {
  public:
    /// Half extent along X.
    T half_x{ T(0) };

    /// Half extent along Y.
    T half_y{ T(0) };

    /// Half extent along Z.
    T half_z{ T(0) };

  public:
    /// Constructs the degenerate box (all extents 0), which `isValid()` refuses.
    BoxSolid() = default;

    /**
     * @brief Constructs a box from its three half extents.
     *
     * @param hx Half extent along X.
     * @param hy Half extent along Y.
     * @param hz Half extent along Z.
     */
    BoxSolid(T hx, T hy, T hz)
      : half_x(hx)
      , half_y(hy)
      , half_z(hz)
    {}

  public:
    [[nodiscard]] bool                              isValid() const noexcept override;
    [[nodiscard]] bool                              hasVolume(T eps) const noexcept override;
    [[nodiscard]] Aabb<T>                           bounds() const noexcept override;
    [[nodiscard]] std::optional<SolidProjection<T>> closestPoint(const vn::math::Point3<T>& query, T tolerance = vn::math::EPS<T>()) const override;
    [[nodiscard]] std::vector<RayHit<T>>            intersectWith(const Ray<T>& ray, T eps = vn::math::EPS<T>()) const override;
};

template <vn::math::FP T>
bool BoxSolid<T>::isValid() const noexcept
{
    return half_x > T(0) && half_y > T(0) && half_z > T(0);
}

template <vn::math::FP T>
bool BoxSolid<T>::hasVolume(T eps) const noexcept
{
    return half_x * T(2) > eps && half_y * T(2) > eps && half_z * T(2) > eps;
}

template <vn::math::FP T>
Aabb<T> BoxSolid<T>::bounds() const noexcept
{
    return Aabb<T>(vn::math::Point3<T>(-half_x, -half_y, -half_z), vn::math::Point3<T>(half_x, half_y, half_z));
}

template <vn::math::FP T>
std::optional<SolidProjection<T>> BoxSolid<T>::closestPoint(const vn::math::Point3<T>& query, T tolerance) const
{
    (void)tolerance;
    if (!isValid()) {
        return std::nullopt;
    }

    const auto point = vn::math::Point3<T>(std::clamp(query.x, -half_x, half_x), std::clamp(query.y, -half_y, half_y), std::clamp(query.z, -half_z, half_z));
    return SolidProjection<T>{ point, (point - query).length() };
}

template <vn::math::FP T>
std::vector<RayHit<T>> BoxSolid<T>::intersectWith(const Ray<T>& ray, T eps) const
{
    std::vector<RayHit<T>> hits;
    if (!isValid()) {
        return hits;
    }

    detail::solidAppendBoxHits(hits, ray, half_x, half_y, half_z, eps);
    detail::solidSortHits(hits, eps);
    return hits;
}

using BoxSolidf        = BoxSolid<float>;
using BoxSolidd        = BoxSolid<double>;

VN_GEOM_SPATIAL_NS_END
