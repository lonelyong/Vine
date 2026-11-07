#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Rect2.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/geom/detail/BezierBlend.hpp>

#include "Curve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief A planar Bezier curve: its poles and, optionally, one weight per pole.
 *
 * @details
 * - The data model **is** the interface here: a Bezier curve is its poles, so `degree()` is derived from them and no
 *   separate degree is stored that could disagree with the pole count. Non-empty `weights` make it rational.
 * - The three numeric answers are one algorithm applied three times: de Casteljau on the poles, on their first
 *   differences and on their second differences, scaled by the degree and by `degree * (degree - 1)`. A rational curve
 *   runs the same passes on homogeneous poles and divides by the weight afterwards.
 * - The parameter domain is `[0, 1]`, the curve never repeats, and it is `CN` inside the domain: a polynomial has every
 *   derivative.
 * - A parameter outside `[0, 1]` is the polynomial's own extrapolation, not an error, because the blend is defined for
 *   every real parameter.
 * - This is the module's first curve that owns memory: `std::vector` poles mean deep copies, not a trivially copyable
 *   value (R2 records this exception).
 *
 * @warning
 * - The fields are public and nothing re-checks them on write: changing `poles` or `weights` behind the curve's back
 *   makes `degree()` and `isRational()` describe the new data while the invariants `isValid()` states may no longer
 *   hold. Build a new curve instead of editing this one.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
