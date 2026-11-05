#pragma once

#include "geom2d_global.hpp"

#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A planar line in point-direction parametric form: `P(t) = origin + t * direction`.
 *
 * @details
 * - The direction does not need to be unit length: the line is defined for any non-zero direction, and the parameter
 *   is a multiple of it, not a distance.
 * - The domain is the whole real line, so `firstParameter()` and `lastParameter()` are infinite and the line is
 *   neither closed nor periodic.
 * - Every derivative above the first is zero: the parameterisation is linear.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class LineCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

  public:
    /**
     * @brief Constructs the line through @p origin_point along @p line_direction.
     *
     * @param origin_point Any point on the line.
     * @param line_direction Direction vector; does not need to be unit length, but must be non-zero for `isValid()`.
     */
    constexpr LineCurve(const Point& origin_point, const Vector& line_direction)
      : origin(origin_point)
      , direction(line_direction)
    {
    }

  public:
    /**
     * @brief Returns the point at parameter @p t.
     *
     * @details
     * - `t` is a multiple of `direction`, so it is a distance only when `direction` is unit length.
     *
     * @param t Multiple of `direction` from `origin`; negative values go backwards.
     * @return `origin + direction * t`.
     */
    [[nodiscard]] constexpr Point pointAt(T t) const
    {
        return origin + direction * t;
    }

    /**
     * @brief Returns the point at parameter @p u; the same value as `pointAt()`.
     *
     * @param u Multiple of `direction` from `origin`.
     * @return `origin + direction * u`.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns `direction`: a straight line has the same velocity everywhere.
     *
     * @param u Unused; the derivative does not depend on the parameter.
     * @return The direction vector, not normalised.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        (void)u;
        return direction;
    }

    /**
     * @brief Returns the zero vector: a straight line does not turn.
     *
     * @param u Unused; the derivative does not depend on the parameter.
     * @return The zero vector.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        (void)u;
        return Vector::zero();
    }

    /**
     * @brief Returns the zero vector: the parameterisation is linear, so nothing above the first derivative survives.
     *
     * @details
     * - The member is not part of `Curve<T>`: it exists because `OffsetCurve` asks its basis for a third derivative, and
     *   a curve type that cannot answer it does not have to (2.13).
     *
     * @param u Unused; the derivative does not depend on the parameter.
     * @return The zero vector.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        (void)u;
        return Vector::zero();
    }

    /**
     * @brief Returns negative infinity: the line has no first parameter.
     *
     * @return `-infinity`.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns positive infinity: the line has no last parameter.
     *
     * @return `+infinity`.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns false: a line never meets itself.
     *
     * @return false always.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a line does not repeat.
     *
     * @return false always.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns zero: a non-periodic curve has no period.
     *
     * @return 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: every derivative exists inside the domain.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns whether the direction is usable.
     *
     * @details
     * - A zero - or epsilon-short - direction names no line; the other queries then answer the degenerate values this
     *   class documents (`pointAt()` returns the origin) instead of producing a NaN.
     *
     * @return True when `direction.length()` is greater than the floating-point epsilon.
     */
    [[nodiscard]] bool isValid() const override
    {
        return direction.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the parameter of the same point when the direction is flipped.
     *
     * @details
     * - The line is unbounded, so the reflection is the negation: `origin + direction * (-u)` is the mirrored point.
     *
     * @param u Parameter in this line's own units.
     * @return `-u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return -u;
    }

  public:
    Point  origin;
    Vector direction;
};

using LineCurvef = LineCurve<float>;
using LineCurved = LineCurve<double>;

VN_GEOM2D_NS_END
