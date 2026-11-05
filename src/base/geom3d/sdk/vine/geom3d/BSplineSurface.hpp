#pragma once

#include "geom3d_global.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <vine/math/BSplineBasis.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "BoundedSurface.hpp"

VN_GEOM3D_NS_BEGIN

/**
 * @brief A B-spline patch: a control net, a degree and a knot vector per direction, and optional weights.
 *
 * @details
 * - The net is stored **row-major with `u` contiguous**: `poles[iu + u_pole_count * iv]`, the same layout
 *   `BezierSurface` uses, so the two patch types read alike.
 * - Each direction stores its **distinct knots and their multiplicities**, not the flattened vector: the multiplicity is
 *   what a caller asks about, and one copy of the knots cannot disagree with itself.
 * - `continuity()` is the worse of the two directions' `C(degree - largest internal multiplicity)`, which is the fact a
 *   B-spline patch is worth having as a type for.
 * - Closure is a **data** test here as well: with both ends clamped, the `u = 0` boundary curve is the curve of the
 *   first net column, so two edges agree exactly when their homogeneous columns do.
 * - Both parameters are clamped into their knot domains before evaluation, exactly as `BSplineCurve` does, and the
 *   basis functions come from `BSplineBasis.hpp`, which the curve uses too (C3).
 *
 * @note
 * - The clamped form is what `isValid()` accepts: both ends must repeat the knot `degree + 1` times.
 *
 * @warning
 * - The fields are public and nothing re-checks them: changing the counts, the net, the knots or the weights behind
 *   the patch's back makes `isValid()` and the degrees describe data that no longer agrees.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BSplineSurface : public BoundedSurface<T> {
  public:
    using value_type        = T;
    using Point             = vn::math::Point3<T>;
    using Vector            = vn::math::Vector3<T>;
    using PoleArray         = std::vector<Point>;
    using KnotArray         = std::vector<T>;
    using MultiplicityArray = std::vector<int>;
    using WeightArray       = std::vector<T>;

  public:
    /// Constructs a patch without data, which `isValid()` reports as unusable.
    BSplineSurface() = default;

    /**
     * @brief Constructs a B-spline patch from its net, degrees, knots and multiplicities.
     *
     * @param the_poles             Control net, `u` contiguous.
     * @param u_pole_count          Number of poles in the `u` direction.
     * @param v_pole_count          Number of poles in the `v` direction.
     * @param the_degree_u          Degree in `u`; at least 1 for `isValid()`.
     * @param the_degree_v          Degree in `v`; at least 1 for `isValid()`.
     * @param the_u_knots           Distinct knots in `u`, strictly increasing.
     * @param the_u_multiplicities  One multiplicity per distinct `u` knot: `degree_u + 1` at both ends, `1` to
     *                              `degree_u` inside.
     * @param the_v_knots           Distinct knots in `v`, strictly increasing.
     * @param the_v_multiplicities  One multiplicity per distinct `v` knot: `degree_v + 1` at both ends, `1` to
     *                              `degree_v` inside.
     * @param the_weights           One weight per pole in the net's layout, or empty for a polynomial patch.
     */
    BSplineSurface(PoleArray the_poles, int u_pole_count, int v_pole_count, int the_degree_u, int the_degree_v,
                   KnotArray the_u_knots, MultiplicityArray the_u_multiplicities, KnotArray the_v_knots,
                   MultiplicityArray the_v_multiplicities, WeightArray the_weights = {})
      : poles(std::move(the_poles))
      , u_pole_count(u_pole_count)
      , v_pole_count(v_pole_count)
      , degree_u(the_degree_u)
      , degree_v(the_degree_v)
      , u_knots(std::move(the_u_knots))
      , u_multiplicities(std::move(the_u_multiplicities))
      , v_knots(std::move(the_v_knots))
      , v_multiplicities(std::move(the_v_multiplicities))
      , weights(std::move(the_weights))
    {}

  public:
    /**
     * @brief Returns whether the patch carries weights.
     *
     * @return True when `weights` is non-empty.
     */
    [[nodiscard]] bool isRational() const
    {
        return !weights.empty();
    }

    /**
     * @brief Returns the pole at net position (`iu`, `iv`).
     *
     * @param iu Column index, `0` to `u_pole_count - 1`.
     * @param iv Row index, `0` to `v_pole_count - 1`.
     * @return The pole; the last pole for an index past the net.
     */
    [[nodiscard]] Point poleAt(int iu, int iv) const
    {
        if (poles.empty()) {
            return Point();
        }
        const std::size_t index = offset(iu, iv);
        return index < poles.size() ? poles[index] : poles.back();
    }

    /**
     * @brief Returns whether the net, the knots and the weights form a usable patch.
     *
     * @details
     * - Both directions must be in clamped form (`math::isClampedKnotVector()`), the net length must match the counts, and the
     *   weights must be absent or one strictly positive value per pole.
     *
     * @return True when every count agrees and both ends are clamped.
     */
    [[nodiscard]] bool isValid() const override
    {
        if (degree_u < 1 || degree_v < 1 || u_pole_count < 2 || v_pole_count < 2) {
            return false;
        }

        const std::size_t expected = static_cast<std::size_t>(u_pole_count) * static_cast<std::size_t>(v_pole_count);
        if (poles.size() != expected) {
            return false;
        }

        if (!math::isClampedKnotVector(std::span<const T>(u_knots), std::span<const int>(u_multiplicities), degree_u,
                                 static_cast<std::size_t>(u_pole_count))) {
            return false;
        }

        if (!math::isClampedKnotVector(std::span<const T>(v_knots), std::span<const int>(v_multiplicities), degree_v,
                                 static_cast<std::size_t>(v_pole_count))) {
            return false;
        }

        if (weights.empty()) {
            return true;
        }

        if (weights.size() != expected) {
            return false;
        }

        for (const T weight : weights) {
            if (!(weight > vn::math::EPS<T>())) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Returns the smoothness at the worst join of either direction.
     *
     * @details
     * - Each direction answers its own knot vector's fact through `math::knotContinuity()` in `BSplineBasis.hpp`, and the
     *   patch is as smooth as its worse direction.
     *
     * @return The continuity at the least smooth join of either direction.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        const Continuity along_u = math::knotContinuity(degree_u, std::span<const int>(u_multiplicities));
        const Continuity along_v = math::knotContinuity(degree_v, std::span<const int>(v_multiplicities));
        return along_u < along_v ? along_u : along_v;
    }

    /**
     * @brief Returns whether the first and the last net column coincide.
     *
     * @return True when the patch meets itself along `u`.
     */
    [[nodiscard]] bool isUClosed() const override
    {
        if (!isValid()) {
            return false;
        }

        for (int iv = 0; iv < v_pole_count; ++iv) {
            if (!samePole(0, iv, u_pole_count - 1, iv)) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Returns whether the first and the last net row coincide.
     *
     * @return True when the patch meets itself along `v`.
     */
    [[nodiscard]] bool isVClosed() const override
    {
        if (!isValid()) {
            return false;
        }

        for (int iu = 0; iu < u_pole_count; ++iu) {
            if (!samePole(iu, 0, iu, v_pole_count - 1)) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Returns false: this type describes the clamped, non-periodic form.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isUPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns false: this type describes the clamped, non-periodic form.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isVPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns the first distinct `u` knot: the lower end of the domain.
     *
     * @return `u_knots.front()`, or 0 when no knots are set.
     */
    [[nodiscard]] T firstUParameter() const override
    {
        return u_knots.empty() ? T(0) : u_knots.front();
    }

    /**
     * @brief Returns the last distinct `u` knot: the upper end of the domain.
     *
     * @return `u_knots.back()`, or 0 when no knots are set.
     */
    [[nodiscard]] T lastUParameter() const override
    {
        return u_knots.empty() ? T(0) : u_knots.back();
    }

    /**
     * @brief Returns the first distinct `v` knot: the lower end of the domain.
     *
     * @return `v_knots.front()`, or 0 when no knots are set.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return v_knots.empty() ? T(0) : v_knots.front();
    }

    /**
     * @brief Returns the last distinct `v` knot: the upper end of the domain.
     *
     * @return `v_knots.back()`, or 0 when no knots are set.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return v_knots.empty() ? T(0) : v_knots.back();
    }

    /**
     * @brief Returns 0: the clamped form is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T uPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns 0: the clamped form is not periodic.
     *
     * @return Always 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the point at parameters @p u and @p v by summing the basis functions of their spans.
     *
     * @details
     * - The two parameters are clamped into their knot domains first, for the reason `BSplineCurve` states: outside the
     *   domain a clamped patch has no polynomial span, so it continues as its boundary rather than extrapolating.
     * - A rational patch divides the weighted sum by the sum of the weights; both come from one pass.
     *
     * @param u First parameter, in the `u` knot domain.
     * @param v Second parameter, in the `v` knot domain.
     * @return The point on the patch; the first pole for a patch `isValid()` refuses.
     */
    [[nodiscard]] Point evaluate(T u, T v) const override
    {
        if (!isValid()) {
            return poles.empty() ? Point() : poles.front();
        }

        Vector numerator(T(0), T(0), T(0));
        T      weight = T(0);
        accumulate(clamped(u, true), clamped(v, false), 0, 0, numerator, weight);

        if (!isRational() || std::abs(weight) <= vn::math::EPS<T>()) {
            return Point(numerator.x, numerator.y, numerator.z);
        }

        return Point(numerator.x / weight, numerator.y / weight, numerator.z / weight);
    }

    /**
     * @brief Returns the first derivative with respect to @p u.
     *
     * @details
     * - The polynomial form differentiates the `u` basis functions and keeps the `v` ones: one tensor sum answers it.
     * - The rational form applies the quotient rule: `P_u = (A_u - P * w_u) / w`.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The velocity vector along @p u; the zero vector for a patch `isValid()` refuses.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        return directionalDerivative(u, v, true);
    }

    /**
     * @brief Returns the first derivative with respect to @p v.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The velocity vector along @p v; the zero vector for a patch `isValid()` refuses.
     */
    [[nodiscard]] Vector derivativeV(T u, T v) const override
    {
        return directionalDerivative(u, v, false);
    }

  private:
    /**
     * @brief Returns the flat index of the pole at net position (`iu`, `iv`).
     *
     * @param iu Column index.
     * @param iv Row index.
     * @return `iu + u_pole_count * iv`.
     */
    [[nodiscard]] std::size_t offset(int iu, int iv) const
    {
        return static_cast<std::size_t>(iu + u_pole_count * iv);
    }

    /**
     * @brief Returns @p value clamped into the knot domain of one direction.
     *
     * @param value    Parameter as the caller gave it.
     * @param along_u  True for the `u` direction, false for `v`.
     * @return The parameter inside that direction's domain.
     */
    [[nodiscard]] T clamped(T value, bool along_u) const
    {
        const T first = along_u ? firstUParameter() : firstVParameter();
        const T last  = along_u ? lastUParameter() : lastVParameter();
        return (value < first) ? first : ((value > last) ? last : value);
    }

    /**
     * @brief Returns whether two net positions hold the same homogeneous control point.
     *
     * @param iu_a Column of the first pole.
     * @param iv_a Row of the first pole.
     * @param iu_b Column of the second pole.
     * @param iv_b Row of the second pole.
     * @return True when the two homogeneous control points are equal within the floating-point epsilon.
     */
    [[nodiscard]] bool samePole(int iu_a, int iv_a, int iu_b, int iv_b) const
    {
        const std::size_t a = offset(iu_a, iv_a);
        const std::size_t b = offset(iu_b, iv_b);
        if (a >= poles.size() || b >= poles.size()) {
            return false;
        }

        const T weight_a = isRational() ? weights[a] : T(1);
        const T weight_b = isRational() ? weights[b] : T(1);
        if (std::abs(weight_a - weight_b) > vn::math::EPS<T>()) {
            return false;
        }

        return (poles[a] - poles[b]).length() <= vn::math::EPS<T>();
    }

    /**
     * @brief Sums `Nu_i^(du)(u) * Nv_j^(dv)(v) * P[ij]` and the same sum of weights over the two spans.
     *
     * @param u                First parameter, already clamped.
     * @param v                Second parameter, already clamped.
     * @param u_derivative     Derivative order to take of the `u` basis functions.
     * @param v_derivative     Derivative order to take of the `v` basis functions.
     * @param numerator        Receives the (weighted) sum of poles.
     * @param weight           Receives the sum of the (weighted) basis functions.
     */
    void accumulate(T u, T v, int u_derivative, int v_derivative, Vector& numerator, T& weight) const
    {
        numerator = Vector(T(0), T(0), T(0));
        weight    = T(0);

        const std::size_t u_span  = math::knotSpan<T>(u_knots, u_multiplicities, degree_u, u);
        const std::size_t v_span  = math::knotSpan<T>(v_knots, v_multiplicities, degree_v, v);
        const std::size_t u_first = u_span - static_cast<std::size_t>(degree_u);
        const std::size_t v_first = v_span - static_cast<std::size_t>(degree_v);

        for (std::size_t step_v = 0; step_v <= static_cast<std::size_t>(degree_v); ++step_v) {
            const std::size_t j = v_first + step_v;
            const T basis_v = math::basisValue<T>(v_knots, v_multiplicities, j, degree_v, v_derivative, v);

            Vector row(T(0), T(0), T(0));
            T      row_weight = T(0);
            for (std::size_t step_u = 0; step_u <= static_cast<std::size_t>(degree_u); ++step_u) {
                const std::size_t i      = u_first + step_u;
                const T           basis_u = math::basisValue<T>(u_knots, u_multiplicities, i, degree_u, u_derivative, u);
                const T           factor  = isRational() ? basis_u * weights[offset(static_cast<int>(i),
                                                                                   static_cast<int>(j))]
                                                         : basis_u;
                row += poles[offset(static_cast<int>(i), static_cast<int>(j))].toVector() * factor;
                row_weight += factor;
            }

            numerator += row * basis_v;
            weight += row_weight * basis_v;
        }
    }

    /**
     * @brief Returns the derivative along one parameter, applying the quotient rule for a rational patch.
     *
     * @param u        First parameter.
     * @param v        Second parameter.
     * @param along_u  True for `d/du`, false for `d/dv`.
     * @return The velocity vector; the zero vector for a patch `isValid()` refuses.
     */
    [[nodiscard]] Vector directionalDerivative(T u, T v, bool along_u) const
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        const T first = clamped(u, true);
        const T second = clamped(v, false);

        Vector derivative(T(0), T(0), T(0));
        T      weight_derivative = T(0);
        accumulate(first, second, along_u ? 1 : 0, along_u ? 0 : 1, derivative, weight_derivative);

        if (!isRational()) {
            return derivative;
        }

        Vector value(T(0), T(0), T(0));
        T      weight = T(0);
        accumulate(first, second, 0, 0, value, weight);
        if (std::abs(weight) <= vn::math::EPS<T>()) {
            return Vector(T(0), T(0), T(0));
        }

        return (derivative - (value / weight) * weight_derivative) / weight;
    }

  public:
    /// Control net, row-major with `u` contiguous: `poles[iu + u_pole_count * iv]`.
    PoleArray poles;

    /// Number of poles in the `u` direction.
    int u_pole_count{ 0 };

    /// Number of poles in the `v` direction.
    int v_pole_count{ 0 };

    /// Degree in the `u` direction.
    int degree_u{ 0 };

    /// Degree in the `v` direction.
    int degree_v{ 0 };

    /// Distinct knots in `u`, strictly increasing.
    KnotArray u_knots;

    /// Multiplicity of each distinct `u` knot, in the same order as `u_knots`.
    MultiplicityArray u_multiplicities;

    /// Distinct knots in `v`, strictly increasing.
    KnotArray v_knots;

    /// Multiplicity of each distinct `v` knot, in the same order as `v_knots`.
    MultiplicityArray v_multiplicities;

    /// One weight per pole for a rational patch, empty for a polynomial one.
    WeightArray weights;
};

using BSplineSurfacef = BSplineSurface<float>;
using BSplineSurfaced = BSplineSurface<double>;

VN_GEOM3D_NS_END
