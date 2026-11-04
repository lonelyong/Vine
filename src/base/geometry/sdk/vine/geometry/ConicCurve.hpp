#pragma once

#include "geometry_global.hpp"

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"
#include "Frame.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A curve that is a conic section: a circle, an ellipse, a hyperbola or a parabola.
 *
 * @details
 * - A conic is one analytic form: an origin in a plane, the plane's normal, and an in-plane axis the parameterisation
 *   starts from. That frame is owned here, once, so the four conics differ only in the scalar that sizes them (a
 *   radius, two semi-axes, a focal length) and in what their parameter means.
 * - `continuity()` is answered here as `CN`: every conic is analytic inside its domain, and four copies of the same
 *   answer would be four places to disagree with each other.
 * - The frame is normalised at construction. The fields are public, as elsewhere in this module's value types, but
 *   writing them afterwards leaves three vectors describing frames that no longer agree - build a new curve instead
 *   of editing this one.
 * - The class adds no evaluation: each conic answers `evaluate()` and its own parameter range on its own, because the
 *   parameter is an eccentric angle for a circle or an ellipse, a hyperbolic angle for a hyperbola and an offset for
 *   a parabola.
 *
 * @note
 * - A degenerate frame keeps the fallback `x_direction = +X` and leaves `yAxis() == 0`, so the parameterisation
 *   collapses onto the world X axis through the centre: a segment for the circle and the ellipse, a ray for the
 *   parabola and two opposite rays for the hyperbola. That is the answer the four conics document at `pointAt()`, it
 *   never contains a NaN, and each conic's `isValid()` is the one place that reports the frame as unusable.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ConicCurve : public Curve<T> {
  public:
    using value_type = T;
    using typename Curve<T>::Point;
    using typename Curve<T>::Vector;

  public:
    /// Constructs the unit circle in the plane `z = 0`, with its frame at the origin.
    constexpr ConicCurve() = default;

  protected:
    /**
     * @brief Constructs the frame every conic shares: an origin, a plane normal and the axis the parameter starts from.
     *
     * @param center_point   Origin of the frame: the centre of the conic, or its vertex for a parabola.
     * @param plane_normal   Normal of the plane the conic lies in; normalized before storage, and a zero vector
     *                       leaves the curve in an undefined plane.
     * @param preferred_axis Direction the parameterisation should start from; projected into the plane and normalized.
     *                       A zero vector selects the least aligned world axis, which is what a circle needs because
     *                       it has no axis of its own.
     */
    ConicCurve(const Point& center_point, const Vector& plane_normal,
               const Vector& preferred_axis = Vector(T(0), T(0), T(0)))
      : center(center_point)
      , normal(plane_normal.length() > vn::math::EPS<T>() ? plane_normal.normalized() : Vector(T(0), T(0), T(0)))
      , x_direction(inPlaneAxis(preferred_axis, plane_normal))
    {}

  public:
    /**
     * @brief Returns the in-plane axis that completes the frame: `normal x x_direction`.
     *
     * @details
     * - Derived rather than stored: a third vector would be a third thing to keep orthogonal to the other two.
     *
     * @return The minor / conjugate axis direction, unit when the stored frame is unit.
     */
    [[nodiscard]] Vector yAxis() const
    {
        return normal.cross(x_direction);
    }

    /**
     * @brief Returns the normal of the plane the conic lies in.
     *
     * @return The stored unit normal, or a zero vector when the conic was built without a usable normal.
     */
    [[nodiscard]] constexpr const Vector& planeNormal() const
    {
        return normal;
    }

    /**
     * @brief Returns `CN`: a conic is analytic inside its domain.
     *
     * @details
     * - The answer does not depend on the scalars: a circle, an ellipse, a hyperbola and a parabola are all infinitely
     *   differentiable wherever they are defined, so it is stated once, here.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

  public:
    /// Origin of the frame: the centre of the conic, or its vertex for a parabola.
    Point center;

    /// Unit normal of the plane the conic lies in; zero when the conic was built without a usable normal.
    Vector normal{ T(0), T(0), T(1) };

    /// Unit in-plane direction the parameterisation starts from.
    Vector x_direction{ T(1), T(0), T(0) };
};

VN_GEOMETRY_NS_END
