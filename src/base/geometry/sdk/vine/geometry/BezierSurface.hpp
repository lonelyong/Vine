#pragma once

#include "geometry_global.hpp"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "BezierBlend.hpp"
#include "BoundedSurface.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A Bezier patch: a rectangular control net plus an optional weight per pole, parameterised on `[0, 1]^2`.
 *
 * @details
 * - The net is stored **row-major with `u` contiguous**: `poles[iu + u_pole_count * iv]`. One flat array and two
 *   counts, not a `vector<vector<Point>>`, so a row is a stride and a column is a stride the other way, without a
 *   second length convention per row.
 * - The degrees are derived from the counts (`pole_count - 1`), for the same reason `BezierCurve` does not store one:
 *   two homes for one fact can disagree.
 * - Both parameters are on `[0, 1]`, the patch is never periodic, it is `CN` inside that square, and it is closed in a
 *   direction exactly when its first and last net column (for `u`) or row (for `v`) coincide - a data test, because a
 *   clamped Bezier patch's boundary curves **are** those columns and rows.
 * - A non-empty `weights` array makes the patch rational; the array uses the same layout and must be either empty or
 *   one strictly positive weight per pole.
 * - Evaluation is de Casteljau twice: along `u` for every row of the net, then along `v` across the results. The
 *   rational form runs the same passes on the weighted net and on the weights, then divides - which is why the two
 *   never disagree about the parameterisation.
 *
 * @note
 * - Every query copies the arrays it blends, because each de Casteljau pass needs one scratch value per row.
 *
 * @warning
 * - The fields are public and nothing re-checks them: changing the counts, the net or the weights behind the patch's
 *   back makes `degreeU()` / `isValid()` describe data that no longer agrees.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BezierSurface : public BoundedSurface<T> {
  public:
    using value_type  = T;
    using Point       = vn::math::Point3<T>;
    using Vector      = vn::math::Vector3<T>;
    using PoleArray   = std::vector<Point>;
    using WeightArray = std::vector<T>;

  public:
    /// Constructs a patch without poles, which `isValid()` reports as unusable.
    BezierSurface() = default;

    /**
     * @brief Constructs a patch from a row-major control net and, optionally, one weight per pole.
     *
     * @param the_poles      Control net, `u` contiguous; at least a 2x2 net is needed for `isValid()`.
     * @param u_pole_count   Number of poles in the `u` direction.
     * @param v_pole_count   Number of poles in the `v` direction.
     * @param the_weights    One weight per pole in the same layout, or empty for a polynomial patch.
     */
    explicit BezierSurface(PoleArray the_poles, int u_pole_count, int v_pole_count, WeightArray the_weights = {})
      : poles(std::move(the_poles))
      , u_pole_count(u_pole_count)
      , v_pole_count(v_pole_count)
      , weights(std::move(the_weights))
    {}

  public:
    /**
     * @brief Returns the degree in @p u: one less than the number of net columns.
     *
     * @return `u_pole_count - 1`, or -1 when the net has no columns.
     */
    [[nodiscard]] int degreeU() const
    {
        return u_pole_count - 1;
    }

    /**
     * @brief Returns the degree in @p v: one less than the number of net rows.
     *
     * @return `v_pole_count - 1`, or -1 when the net has no rows.
     */
    [[nodiscard]] int degreeV() const
    {
        return v_pole_count - 1;
    }

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
        const std::size_t index = static_cast<std::size_t>(iu + u_pole_count * iv);
        return index < poles.size() ? poles[index] : poles.back();
    }

    /**
     * @brief Returns whether the net and the weights form a usable patch.
     *
     * @details
     * - Every weight must be strictly positive, for the reason `BezierCurve` states: the patch is a convex combination
     *   of its poles only when the weights are, and a non-positive weight makes the rational denominator vanish.
     *
     * @return True when the counts are at least 2x2, the net length matches them and the weights are either absent or
     *         one positive value per pole.
     */
    [[nodiscard]] bool isValid() const override
    {
        if (u_pole_count < 2 || v_pole_count < 2) {
            return false;
        }

        const std::size_t expected = static_cast<std::size_t>(u_pole_count) * static_cast<std::size_t>(v_pole_count);
        if (poles.size() != expected) {
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
     * @brief Returns whether the first and the last net column coincide.
     *
     * @details
     * - For a clamped Bezier patch the `u = 0` boundary curve is the Bezier curve of the first net column, so the two
     *   edges agree exactly when the columns do - poles and, for a rational patch, weights included.
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
     * @brief Returns true: a Bezier patch is never periodic, however its net is chosen.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isUPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns false: a Bezier patch never repeats along @p v.
     *
     * @return Always false.
     */
    [[nodiscard]] bool isVPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns the start of both parameter ranges.
     *
     * @return 0.
     */
    [[nodiscard]] T firstUParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the @p u range.
     *
     * @return 1.
     */
    [[nodiscard]] T lastUParameter() const override
    {
        return T(1);
    }

    /**
     * @brief Returns the start of the @p v range.
     *
     * @return 0.
     */
    [[nodiscard]] T firstVParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the @p v range.
     *
     * @return 1.
     */
    [[nodiscard]] T lastVParameter() const override
    {
        return T(1);
    }

    /**
     * @brief Returns 0: a Bezier patch is not periodic in @p u.
     *
     * @return Always 0.
     */
    [[nodiscard]] T uPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns 0: a Bezier patch is not periodic in @p v.
     *
     * @return Always 0.
     */
    [[nodiscard]] T vPeriod() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a Bezier patch is one polynomial piece in each direction.
     *
     * @return `Continuity::CN`.
     */
    [[nodiscard]] Continuity continuity() const override
    {
        return Continuity::CN;
    }

    /**
     * @brief Returns the point at parameters @p u and @p v by de Casteljau in both directions.
     *
     * @details
     * - The parameters are used as given, so a value outside `[0, 1]` is the net's polynomial extension rather than an
     *   error - the same rule `BezierCurve` states.
     *
     * @param u First parameter in `[0, 1]` for the patch itself.
     * @param v Second parameter in `[0, 1]` for the patch itself.
     * @return The point on the patch; the first pole for a patch `isValid()` refuses.
     */
    [[nodiscard]] Point evaluate(T u, T v) const override
    {
        if (!isValid()) {
            return poles.empty() ? Point() : poles.front();
        }

        Point numerator;
        T     weight = T(1);
        evaluateNets(u, v, numerator, weight);

        if (!isRational() || std::abs(weight) <= vn::math::EPS<T>()) {
            return numerator;
        }

        return Point(numerator.x / weight, numerator.y / weight, numerator.z / weight);
    }

    /**
     * @brief Returns the first derivative with respect to @p u.
     *
     * @details
     * - The `u` derivative of the net is `degreeU` times the net of its `u` differences, so the same two de Casteljau
     *   passes answer it.
     * - A rational patch applies the quotient rule to the homogeneous sums: `P_u = (A_u - P * w_u) / w`.
     *
     * @param u First parameter.
     * @param v Second parameter.
     * @return The velocity vector along @p u; the zero vector for a patch `isValid()` refuses.
     */
    [[nodiscard]] Vector derivativeU(T u, T v) const override
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        Vector derivative;
        T      weight_derivative = T(0);
        derivativeNets(u, v, true, derivative, weight_derivative);

        if (!isRational()) {
            return derivative;
        }

        return rationalDerivative(u, v, derivative, weight_derivative);
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
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        Vector derivative;
        T      weight_derivative = T(0);
        derivativeNets(u, v, false, derivative, weight_derivative);

        if (!isRational()) {
            return derivative;
        }

        return rationalDerivative(u, v, derivative, weight_derivative);
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
     * @brief Returns whether two net positions hold the same homogeneous control point.
     *
     * @details
     * - Two columns define the same boundary curve only when their poles **and** their weights agree, so the comparison
     *   is made on `w * P` and on `w` rather than on the poles alone.
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
     * @brief Blends every `u` row of the net at @p u, leaving one value per row.
     *
     * @details
     * - The net is `u`-contiguous, so a row is a contiguous slice; a column is a stride. The first pass therefore runs
     *   along the contiguous direction, and the second pass across the `v_pole_count` results.
     *
     * @param values Flat net, `u` contiguous.
     * @param u      Parameter to blend at.
     * @return One blended value per row, in `v` order.
     */
    template <typename Value>
    [[nodiscard]] std::vector<Value> blendRows(const std::vector<Value>& values, T u) const
    {
        std::vector<Value> blended;
        blended.reserve(static_cast<std::size_t>(v_pole_count));

        for (int iv = 0; iv < v_pole_count; ++iv) {
            const auto first = values.begin() + static_cast<std::ptrdiff_t>(offset(0, iv));
            std::vector<Value> row(first, first + u_pole_count);
            blended.push_back(deCasteljau(std::move(row), u));
        }

        return blended;
    }

    /**
     * @brief Blends the `u` differences of every row at @p u, scaled by the degree.
     *
     * @param values Flat net, `u` contiguous.
     * @param u      Parameter to blend at.
     * @return One `u`-derivative value per row, in `v` order.
     */
    template <typename Value>
    [[nodiscard]] std::vector<DifferenceOf<Value>> blendRowDifferences(const std::vector<Value>& values, T u) const
    {
        std::vector<DifferenceOf<Value>> blended;
        blended.reserve(static_cast<std::size_t>(v_pole_count));

        for (int iv = 0; iv < v_pole_count; ++iv) {
            const auto first = values.begin() + static_cast<std::ptrdiff_t>(offset(0, iv));
            std::vector<Value> row(first, first + u_pole_count);
            blended.push_back(deCasteljau(differences(row), u) * T(degreeU()));
        }

        return blended;
    }

    /**
     * @brief Returns the weighted net `w * P` and the weight net `w` of the patch.
     *
     * @param weighted        Receives `w * P` in the net's layout.
     * @param weight_net      Receives the weights in the net's layout.
     */
    void weightedNets(std::vector<Point>& weighted, std::vector<T>& weight_net) const
    {
        weighted.assign(poles.size(), Point());
        weight_net.assign(poles.size(), T(1));
        if (!isRational()) {
            weighted = poles;
            return;
        }

        for (std::size_t index = 0; index < poles.size(); ++index) {
            const T weight = weights[index];
            weighted[index] = Point(poles[index].x * weight, poles[index].y * weight, poles[index].z * weight);
            weight_net[index] = weight;
        }
    }

    /**
     * @brief Returns the weighted numerator and the weight at `(u, v)`.
     *
     * @param u         First parameter.
     * @param v         Second parameter.
     * @param numerator Receives `A(u, v)`.
     * @param weight    Receives `w(u, v)`.
     */
    void evaluateNets(T u, T v, Point& numerator, T& weight) const
    {
        std::vector<Point> weighted;
        std::vector<T>     weight_net;
        weightedNets(weighted, weight_net);

        numerator = deCasteljau(blendRows(weighted, u), v);
        weight    = deCasteljau(blendRows(weight_net, u), v);
    }

    /**
     * @brief Returns the derivative of the weighted numerator and of the weight at `(u, v)`.
     *
     * @param u                   First parameter.
     * @param v                   Second parameter.
     * @param along_u             True for `d/du`, false for `d/dv`.
     * @param numerator_derivative Receives the derivative of `A`.
     * @param weight_derivative    Receives the derivative of `w`.
     */
    void derivativeNets(T u, T v, bool along_u, Vector& numerator_derivative, T& weight_derivative) const
    {
        std::vector<Point> weighted;
        std::vector<T>     weight_net;
        weightedNets(weighted, weight_net);

        if (along_u) {
            numerator_derivative = deCasteljau(blendRowDifferences(weighted, u), v);
            weight_derivative    = deCasteljau(blendRowDifferences(weight_net, u), v);
            return;
        }

        numerator_derivative = deCasteljau(differences(blendRows(weighted, u)), v) * T(degreeV());
        weight_derivative = deCasteljau(differences(blendRows(weight_net, u)), v) * T(degreeV());
    }

    /**
     * @brief Returns `(A' - P * w') / w` for a rational patch.
     *
     * @param u                First parameter.
     * @param v                Second parameter.
     * @param numerator_rate   Derivative of the weighted numerator at `(u, v)`.
     * @param weight_rate      Derivative of the weight at `(u, v)`.
     * @return The velocity vector; the zero vector when the weight vanished.
     */
    [[nodiscard]] Vector rationalDerivative(T u, T v, const Vector& numerator_rate, T weight_rate) const
    {
        Point numerator;
        T     weight = T(0);
        evaluateNets(u, v, numerator, weight);
        if (std::abs(weight) <= vn::math::EPS<T>()) {
            return Vector(T(0), T(0), T(0));
        }

        const Point point(numerator.x / weight, numerator.y / weight, numerator.z / weight);
        return (numerator_rate - point.toVector() * weight_rate) / weight;
    }

  public:
    /// Control net, row-major with `u` contiguous: `poles[iu + u_pole_count * iv]`.
    PoleArray poles;

    /// Number of poles in the `u` direction.
    int u_pole_count{ 0 };

    /// Number of poles in the `v` direction.
    int v_pole_count{ 0 };

    /// One weight per pole for a rational patch, empty for a polynomial one.
    WeightArray weights;
};

using BezierSurfacef = BezierSurface<float>;
using BezierSurfaced = BezierSurface<double>;

VN_GEOMETRY_NS_END
