#pragma once

#include "geometry_global.hpp"

#include <cmath>
#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "ElementarySurface.hpp"
#include "Placement.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A circular cone: a semi-angle and a reference radius around a stored axis.
 *
 * @details
 * - This is the *surface*, not the solid: the two nappes, no base and no inside. The closed, placeable version a model
 *   holds is `Cone` (see `Primitive.hpp`).
 * - `evaluate(u, v)` uses OCCT's `Geom_ConicalSurface` parameterisation:
 *   `r(v) = reference_radius + v * sin(semi_angle)` and
 *   `P = origin + r(v) * (cos u * X + sin u * Y) + v * cos(semi_angle) * axis`.
 *   `u` is the azimuth in `[0, 2 * pi]`; `v` is the **arc length along a generatrix** from the reference circle, not a
 *   height along the axis - that is why `derivativeV()` is a unit vector.
 * - `u` closes and repeats with `2 * pi`; `v` is unbounded and neither closes nor repeats, so both nappes are covered
 *   by one parameter range: `v` below the apex crosses to the other nappe.
 * - `continuity()` is `CN`: a cone is analytic inside its domain.
 * - With a degenerate placement the parameterisation collapses onto the placement's `x_direction` and `isValid()` is
 *   false - the same rule the conics state (`ConicCurve`).
 *
 * @warning
 * - A semi-angle at or beyond `pi / 2` is a plane, not a cone, so `isValid()` refuses it; nothing clamps it.
 * - `reference_radius`, `semi_angle` and `placement` are plain state: `isValid()` is the only judgement.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ConicalSurface : public ElementarySurface<T> {
  public:
    using typename ElementarySurface<T>::Point;
    using typename ElementarySurface<T>::Vector;

  public:
    /**
     * @brief Constructs a cone from a point on its axis, the axis direction, the reference radius and the semi-angle.
     *
     * @param origin_point     A point on the axis; the reference circle is centred here.
     * @param axis_direction   Direction of the axis; normalized before storage, and a zero vector leaves the surface
     *                         undefined.
     * @param reference_radius Radius at @p v `= 0`, which must not be negative for `isValid()`.
     * @param cone_semi_angle  Angle between the axis and a generatrix, in radians; its absolute value must lie strictly
     *                         between 0 and `pi / 2` for `isValid()`.
     */
    ConicalSurface(const Point& origin_point, const Vector& axis_direction, T reference_radius, T cone_semi_angle)
      : placement(origin_point, axis_direction)
      , reference_radius(reference_radius)
      , semi_angle(cone_semi_angle)
    {}

  public:
    /**
     * @brief Returns the point at azimuth @p angle and generatrix position @p offset.
     *
     * @param angle  Azimuth in radians, measured from `x_direction` towards `yAxis()`.
     * @param offset Signed arc length along a generatrix from the reference circle; negative values run towards the
     *               apex and beyond it onto the other nappe.
     * @return The point on the sheet; the apex itself for the one @p offset that reaches it.
     */
    [[nodiscard]] Point pointAt(T angle, T offset) const
    {
        const T radial = reference_radius + offset * std::sin(semi_angle);
        return placement.origin
             + (placement.x_direction * std::cos(angle) + placement.yAxis() * std::sin(angle)) * radial
             + placement.axis * (offset * std::cos(semi_angle));
    }

    /**
     * @brief Returns the point at parameters @p u (azimuth) and @p v (generatrix position); the same as `pointAt()`.
     *
     * @param u Azimuth in radians.
     * @param v Signed arc length along a generatrix.
     * @return The point on the sheet.
     */
    [[nodiscard]] Point evaluate(T u, T v) const override
    {
        return pointAt(u, v);
    }

    /**
     * @brief Returns the first derivative with respect to the azimuth: the velocity along a parallel.
     *
     * @details
     * - `dP/du = r(v) * (-X sin u + Y cos u)`: its length is the radius at @p v, so it vanishes at the apex - the one
     *   singular point of this parameterisation.
     *
     * @param u Azimuth in radians.
     * @param v Signed arc length along a generatrix.
     * @return The velocity along @p u.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        const T radial = reference_radius + v * std::sin(semi_angle);
        return (placement.yAxis() * std::cos(u) - placement.x_direction * std::sin(u)) * radial;
    }

    /**
     * @brief Returns the first derivative with respect to the generatrix position.
     *
     * @details
     * - `dP/dv = sin(alpha) * (X cos u + Y sin u) + cos(alpha) * axis`, which is a unit vector: `v` is arc length,
     *   so the surface has no scaling between its parameter and its length in the @p v direction.
     *
     * @param u Azimuth in radians.
     * @param v Signed arc length; unused, the generatrix direction does not depend on it.
     * @return The velocity along @p v, of length 1.
     */
    [[nodiscard]] Vector derivativeV(T u, T v) const override
    {
        (void)v;
        return (placement.x_direction * std::cos(u) + placement.yAxis() * std::sin(u)) * std::sin(semi_angle)
             + placement.axis * std::cos(semi_angle);
    }

    /**
     * @brief Returns the start of the azimuth domain.
     *
     * @return 0 radians.
     */
    [[nodiscard]] T firstUParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the azimuth domain.
     *
     * @return One full turn, `2 * pi` radians.
     */
    [[nodiscard]] T lastUParameter() const override
    {
        return T(2) * static_cast<T>(vn::math::PI);
    }

    /**
     * @brief Returns the lower end of the generatrix domain: both nappes are unbounded.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the generatrix domain: both nappes are unbounded.
     *
     * @return Positive infinity.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns true: the azimuth wraps around the axis.
     *
     * @return Always true.
     */
    [[nodiscard]] bool isUClosed() const override
    {
        return true;
    }

    /**
     * @brief Returns false: the generatrix direction is unbounded, not a seam.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isVClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns true: the azimuth repeats with its period.
     *
     * @return Always true.
     */
    [[nodiscard]] bool isUPeriodic() const override
    {
        return true;
    }

    /**
     * @brief Returns false: the generatrix direction does not repeat.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isVPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns the azimuth period in radians.
     *
     * @return `2 * pi`.
     */
    [[nodiscard]] T uPeriod() const override
    {
        return lastUParameter() - firstUParameter();
    }

    /**
     * @brief Returns 0: the generatrix direction is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a cone is analytic inside its domain.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns the outward unit normal at azimuth @p angle.
     *
     * @details
     * - The normal is `cos(alpha) * (X cos u + Y sin u) - sin(alpha) * axis`: it tilts away from the axis by the
     *   semi-angle, and it does not depend on @p offset because a cone's normal is constant along a generatrix.
     *
     * @param angle Azimuth in radians.
     * @return The unit normal; a zero vector for an unusable surface.
     */
    [[nodiscard]] Vector normalAt(T angle) const
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        return (placement.x_direction * std::cos(angle) + placement.yAxis() * std::sin(angle)) * std::cos(semi_angle)
             - placement.axis * std::sin(semi_angle);
    }

    /**
     * @brief Returns whether the placement, the reference radius and the semi-angle are usable.
     *
     * @return True when the placement's main direction is a direction, the reference radius is not negative and the
     *         absolute semi-angle is strictly inside `(0, pi / 2)`.
     */
    [[nodiscard]] bool isValid() const override
    {
        const T magnitude = std::abs(semi_angle);
        return placement.hasUsableAxis() && reference_radius >= T(0) && magnitude > vn::math::EPS<T>()
            && magnitude < static_cast<T>(vn::math::PI_HALF) - vn::math::EPS<T>();
    }

  public:
    /// Placement of the cone: one point on its axis (the reference circle's centre), the axis and the `u` start.
    Placement<T> placement;

    /// Radius of the reference circle at `v = 0`.
    T reference_radius{ T(1) };

    /// Angle between the axis and a generatrix, in radians.
    T semi_angle{ static_cast<T>(vn::math::PI_QUARTER) };
};

using ConicalSurfacef = ConicalSurface<float>;
using ConicalSurfaced = ConicalSurface<double>;

/**
 * @brief Returns the signed distance from @p point to the cone: positive outside it.
 *
 * @details
 * - The cone is `radial = reference_radius + axial * tan(semi_angle)`, so the signed distance is that equation
 *   multiplied by `cos(semi_angle)`: `cos(a) * (|radial| - reference_radius) - sin(a) * axial`. The cosine is what
 *   normalises the gradient to unit length, and on a point of the cone the two terms cancel exactly.
 * - `axial` is measured from the placement origin along the axis, matching the parameterisation's `v * cos(a)`.
 *
 * @param surface Cone to measure against.
 * @param point   Point to measure.
 * @return Signed distance to the cone.
 */
template <vn::math::FP T>
[[nodiscard]] T signedValue(const ConicalSurface<T>& surface, const vn::math::Point3<T>& point)
{
    const auto offset = point - surface.placement.origin;
    const T    axial  = offset.dot(surface.placement.axis);
    const T    radial = (offset - surface.placement.axis * axial).length();
    return std::cos(surface.semi_angle) * (radial - surface.reference_radius)
         - std::sin(surface.semi_angle) * axial;
}

VN_GEOMETRY_NS_END
