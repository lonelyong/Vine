#pragma once

#include "math_global.hpp"

#include <algorithm>
#include <cstdint>

#include "Point2.hpp"
#include "Vector2.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief Axis-aligned 2D rectangle, defined by its minimum and maximum corners.
 *
 * Each field preserves its own direction:
 * - xmin / ymin only decrease (via min operations).
 * - xmax / ymax only increase (via max operations).
 *
 * Accessors return stored values as-is — extents may be negative when
 * the rectangle is inverted (max < min).  Use isNeg() to check.
 *
 * The output of intersectWith() is always normalized (min ≤ max).
 *
 * @tparam T Scalar type (float, double, integer).
 */
template <typename T>
class Rect2 {
  public:
    using value_type = T;

    /**
     * @brief Construct a zero-sized rectangle at the origin.
     */
    constexpr Rect2()
      : xmin(T())
      , ymin(T())
      , xmax(T())
      , ymax(T())
    {}

    /**
     * @brief Construct from minimum and maximum corners.
     * @param minPt Minimum corner (lower-left in math coords).
     * @param maxPt Maximum corner (upper-right in math coords).
     */
    constexpr Rect2(const Point2<T>& minPt, const Point2<T>& maxPt)
      : xmin(minPt.x)
      , ymin(minPt.y)
      , xmax(maxPt.x)
      , ymax(maxPt.y)
    {}

    /**
     * @brief Convenience: construct from origin point and size vector.
     * @param origin Minimum corner.
     * @param size   Non-negative extents.
     */
    constexpr Rect2(const Point2<T>& origin, const Vector2<T>& size)
      : xmin(origin.x)
      , ymin(origin.y)
      , xmax(origin.x + size.x)
      , ymax(origin.y + size.y)
    {}

    /**
     * @brief Construct from raw scalar components.
     * @param x0 Minimum X.
     * @param y0 Minimum Y.
     * @param x1 Maximum X.
     * @param y1 Maximum Y.
     */
    constexpr Rect2(T x0, T y0, T x1, T y1)
      : xmin(x0)
      , ymin(y0)
      , xmax(x1)
      , ymax(y1)
    {}

    /* ---- accessors ---- */

    /** @brief Minimum corner (left, bottom in math Y-up). */
    [[nodiscard]]
    constexpr Point2<T> min() const { return Point2<T>(xmin, ymin); }

    /** @brief Maximum corner (right, top in math Y-up). */
    [[nodiscard]]
    constexpr Point2<T> max() const { return Point2<T>(xmax, ymax); }

    /** @brief Width (xmax - xmin), negative if inverted. */
    [[nodiscard]]
    constexpr T width() const { return xmax - xmin; }

    /** @brief Height (ymax - ymin), negative if inverted. */
    [[nodiscard]]
    constexpr T height() const { return ymax - ymin; }

    /** @brief Size vector, components may be negative if inverted. */
    [[nodiscard]]
    constexpr Vector2<T> size() const { return Vector2<T>(width(), height()); }

    /** @brief Center point. */
    [[nodiscard]]
    constexpr Point2<T> center() const
    {
        return Point2<T>((xmin + xmax) / T(2), (ymin + ymax) / T(2));
    }

    /* ---- queries ---- */

    /**
     * @brief Check whether the rectangle has negative extent (max < min in any axis).
     */
    [[nodiscard]]
    constexpr bool isNeg() const { return xmax < xmin || ymax < ymin; }

    /**
     * @brief Fix inverted axes in-place so that min ≤ max on every axis.
     */
    constexpr void normalize()
    {
        if (xmax < xmin) { auto t = xmin; xmin = xmax; xmax = t; }
        if (ymax < ymin) { auto t = ymin; ymin = ymax; ymax = t; }
    }

    /**
     * @brief Return a normalized copy (min ≤ max on every axis).
     */
    [[nodiscard]]
    constexpr Rect2<T> normalized() const
    {
        return Rect2<T>(std::min(xmin, xmax), std::min(ymin, ymax),
                        std::max(xmin, xmax), std::max(ymin, ymax));
    }

    /**
     * @brief Check whether the rectangle has exactly zero area.
     */
    [[nodiscard]]
    constexpr bool isZero() const { return xmax == xmin && ymax == ymin; }

    /**
     * @brief Check whether the rectangle has near-zero area.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(Real<T>)
    {
        return math::isZero<T>(width(), eps) && math::isZero<T>(height(), eps);
    }

    /**
     * @brief Check whether a point lies inside the rectangle (inclusive).
     */
    [[nodiscard]]
    bool contains(T x, T y) const;

    /**
     * @brief Check whether a point lies inside the rectangle (inclusive).
     */
    [[nodiscard]]
    bool contains(const Point2<T>& pt) const;

    /* ---- modifiers ---- */

    /**
     * @brief Expand to include a point.
     *
     * xmin/ymin decrease if needed, xmax/ymax increase if needed.
     */
    void expandBy(const Point2<T>& pt);

    /**
     * @brief Expand to include another rectangle.
     *
     * xmin/ymin decrease if needed, xmax/ymax increase if needed.
     */
    void expandBy(const Rect2<T>& rect);

    /**
     * @brief Compute intersection with another rectangle.
     *
     * The output rectangle is always normalized (min ≤ max).
     *
     * @param rect Other rectangle.
     * @param out  Normalized output.
     *             When disjoint, represents the gap between the two rectangles.
     * @return true if intersecting, false if disjoint.
     */
    bool intersectWith(const Rect2<T>& rect, Rect2<T>& out) const;

    /* ---- operators ---- */

    [[nodiscard]]
    constexpr bool operator==(const Rect2<T>& r) const
    {
        return xmin == r.xmin && ymin == r.ymin && xmax == r.xmax && ymax == r.ymax;
    }

    [[nodiscard]]
    constexpr bool operator!=(const Rect2<T>& r) const { return !(*this == r); }

  public:
    union {
        struct {
            T xmin, ymin, xmax, ymax;
        };
        T data[4];
    };
};

using Rect2i = Rect2<int32_t>;
using Rect2f = Rect2<float>;
using Rect2d = Rect2<double>;

VN_MATH_NS_END
