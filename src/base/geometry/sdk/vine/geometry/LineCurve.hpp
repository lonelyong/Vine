#pragma once

#include "geometry_global.hpp"

#include <limits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A 3D line in point–direction parametric form.
 *
 *     P(t) = origin + t * direction,  t ∈ ℝ
 *
 * The direction vector does not need to be unit-length;
 * the line is defined for any non-zero direction.
 * The parameter t measures signed distance along the line scaled by |direction|.
 *
 * @tparam T Floating-point type (float or double).
 */
template <vn::math::FP T>
class LineCurve : public Curve<T>
{
  public:
    using value_type = T;

    /**
     * @brief Result of the line–line closest-point computation.
     *
     * Contains both topological flags (intersect / parallel / collinear)
     * and the geometric closest-point pair with their parameter values.
     */
    struct Intersection {
        /// Whether the two lines intersect (collinear, or closest distance < tolerance).
        bool is_intersected{ false };

        /// Whether the two lines are parallel (direction cross product is near zero).
        bool is_parallel{ false };

        /// Whether the two lines are collinear (parallel and the connecting vector
        /// between origins is also parallel to the direction).
        bool is_collinear{ false };

        /**
         * @brief Closest-point pair and their parameter values.
         *
         * A point on a line is parameterized as:
         *
         *     P(t) = origin + t * direction
         *
         * Non-parallel case: t1, t2 are the parameters of the
         * unique closest-point pair on each line.  pt1, pt2 are the
         * corresponding 3D points, and distance = |pt1 - pt2|.
         *
         * Parallel case: infinitely many closest-point pairs exist.
         * The result anchors on L1's origin:
         *   - t1 = 0,  pt1 = L1's origin.
         *   - t2 is the parameter of the projection of L1's origin onto L2.
         *   - pt2 is that projected point.
         *   - distance is the perpendicular distance between the two lines.
         */
        struct {
            /// Parameter of the closest point on this line (L1).
            /// Set to 0 when the lines are parallel.
            T t1{ 0 };

            /// Parameter of the closest point on the other line (L2).
            /// When parallel: parameter of the projection of L1's origin onto L2.
            T t2{ 0 };

            /// Closest point on this line (L1).
            /// Equals L1's origin when the lines are parallel.
            math::Point3<T> pt1;

            /// Closest point on the other line (L2).
            /// When parallel: the projection of L1's origin onto L2.
            math::Point3<T> pt2;

            /// Euclidean distance between the two closest points.
            T distance{ 0 };
        } params;
    };

  public:
    /**
     * @brief Constructs a line from an origin point and a direction vector.
     *
     *     L = { origin + t * direction | t ∈ ℝ }
     *
     * @param origin    Any point on the line (typically the "starting" point).
     * @param direction Direction vector; does not need to be unit-length.
     */
    constexpr LineCurve(const math::Point3<T>& origin, const math::Vector3<T>& direction)
      : origin(origin)
      , direction(direction)
    {}

  public:
    /**
     * @brief Returns the point at parameter @p t.
     *
     * @details
     * - `t` is a multiple of `direction`, so it is a distance only when `direction` is unit length — the same rule
     *   this class states for its parameter.
     *
     * @param t Multiple of `direction` from `origin`; negative values go backwards.
     * @return `origin + direction * t`.
     */
    [[nodiscard]] constexpr math::Point3<T> pointAt(T t) const
    {
        return origin + direction * t;
    }

    /**
     * @brief Returns the point at parameter @p u; the same value as `pointAt()`.
     *
     * @param u Multiple of `direction` from `origin`.
     * @return `origin + direction * u`.
     */
    [[nodiscard]] math::Point3<T> evaluate(T u) const override
    {
        return pointAt(u);
    }

    /**
     * @brief Returns the lower end of the range: a line is unbounded.
     *
     * @return Negative infinity.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return -std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns the upper end of the range: a line is unbounded.
     *
     * @return Positive infinity.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return std::numeric_limits<T>::infinity();
    }

    /**
     * @brief Returns false: an infinite line does not close.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a line does not repeat.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a line is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the constant first derivative: the direction vector itself.
     *
     * @details
     * - `origin + u * direction` differentiates to `direction` for every @p u, so this is the speed vector and its
     *   length is `|direction|`; it is a unit tangent only when `direction` is unit length.
     *
     * @param u Parameter; ignored, the derivative of a line is constant.
     * @return `direction`, unnormalised.
     */
    [[nodiscard]] math::Vector3<T> derivative(T u) const override
    {
        (void)u;
        return direction;
    }

