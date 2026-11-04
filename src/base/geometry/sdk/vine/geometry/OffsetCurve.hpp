#pragma once

#include "geometry_global.hpp"

#include <memory>
#include <utility>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Curve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A curve held at a signed distance from a basis curve, in the plane the reference direction defines.
 *
 * @details
 * - The offset point is the basis point pushed along `normalize(reference_direction x tangent)`: the reference
 *   direction picks the plane, the basis' first derivative picks the side. That is why an offset curve needs three
 *   things - the basis, the distance and a reference direction - and why the distance alone is not enough.
 * - Every parameter-domain answer is the basis': the offset inherits the range, the period and the closure, because it
 *   is evaluated at the same parameter and is only displaced at right angles to the basis.
 * - **The numeric answers are not implemented**: `evaluate()`, `derivative()` and `secondDerivative()` are still
 *   pure, so the type cannot be constructed. They are the three that need the basis' own derivatives and the
 *   cross-product guard, that is the one piece of work left.
 *
 * @warning
 * - @p basis_curve must not be null: the forwarding queries and the future evaluation dereference it.
 * - The offset is only well defined where `normalize(reference_direction x tangent)` exists. Nothing here checks
 *   that, and the basis' continuity does not describe it, so a curve whose tangent becomes parallel to the reference
 *   direction has no offset there.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class OffsetCurve : public Curve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;
    using BasisPtr   = std::shared_ptr<const Curve<T>>;

  public:
    /**
     * @brief Constructs an offset curve from a basis curve, a signed distance and a reference direction.
     *
     * @param basis                     The curve being offset; must not be null.
     * @param offset_distance            Signed distance from the basis, positive towards
     *                                  `normalize(reference_direction x tangent)`.
     * @param the_reference_direction    Direction that, crossed with the basis' tangent, gives the offset direction;
     *                                  expected unit length.
     */
    OffsetCurve(BasisPtr basis, T offset_distance, const Vector& the_reference_direction)
      : basis_curve(std::move(basis))
      , offset(offset_distance)
      , reference_direction(the_reference_direction)
    {}

  public:
    /**
     * @brief Returns the first parameter of the basis curve.
     *
     * @return The basis' `firstParameter()`.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return basis_curve->firstParameter();
    }

    /**
     * @brief Returns the last parameter of the basis curve.
     *
     * @return The basis' `lastParameter()`.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return basis_curve->lastParameter();
    }

    /**
     * @brief Returns whether the offset closes, which it does exactly when the basis closes.
     *
     * @return The basis' `isClosed()`.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return basis_curve->isClosed();
    }

    /**
     * @brief Returns whether the offset repeats, which it does exactly when the basis repeats.
     *
     * @return The basis' `isPeriodic()`.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return basis_curve->isPeriodic();
    }

    /**
     * @brief Returns the basis period.
     *
     * @return The basis period, or 0 when the basis is not periodic.
     */
    [[nodiscard]] T period() const override
    {
        return basis_curve->isPeriodic() ? basis_curve->period() : T(0);
    }

    /**
     * @brief Returns the basis continuity.
     *
     * @details
     * - The offset cannot be smoother than what it is pushed along: it is a function of the basis' first derivative,
     *   so a `C0` basis gives no offset direction at all. The value is forwarded unchanged; the caveat above is the
     *   caller's to check, not something this class can answer from the basis alone.
     *
     * @return The basis' `continuity()`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return basis_curve->continuity();
    }

    /**
     * @brief Returns the basis' reversed parameter: offsetting does not reparameterise.
     *
     * @param u Parameter in the basis curve's units.
     * @return The basis' `reversedParameter(u)`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return basis_curve->reversedParameter(u);
    }

    /**
     * @brief Returns whether the basis and the reference direction are usable.
     *
     * @return True when the basis is not null and the reference direction is non-zero.
     */
    [[nodiscard]] bool isValid() const
    {
        return basis_curve != nullptr && reference_direction.length() > vn::math::EPS<T>();
    }

  public:
    /// The curve the distance is measured from.
    BasisPtr basis_curve;

    /// Signed distance from the basis curve.
    T offset{ T(0) };

    /// Direction that, crossed with the basis' tangent, gives the offset direction.
    Vector reference_direction;
};

using OffsetCurvef = OffsetCurve<float>;
using OffsetCurved = OffsetCurve<double>;

VN_GEOMETRY_NS_END
