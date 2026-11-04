#pragma once

#include "geometry_global.hpp"

#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "BoundedCurve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A Bezier curve: one pole per control point, plus an optional weight per pole, parameterised on [0, 1].
 *
 * @details
 * - The data model **is** the interface here: a Bezier curve is its poles, so `degree()` is derived from them and no
 *   separate degree is stored that could disagree with the pole count. Non-empty `weights` make it rational.
 * - Everything that is a fact about the *type* rather than a computation is answered already: the domain is `[0, 1]`,
 *   it is never periodic, it is `CN` inside that domain, it is closed when the first and last pole coincide, and the
 *   reversed parameter is `1 - u`.
 * - **The three numeric answers are not implemented**: `evaluate()`, `derivative()` and `secondDerivative()` (the
 *   de Casteljau passes) are still pure, so the type cannot be constructed. Defining the data model first keeps the
 *   contract - which is what callers depend on - separate from the one algorithm that is still missing.
 * - This is the first curve that owns memory: `std::vector` poles mean deep copies, not a trivially copyable value.
 *
 * @warning
 * - The fields are public and nothing re-checks them on write: changing `poles` or `weights` behind the curve's back
 *   makes `degree()` and `isRational()` describe the new data while the invariants `isValid()` states may no longer
 *   hold.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BezierCurve : public BoundedCurve<T> {
  public:
    using value_type = T;
    using Point      = vn::math::Point3<T>;
    using Vector     = vn::math::Vector3<T>;
    using PoleArray  = std::vector<Point>;
    using WeightArray = std::vector<T>;

  public:
    /// Constructs the curve without poles, which `isValid()` reports as unusable.
    BezierCurve() = default;

    /**
     * @brief Constructs a Bezier curve from its poles and, optionally, one weight per pole.
     *
     * @param the_poles   Poles in order; at least two are needed for `isValid()`.
     * @param the_weights One weight per pole, or empty for a polynomial (non-rational) curve.
     */
    explicit BezierCurve(PoleArray the_poles, WeightArray the_weights = {})
      : poles(std::move(the_poles))
      , weights(std::move(the_weights))
    {}

  public:
    /**
     * @brief Returns the polynomial degree: one less than the number of poles.
     *
     * @details
     * - Derived rather than stored: a stored degree and a pole array are two homes for the same fact, and only one of
     *   them can be authoritative.
     *
     * @return `poles.size() - 1`, or -1 when the curve has no poles.
     */
    [[nodiscard]] int degree() const
    {
        return static_cast<int>(poles.size()) - 1;
    }

    /**
     * @brief Returns whether the curve carries weights.
     *
     * @return True when `weights` is non-empty.
     */
    [[nodiscard]] bool isRational() const
    {
        return !weights.empty();
    }

    /**
     * @brief Returns whether the poles and weights form a usable curve.
     *
     * @return True when there are at least two poles and the weights are either empty or one per pole.
     */
    [[nodiscard]] bool isValid() const
    {
        return poles.size() >= 2 && (weights.empty() || weights.size() == poles.size());
    }

    /**
     * @brief Returns whether the first and last pole coincide.
     *
     * @details
     * - A Bezier curve is closed exactly when it ends where it started: the endpoints are the end poles, so this is a
     *   data test, not an evaluation.
     *
     * @return True when the two end poles are closer than the floating-point epsilon.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return poles.size() >= 2 && (poles.front() - poles.back()).length() <= vn::math::EPS<T>();
    }

    /**
     * @brief Returns the start of the natural range.
     *
     * @return 0.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the natural range.
     *
     * @return 1.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return T(1);
    }

    /**
     * @brief Returns false: a Bezier curve is never periodic, however its poles are chosen.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a Bezier curve is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a Bezier curve is a single polynomial piece.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns `1 - u`: the same point reached from the other end of the domain.
     *
     * @param u Parameter in `[0, 1]`.
     * @return `1 - u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return T(1) - u;
    }

  public:
    /// Poles in order; the endpoints of the curve are the first and last of them.
    PoleArray poles;

    /// One weight per pole for a rational curve, empty for a polynomial one.
    WeightArray weights;
};

using BezierCurvef = BezierCurve<float>;
using BezierCurved = BezierCurve<double>;

VN_GEOMETRY_NS_END
