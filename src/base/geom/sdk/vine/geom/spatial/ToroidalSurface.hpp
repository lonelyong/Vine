#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "ElementarySurface.hpp"
#include "Placement.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A torus: a minor circle of a given radius swept around a stored axis at a major radius.
 *
 * @details
 * - This is the *surface*, not the solid: no inside. The closed, placeable version a model holds would be a torus
 *   primitive.
 * - `evaluate(u, v) = origin + (R + r * cos v) * (cos u * X + sin u * Y) + r * sin v * axis`, OCCT's
 *   `Geom_ToroidalSurface` parameterisation: `u` is the azimuth about the axis and `v` the angle around the tube,
 *   both in `[0, 2 * pi]` and both measured from `x_direction` / from the equatorial plane.
 * - Both directions close and repeat with `2 * pi`, so a torus is the one elementary surface that is periodic twice;
 *   its two families of parameter lines are both circles.
 * - `continuity()` is `CN`: a torus is analytic inside its domain.
 * - With a degenerate placement the parameterisation collapses onto the placement's `x_direction` and `isValid()` is
 *   false - the same rule the conics state (`ConicCurve`).
 *
 * @warning
 * - `major_radius` must exceed `minor_radius`, otherwise the tube swallows the axis and the parameterisation is no
 *   longer a torus (it becomes a spindle, or a sphere when the two are equal); `isValid()` refuses it.
 * - `major_radius`, `minor_radius` and `placement` are plain state: `isValid()` is the only judgement.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ToroidalSurface : public ElementarySurface<T> {
  public:
    using typename ElementarySurface<T>::Point;
    using typename ElementarySurface<T>::Vector;

  public:
    /**
     * @brief Constructs a torus from a point on its axis, the axis direction and the two radii.
     *
     * @param origin_point     Centre of the torus: the point where the axis meets the equatorial plane.
     * @param axis_direction   Direction of the axis; normalized before storage, and a zero vector leaves the surface
     *                         undefined.
     * @param major_radius     Distance from the axis to the centre of the tube, which must exceed
     *                         @p minor_radius for `isValid()`.
     * @param minor_radius     Radius of the tube, which must be positive for `isValid()`.
     */
    ToroidalSurface(const Point& origin_point, const Vector& axis_direction, T major_radius, T minor_radius)
      : placement(origin_point, axis_direction)
      , major_radius(major_radius)
      , minor_radius(minor_radius)
    {}

  public:
    /**
     * @brief Returns the point at azimuth @p angle and tube angle @p section_angle.
     *
     * @param angle         Azimuth about the axis, measured from `x_direction` towards `yAxis()`.
     * @param section_angle Angle around the tube, measured from the equatorial plane towards `axis`.
     * @return The point on the sheet.
     */
    [[nodiscard]] Point pointAt(T angle, T section_angle) const
    {
        const T radial = major_radius + minor_radius * std::cos(section_angle);
        return placement.origin
             + (placement.x_direction * std::cos(angle) + placement.yAxis() * std::sin(angle)) * radial
             + placement.axis * (minor_radius * std::sin(section_angle));
    }

    /**
     * @brief Returns the point at parameters @p u (azimuth) and @p v (tube angle); the same as `pointAt()`.
     *
     * @param u Azimuth in radians.
     * @param v Tube angle in radians.
     * @return The point on the sheet.
     */
    [[nodiscard]] Point evaluate(T u, T v) const override
    {
        return pointAt(u, v);
    }

    /**
     * @brief Returns the first derivative with respect to the azimuth: the velocity along a parallel of the torus.
     *
     * @details
     * - `dP/du = (R + r * cos v) * (-X sin u + Y cos u)`: its length is the distance from the axis to the point, so it
     *   is largest on the outer equator and smallest on the inner one, and it vanishes nowhere while `R > r`.
     *
     * @param u Azimuth in radians.
     * @param v Tube angle in radians.
     * @return The velocity along @p u.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        const T radial = major_radius + minor_radius * std::cos(v);
        return (placement.yAxis() * std::cos(u) - placement.x_direction * std::sin(u)) * radial;
    }

    /**
     * @brief Returns the first derivative with respect to the tube angle: the velocity around the tube.
     *
     * @details
     * - `dP/dv = r * (-sin v * (X cos u + Y sin u) + cos v * axis)`, of length `r`: the tube is parameterised by arc
     *   length, so the surface's `v` speed is the minor radius everywhere.
     *
     * @param u Azimuth in radians.
     * @param v Tube angle in radians.
     * @return The velocity along @p v, of length `minor_radius`.
     */
    [[nodiscard]] Vector derivativeV(T u, T v) const override
    {
        return (placement.x_direction * std::cos(u) + placement.yAxis() * std::sin(u))
                 * (-minor_radius * std::sin(v))
             + placement.axis * (minor_radius * std::cos(v));
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
     * @brief Returns the start of the tube-angle domain.
     *
     * @return 0 radians, on the outer side of the equatorial plane.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the tube-angle domain.
     *
     * @return One full turn, `2 * pi` radians.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return T(2) * static_cast<T>(vn::math::PI);
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
     * @brief Returns true: the tube angle wraps around the tube.
     *
     * @return Always true.
     */
    [[nodiscard]] bool isVClosed() const override
    {
        return true;
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
     * @brief Returns true: the tube angle repeats with its period.
     *
     * @return Always true.
     */
    [[nodiscard]] bool isVPeriodic() const override
    {
        return true;
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
     * @brief Returns the tube-angle period in radians.
     *
     * @return `2 * pi`.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return lastVParameter() - firstVParameter();
    }

    /**
     * @brief Returns `CN`: a torus is analytic inside its domain.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns the outward unit normal at the given parameters.
     *
     * @details
     * - `normalAt = cos v * (X cos u + Y sin u) + sin v * axis`: the direction from the centre of the tube towards the
     *   point, which is `derivativeU() x derivativeV()` normalised.
     *
     * @param angle         Azimuth in radians.
     * @param section_angle Tube angle in radians.
     * @return The unit normal pointing away from the tube's centre circle; a zero vector for an unusable surface.
     */
    [[nodiscard]] Vector normalAt(T angle, T section_angle) const
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        return (placement.x_direction * std::cos(angle) + placement.yAxis() * std::sin(angle))
                 * std::cos(section_angle)
             + placement.axis * std::sin(section_angle);
    }

    /**
     * @brief Returns whether the placement and the two radii are usable.
     *
     * @return True when the placement's main direction is a direction, the tube radius is positive and the major radius
     *         exceeds it.
     */
    [[nodiscard]] bool isValid() const override
    {
        return placement.hasUsableAxis() && minor_radius > vn::math::EPS<T>()
            && major_radius > minor_radius + vn::math::EPS<T>();
    }

  public:
    /// Placement of the torus: the point where its axis meets the equatorial plane, the axis and the `u` start.
    Placement<T> placement;

    /// Distance from the axis to the centre of the tube.
    T major_radius{ T(2) };

    /// Radius of the tube.
    T minor_radius{ T(1) };
};

using ToroidalSurfacef = ToroidalSurface<float>;
using ToroidalSurfaced = ToroidalSurface<double>;

/**
 * @brief Returns the signed distance from @p point to the torus: positive outside it.
 *
 * @details
 * - The tube's centre circle lies at `major_radius` from the axis, so the distance to it is
 *   `|(radial - major_radius, axial)|` and the distance to the torus is that minus `minor_radius`.
 * - That is the standard tube formula: exact inside and outside, unit gradient everywhere including the two circles
 *   where the outer and inner halves meet.
 *
 * @param surface Torus to measure against.
 * @param point   Point to measure.
 * @return Signed distance to the torus.
 */
template <vn::math::FP T>
[[nodiscard]] T signedValue(const ToroidalSurface<T>& surface, const vn::math::Point3<T>& point)
{
    const auto offset = point - surface.placement.origin;
    const T    axial  = offset.dot(surface.placement.axis);
    const T    radial = (offset - surface.placement.axis * axial).length();
    const T    ring   = radial - surface.major_radius;
    return std::sqrt(ring * ring + axial * axial) - surface.minor_radius;
}

VN_GEOM_SPATIAL_NS_END
