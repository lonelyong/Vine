#pragma once

#include "geom3d_global.hpp"

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Frame.hpp"

VN_GEOM3D_NS_BEGIN

/**
 * @brief A right-handed coordinate system in space: an origin, a main direction and the direction `u` starts from.
 *
 * @details
 * - This is Vine's counterpart of OCCT's `gp_Ax3`, minus the handedness switch: `origin` plus `axis` (the main
 *   direction, OCCT's Z) plus `x_direction`, with `yAxis()` derived as `axis x x_direction` so a third vector cannot
 *   disagree with the first two. A mirrored frame is expressed by building a new placement, not by a flag.
 * - It carries no scale and applies to nothing: it says *where and how something is placed*, not how to move anything.
 *   Converting between two placements is a transform's job (`math::Isometry3`), not this type's.
 * - The frame is normalised at construction: `axis` becomes unit, `x_direction` is projected into the plane orthogonal
 *   to it and normalised, and a degenerate input keeps the fallback (`axis == 0`, `x_direction == +X`) that the
 *   concrete types report through their `isValid()`.
 * - The rule that turns a preferred direction into an in-plane axis is `inPlaneAxis()` in `Frame.hpp`, shared with the
 *   conic curves and with the plane's derived axes.
 *
 * @note
 * - It is a value, not a layer: the elementary surfaces that need a placement hold one as a member, so the type tree
 *   keeps OCCT's shape (`ElementarySurface` -> the concrete surface) and this type adds no virtual function.
 * - The fields are public, as elsewhere in this module's value types, but writing them afterwards leaves three vectors
 *   describing frames that no longer agree - build a new placement instead.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class Placement {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

  public:
    /// Constructs the world frame: the origin, the +Z main direction and the +X direction.
    constexpr Placement() = default;

    /**
     * @brief Constructs a frame from an origin, a main direction and, optionally, the direction `u` starts from.
     *
     * @param origin_point   The frame's origin.
     * @param axis_direction Main direction (the frame's Z); normalized before storage, and a zero vector leaves the
     *                       frame undefined while keeping the `x_direction` fallback.
     * @param preferred_axis Direction the frame's X should point along; projected into the plane orthogonal to the main
     *                       direction. A zero vector selects the least aligned world axis.
     */
    Placement(const Point& origin_point, const Vector& axis_direction,
              const Vector& preferred_axis = Vector(T(0), T(0), T(0)))
      : origin(origin_point)
      , axis(axis_direction.length() > vn::math::EPS<T>() ? axis_direction.normalized() : Vector(T(0), T(0), T(0)))
      , x_direction(inPlaneAxis(preferred_axis, axis_direction))
    {}

  public:
    /**
     * @brief Returns the direction that completes the frame: `axis x x_direction`.
     *
     * @return The frame's Y direction; a unit vector when the stored frame is unit.
     */
    [[nodiscard]] Vector yAxis() const
    {
        return axis.cross(x_direction);
    }

    /**
     * @brief Returns whether the stored main direction is usable.
     *
     * @return True when `axis` is longer than the floating-point epsilon.
     */
    [[nodiscard]] bool hasUsableAxis() const
    {
        return axis.length() > vn::math::EPS<T>();
    }

  public:
    /// The frame's origin.
    Point origin;

    /// Unit main direction (the frame's Z); zero when the placement was built without a usable axis.
    Vector axis{ T(0), T(0), T(1) };

    /// Unit direction the frame's X points along.
    Vector x_direction{ T(1), T(0), T(0) };
};

using Placementf = Placement<float>;
using Placementd = Placement<double>;

VN_GEOM3D_NS_END