    /**
     * @brief Returns the zero vector: a line does not bend.
     *
     * @param u Parameter; ignored, the second derivative of a line is zero everywhere.
     * @return `Vector3(0, 0, 0)`.
     */
    [[nodiscard]] math::Vector3<T> secondDerivative(T u) const override
    {
        (void)u;
        return math::Vector3<T>(T(0), T(0), T(0));
    }

    /**
     * @brief Returns `CN`: a line is infinitely differentiable inside its range.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns `-u`: the point reached from `origin` in the opposite direction.
     *
     * @param u Parameter in this line's units.
     * @return `-u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return -u;
    }

    /**
     * @brief Returns whether the direction is usable.
     *
     * @details
     * - A zero direction leaves a single point instead of a line: `closestPoint()` answers that point and
     *   `intersectWith()` reports no closest-point pair, so a caller checks here instead of discovering a NaN.
     *
     * @return True when `direction` is longer than the floating-point epsilon.
     */
    [[nodiscard]] bool isValid() const override
    {
        return direction.length() > math::EPS<T>();
    }

    /**
     * @brief Computes the closest-point pair between this line and another.
     *
     * Non-parallel (skew or intersecting):
     *
     * The closest-point pair (pt1, pt2) is the unique pair that minimizes
     * |P1(t1) - P2(t2)|.  It is found analytically by solving the 2×2
     * linear system derived from ∂/∂t1, ∂/∂t2 of the squared distance:
     *
     *     [ d1·d1   -d1·d2 ] [ t1 ]   [ -r·d1 ]
     *     [ d1·d2   -d2·d2 ] [ t2 ] = [ -r·d2 ]
     *
     * where r = p2 - p1 is the vector connecting the two origins.
     * The denominator |d1|²|d2|² − (d1·d2)² is exactly |d1 × d2|²
     * (Lagrange's identity).
     *
     * Parallel:
     *
     * Parallelism is detected when |d1 × d2|² < eps².
     * If additionally |r × d1|² < eps², the lines are collinear.
     * The closest-point pair is anchored at L1's origin projected onto L2
     * (see Intersection::params for details).
     *
     * Degenerate input: a line with a zero direction has no closest-point pair, and nothing is divided by its
     * length, so the result carries no flag and no number.
     *
     * @param other The other line.
     * @param eps   Numerical tolerance for zero checks.
     * @return Intersection containing flags and the closest-point pair.
     */
    [[nodiscard]]
    Intersection intersectWith(const LineCurve& other, T eps = math::EPS<T>()) const;

    /**
     * @brief Projects a point onto this line (orthogonal projection).
     *
     * The closest point is given by:
     *
     *     t = (pt - origin) · direction / |direction|²
     *     P_closest = origin + t * direction
     *
     * The returned point minimizes the Euclidean distance to @p pt among
     * all points on the line.
     *
     * @param pt Point to project.
     * @return Closest point on this line.
     */
    [[nodiscard]]
    math::Point3<T> closestPoint(const math::Point3<T>& pt) const;

  public:
    math::Point3<T>  origin;
    math::Vector3<T> direction;
};

template <vn::math::FP T>
math::Point3<T> LineCurve<T>::closestPoint(const math::Point3<T>& pt) const
{
    // P(t) = origin + t·direction
    // (pt − P(t))·direction = 0  →  t = (pt−origin)·direction / |direction|²

    // One threshold for "the direction is unusable": the same one `isValid()` reports, so a line whose direction is
    // merely short is still projected instead of silently collapsing to its origin.
    if (!isValid()) {
        return origin;
    }

    const auto             denom = direction.dot(direction);
    const math::Vector3<T> v     = pt - origin;
    const double           t     = v.dot(direction) / denom;

    return origin + direction * t;
}

using LineCurvef = LineCurve<float>;
using LineCurved = LineCurve<double>;

VN_GEOMETRY_NS_END
