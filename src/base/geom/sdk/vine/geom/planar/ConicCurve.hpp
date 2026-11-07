#pragma once

#include <vine/geom/geom_global.hpp>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Vector2.hpp>

#include "Curve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief A planar conic: an origin and the in-plane axis the parameterisation starts from.
 *
 * @details
 * - The three conics that are not circles share exactly this frame - the centre (the vertex for a parabola) and the
 *   direction their parameter measures from - so it is owned here once, and the derived types differ only in the
 *   scalars that size them and in what their parameter means.
 * - `yAxis()` is the stored direction rotated by +90 degrees, derived rather than stored: a third vector would be a
 *   third thing to keep orthogonal to the other two.
 * - `continuity()` is answered here as `CN`: every conic is analytic inside its domain, and three copies of the same
 *   answer would be three places to disagree with each other.
 * - A circle does not derive from this class: its start direction is fixed by the plane, so it has no axis to share
 *   (see `CircleCurve`).
 * - The direction is normalised at construction. The fields are public, as elsewhere in this module's value types,
 *   but writing them afterwards leaves the frame inconsistent with the parameterisation - build a new curve instead.
 *
 * @note
 * - A zero direction is stored as a zero vector, which leaves `yAxis() == 0` and collapses every point query onto
 *   `center`: a `Point2` answer instead of a NaN, and each conic's `isValid()` is the one place that reports the
 *   frame as unusable.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class ConicCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point2<T>;
    using Vector     = vn::math::Vector2<T>;

  protected:
    /**
     * @brief Constructs the frame every planar conic shares: an origin and the axis the parameter starts from.
     *
     * @param center_point   Origin of the frame: the centre of the conic, or its vertex for a parabola.
     * @param axis_direction Direction the parameterisation should start from; normalised before storage, and a zero
     *                       vector leaves the conic without an axis.
     */
    constexpr ConicCurve(const Point& center_point, const Vector& axis_direction)
      : center(center_point)
      , x_direction(axis_direction.length() > vn::math::EPS<T>() ? axis_direction.normalized() : Vector::zero())
    {}

  public:
    /**
     * @brief Returns the direction that completes the frame: `x_direction` rotated by +90 degrees.
     *
     * @return `x_direction` rotated by +90 degrees: unit while the stored direction is unit, the zero vector when it
     *         is zero.
     */
    [[nodiscard]] constexpr Vector yAxis() const
    {
        return Vector{ -x_direction.y, x_direction.x };
    }

    /**
     * @brief Returns `CN`: a conic is analytic inside its domain.
     *
     * @details
     * - The answer does not depend on the scalars: an ellipse, a hyperbola and a parabola are all infinitely
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

    /// Unit direction the parameterisation starts from; zero when the conic was built without a usable direction.
    Vector x_direction{ T(1), T(0) };
};

VN_GEOM_PLANAR_NS_END