class BezierCurve : public Curve<T> {
  public:
    using value_type  = T;
    using Point       = vn::math::Point2<T>;
    using Vector      = vn::math::Vector2<T>;
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
     *   are positive, and that is what keeps the denominator away from zero inside `[0, 1]`.
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
     * @brief Returns the point at parameter @p u by de Casteljau's algorithm.
     *
     * @details
     * - The polynomial form blends the poles themselves; the rational form blends the homogeneous poles and divides the
     *   result by its weight, which is the same pass with one extra component.
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
            return geom::detail::deCasteljau(poles, u);
        }

        return homogeneousToPoint(geom::detail::deCasteljau(homogeneousPoles(), u));
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
            return Vector::zero();
        }

        const int order = degree();
        if (!isRational()) {
            return geom::detail::deCasteljau(geom::detail::differences(poles), u) * T(order);
        }

        const auto homogeneous = homogeneousPoles();
        const auto first       = geom::detail::differences(homogeneous);
        return quotientDerivative(geom::detail::deCasteljau(homogeneous, u),
                                  geom::detail::deCasteljau(first, u) * T(order));
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
            return Vector::zero();
        }

        const int order = degree();
        if (!isRational()) {
            const auto second = geom::detail::differences(geom::detail::differences(poles));
            if (second.empty()) {
                return Vector::zero();
            }
            return geom::detail::deCasteljau(second, u) * T(order * (order - 1));
        }

        const auto homogeneous  = homogeneousPoles();
        const auto first        = geom::detail::differences(homogeneous);
        const auto second       = geom::detail::differences(first);
        const auto acceleration = second.empty() ? Homogeneous{}
                                                 : geom::detail::deCasteljau(second, u) * T(order * (order - 1));
        return quotientSecondDerivative(geom::detail::deCasteljau(homogeneous, u),
                                        geom::detail::deCasteljau(first, u) * T(order), acceleration);
    }

    /**
     * @brief Returns the third derivative at @p u.
     *
     * @details
     * - The polynomial form is `n * (n - 1) * (n - 2)` times the Bezier curve of the third differences; a degree below
     *   three answers the zero vector.
     * - A rational curve answers `P''' = (A''' - 3 * P'' * w' - 3 * P' * w'' - P * w''') / w` on the homogeneous
     *   derivatives, which is Leibniz' rule applied to `A / w`.
     *
     * @param u Parameter in `[0, 1]`.
     * @return The jerk vector at @p u; the zero vector for an unusable curve.
     */
    [[nodiscard]] Vector thirdDerivative(T u) const
    {
        if (!isValid()) {
            return Vector::zero();
        }

        const int order = degree();
        const T   scale = T(order * (order - 1) * (order - 2));

        if (!isRational()) {
            const auto third = geom::detail::differences(geom::detail::differences(geom::detail::differences(poles)));
            if (third.empty()) {
                return Vector::zero();
            }
            return geom::detail::deCasteljau(third, u) * scale;
        }

        const auto homogeneous = homogeneousPoles();
        const auto first       = geom::detail::differences(homogeneous);
        const auto second      = geom::detail::differences(first);
        const auto third       = geom::detail::differences(second);
        const auto jerk        = third.empty() ? Homogeneous{} : geom::detail::deCasteljau(third, u) * scale;
        return quotientThirdDerivative(
            geom::detail::deCasteljau(homogeneous, u), geom::detail::deCasteljau(first, u) * T(order),
            second.empty() ? Homogeneous{} : geom::detail::deCasteljau(second, u) * T(order * (order - 1)), jerk);
    }

    /**
     * @brief Returns 3: the third differences answer the third derivative in closed form.
     *
     * @return 3.
     */
    [[nodiscard]] int maxDerivativeOrder() const override
    {
        return 3;
    }

    /**
     * @brief Returns the derivative of the requested order, including the third.
     *
     * @param u Parameter in this curve's own units.
     * @param order Derivative order, at least 1.
     * @return The derivative, or `std::nullopt` outside `[1, 3]`.
     */
    [[nodiscard]] std::optional<Vector> derivativeAt(T u, int order) const override
    {
        if (order == 3) {
            return thirdDerivative(u);
        }

        return Curve<T>::derivativeAt(u, order);
    }

    /**
     * @brief Returns the start of the parameter range.
     *
     * @return 0.
     */
    [[nodiscard]] T firstParameter() const override
    {
        return T(0);
    }

    /**
     * @brief Returns the end of the parameter range.
     *
     * @return 1.
     */
    [[nodiscard]] T lastParameter() const override
    {
        return T(1);
    }

    /**
     * @brief Returns whether the curve ends where it started.
     *
     * @details
     * - The endpoints of a Bezier curve are its end poles, so closure is a statement about the data, not about the
     *   shape in between.
     *
     * @return True when the first and last poles are closer than the floating-point epsilon.
     */
    [[nodiscard]] bool isClosed() const override
    {
        return poles.size() >= 2 && (poles.front() - poles.back()).length() <= vn::math::EPS<T>();
    }

    /**
     * @brief Returns false: a Bezier curve never repeats, however its poles are chosen.
     *
     * @return false always.
     */
    [[nodiscard]] bool isPeriodic() const override
    {
        return false;
    }

    /**
     * @brief Returns 0: a non-periodic curve has no period.
     *
     * @return 0.
     */
    [[nodiscard]] T period() const override
    {
        return T(0);
    }

    /**
     * @brief Returns `CN`: a polynomial has every derivative inside its domain.
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
     * @details
     * - The domain is `[0, 1]`, so the reflection is `1 - u`; it is an involution and swaps the two ends.
     * - The point it names is the one on the reversed curve, which is this curve with its poles in reverse order - it
     *   is not `evaluate(u)` itself.
     *
     * @param u Parameter in `[0, 1]`.
     * @return `1 - u`.
     */
    [[nodiscard]] T reversedParameter(T u) const override
    {
        return T(1) - u;
    }

    /**
     * @brief Returns the axis-aligned box around the poles.
     *
     * @details
     * - The box is the one spanned by the poles, not the tight box of the curve: a Bezier curve lies inside the convex
     *   hull of its poles, so the pole box contains the curve but is generally looser than its own `bounds()`.
     * - Making it tight needs the roots of each coordinate's derivative, which no consumer has asked for yet.
     *
     * @return Bounds of the poles; the degenerate box at the origin when the curve has none.
     */
    [[nodiscard]] vn::math::Rect2<T> bounds() const
    {
        if (poles.empty()) {
            return vn::math::Rect2<T>();
        }

        T xmin = poles.front().x;
        T xmax = poles.front().x;
        T ymin = poles.front().y;
        T ymax = poles.front().y;
        for (const Point& pole : poles) {
            xmin = std::min(xmin, pole.x);
            xmax = std::max(xmax, pole.x);
            ymin = std::min(ymin, pole.y);
            ymax = std::max(ymax, pole.y);
        }

        return vn::math::Rect2<T>(Point(xmin, ymin), Point(xmax, ymax));
    }

  private:
    /// One pole in homogeneous coordinates: the point scaled by its weight, plus the weight itself.
    struct Homogeneous {
        T x{ T(0) };
        T y{ T(0) };
        T w{ T(0) };

        [[nodiscard]] Homogeneous operator+(const Homogeneous& right) const
        {
            return { x + right.x, y + right.y, w + right.w };
        }

        [[nodiscard]] Homogeneous operator-(const Homogeneous& right) const
        {
            return { x - right.x, y - right.y, w - right.w };
        }

        [[nodiscard]] Homogeneous operator*(T scale) const
        {
            return { x * scale, y * scale, w * scale };
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
            homogeneous.push_back({ poles[index].x * weight, poles[index].y * weight, weight });
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
     * @return The point `(x, y) / w`, or `(x, y)` when `w` is at the floating-point epsilon.
     */
    [[nodiscard]] static Point homogeneousToPoint(const Homogeneous& value)
    {
        if (std::abs(value.w) <= vn::math::EPS<T>()) {
            return Point(value.x, value.y);
        }

        return Point(value.x / value.w, value.y / value.w);
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
            return Vector::zero();
        }

        const Point  point       = homogeneousToPoint(value);
        const Vector a_velocity(velocity.x, velocity.y);
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
            return Vector::zero();
        }

        const Point  point     = homogeneousToPoint(value);
        const Vector first     = quotientDerivative(value, velocity);
        const Vector numerator = Vector(acceleration.x, acceleration.y) - first * (T(2) * velocity.w)
                               - point.toVector() * acceleration.w;
        return numerator / value.w;
    }

    /**
     * @brief Returns `(A''' - 3 * P'' * w' - 3 * P' * w'' - P * w''') / w`, the third derivative of a rational curve.
     *
     * @param value        Homogeneous value `(A, w)` at the parameter.
     * @param velocity     Its first derivative `(A', w')` at the same parameter.
     * @param acceleration Its second derivative `(A'', w'')` at the same parameter.
     * @param jerk         Its third derivative `(A''', w''')` at the same parameter.
     * @return The jerk vector; the zero vector when the weight vanished.
     */
    [[nodiscard]] static Vector quotientThirdDerivative(const Homogeneous& value, const Homogeneous& velocity,
                                                        const Homogeneous& acceleration, const Homogeneous& jerk)
    {
        if (std::abs(value.w) <= vn::math::EPS<T>()) {
            return Vector::zero();
        }

        const Vector first     = quotientDerivative(value, velocity);
        const Vector second    = quotientSecondDerivative(value, velocity, acceleration);
        const Vector numerator = Vector(jerk.x, jerk.y) - second * (T(3) * velocity.w)
                               - first * (T(3) * acceleration.w)
                               - homogeneousToPoint(value).toVector() * jerk.w;
        return numerator / value.w;
    }

  public:
    /// Poles in order; the end points of the curve are the first and last of them.
    PoleArray poles;

    /// One weight per pole for a rational curve, empty for a polynomial one.
    WeightArray weights;
};

using BezierCurvef = BezierCurve<float>;
using BezierCurved = BezierCurve<double>;

VN_GEOM_PLANAR_NS_END
