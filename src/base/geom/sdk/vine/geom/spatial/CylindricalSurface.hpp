#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "ElementarySurface.hpp"
#include "Placement.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A circular cylinder: a radius around a stored axis, parameterised by an azimuth and a height.
 *
 * @details
 * - This is the *surface*, not the solid: it has no caps and no inside, so it exists for the questions a sheet answers
 *   - a trimmed B-rep face, a point projected onto it, a ray that must hit one side. The closed, placeable version a
 *   model holds is `CylinderSolid` (see `Primitive.hpp`).
 * - `evaluate(u, v) = origin + radius * (cos u * X + sin u * Y) + v * axis`, the parameterisation OCCT's
 *   `Geom_CylindricalSurface` uses: `u` is the azimuth in `[0, 2 * pi]` measured from `x_direction`, `v` is the signed
 *   distance along the axis in length units.
 * - `u` closes and repeats with `2 * pi`; `v` is unbounded and neither closes nor repeats, which is why a cylinder is
 *   an elementary surface rather than a bounded one.
 * - The two tangents are the parallel's velocity `radius * (-sin u * X + cos u * Y)` and the unit axis, so `v` is arc
 *   length along the sheet.
 * - `continuity()` is `CN`: a cylinder is analytic inside its domain.
 * - With a degenerate placement the parameterisation collapses onto the placement's `x_direction` and `isValid()` is
 *   false - the same rule the conics state (`ConicCurve`).
 *
 * @warning
 * - `radius` and `placement` are plain state: nothing re-validates them after construction, and `isValid()` is the only
 *   judgement.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class CylindricalSurface : public ElementarySurface<T> {
  public:
    using typename ElementarySurface<T>::Point;
    using typename ElementarySurface<T>::Vector;

  public:
    /**
     * @brief Constructs a cylinder from a point on its axis, the axis direction and a radius.
     *
     * @param origin_point    A point on the axis.
     * @param axis_direction  Direction of the axis; normalized before storage, and a zero vector leaves the surface
     *                        undefined.
     * @param cylinder_radius Distance from the axis, which must be positive for `isValid()`.
     */
    CylindricalSurface(const Point& origin_point, const Vector& axis_direction, T cylinder_radius)
      : placement(origin_point, axis_direction)
      , radius(cylinder_radius)
    {}

  public:
    /**
     * @brief Returns the point at azimuth @p angle and height @p height.
     *
     * @param angle  Azimuth in radians, measured from `x_direction` towards `yAxis()`.
     * @param height Signed distance along `axis` from `origin`.
     * @return The point on the sheet.
     */
    [[nodiscard]] Point pointAt(T angle, T height) const
    {
        return placement.origin
             + (placement.x_direction * std::cos(angle) + placement.yAxis() * std::sin(angle)) * radius
             + placement.axis * height;
    }

    /**
     * @brief Returns the point at parameters @p u (azimuth) and @p v (height); the same value as `pointAt()`.
     *
     * @param u Azimuth in radians.
     * @param v Signed distance along the axis.
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
     * - `dP/du = radius * (-X sin u + Y cos u)`: its length is the radius, so it is a unit tangent only for the unit
     *   cylinder. It vanishes nowhere, because a cylinder has no pole.
     *
     * @param u Azimuth in radians.
     * @param v Height; unused, the parallel's velocity does not depend on it.
     * @return The velocity along @p u.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        (void)v;
        return (placement.yAxis() * std::cos(u) - placement.x_direction * std::sin(u)) * radius;
    }

    /**
     * @brief Returns the first derivative with respect to the height: the unit axis direction.
     *
     * @param u Azimuth; unused, the axis direction does not depend on it.
     * @param v Height; unused, the axis direction does not depend on it.
     * @return `axis`, a unit vector.
     */
    [[nodiscard]] Vector derivativeV(T u, T v) const override
    {
        (void)u;
        (void)v;
        return placement.axis;
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
     * @brief Returns the lower end of the height domain: a cylinder has no ends.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the height domain: a cylinder has no ends.
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
     * @brief Returns false: the height direction is unbounded, not a seam.
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
     * @brief Returns false: the height direction does not repeat.
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
     * @brief Returns 0: the height direction is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a cylinder is analytic inside its domain.
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
     * - The normal of a cylinder does not depend on the height: it is the radial direction `cos u * X + sin u * Y`,
     *   which is `derivativeU() x derivativeV()` normalised.
     *
     * @param angle Azimuth in radians.
     * @return The unit normal pointing away from the axis; a zero vector for an unusable surface.
     */
    [[nodiscard]] Vector normalAt(T angle) const
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        return placement.x_direction * std::cos(angle) + placement.yAxis() * std::sin(angle);
    }

    /**
     * @brief Returns whether the placement and the radius are usable.
     *
     * @return True when the placement's main direction is a direction and the radius is positive.
     */
    [[nodiscard]] bool isValid() const override
    {
        return placement.hasUsableAxis() && radius > vn::math::EPS<T>();
    }

  public:
    /// Placement of the cylinder: one point on its axis, the axis direction and the direction `u` starts from.
    Placement<T> placement;

    /// Distance from the axis.
    T radius{ T(1) };
};

using CylindricalSurfacef = CylindricalSurface<float>;
using CylindricalSurfaced = CylindricalSurface<double>;

/**
 * @brief Returns the signed distance from @p point to the cylinder: positive outside it.
 *
 * @details
 * - The axial part of `p - origin` is dropped, so only the distance to the axis counts: `|radial| - radius`.
 * - The answer does not depend on the position along the axis, which is what makes the cylinder unbounded.
 *
 * @param surface CylinderSolid to measure against.
 * @param point   Point to measure.
 * @return Signed distance to the cylinder.
 */
template <vn::math::FP T>
[[nodiscard]] T signedValue(const CylindricalSurface<T>& surface, const vn::math::Point3<T>& point)
{
    const auto offset  = point - surface.placement.origin;
    const T    axial   = offset.dot(surface.placement.axis);
    const auto radial  = offset - surface.placement.axis * axial;
    return radial.length() - surface.radius;
}

VN_GEOM_SPATIAL_NS_END
