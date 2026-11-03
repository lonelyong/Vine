#pragma once

#include "geometry_global.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

VN_GEOMETRY_NS_BEGIN

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

using Aabbi = Aabb<int32_t>;
using Aabbf = Aabb<float>;
using Aabbd = Aabb<double>;

VN_GEOMETRY_NS_END
