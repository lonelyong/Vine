#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Ray.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

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
 * @brief The two distances at which a ray crosses an axis-aligned box, and the outward wall of each crossing.
 *
 * @details
 * - This is the only slab implementation in the module: `Aabb::intersectWith()` collapses it to the nearest
 *   crossing, while `BoxSolid` keeps both as its enter / exit crossings (`G5`).
 * - `enter_t` may be negative when the origin is inside; `exit_t` is never negative on success.
 * - `enter_sign` / `exit_sign` are the OUTWARD normal components of the two walls on their own axis; `Aabb` turns
 *   the reported normal towards the ray afterwards.
 *
 * @param ray    The ray, whose direction is expected to be unit length.
 * @param xmin   Lower bound along X.
 * @param ymin   Lower bound along Y.
 * @param zmin   Lower bound along Z.
 * @param xmax   Upper bound along X.
 * @param ymax   Upper bound along Y.
 * @param zmax   Upper bound along Z.
 * @param eps    Threshold that makes a ray aimless.
 * @param slab_eps Threshold for treating a direction component as parallel to its slab; callers that must keep a tiny
 *                 but real component pass 0.
 * @param interval Receives both crossings on success; untouched otherwise.
 * @return `true` when the box lies in front of the origin or around it.
 */
template <vn::math::FP T>
[[nodiscard]] bool rayBoxInterval(const Ray<T>& ray,
                                  T xmin,
                                  T ymin,
                                  T zmin,
                                  T xmax,
                                  T ymax,
                                  T zmax,
                                  T eps,
                                  T slab_eps,
                                  RayBoxInterval<T>& interval)
{
    if (!(ray.direction.length2() > eps * eps)) {
        return false; // an aimless ray crosses nothing
    }

    const T origin[3]    = { ray.origin.x, ray.origin.y, ray.origin.z };
    const T direction[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
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

        T    near_t    = (lower[axis] - origin[axis]) / direction[axis];
        T    far_t     = (upper[axis] - origin[axis]) / direction[axis];
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

/**
 * @brief Axis-aligned 3D box, defined by its minimum and maximum corners.
 *
 * @details
 * - Each field preserves its own direction: xmin / ymin / zmin only decrease (via min operations), xmax / ymax / zmax
 *   only increase (via max operations).
 * - Accessors return stored values as-is — extents may be negative when the box is inverted (max < min).
 *   Use isNeg() to check, normalize() to fix in place.
 * - The output of intersectWith() is always normalized (min ≤ max).
 *
 * @note
 * - The type is an AABB, not a "rectangle": the three sizes are length() / width() / height(), in that axis order.
 *
 * @tparam T Scalar type (float, double, integer).
 */
template <typename T>
class Aabb
{
  public:
    using value_type = T;

    /**
     * @brief Construct a zero-sized box at the origin.
     */
    constexpr Aabb()
      : xmin(T())
      , ymin(T())
      , zmin(T())
      , xmax(T())
      , ymax(T())
      , zmax(T())
    {}

    /**
     * @brief Construct from minimum and maximum corners.
     * @param minPt Minimum corner.
     * @param maxPt Maximum corner.
     */
    constexpr Aabb(const math::Point3<T>& minPt, const math::Point3<T>& maxPt)
      : xmin(minPt.x)
      , ymin(minPt.y)
      , zmin(minPt.z)
      , xmax(maxPt.x)
      , ymax(maxPt.y)
      , zmax(maxPt.z)
    {}

    /**
     * @brief Convenience: construct from origin point and size vector.
     * @param origin Minimum corner.
     * @param size   Non-negative extents.
     */
    constexpr Aabb(const math::Point3<T>& origin, const math::Vector3<T>& size)
      : xmin(origin.x)
      , ymin(origin.y)
      , zmin(origin.z)
      , xmax(origin.x + size.x)
      , ymax(origin.y + size.y)
      , zmax(origin.z + size.z)
    {}

    /**
     * @brief Construct from raw scalar components.
     * @param x0 Minimum X.
     * @param y0 Minimum Y.
     * @param z0 Minimum Z.
     * @param x1 Maximum X.
     * @param y1 Maximum Y.
     * @param z1 Maximum Z.
     */
    constexpr Aabb(T x0, T y0, T z0, T x1, T y1, T z1)
      : xmin(x0)
      , ymin(y0)
      , zmin(z0)
      , xmax(x1)
      , ymax(y1)
      , zmax(z1)
    {}

    /* ---- accessors ---- */

    /**
     * @brief Minimum corner.
     *
     * @return The stored (xmin, ymin, zmin) corner.
     */
    [[nodiscard]]
    constexpr math::Point3<T> min() const
    {
        return math::Point3<T>(xmin, ymin, zmin);
    }

    /**
     * @brief Maximum corner.
     *
     * @return The stored (xmax, ymax, zmax) corner.
     */
    [[nodiscard]]
    constexpr math::Point3<T> max() const
    {
        return math::Point3<T>(xmax, ymax, zmax);
    }

    /**
     * @brief Size along X.
     *
     * @return xmax - xmin, negative when the box is inverted.
     */
    [[nodiscard]]
    constexpr T length() const
    {
        return xmax - xmin;
    }

    /**
     * @brief Size along Y.
     *
     * @return ymax - ymin, negative when the box is inverted.
     */
    [[nodiscard]]
    constexpr T width() const
    {
        return ymax - ymin;
    }

    /**
     * @brief Size along Z.
     *
     * @return zmax - zmin, negative when the box is inverted.
     */
    [[nodiscard]]
    constexpr T height() const
    {
        return zmax - zmin;
    }

    /**
     * @brief Size vector, components may be negative if inverted.
     *
     * @return (length(), width(), height()).
     */
    [[nodiscard]]
    constexpr math::Vector3<T> size() const
    {
        return math::Vector3<T>(length(), width(), height());
    }

    /**
     * @brief Center point.
     *
     * @return The midpoint of the two corners.
     */
    [[nodiscard]]
    constexpr math::Point3<T> center() const
    {
        return math::Point3<T>((xmin + xmax) / T(2), (ymin + ymax) / T(2), (zmin + zmax) / T(2));
    }

    /* ---- queries ---- */

    /**
     * @brief Check whether the box has negative extent.
     *
     * @return True when any axis has max < min.
     */
    [[nodiscard]]
    constexpr bool isNeg() const
    {
        return xmax < xmin || ymax < ymin || zmax < zmin;
    }

    /**
     * @brief Check whether the box is empty (an inverted min/max sentinel).
     *
     * Equivalent to isNeg(); a box built with empty() and only expanded is empty until the first expandBy().
     *
     * @return True when the box is still the inverted sentinel.
     */
    [[nodiscard]]
    constexpr bool isEmpty() const
    {
        return isNeg();
    }

    /**
     * @brief Check whether the box is usable (min <= max on every axis).
     *
     * @return True when no axis is inverted.
     */
    [[nodiscard]]
    constexpr bool isValid() const
    {
        return !isNeg();
    }

    /**
     * @brief Returns an empty box (inverted sentinel) to start an expansion.
     *
     * The first expandBy() of a point/box turns it into that point/box.
     *
     * @return A box whose min is the largest representable value and whose max is the lowest.
     */
    [[nodiscard]]
    static constexpr Aabb<T> empty()
    {
        return Aabb<T>(std::numeric_limits<T>::max(),
                       std::numeric_limits<T>::max(),
                       std::numeric_limits<T>::max(),
                       std::numeric_limits<T>::lowest(),
                       std::numeric_limits<T>::lowest(),
                       std::numeric_limits<T>::lowest());
    }

    /**
     * @brief Fix inverted axes in-place so that min ≤ max on every axis.
     */
    constexpr void normalize()
    {
        if (xmax < xmin) {
            auto t = xmin;
            xmin   = xmax;
            xmax   = t;
        }
        if (ymax < ymin) {
            auto t = ymin;
            ymin   = ymax;
            ymax   = t;
        }
        if (zmax < zmin) {
            auto t = zmin;
            zmin   = zmax;
            zmax   = t;
        }
    }

    /**
     * @brief Return a normalized copy (min ≤ max on every axis).
     *
     * @return A copy with each inverted axis swapped; the receiver is unchanged.
     */
    [[nodiscard]]
    constexpr Aabb<T> normalized() const
    {
        return Aabb<T>(std::min(xmin, xmax), std::min(ymin, ymax), std::min(zmin, zmax), std::max(xmin, xmax), std::max(ymin, ymax), std::max(zmin, zmax));
    }

    /**
     * @brief Check whether the box has exactly zero volume.
     *
     * @return True when all three extents are exactly zero.
     */
    [[nodiscard]]
    constexpr bool isZero() const
    {
        return xmax == xmin && ymax == ymin && zmax == zmin;
    }

    /**
     * @brief Check whether the box has near-zero volume.
     *
     * @param eps Numerical tolerance per axis.
     * @return True when every axis extent is within @p eps of zero.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(math::Real<T>)
    {
        return math::isZero<T>(length(), eps) && math::isZero<T>(width(), eps) && math::isZero<T>(height(), eps);
    }

    /**
     * @brief Compute the box enclosing a set of points.
     *
     * @param points Points to enclose (any axis order).
     * @return Minimal enclosing box; the default (zero) box when empty.
     */
    [[nodiscard]]
    static Aabb<T> compute(std::span<const math::Vector3<T>> points);

    /**
     * @brief Check whether a point lies inside the box (inclusive).
     *
     * @param x X coordinate.
     * @param y Y coordinate.
     * @param z Z coordinate.
     * @return True when the point is inside (or on the boundary of) the box.
     */
    [[nodiscard]]
    bool contains(T x, T y, T z) const;

    /**
     * @brief Check whether a point lies inside the box (inclusive).
     *
     * @param pt Point to test.
     * @return True when the point is inside (or on the boundary of) the box.
     */
    [[nodiscard]]
    bool contains(const math::Point3<T>& pt) const;

    /* ---- modifiers ---- */

    /**
     * @brief Expand to include a point (as a vector, e.g. a vertex position).
     *
     * @param pt Point to include.
     */
    void expandBy(const math::Vector3<T>& pt)
    {
        xmin = std::min<T>(xmin, pt.x);
        ymin = std::min<T>(ymin, pt.y);
        zmin = std::min<T>(zmin, pt.z);
        xmax = std::max<T>(xmax, pt.x);
        ymax = std::max<T>(ymax, pt.y);
        zmax = std::max<T>(zmax, pt.z);
    }

    /**
     * @brief Expand to include a point.
     *
     * xmin/ymin/zmin decrease if needed, xmax/ymax/zmax increase if needed.
     *
     * @param pt Point to include.
     */
    void expandBy(const math::Point3<T>& pt);

    /**
     * @brief Expand to include another box.
     *
     * xmin/ymin/zmin decrease if needed, xmax/ymax/zmax increase if needed.
     *
     * @param rect Box to include.
     */
    void expandBy(const Aabb<T>& rect);

    /**
     * @brief Compute intersection with another box.
     *
     * The output box is always normalized (min ≤ max).
     *
     * @param rect Other box.
     * @param out  Normalized output.
     *             When disjoint, represents the gap between the two boxes.
     * @return true if intersecting, false if disjoint.
     */
    bool intersectWith(const Aabb<T>& rect, Aabb<T>& out) const;

    /**
     * @brief Intersects the box with a ray.
     *
     * @details
     * - The ray must be in the box's own space; nothing is transformed here.
     * - Only the NEAREST hit is reported: an `Aabb` is a bound, so its query answers "where did the ray first touch".
     *   `Solid<T>::intersectWith()` is the query that reports both the enter and the exit crossing.
     * - A ray that starts inside the box reports `t = 0` and the far wall it meets, turned to face back at the ray, so
     *   `normal` faces towards the ray for every hit - the same rule the plane and triangle entry points follow.
     *
     * @param ray The ray, whose direction is unit length (see Ray).
     * @param hit Receives the nearest hit when the call returns true; untouched otherwise.
     * @param eps Tolerance that makes a ray aimless: a direction this short crosses nothing.
     * @return True when the box lies in front of the ray's origin or around it.
     */
    template <math::FP U> requires std::is_same_v<T, U>
    [[nodiscard]] bool intersectWith(const Ray<U>& ray, RayHit<U>& hit, U eps = math::EPS<U>()) const;

    /* ---- operators ---- */

    /**
     * @brief Compare the six stored values for equality.
     *
     * @param r Box to compare with.
     * @return True when every corresponding corner component is equal.
     */
    [[nodiscard]]
    constexpr bool operator==(const Aabb<T>& r) const
    {
        return xmin == r.xmin && ymin == r.ymin && zmin == r.zmin && xmax == r.xmax && ymax == r.ymax && zmax == r.zmax;
    }

    /**
     * @brief Compare the six stored values for inequality.
     *
     * @param r Box to compare with.
     * @return True when any corresponding corner component differs.
     */
    [[nodiscard]]
    constexpr bool operator!=(const Aabb<T>& r) const
    {
        return !(*this == r);
    }

  public:
    union
    {
        struct {
            T xmin, ymin, zmin, xmax, ymax, zmax;
        };

        T data[6];
    };
};

template <typename T>
bool Aabb<T>::contains(T x, T y, T z) const
{
    return x >= xmin && x <= xmax && y >= ymin && y <= ymax && z >= zmin && z <= zmax;
}

template <typename T>
bool Aabb<T>::contains(const math::Point3<T>& pt) const
{
    return pt.x >= xmin && pt.x <= xmax && pt.y >= ymin && pt.y <= ymax && pt.z >= zmin && pt.z <= zmax;
}

template <typename T>
void Aabb<T>::expandBy(const math::Point3<T>& pt)
{
    xmin = std::min<T>(xmin, pt.x);
    ymin = std::min<T>(ymin, pt.y);
    zmin = std::min<T>(zmin, pt.z);
    xmax = std::max<T>(xmax, pt.x);
    ymax = std::max<T>(ymax, pt.y);
    zmax = std::max<T>(zmax, pt.z);
}

template <typename T>
void Aabb<T>::expandBy(const Aabb<T>& rect)
{
    xmin = std::min<T>(xmin, rect.xmin);
    ymin = std::min<T>(ymin, rect.ymin);
    zmin = std::min<T>(zmin, rect.zmin);
    xmax = std::max<T>(xmax, rect.xmax);
    ymax = std::max<T>(ymax, rect.ymax);
    zmax = std::max<T>(zmax, rect.zmax);
}

template <typename T>
bool Aabb<T>::intersectWith(const Aabb<T>& rect, Aabb<T>& out) const
{
    auto ix0 = std::max<T>(xmin, rect.xmin);
    auto iy0 = std::max<T>(ymin, rect.ymin);
    auto iz0 = std::max<T>(zmin, rect.zmin);
    auto ix1 = std::min<T>(xmax, rect.xmax);
    auto iy1 = std::min<T>(ymax, rect.ymax);
    auto iz1 = std::min<T>(zmax, rect.zmax);

    out.xmin = std::min<T>(ix0, ix1);
    out.ymin = std::min<T>(iy0, iy1);
    out.zmin = std::min<T>(iz0, iz1);
    out.xmax = std::max<T>(ix0, ix1);
    out.ymax = std::max<T>(iy0, iy1);
    out.zmax = std::max<T>(iz0, iz1);

    return ix0 <= ix1 && iy0 <= iy1 && iz0 <= iz1;
}

template <typename T>
Aabb<T> Aabb<T>::compute(std::span<const math::Vector3<T>> points)
{
    if (points.empty()) {
        return Aabb<T>();
    }
    Aabb<T> box;
    box.xmin = box.xmax = points.front().x;
    box.ymin = box.ymax = points.front().y;
    box.zmin = box.zmax = points.front().z;
    for (const auto& p : points) {
        box.xmin = std::min<T>(box.xmin, p.x);
        box.ymin = std::min<T>(box.ymin, p.y);
        box.zmin = std::min<T>(box.zmin, p.z);
        box.xmax = std::max<T>(box.xmax, p.x);
        box.ymax = std::max<T>(box.ymax, p.y);
        box.zmax = std::max<T>(box.zmax, p.z);
    }
    return box;
}

template <typename T>
template <math::FP U> requires std::is_same_v<T, U>
bool Aabb<T>::intersectWith(const Ray<U>& ray, RayHit<U>& hit, U eps) const
{
    detail::RayBoxInterval<U> interval;
    if (!detail::rayBoxInterval(ray, xmin, ymin, zmin, xmax, ymax, zmax, eps, U(0), interval)) {
        return false;
    }

    // A ray that starts inside meets the far wall from within: t = 0 keeps every hit ahead of the origin, and the
    // normal is the exit face turned around, so it still faces back towards the ray (C2 holds for every hit).
    const bool inside = interval.enter_t < T(0);
    hit.t             = inside ? T(0) : interval.enter_t;
    hit.point         = ray.pointAt(hit.t);
    hit.normal        = math::Vector3<T>(T(0), T(0), T(0));

    const int axis = inside ? interval.exit_axis : interval.enter_axis;
    const T   sign = inside ? -interval.exit_sign : interval.enter_sign;
    if (axis == 0) {
        hit.normal.x = sign;
    } else if (axis == 1) {
        hit.normal.y = sign;
    } else if (axis == 2) {
        hit.normal.z = sign;
    }

    return true;
}

using Aabbi = Aabb<int32_t>;
using Aabbf = Aabb<float>;
using Aabbd = Aabb<double>;
VN_GEOM_SPATIAL_NS_END
