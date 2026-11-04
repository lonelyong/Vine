#pragma once

#include "geometry_global.hpp"

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Continuity.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An evaluable surface: a point for every parameter pair, plus the domain, its closures and its smoothness.
 *
 * @details
 * - This is the **interface** an algorithm consumes when it does not know which surface it holds: it evaluates a point,
 *   asks for the two first derivatives, asks for the parameter rectangle and for whether the surface closes in either
 *   direction. The concrete value types (`Plane`, `SphericalSurface`, ...) implement it, so a caller that does know the
 *   type keeps their cheaper, non-virtual members.
 * - The interface carries the same three groups as `Curve<T>`, one dimension up: evaluation with its tangents, a domain
 *   per parameter with its closure and period, and the smoothness class. That is what "a surface is a two-parameter
 *   locus" turns into when it has to be callable through a base.
 * - A surface answers outside its declared rectangle too: the domain is a property of the surface, not a precondition
 *   of `evaluate`.
 * - `kDimension` stays as the one compile-time fact: a point here needs two parameters.
 *
 * @warning
 * - A destructor here means `Plane` and `SphericalSurface` each carry a vptr and are no longer trivially copyable;
 *   keep hot query paths (`Plane::intersectWith`) on the value type, not on `Surface<T>&`.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class Surface {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;

    /// Number of parameters a point on the geometry needs.
    static constexpr int kDimension = 2;

  public:
    virtual ~Surface() = default;

  public:
    /**
     * @brief Returns the point at parameters @p u and @p v.
     *
     * @param u First parameter, in this surface's own units (an azimuth for a sphere, an in-plane coordinate for a
     *          plane).
     * @param v Second parameter, in this surface's own units (a polar angle for a sphere).
     * @return The point on the surface.
     */
    [[nodiscard]] virtual Point evaluate(T u, T v) const = 0;

    /**
     * @brief Returns the first derivative with respect to @p u, `dP/du`.
     *
     * @details
     * - This is one edge of the tangent plane, not a unit vector: its length is the parameterisation's speed in that
     *   direction.
     * - The two tangents are the whole first-order information: a normal is `derivativeU() x derivativeV()`
     *   normalised, so it is not declared here, and neither is any orientation convention.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The velocity vector along @p u at `(u, v)`; the zero vector at a singular point.
     */
    [[nodiscard]] virtual Vector derivativeU(T u, T v) const = 0;

    /**
     * @brief Returns the first derivative with respect to @p v, `dP/dv`.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The velocity vector along @p v at `(u, v)`; the zero vector at a singular point.
     */
    [[nodiscard]] virtual Vector derivativeV(T u, T v) const = 0;

    /**
     * @brief Returns the first parameter of the natural domain along @p u.
     *
     * @return The lower bound of @p u; `-infinity` when unbounded in that direction.
     */
    [[nodiscard]] virtual T firstUParameter() const = 0;

    /**
     * @brief Returns the last parameter of the natural domain along @p u.
     *
     * @return The upper bound of @p u; `+infinity` when unbounded in that direction.
     */
    [[nodiscard]] virtual T lastUParameter() const = 0;

    /**
     * @brief Returns the first parameter of the natural domain along @p v.
     *
     * @return The lower bound of @p v; `-infinity` when unbounded in that direction.
     */
    [[nodiscard]] virtual T firstVParameter() const = 0;

    /**
     * @brief Returns the last parameter of the natural domain along @p v.
     *
     * @return The upper bound of @p v; `+infinity` when unbounded in that direction.
     */
    [[nodiscard]] virtual T lastVParameter() const = 0;

    /**
     * @brief Returns whether the surface meets itself along the @p u direction.
     *
     * @return True when `evaluate(firstUParameter(), v)` equals `evaluate(lastUParameter(), v)` for every v.
     */
    [[nodiscard]] virtual bool isUClosed() const = 0;

    /**
     * @brief Returns whether the surface meets itself along the @p v direction.
     *
     * @return True when `evaluate(u, firstVParameter())` equals `evaluate(u, lastVParameter())` for every u.
     */
    [[nodiscard]] virtual bool isVClosed() const = 0;

    /**
     * @brief Returns whether the surface repeats with a period along @p u.
     *
     * @return True when `evaluate(u + uPeriod(), v)` equals `evaluate(u, v)` for every u and v.
     */
    [[nodiscard]] virtual bool isUPeriodic() const = 0;

    /**
     * @brief Returns whether the surface repeats with a period along @p v.
     *
     * @return True when `evaluate(u, v + vPeriod())` equals `evaluate(u, v)` for every u and v.
     */
    [[nodiscard]] virtual bool isVPeriodic() const = 0;

    /**
     * @brief Returns the period along @p u, meaningful only when `isUPeriodic()` is true.
     *
     * @return The period in this surface's @p u units; 0 for a non-periodic direction.
     */
    [[nodiscard]] virtual T uPeriod() const = 0;

    /**
     * @brief Returns the period along @p v, meaningful only when `isVPeriodic()` is true.
     *
     * @return The period in this surface's @p v units; 0 for a non-periodic direction.
     */
    [[nodiscard]] virtual T vPeriod() const = 0;

    /**
     * @brief Returns the smoothness class inside the domain.
     *
     * @details
     * - This is the highest order of derivative continuous everywhere inside the parameter rectangle; the answer for an
     *   analytic surface is `Continuity::CN`.
     * - A singular point of the parameterisation (a pole of a sphere) is not a loss of continuity of the surface, and
     *   neither is a seam: seams are reported by `isUClosed()` / `isVClosed()`.
     *
     * @return The surface's continuity.
     */
    [[nodiscard]] virtual Continuity continuity() const = 0;

    /**
     * @brief Returns whether the surface is at least `C`-continuous of order @p order.
     *
     * @details
     * - This is a query over `continuity()`, not a second description of the surface, exactly as `Curve<T>::isCN()`
     *   is: one ladder, two ways of asking about it.
     * - A geometric continuity does not answer a derivative question: `G1` / `G2` are false for order 1 and above.
     *
     * @param order Derivative order being asked about.
     * @return True when every derivative up to @p order is continuous inside the domain.
     */
    [[nodiscard]] bool isCN(int order) const
    {
        if (order < 0) {
            return false;
        }

        if (order >= 4) {
            return continuity() == Continuity::CN;
        }

        const Continuity rungs[4] = { Continuity::C0, Continuity::C1, Continuity::C2, Continuity::C3 };
        return continuity() >= rungs[order];
    }
};

VN_GEOMETRY_NS_END
