#pragma once

#include <vine/geom/geom_global.hpp>

#include <limits>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "ConicCurve.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A parabola: a plane, a vertex, an axis direction and a focal length.
 *
 * @details
 * - `pointAt(u)` walks the curve as `P(u) = vertex + (u * u / (4 * focal_length)) * X + u * Y`, where `X` points
 *   from the vertex towards the focus and `Y` is `normal x X`. Every point is the same distance from the focus and
 *   from the directrix, which is what makes `focal_length` the only length the curve needs.
 * - The parameter is the offset along the axis' perpendicular, so it is a length on the `Y` axis and a length on
 *   the `X` axis only after squaring; it is not a distance along the curve.
 * - The curve is unbounded and not periodic: `firstParameter()` / `lastParameter()` are `-infinity` / `+infinity`.
 *
 * @warning
 * - The frame (`center`, `normal`, `x_direction`) is inherited from `ConicCurve` and is normalised once at
 *   construction: the inherited `center` **is** the vertex of the parabola. Assigning those fields afterwards leaves
 *   three vectors describing frames that no longer agree, so build a new parabola instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ParabolaCurve : public ConicCurve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

    /// The frame is inherited from `ConicCurve`; these keep the member bodies readable.
    using ConicCurve<T>::center;
    using ConicCurve<T>::normal;
    using ConicCurve<T>::x_direction;
    using ConicCurve<T>::yAxis;

  public:
    /// Constructs the parabola `y = x^2 / 4` with its vertex at the origin of the XY plane.
    constexpr ParabolaCurve() = default;

    /**
     * @brief Constructs a parabola from its vertex, its plane, its axis and its focal length.
     *
     * @param vertex_point    Vertex of the parabola.
     * @param plane_normal    Normal of the plane the parabola lies in; normalized before storage, and a zero vector
     *                        leaves the curve in an undefined plane.
     * @param axis_direction  Direction from the vertex towards the focus; projected into the plane and normalized
     *                        before storage.
     * @param focal_distance  Distance from the vertex to the focus, which must be positive for `isValid()`.
     */
    ParabolaCurve(const Point& vertex_point, const Vector& plane_normal, const Vector& axis_direction,
                  T focal_distance)
      : ConicCurve<T>(vertex_point, plane_normal, axis_direction)
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
     * @return The point on the parabola; with a degenerate frame it collapses onto the ray along `x_direction`
     *         (see `ConicCurve`).
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
     * @param u Parameter; ignored, the second derivative of a parabola is constant.
     * @return `x_direction / (2 * focal_length)`.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        (void)u;
        return x_direction * (T(1) / (T(2) * focal_length));
    }

    /**
     * @brief Returns the zero vector: a parabola's derivatives stop changing after the second.
     *
     * @param u Parameter; ignored.
     * @return `Vector3(0, 0, 0)`.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        (void)u;
        return Vector(T(0), T(0), T(0));
    }

    /**
     * @brief Returns 3: this type answers the third derivative in closed form.
     *
     * @return 3.
     */
    [[nodiscard]] int maxDerivativeOrder() const override
    {
        return 3;
    }

    /**
     * @brief Returns the derivative of the requested order, including the third.
     *
     * @param u Parameter in this curve's own units.
     * @param order Derivative order, at least 1.
     * @return The derivative, or `std::nullopt` outside `[1, 3]`.
     */
    [[nodiscard]] std::optional<Vector> derivativeAt(T u, int order) const override
    {
        if (order == 3) {
            return thirdDerivative(u);
        }

        return Curve<T>::derivativeAt(u, order);
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
     * @return Always false.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a parabola does not repeat.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a parabola is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `-u`: the same point reached by walking the parabola from the other end.
     *
     * @param u Offset along the perpendicular axis.
     * @return `-u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return -u;
    }

    /**
     * @brief Returns whether the focal length and the normal are usable.
     *
     * @return True when the focal length is positive and the normal is non-zero.
     */
    [[nodiscard]] bool isValid() const override
    {
        return focal_length > vn::math::EPS<T>() && normal.length() > vn::math::EPS<T>();
    }

  public:
    /// Distance from the vertex to the focus.
    T focal_length{ T(1) };
};

using ParabolaCurvef = ParabolaCurve<float>;
using ParabolaCurved = ParabolaCurve<double>;

VN_GEOM_SPATIAL_NS_END
