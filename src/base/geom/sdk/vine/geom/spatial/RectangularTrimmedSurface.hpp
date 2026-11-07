#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <memory>
#include <utility>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "BoundedSurface.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief A sheet restricted to a parameter rectangle: a basis surface plus the four bounds.
 *
 * @details
 * - The trim is a view, not a copy: every numeric query forwards to the basis at the same parameters, so trimming
 *   costs one handle and four numbers. The basis is held by `std::shared_ptr<const Surface<T>>`, so it may be shared
 *   and may outlive a local - the same arrangement `TrimmedCurve` uses.
 * - Closure and periodicity belong to the basis **and** to the rectangle: a trim is closed in one direction only when
 *   the basis closes there, the rectangle covers that whole range **and** its span is a whole number of basis periods
 *   (otherwise the two boundary edges are different points of the basis); it is periodic only when the basis is
 *   periodic and the rectangle spans at least one period. Anything shorter is a patch with two edges in that
 *   direction.
 * - `evaluate()` forwards at any parameter, including one outside the rectangle: asking past an edge answers the basis,
 *   not a clamped edge point, because a trim restricts a range rather than cutting the sheet short.
 * - The four values are stored as given: a reversed rectangle is a caller error that `isValid()` reports rather than
 *   an interval this type silently reorders.
 *
 * @warning
 * - A null @p basis is a caller error that `isValid()` reports; a query asked anyway answers the degenerate value of
 *   its family (the origin point, a zero vector, `C0`, false) rather than dereferencing the handle, so the mistake
 *   stays a wrong answer instead of undefined behaviour.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class RectangularTrimmedSurface : public BoundedSurface<T> {
  public:
    using typename Surface<T>::Point;
    using typename Surface<T>::Vector;
    using BasisPtr = std::shared_ptr<const Surface<T>>;

  public:
    /**
     * @brief Constructs a rectangular trim over a basis surface.
     *
     * @param basis_surface The surface the rectangle is taken on; must not be null.
     * @param first_u       Lower bound of the @p u range.
     * @param last_u        Upper bound of the @p u range.
     * @param first_v       Lower bound of the @p v range.
     * @param last_v        Upper bound of the @p v range.
     */
    RectangularTrimmedSurface(BasisPtr basis_surface, T first_u, T last_u, T first_v, T last_v)
      : basis(std::move(basis_surface))
      , first_u(first_u)
      , last_u(last_u)
      , first_v(first_v)
      , last_v(last_v)
    {}

  public:
    /**
     * @brief Returns the point at parameters @p u and @p v by evaluating the basis surface.
     *
     * @param u First parameter, in the basis surface's units.
     * @param v Second parameter, in the basis surface's units.
     * @return The point on the basis surface; the basis' own answer also outside the rectangle, the origin without a
     *         basis.
     */
    [[nodiscard]] Point evaluate(T u, T v) const override
    {
        return basis != nullptr ? basis->evaluate(u, v) : Point();
    }

    /**
     * @brief Returns the basis' derivative with respect to @p u.
     *
     * @details
     * - Trimming a rectangle does not reparameterise: the piece keeps the basis' speeds, so the tangents are the
     *   basis' tangents at the same parameters, not rescaled ones.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The basis' velocity along @p u; the zero vector without a basis.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        return basis != nullptr ? basis->derivativeU(u, v) : Vector(T(0), T(0), T(0));
    }

    /**
     * @brief Returns the basis' derivative with respect to @p v.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The basis' velocity along @p v; the zero vector without a basis.
     */
    [[nodiscard]] Vector derivativeV(T u, T v) const override
    {
        return basis != nullptr ? basis->derivativeV(u, v) : Vector(T(0), T(0), T(0));
    }

    /**
     * @brief Returns the lower bound of the @p u range.
     *
     * @return The stored first @p u.
     */
    [[nodiscard]] T firstUParameter() const override
    {
        return first_u;
    }

    /**
     * @brief Returns the upper bound of the @p u range.
     *
     * @return The stored last @p u.
     */
    [[nodiscard]] T lastUParameter() const override
    {
        return last_u;
    }

    /**
     * @brief Returns the lower bound of the @p v range.
     *
     * @return The stored first @p v.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return first_v;
    }

    /**
     * @brief Returns the upper bound of the @p v range.
     *
     * @return The stored last @p v.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return last_v;
    }

    /**
     * @brief Returns whether the trim spans a whole closed basis along @p u and its two @p u edges coincide.
     *
     * @details
     * - Covering the basis' range is necessary but not sufficient: the two edges have to land on the same points.
     *   On a periodic basis that means the span is a whole number of periods, and then only a rectangle covering the
     *   natural range still qualifies. On a non-periodic basis there is no repetition to land on, so the rectangle
     *   must end on the basis' own two ends - an extrapolated patch ends somewhere else.
     * - The whole-turn test is done in turn units, so a non-positive basis period is refused rather than divided by.
     *
     * @return True when the basis closes along @p u, the rectangle covers its whole range and its two @p u ends land on
     *         the same points.
     */
    [[nodiscard]] bool isUClosed() const override
    {
        return basis != nullptr && basis->isUClosed() && spansWholeURange() && uEndsMeet();
    }

    /**
     * @brief Returns whether the trim spans a whole closed basis along @p v and its two @p v edges coincide.
     *
     * @details
     * - The rule is the one `isUClosed()` states, applied to the other direction: covering the range is necessary,
     *   and a periodic basis additionally needs a whole number of periods so the two edges land on the same points.
     *
     * @return True when the basis closes along @p v, the rectangle covers its whole range and its two @p v ends land on
     *         the same points.
     */
    [[nodiscard]] bool isVClosed() const override
    {
        return basis != nullptr && basis->isVClosed() && spansWholeVRange() && vEndsMeet();
    }

    /**
     * @brief Returns whether the trim spans at least one period of a periodic basis along @p u.
     *
     * @return True when the basis is periodic along @p u and the rectangle covers a period.
     */
    [[nodiscard]] bool isUPeriodic() const override
    {
        return basis != nullptr && basis->isUPeriodic() && last_u - first_u >= basis->uPeriod();
    }

    /**
     * @brief Returns whether the trim spans at least one period of a periodic basis along @p v.
     *
     * @return True when the basis is periodic along @p v and the rectangle covers a period.
     */
    [[nodiscard]] bool isVPeriodic() const override
    {
        return basis != nullptr && basis->isVPeriodic() && last_v - first_v >= basis->vPeriod();
    }

    /**
     * @brief Returns the period of this piece along @p u, or 0 when the piece itself does not repeat.
     *
     * @details
     * - The contract belongs to the interface: a rectangle narrower than a period is a patch with two edges, so it
     *   answers 0 even though its basis repeats. A caller that wants the basis' period asks the basis, which this type
     *   keeps public as `basis`.
     *
     * @return The basis' `uPeriod()` when `isUPeriodic()`, otherwise 0.
     */
    [[nodiscard]] T uPeriod() const override
    {
        return isUPeriodic() ? basis->uPeriod() : T(0);
    }

    /**
     * @brief Returns the period of this piece along @p v, or 0 when the piece itself does not repeat.
     *
     * @return The basis' `vPeriod()` when `isVPeriodic()`, otherwise 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return isVPeriodic() ? basis->vPeriod() : T(0);
    }

    /**
     * @brief Returns the basis' continuity: a trim removes a range, it does not smooth a crease.
     *
     * @return The basis surface's `continuity()`; `C0` without a basis, since nothing is known to be smoother.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis != nullptr ? basis->continuity() : Continuity::C0;
    }

    /**
     * @brief Returns whether the trim has a basis and two ordered ranges.
     *
     * @details
     * - Both things that can go wrong are caller input: a null basis leaves nothing to evaluate, and a reversed range
     *   (`first > last` in either direction) describes a different piece than the caller asked for.
     *
     * @return True when the basis is present and `first_u <= last_u`, `first_v <= last_v`.
     */
    [[nodiscard]] bool isValid() const override
    {
        return basis != nullptr && first_u <= last_u && first_v <= last_v;
    }

  private:
    /**
     * @brief Returns whether the rectangle covers the basis' whole @p u range.
     *
     * @return True when the stored bounds are at least as wide as the basis' own.
     */
    [[nodiscard]] bool spansWholeURange() const
    {
        return first_u <= basis->firstUParameter() && last_u >= basis->lastUParameter();
    }

    /**
     * @brief Returns whether the rectangle covers the basis' whole @p v range.
     *
     * @return True when the stored bounds are at least as wide as the basis' own.
     */
    [[nodiscard]] bool spansWholeVRange() const
    {
        return first_v <= basis->firstVParameter() && last_v >= basis->lastVParameter();
    }

    /**
     * @brief Returns whether the rectangle's two @p u ends land on the same points of the basis.
     *
     * @details
     * - A periodic basis repeats, so the ends meet exactly when the span is an integer number of periods, wherever the
     *   rectangle starts; a non-positive period cannot define a turn and answers false.
     * - A basis that does not repeat has nothing to land on beyond its own ends: only the natural range's two ends
     *   coincide, so the rectangle must use them (within the floating-point tolerance). An extrapolated patch ends on
     *   different points and answers false.
     * - The turn comparison is in turn units (span divided by period), which keeps the tolerance independent of how
     *   many turns the rectangle covers.
     *
     * @return True when the span is an integer number of periods on a periodic basis, or the bounds are the basis'
     *         own natural ends on a non-periodic one.
     */
    [[nodiscard]] bool uEndsMeet() const
    {
        if (!basis->isUPeriodic()) {
            return std::abs(first_u - basis->firstUParameter()) <= vn::math::EPS<T>()
                && std::abs(last_u - basis->lastUParameter()) <= vn::math::EPS<T>();
        }

        const T period = basis->uPeriod();
        if (!(period > vn::math::EPS<T>())) {
            return false;
        }

        const T turns = (last_u - first_u) / period;
        return std::abs(turns - std::round(turns)) <= vn::math::EPS<T>();
    }

    /**
     * @brief Returns whether the rectangle's two @p v ends land on the same points of the basis.
     *
     * @return True when the span is an integer number of periods on a periodic basis, or the bounds are the basis'
     *         own natural ends on a non-periodic one; false when a periodic basis has no usable period.
     */
    [[nodiscard]] bool vEndsMeet() const
    {
        if (!basis->isVPeriodic()) {
            return std::abs(first_v - basis->firstVParameter()) <= vn::math::EPS<T>()
                && std::abs(last_v - basis->lastVParameter()) <= vn::math::EPS<T>();
        }

        const T period = basis->vPeriod();
        if (!(period > vn::math::EPS<T>())) {
            return false;
        }

        const T turns = (last_v - first_v) / period;
        return std::abs(turns - std::round(turns)) <= vn::math::EPS<T>();
    }

  public:
    /// The surface the rectangle is taken on.
    BasisPtr basis;

    /// Lower bound of the @p u range.
    T first_u{ T(0) };

    /// Upper bound of the @p u range.
    T last_u{ T(0) };

    /// Lower bound of the @p v range.
    T first_v{ T(0) };

    /// Upper bound of the @p v range.
    T last_v{ T(0) };
};

using RectangularTrimmedSurfacef = RectangularTrimmedSurface<float>;
using RectangularTrimmedSurfaced = RectangularTrimmedSurface<double>;

VN_GEOM_SPATIAL_NS_END
