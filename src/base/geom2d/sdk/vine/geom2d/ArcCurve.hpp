#pragma once

#include "geom2d_global.hpp"

#include <algorithm>
#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Vector2.hpp>

#include "CircleCurve.hpp"
#include "Curve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A circular arc: a circle stored by value plus the parameter interval the caller cares about.
 *
 * @details
 * - It is the planar analogue of the space `TrimmedCurve`, with the basis held **by value** instead of by a
 *   `shared_ptr`: this module's value types own no dynamic memory, and a circle is two fields, so a trim can copy it.
 *   A generic planar trim over an arbitrary basis waits for a second basis that needs one.
 * - Closure and periodicity belong to the basis plus the interval: the arc is closed only when its interval spans the
 *   circle's whole range, and periodic only when it spans a full period. A shorter piece is a curve with two ends and
 *   answers `period() == 0` even though its basis repeats.
 * - The interval is stored as given, and `evaluate()` answers the circle at any parameter, including one outside the
 *   arc: a trim removes a range rather than cutting the curve short.
 * - Trimming does not reparameterise: the arc keeps the circle's speed, so `derivative()` at @p u is the circle's
 *   velocity at @p u, not a rescaled one.
 *
 * @warning
 * - A reversed interval (`first_angle > last_angle`) is a caller error that `isValid()` reports; the queries still
 *   answer the circle rather than reordering the interval.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ArcCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

  public:
    /**
     * @brief Constructs the arc of @p basis_circle between two angles.
     *
     * @param basis_circle The circle the arc is taken on; copied by value.
     * @param first_angle  First angle of the arc in radians.
     * @param last_angle   Last angle of the arc in radians; must not precede @p first_angle for `isValid()`.
     */
    constexpr ArcCurve(const CircleCurve<T>& basis_circle, T first_angle, T last_angle)
      : circle(basis_circle)
      , first_angle(first_angle)
      , last_angle(last_angle)
    {}

  public:
    /**
     * @brief Returns the point at angle @p u by evaluating the circle.
     *
     * @param u Angle in radians; values outside the arc still answer the circle.
     * @return The point on the circle.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return circle.pointAt(u);
    }

    /**
     * @brief Returns the circle's velocity at angle @p u.
     *
     * @param u Angle in radians.
     * @return The velocity vector, whose length is the radius.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        return circle.derivative(u);
    }

    /**
     * @brief Returns the circle's acceleration at angle @p u.
     *
     * @param u Angle in radians.
     * @return The acceleration vector, pointing towards the centre with length `radius`.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        return circle.secondDerivative(u);
    }

    /**
     * @brief Returns the first parameter of the arc.
     *
     * @return The stored first angle.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return first_angle;
    }

    /**
     * @brief Returns the last parameter of the arc.
     *
     * @return The stored last angle.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return last_angle;
    }

    /**
     * @brief Returns whether the arc spans the circle's whole range.
     *
     * @details
     * - A quarter turn is a piece with two distinct ends, so it answers false even though its basis is closed.
     *
     * @return True when the interval covers `[0, 2*pi]` and the circle itself is closed.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return circle.isClosed() && first_angle <= circle.firstParameter() && last_angle >= circle.lastParameter();
    }

    /**
     * @brief Returns whether the arc spans a full period of the circle.
     *
     * @return True when the interval is at least one period long and the circle is periodic.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return circle.isPeriodic() && last_angle - first_angle >= circle.period();
    }

    /**
     * @brief Returns the period of this arc, or 0 when the arc itself does not repeat.
     *
     * @details
     * - The contract belongs to the interface, not to the basis: a piece shorter than a period answers 0 even though
     *   its circle repeats. The circle stays public as `circle` for a caller that wants the basis' own period.
     *
     * @return The circle's period when `isPeriodic()`, otherwise 0.
     */
    [[nodiscard]] T period() const override
    {
        return isPeriodic() ? circle.period() : T(0);
    }

    /**
     * @brief Returns the circle's continuity: a trim does not smooth a corner, and a circle has none.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return circle.continuity();
    }

    /**
     * @brief Returns whether the basis circle is usable and the interval is not reversed.
     *
     * @details
     * - Both things that can go wrong are caller input: a degenerate circle leaves nothing to evaluate, and a reversed
     *   interval describes a different piece than the caller asked for.
     *
     * @return True when the circle is valid and `first_angle <= last_angle`.
     */
    [[nodiscard]] bool isValid() const override
    {
        return circle.isValid() && first_angle <= last_angle;
    }

    /**
     * @brief Returns the angle of the same point when this arc is walked the other way.
     *
     * @details
     * - The reflection is inside the arc, not inside the circle: `first + last - u`. Reflecting on the circle would
     *   answer angles outside the piece for any arc shorter than a full turn.
     * - It is an involution and it swaps the two ends: `reversedParameter(firstParameter()) == lastParameter()`.
     *   The point it names is the one on the reversed arc, which is this arc reflected about the axis through the
     *   middle of its interval - not `evaluate(u)` itself.
     *
     * @param u Angle in radians.
     * @return `first_angle + last_angle - u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return first_angle + last_angle - u;
    }

    /**
     * @brief Returns the axis-aligned box around the arc.
     *
     * @details
     * - The candidates are the two ends and the four angles where the circle is extremal in a world axis (`0`,
     *   `pi/2`, `pi`, `3*pi/2`), each taken modulo one turn and kept only when it falls inside the interval.
     * - The interval's direction does not change its point set, so a reversed interval answers the same box as its
     *   two ends would in order.
     *
     * @return Bounds of the arc.
     */
    [[nodiscard]] vn::math::Rect2<T> bounds() const
    {
        const T first = std::min(first_angle, last_angle);
        const T last  = std::max(first_angle, last_angle);

        const Point first_point = circle.pointAt(first);
        T           xmin        = first_point.x;
        T           xmax        = first_point.x;
        T           ymin        = first_point.y;
        T           ymax        = first_point.y;

        const auto include = [&](T angle) {
            const Point point = circle.pointAt(angle);
            xmin              = std::min(xmin, point.x);
            xmax              = std::max(xmax, point.x);
            ymin              = std::min(ymin, point.y);
            ymax              = std::max(ymax, point.y);
        };

        include(last);

        const T turn         = T(2) * static_cast<T>(vn::math::PI);
        const T quarter_turn = static_cast<T>(vn::math::PI_HALF);
        for (const T base : { T(0), quarter_turn, T(2) * quarter_turn, T(3) * quarter_turn }) {
            const T candidate = base + turn * std::ceil((first - base) / turn);
            if (candidate <= last) {
                include(candidate);
            }
        }

        return vn::math::Rect2<T>(Point(xmin, ymin), Point(xmax, ymax));
    }

  public:
    /// The circle the arc is taken on.
    CircleCurve<T> circle;

    /// First angle of the arc, in radians.
    T first_angle{ T(0) };

    /// Last angle of the arc, in radians.
    T last_angle{ T(0) };
};

using ArcCurvef = ArcCurve<float>;
using ArcCurved = ArcCurve<double>;

VN_GEOM2D_NS_END
