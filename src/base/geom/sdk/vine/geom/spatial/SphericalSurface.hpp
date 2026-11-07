#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "ElementarySurface.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief An unbounded spherical sheet: a centre and a radius, parameterised by two angles.
 *
 * @details
 * - This is the *surface*, not the solid: it has no inside, so it exists for the questions a sheet answers — a
 *   trimmed B-rep face, a ray that must hit one side, a point projected onto the sheet. The closed, placeable
 *   version a model holds is `SphereSolid` (see `Primitive.hpp`).
 * - `pointAt(azimuth, polar)` uses standard spherical coordinates: azimuth around +Z, polar measured **from +Z**, both
 *   in radians. `polar = 0` is the +Z pole, `polar = pi / 2` the equator. As a `Surface<T>` the two parameters are
 *   `u = azimuth` in `[0, 2 * pi]` and `v = polar` in `[0, pi]`.
 * - **This deliberately differs from OCCT**: `Geom_SphericalSurface` measures `v` from the **equator** and uses the
 *   domain `[-pi / 2, pi / 2]`. A non-negative domain whose ends are the two poles makes `v` readable without a sign,
 *   so the conversion from OCCT data is one shift: `v_occt = pi / 2 - v`.
 * - `u` closes and repeats with `2 * pi`; `v` does not, because its ends are two points (the poles), not a seam.
 * - It carries the vptr of `Surface<T>`: the two tangents and the domain are answered through the interface and the
 *   azimuth / polar answers stay non-virtual for callers that know the type.
 * - No ray query yet: it arrives when a consumer needs one (`../backlog/geometry.md` G2), and as a member here when
 *   it does (`geometry-kinds.md` 2.6).
 *
 * @warning
 * - `centre` and `radius` are plain state: nothing re-validates them, and `isValid()` is the only judgement.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class SphericalSurface : public ElementarySurface<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

  public:
    /// Constructs the unit sphere surface at the origin.
    constexpr SphericalSurface() = default;

    /**
     * @brief Constructs a spherical sheet.
     *
     * @param centre_point Centre of the sphere.
     * @param sphere_radius Radius, which must be positive for `isValid()`.
     */
    constexpr SphericalSurface(const Point& centre_point, T sphere_radius)
      : centre(centre_point)
      , radius(sphere_radius)
    {}

  public:
    /**
     * @brief Returns the point at the given angles.
     *
     * @param azimuth Angle around +Z, in radians, measured from the +X axis towards +Y.
     * @param polar   Angle from +Z, in radians; 0 is the +Z pole, pi/2 the equator, pi the -Z pole.
     * @return The point on the sheet.
     */
    [[nodiscard]] Point pointAt(T azimuth, T polar) const
    {
        const T sin_polar = std::sin(polar);
        return Point(centre.x + radius * sin_polar * std::cos(azimuth),
                     centre.y + radius * sin_polar * std::sin(azimuth),
                     centre.z + radius * std::cos(polar));
    }

    /**
     * @brief Returns the outward unit normal at the given angles.
     *
     * @param azimuth Angle around +Z, in radians.
     * @param polar   Angle from +Z, in radians.
     * @return The normal pointing away from the centre, or a zero vector for a degenerate sheet.
     */
    [[nodiscard]] Vector normalAt(T azimuth, T polar) const
    {
        if (radius <= vn::math::EPS<T>()) {
            return Vector(T(0), T(0), T(0));
        }
        const T sin_polar = std::sin(polar);
        return Vector(sin_polar * std::cos(azimuth), sin_polar * std::sin(azimuth), std::cos(polar));
    }

    /**
     * @brief Returns the point at parameters @p u (azimuth) and @p v (polar); the same value as `pointAt()`.
     *
     * @param u Azimuth in radians, measured from +X towards +Y.
     * @param v Polar angle in radians, measured from +Z.
     * @return The point on the sheet.
     */
    [[nodiscard]] Point evaluate(T u, T v) const override
    {
        return pointAt(u, v);
    }

    /**
     * @brief Returns the first derivative with respect to the azimuth.
     *
     * @details
     * - `dP/du = radius * sin(polar) * (-sin(azimuth), cos(azimuth), 0)`: the velocity of a parallel, so it vanishes at
     *   both poles - the singular points of this parameterisation.
     *
     * @param u Azimuth in radians.
     * @param v Polar angle in radians.
     * @return The velocity vector along @p u.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        const T speed = radius * std::sin(v);
        return Vector(-speed * std::sin(u), speed * std::cos(u), T(0));
    }

    /**
     * @brief Returns the first derivative with respect to the polar angle.
     *
     * @details
     * - `dP/dv = radius * (cos(polar) * cos(azimuth), cos(polar) * sin(azimuth), -sin(polar))`: the velocity of a
     *   meridian, of constant length `radius`.
     *
     * @param u Azimuth in radians.
     * @param v Polar angle in radians.
     * @return The velocity vector along @p v.
     */
    [[nodiscard]] Vector derivativeV(T u, T v) const override
    {
        const T cos_polar = std::cos(v);
        return Vector(radius * cos_polar * std::cos(u), radius * cos_polar * std::sin(u), -radius * std::sin(v));
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
     * @brief Returns the start of the polar domain.
     *
     * @return 0 radians, the +Z pole.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the polar domain.
     *
     * @return `pi` radians, the -Z pole.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return static_cast<T>(vn::math::PI);
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
     * @brief Returns false: the polar range ends at two points, not at a seam.
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
     * @brief Returns false: the polar angle does not repeat.
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
     * @brief Returns 0: the polar direction is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a sphere is analytic, and its poles are singularities of the parameterisation, not of the
     * surface.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns whether the radius is usable.
     *
     * @return True when the radius is positive.
     */
    [[nodiscard]] bool isValid() const override
    {
        return radius > vn::math::EPS<T>();
    }

    /**
     * @brief Returns the axis-aligned bounds of the sheet.
     *
     * @return Cube of side 2 * radius around the centre.
     */
    [[nodiscard]] Aabb<T> bounds() const
    {
        return Aabb<T>(Point(centre.x - radius, centre.y - radius, centre.z - radius),
                       Point(centre.x + radius, centre.y + radius, centre.z + radius));
    }

  public:
    /// Centre of the sphere.
    Point centre;

    /// Radius of the sheet.
    T radius{ T(1) };
};

using SphericalSurfacef = SphericalSurface<float>;
using SphericalSurfaced = SphericalSurface<double>;

/**
 * @brief Returns the signed distance from @p point to the sphere: positive outside it.
 *
 * @details
 * - `|p - centre| - radius`, a true distance: its gradient is the unit radial direction.
 * - A zero radius answers `|p - centre|`, which never changes sign, so a search over it finds no crossing.
 *
 * @param surface SphereSolid to measure against.
 * @param point   Point to measure.
 * @return Signed distance to the sphere.
 */
template <vn::math::FP T>
[[nodiscard]] T signedValue(const SphericalSurface<T>& surface, const vn::math::Point3<T>& point)
{
    return (point - surface.centre).length() - surface.radius;
}

VN_GEOM_SPATIAL_NS_END
