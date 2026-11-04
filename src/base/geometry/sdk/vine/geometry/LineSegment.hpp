#pragma once

#include "geometry_global.hpp"

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Curve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A straight segment between two points, parameterised by t in [0, 1].
 *
 * @details
 * - `pointAt(t)` spans the segment: `t = 0` is `start`, `t = 1` is `end`; values outside [0, 1] are on the line the
 *   segment lies on, not on the segment, so callers that need the segment clamp.
 * - This is a curve (`Curve::kDimension == 1`) but not a `Line` and not a `Ray`: the three differ in which part of
 *   the same line they cover, and a boolean flag would make every reader re-derive the parameter's domain.
 * - The type is a value: two points, no identity, no allocation; it is classified by `Curve`, which adds nothing to
 *   its size.
 *
 * @warning
 * - A degenerate segment (`start == end`) has no direction; `isValid()` reports it and `direction()` returns a zero
 *   vector rather than a NaN.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class LineSegment : public Curve {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

  public:
    /// Constructs a degenerate segment at the origin.
    constexpr LineSegment() = default;

    /**
     * @brief Constructs a segment from its two end points.
     *
     * @param start_point Point at `t = 0`.
     * @param end_point   Point at `t = 1`.
     */
    constexpr LineSegment(const Point& start_point, const Point& end_point)
      : start(start_point)
      , end(end_point)
    {}

  public:
    /**
     * @brief Returns the point at parameter @p t.
     *
     * @param t Position along the segment; 0 is the start, 1 the end.
     * @return `start + (end - start) * t`.
     */
    [[nodiscard]] constexpr Point pointAt(T t) const
    {
        return start + (end - start) * t;
    }

    /**
     * @brief Returns the vector from the start to the end.
     *
     * @return `end - start`; zero for a degenerate segment.
     */
    [[nodiscard]] constexpr Vector vector() const
    {
        return end - start;
    }

    /**
     * @brief Returns the unit direction from the start towards the end.
     *
     * @return The unit direction, or a zero vector when the segment is degenerate.
     */
    [[nodiscard]] Vector direction() const
    {
        const Vector span = vector();
        return span.length() > vn::math::EPS<T>() ? span.normalized() : Vector(T(0), T(0), T(0));
    }

    /**
     * @brief Returns the length of the segment.
     *
     * @return `|end - start|`; zero for a degenerate segment.
     */
    [[nodiscard]] T length() const
    {
        return vector().length();
    }

    /**
     * @brief Returns whether the two end points differ.
     *
     * @return True when the segment has a non-zero length.
     */
    [[nodiscard]] bool isValid() const
    {
        return length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the axis-aligned bounds of the segment.
     *
     * @return Bounds spanning the two end points.
     */
    [[nodiscard]] Aabb<T> bounds() const
    {
        Aabb<T> box = Aabb<T>::empty();
        box.expandBy(start);
        box.expandBy(end);
        return box;
    }

  public:
    /// Point at `t = 0`.
    Point start;

    /// Point at `t = 1`.
    Point end;
};

using LineSegmentf = LineSegment<float>;
using LineSegmentd = LineSegment<double>;

VN_GEOMETRY_NS_END
