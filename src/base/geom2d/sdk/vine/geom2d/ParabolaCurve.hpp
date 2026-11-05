#pragma once

#include "geom2d_global.hpp"

#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include "ConicCurve.hpp"

VN_GEOM2D_NS_BEGIN

/**
 * @brief A planar parabola: a vertex, an axis direction and a focal length.
 *
 * @details
 * - `pointAt(u)` walks the curve as `P(u) = vertex + (u * u / (4 * focal_length)) * X + u * Y`, where `X` points
 *   from the vertex towards the focus and `Y` is `x_direction` rotated by +90 degrees. Every point is the same
 *   distance from the focus and from the directrix, which is what makes `focal_length` the only length it needs.
 * - The parameter is the offset along the axis' perpendicular, so it is a length on the `Y` axis and a length on
 *   the `X` axis only after squaring; it is not a distance along the curve.
 * - The curve is unbounded and not periodic: `firstParameter()` / `lastParameter()` are `-infinity` / `+infinity`.
 * - The frame (`center`, `x_direction`, `yAxis()`) and the `CN` answer are inherited from `ConicCurve`; the inherited
 *   `center` **is** the vertex of the parabola.
 *
 * @warning
 * - The frame is normalised at construction: assigning `center` or `x_direction` afterwards leaves a frame that no
 *   longer agrees with the parameterisation, so build a new parabola instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ParabolaCurve : public ConicCurve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

    /// The frame is inherited from `ConicCurve`; these keep the member bodies readable.
    using ConicCurve<T>::center;
    using ConicCurve<T>::x_direction;
    using ConicCurve<T>::yAxis;

  public:
    /**
     * @brief Constructs a parabola from its vertex, its axis and its focal length.
     *
     * @param vertex_point   Vertex of the parabola.
     * @param axis_direction Direction from the vertex towards the focus; normalised before storage.
     * @param focal_distance Distance from the vertex to the focus, which must be positive for `isValid()`.
     */
    constexpr ParabolaCurve(const Point& vertex_point, const Vector& axis_direction, T focal_distance)
      : ConicCurve<T>(vertex_point, axis_direction)
      , focal_length(focal_distance)
    {}

  public:
    /**
     * @brief Returns the point at parameter @p u.
     *
     * @details
     * - `P(u) = vertex + (u * u / (4 * focal_length)) * X + u * Y`; `u = 0` is the vertex.
     *
     * @param u Offset along the perpendicular axis, in the same units as `focal_length`.
     * @return The point on the parabola; `center` when the frame has no usable direction.
     */
    [[nodiscard]] Point pointAt(T u) const
    {
        const Vector y_axis = yAxis();
        return center + x_direction * (u * u / (T(4) * focal_length)) + y_axis * u;
    }

    /**
     * @brief Returns the point at parameter @p u; the same value as `pointAt()`.
     *
     * @param u Offset along the perpendicular axis.
     * @return The point on the parabola.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns the velocity vector at parameter @p u.
     *
     * @details
     * - `dP/du = (u / (2 * focal_length)) * X + Y`: never zero, so the parameterisation has no singular point, and
     *   its length grows symmetrically towards both ends.
     *
     * @param u Offset along the perpendicular axis.
     * @return The velocity vector at @p u.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        return x_direction * (u / (T(2) * focal_length)) + yAxis();
    }

    /**
     * @brief Returns the constant acceleration vector.
     *
     * @details
     * - `d2P/du2 = X / (2 * focal_length)`: a parabola bends by the same amount everywhere, which is the analytic
     *   form of "equal distance to focus and directrix".
     *
     * @param u Unused; the second derivative of a parabola is constant.
     * @return `x_direction / (2 * focal_length)`.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        (void)u;
        return x_direction * (T(1) / (T(2) * focal_length));
    }

    /**
     * @brief Returns the lower end of the range: a parabola is unbounded.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the range: a parabola is unbounded.
     *
     * @return Positive infinity.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns false: the two ends never meet.
     *
     * @return false always.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a parabola does not repeat.
     *
     * @return false always.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a non-periodic curve has no period.
     *
     * @return 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns whether the focal length and the axis direction are usable.
     *
     * @return True when the focal length and the stored direction are longer than the floating-point epsilon.
     */
    [[nodiscard]] bool isValid() const override
    {
        return focal_length > vn::math::EPS<T>() && x_direction.length() > vn::math::EPS<T>();
    }

    /**
     * @brief Returns `-u`: the mirrored point on the same parabola.
     *
     * @details
     * - The curve is unbounded, so the reflection is the negation; it is an involution and swaps the two infinite
     *   ends.
     *
     * @param u Offset along the perpendicular axis.
     * @return `-u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return -u;
    }

  public:
    /// Distance from the vertex to the focus.
    T focal_length{ T(1) };
};

using ParabolaCurvef = ParabolaCurve<float>;
using ParabolaCurved = ParabolaCurve<double>;

VN_GEOM2D_NS_END
