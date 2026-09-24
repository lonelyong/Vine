#pragma once
#include "Point3.hpp"
#include "Vector3.hpp"

VN_MATH_NS_BEGIN

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
template <typename T>
class Line {
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
            Point3<T> pt1;

            /// Closest point on the other line (L2).
            /// When parallel: the projection of L1's origin onto L2.
            Point3<T> pt2;

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
    constexpr Line(const Point3<T>& origin, const Vector3<T>& direction)
      : origin(origin)
      , direction(direction)
    {}

  public:
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
     * @param other The other line.
     * @param eps   Numerical tolerance for zero checks.
     * @return Intersection containing flags and the closest-point pair.
     */
    [[nodiscard]]
    Intersection intersectWith(const Line& other, T eps = EPS<T>()) const;

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
    Point3<T> closestPoint(const Point3<T>& pt) const;

  public:
    Point3<T>  origin;
    Vector3<T> direction;
};

using Linef = Line<float>;
using Lined = Line<double>;

VN_MATH_NS_END
