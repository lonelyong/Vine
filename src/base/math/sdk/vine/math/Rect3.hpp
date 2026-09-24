#pragma once

#include "math_global.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

#include "Math.hpp"

VN_MATH_NS_BEGIN
template <typename T>
class Point3;
template <typename T>
class Vector3;

/**
 * @brief Axis-aligned 3D box, defined by its minimum and maximum corners.
 *
 * Each field preserves its own direction:
 * - xmin / ymin / zmin only decrease (via min operations).
 * - xmax / ymax / zmax only increase (via max operations).
 *
 * Accessors return stored values as-is — extents may be negative when
 * the box is inverted (max < min).  Use isNeg() to check.
 *
 * The output of intersectWith() is always normalized (min ≤ max).
 *
 * @tparam T Scalar type (float, double, integer).
 */
template <typename T>
class Rect3 {
  public:
    using value_type = T;

    /**
     * @brief Construct a zero-sized box at the origin.
     */
    constexpr Rect3()
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
    constexpr Rect3(const Point3<T>& minPt, const Point3<T>& maxPt)
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
    constexpr Rect3(const Point3<T>& origin, const Vector3<T>& size)
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
    constexpr Rect3(T x0, T y0, T z0, T x1, T y1, T z1)
      : xmin(x0)
      , ymin(y0)
      , zmin(z0)
      , xmax(x1)
      , ymax(y1)
      , zmax(z1)
    {}

    /* ---- accessors ---- */

    /** @brief Minimum corner. */
    [[nodiscard]]
    constexpr Point3<T> min() const
    {
        return Point3<T>(xmin, ymin, zmin);
    }

    /** @brief Maximum corner. */
    [[nodiscard]]
    constexpr Point3<T> max() const
    {
        return Point3<T>(xmax, ymax, zmax);
    }

    /** @brief Size along X (xmax - xmin), negative if inverted. */
    [[nodiscard]]
    constexpr T length() const
    {
        return xmax - xmin;
    }

    /** @brief Size along Y (ymax - ymin), negative if inverted. */
    [[nodiscard]]
    constexpr T width() const
    {
        return ymax - ymin;
    }

    /** @brief Size along Z (zmax - zmin), negative if inverted. */
    [[nodiscard]]
    constexpr T height() const
    {
        return zmax - zmin;
    }

    /** @brief Size vector, components may be negative if inverted. */
    [[nodiscard]]
    constexpr Vector3<T> size() const
    {
        return Vector3<T>(length(), width(), height());
    }

    /** @brief Center point. */
    [[nodiscard]]
    constexpr Point3<T> center() const
    {
        return Point3<T>((xmin + xmax) / T(2), (ymin + ymax) / T(2), (zmin + zmax) / T(2));
    }

    /* ---- queries ---- */

    /**
     * @brief Check whether the box has negative extent (max < min in any axis).
     */
    [[nodiscard]]
    constexpr bool isNeg() const
    {
        return xmax < xmin || ymax < ymin || zmax < zmin;
    }

    /**
     * @brief Check whether the box is empty (an inverted min/max sentinel).
     *
     * Equivalent to isNeg(); an AABB built with empty() and only expanded is
     * empty until the first expandBy().
     */
    [[nodiscard]]
    constexpr bool isEmpty() const
    {
        return isNeg();
    }

    /**
     * @brief Check whether the box is usable (min <= max on every axis).
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
     */
    [[nodiscard]]
    static constexpr Rect3<T> empty()
    {
        return Rect3<T>(std::numeric_limits<T>::max(),
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
        if (xmax < xmin) { auto t = xmin; xmin = xmax; xmax = t; }
        if (ymax < ymin) { auto t = ymin; ymin = ymax; ymax = t; }
        if (zmax < zmin) { auto t = zmin; zmin = zmax; zmax = t; }
    }

    /**
     * @brief Return a normalized copy (min ≤ max on every axis).
     */
    [[nodiscard]]
    constexpr Rect3<T> normalized() const
    {
        return Rect3<T>(std::min(xmin, xmax), std::min(ymin, ymax), std::min(zmin, zmax),
                        std::max(xmin, xmax), std::max(ymin, ymax), std::max(zmin, zmax));
    }

    /**
     * @brief Check whether the box has exactly zero volume.
     */
    [[nodiscard]]
    constexpr bool isZero() const
    {
        return xmax == xmin && ymax == ymin && zmax == zmin;
    }

    /**
     * @brief Check whether the box has near-zero volume.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(Real<T>)
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
    static Rect3<T> compute(std::span<const Vector3<T>> points);

    /**
     * @brief Check whether a point lies inside the box (inclusive).
     */
    [[nodiscard]]
    bool contains(T x, T y, T z) const;

    /**
     * @brief Check whether a point lies inside the box (inclusive).
     */
    [[nodiscard]]
    bool contains(const Point3<T>& pt) const;

    /* ---- modifiers ---- */

    /**
     * @brief Expand to include a point (as a vector, e.g. a vertex position).
     */
    void expandBy(const Vector3<T>& pt)
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
     */
    void expandBy(const Point3<T>& pt);

    /**
     * @brief Expand to include another box.
     *
     * xmin/ymin/zmin decrease if needed, xmax/ymax/zmax increase if needed.
     */
    void expandBy(const Rect3<T>& rect);

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
    bool intersectWith(const Rect3<T>& rect, Rect3<T>& out) const;

    /* ---- operators ---- */

    [[nodiscard]]
    constexpr bool operator==(const Rect3<T>& r) const
    {
        return xmin == r.xmin && ymin == r.ymin && zmin == r.zmin && xmax == r.xmax && ymax == r.ymax && zmax == r.zmax;
    }

    [[nodiscard]]
    constexpr bool operator!=(const Rect3<T>& r) const
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

using Rect3i = Rect3<int32_t>;
using Rect3f = Rect3<float>;
using Rect3d = Rect3<double>;

using Aabbi = Rect3<int32_t>;
using Aabbf = Rect3<float>;
using Aabbd = Rect3<double>;

/**
 * @brief Alias of Rect3 under the conventional axis-aligned bounding box
 * name.
 *
 * Aabb<T> is fully equivalent to Rect3<T>: the box may be inverted
 * (max < min); use isNeg()/normalize() to detect or fix that state.
 *
 * @tparam T Scalar type.
 */
template <typename T>
using Aabb = Rect3<T>;

VN_MATH_NS_END
