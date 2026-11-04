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
#include "BoundedCurve.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A Bezier curve: one pole per control point, plus an optional weight per pole, parameterised on [0, 1].
 *
 * @details
 * - The data model **is** the interface here: a Bezier curve is its poles, so `degree()` is derived from them and no
 *   separate degree is stored that could disagree with the pole count. Non-empty `weights` make it rational.
 * - Everything that is a fact about the *type* rather than a computation is answered without evaluating anything: the
 *   domain is `[0, 1]`, it is never periodic, it is `CN` inside that domain, it is closed when the first and last pole
 *   coincide, and the reversed parameter is `1 - u`.
 * - The three numeric answers are one algorithm applied three times: de Casteljau on the poles, on their first
 *   differences and on their second differences, so `P' = n * D1(u)` and `P'' = n * (n - 1) * D2(u)` where `n` is the
 *   degree. A rational curve runs the same passes on homogeneous poles and divides by the weight afterwards.
 * - This is the first curve that owns memory: `std::vector` poles mean deep copies, not a trivially copyable value.
 *
 * @note
 * - Each numeric answer copies the array it blends, because de Casteljau needs one scratch value per pole: a query
 *   allocates a `std::vector` instead of keeping mutable scratch state, which would make the curve unsafe to share.
 *   The zero-allocation rule the cheaper value types follow does not apply to this memory-owning one.
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
    using value_type  = T;
    using Point       = vn::math::Point3<T>;
    using Vector      = vn::math::Vector3<T>;
    using PoleArray   = std::vector<Point>;
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
     * @details
     * - Every weight must be strictly positive: the curve is a convex combination of its poles only when the weights
     *   are, and a zero or negative weight makes the denominator of the rational form vanish inside `[0, 1]`.
     *
     * @return True when there are at least two poles and the weights are either empty or one positive weight per pole.
     */
    [[nodiscard]] bool isValid() const override
    {
        if (poles.size() < 2) {
            return false;
        }

        if (weights.empty()) {
            return true;
        }

        if (weights.size() != poles.size()) {
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

    /**
     * @brief Returns the point at parameter @p u by de Casteljau's algorithm.
     *
     * @details
     * - The polynomial form blends the poles themselves; the rational form blends the homogeneous poles and divides the
     *   result by its weight, which is the same pass with one extra component.
     * - @p u is used as given: a Bezier curve is a polynomial, so a parameter outside `[0, 1]` is a well-defined
     *   extrapolation of the same blending rather than an error.
     * - An unusable curve (see `isValid()`) answers its first pole instead of reading past `weights`, so a malformed
     *   curve never produces a NaN.
     *
     * @param u Parameter in `[0, 1]` for the curve itself; any value for its polynomial extension.
     * @return The point on the curve; the origin when the curve has no poles at all.
     */
    [[nodiscard]] Point evaluate(T u) const override
    {
        if (!isValid()) {
            return poles.empty() ? Point() : poles.front();
        }

        if (!isRational()) {
            return deCasteljau(poles, u);
        }

        return homogeneousToPoint(deCasteljau(homogeneousPoles(), u));
    }

    /**
     * @brief Returns the first derivative at @p u.
     *
     * @details
     * - The derivative of a Bezier curve is the Bezier curve of its `n` difference vectors `P[i + 1] - P[i]` scaled by
     *   the degree `n`, so the same de Casteljau pass answers it and no finite difference is involved.
     * - A rational curve applies the quotient rule to the homogeneous derivatives: `P' = (A' - P * w') / w`.
     *
     * @param u Parameter in `[0, 1]`.
     * @return The velocity vector at @p u; the zero vector for an unusable curve.
     */
    [[nodiscard]] Vector derivative(T u) const override
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        const int order = degree();
        if (!isRational()) {
            return deCasteljau(differences(poles), u) * T(order);
        }

        const auto homogeneous = homogeneousPoles();
        const auto first        = differences(homogeneous);
        return quotientDerivative(deCasteljau(homogeneous, u), deCasteljau(first, u) * T(order));
    }

    /**
     * @brief Returns the second derivative at @p u.
     *
     * @details
     * - The second derivative is `n * (n - 1)` times the Bezier curve of the second differences, which is again the
     *   same blend one level down; a degree below two therefore has a zero second derivative.
     * - A rational curve answers `P'' = (A'' - 2 * P' * w' - P * w'') / w` on the homogeneous derivatives.
     *
     * @param u Parameter in `[0, 1]`.
     * @return The acceleration vector at @p u; the zero vector for an unusable curve.
     */
    [[nodiscard]] Vector secondDerivative(T u) const override
    {
        if (!isValid()) {
            return Vector(T(0), T(0), T(0));
        }

        const int order = degree();
        if (!isRational()) {
            const auto second = differences(differences(poles));
            if (second.empty()) {
                return Vector(T(0), T(0), T(0));
            }
            return deCasteljau(second, u) * T(order * (order - 1));
        }

        const auto homogeneous  = homogeneousPoles();
        const auto first        = differences(homogeneous);
        const auto second       = differences(first);
        const auto acceleration = second.empty() ? Homogeneous{}
                                                : deCasteljau(second, u) * T(order * (order - 1));
        return quotientSecondDerivative(deCasteljau(homogeneous, u), deCasteljau(first, u) * T(order),
                                        acceleration);
    }

  private:
    /// One pole in homogeneous coordinates: the point scaled by its weight, plus the weight itself.
    struct Homogeneous {
        T x{ T(0) };
        T y{ T(0) };
        T z{ T(0) };
        T w{ T(0) };

        [[nodiscard]] Homogeneous operator+(const Homogeneous& right) const
        {
            return { x + right.x, y + right.y, z + right.z, w + right.w };
        }

        [[nodiscard]] Homogeneous operator-(const Homogeneous& right) const
        {
            return { x - right.x, y - right.y, z - right.z, w - right.w };
        }

        [[nodiscard]] Homogeneous operator*(T scale) const
        {
            return { x * scale, y * scale, z * scale, w * scale };
        }
    };

    /**
     * @brief Returns the poles in homogeneous coordinates, widening a polynomial curve to weight 1.
     *
     * @details
     * - Only called on a curve `isValid()` accepts, which is what guarantees `weights` has one entry per pole.
     *
     * @return One homogeneous pole per pole.
     */
    [[nodiscard]] std::vector<Homogeneous> homogeneousPoles() const
    {
        std::vector<Homogeneous> homogeneous;
        homogeneous.reserve(poles.size());
        for (std::size_t index = 0; index < poles.size(); ++index) {
            const T weight = isRational() ? weights[index] : T(1);
            homogeneous.push_back(
                { poles[index].x * weight, poles[index].y * weight, poles[index].z * weight, weight });
        }

        return homogeneous;
    }

    /**
     * @brief Returns the point a homogeneous value stands for, treating a vanished weight as 1.
     *
     * @details
     * - A valid rational curve keeps its denominator away from zero inside `[0, 1]`, so the fallback only covers the
     *   polynomial extension of a rational curve; it keeps the answer finite instead of producing an infinity.
     *
     * @param value Homogeneous value from a de Casteljau pass.
     * @return The point `(x, y, z) / w`, or `(x, y, z)` when `w` is at the floating-point epsilon.
     */
    [[nodiscard]] static Point homogeneousToPoint(const Homogeneous& value)
    {
        if (std::abs(value.w) <= vn::math::EPS<T>()) {
            return Point(value.x, value.y, value.z);
        }

        return Point(value.x / value.w, value.y / value.w, value.z / value.w);
    }

    /**
     * @brief Returns `(A' - P * w') / w`, the derivative of a rational curve at one parameter.
     *
     * @param value    Homogeneous value `(A, w)` at the parameter.
     * @param velocity Its first derivative `(A', w')` at the same parameter.
     * @return The velocity vector; the zero vector when the weight vanished.
     */
    [[nodiscard]] static Vector quotientDerivative(const Homogeneous& value, const Homogeneous& velocity)
    {
        if (std::abs(value.w) <= vn::math::EPS<T>()) {
            return Vector(T(0), T(0), T(0));
        }

        const Point  point    = homogeneousToPoint(value);
        const Vector a_velocity(velocity.x, velocity.y, velocity.z);
        return (a_velocity - point.toVector() * velocity.w) / value.w;
    }

    /**
     * @brief Returns `(A'' - 2 * P' * w' - P * w'') / w`, the second derivative of a rational curve.
     *
     * @param value        Homogeneous value `(A, w)` at the parameter.
     * @param velocity     Its first derivative `(A', w')` at the same parameter.
     * @param acceleration Its second derivative `(A'', w'')` at the same parameter.
     * @return The acceleration vector; the zero vector when the weight vanished.
     */
    [[nodiscard]] static Vector quotientSecondDerivative(const Homogeneous& value, const Homogeneous& velocity,
                                                         const Homogeneous& acceleration)
    {
        if (std::abs(value.w) <= vn::math::EPS<T>()) {
            return Vector(T(0), T(0), T(0));
        }

        const Point  point     = homogeneousToPoint(value);
        const Vector first     = quotientDerivative(value, velocity);
        const Vector numerator = Vector(acceleration.x, acceleration.y, acceleration.z) - first * (T(2) * velocity.w)
                               - point.toVector() * acceleration.w;
        return numerator / value.w;
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
