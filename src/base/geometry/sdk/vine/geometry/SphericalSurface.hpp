#pragma once

#include "geometry_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Aabb.hpp"
#include "Surface.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An unbounded spherical sheet: a centre and a radius, parameterised by two angles.
 *
 * @details
 * - This is the *surface*, not the solid: it has no inside, so it exists for the questions a sheet answers — a
 *   trimmed B-rep face, a ray that must hit one side, a point projected onto the sheet. The closed, placeable
 *   version a model holds is `Sphere` (see `Primitive.hpp`).
 * - `pointAt(azimuth, polar)` uses the same parameterisation OCCT's spherical surface does: azimuth around +Z,
 *   polar measured from +Z, both in radians. `polar = 0` is the +Z pole, `polar = pi / 2` the equator.
 * - This is a surface (`Surface::kDimension == 2`); `Surface` adds nothing to its size.
 * - No ray query yet: it arrives when a consumer needs one (`../backlog/geometry.md` G2), and as a member here when
 *   it does (`geometry-kinds.md` 2.6).
 *
 * @warning
 * - `centre` and `radius` are plain state: nothing re-validates them, and `isValid()` is the only judgement.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class SphericalSurface : public ElementarySurface {
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
     * @brief Returns whether the radius is usable.
     *
     * @return True when the radius is positive.
     */
    [[nodiscard]] bool isValid() const
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

VN_GEOMETRY_NS_END
