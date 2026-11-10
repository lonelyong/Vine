#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <vine/geom/detail/CurveSubdivision.hpp>
#include <vine/geom/detail/CurveTraits.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point2.hpp>
#include <vine/math/Types.hpp>

#include "Curve.hpp"

VN_GEOM_PLANAR_NS_BEGIN

/**
 * @brief A planar curve's discretisation: the points, and the parameter that produced each one.
 *
 * @details
 * - The two arrays are the same answer in two forms: the point a consumer draws or collides with, and the parameter that
 *   produced it. The parameter cannot be recovered from the point, so both are kept.
 * - `points` and `parameters` are parallel and strictly increasing in parameter; they are separate arrays rather than one
 *   array of pairs because positions are what a consumer reads consecutively.
 * - The value is owned by the sampler that produced it (`polyline()`), so a caller that keeps it past the sampler moves or
 *   copies it.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
struct CurvePolyline {
    /// The points, in parameter order, starting at the window's first parameter.
    std::vector<vn::math::Point2<T>> points;

    /// The parameter of each point, in the curve's own units.
    std::vector<T> parameters;
};

using CurvePolylinef = CurvePolyline<float>;
using CurvePolylined = CurvePolyline<double>;

/**
 * @brief Turns one planar curve into a polyline: bind the curve, bind the window, compute, read the result.
 *
 * @details
 * - This is the same contract as the spatial sampler, one space down: the base answers what every rule can answer - the
 *   points and their count - and each subclass carries its own parameters.
 * - `compute()` takes no arguments because the curve and the window are already bound, so a caller walking many curves
 *   configures once and only rebinds the curve per run.
 * - Every setter and every failed `compute()` clears the cached result, so `polyline()` is either the result of the curve
 *   bound last or empty - never the previous curve's points.
 * - Adding a derived reading (per-chord length, the deviation actually reached) is a new accessor on the subclass; no
 *   existing signature changes.
 *
 * @warning
 * - `curve()` stores a reference: the curve must outlive the sampler, which neither copies it nor keeps it alive.
 * - A successful `compute()` freezes the result; editing the curve afterwards does not update `polyline()`. Call
 *   `compute()` again when the curve or the window changed.
 * - A missing or unusable curve, a window that is not finite and forward, an out-of-range rule parameter and an exhausted
 *   point budget all answer the same `false`: check the configuration before blaming the curve.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
class CurveSampler {
  public:
    CurveSampler() = default;
    virtual ~CurveSampler() = default;

  public:
    /**
     * @brief Binds the curve to sample and clears the previous result.
     *
     * @param curve The curve to sample; it must outlive this sampler.
     */
    void curve(const Curve<T>& curve)
    {
        curve_ = &curve;
        clearResult();
    }

    /**
     * @brief Restricts the parameter window to walk and clears the previous result.
     *
     * @details
     * - Not calling it means the curve's own domain, the same omission `arcLength()` accepts.
     * - The window is resolved on `compute()`, not here: a reversed or non-finite window is refused by that run.
     *
     * @param first Lower parameter of the window.
     * @param last Upper parameter of the window; must not precede @p first.
     */
    void range(T first, T last)
    {
        range_ = std::pair<T, T>{ first, last };
        clearResult();
    }

    /**
     * @brief Runs the rule over the bound curve and caches the polyline.
     *
     * @details
     * - The result is cleared first, so a failure can never leave the previous run's points behind.
     * - Unbounded curves (a line, a hyperbola, a parabola) must have been narrowed with `range()`; this never invents a
     *   window.
     *
     * @return True when `polyline()` holds this run's points; false when anything above refused the run.
     */
    [[nodiscard]] bool compute()
    {
        clearResult();

        if (curve_ == nullptr || !curve_->isValid()) {
            return false;
        }

        const auto window = geom::detail::resolveCurveWindow<T>(*curve_, range_);
        if (!window) {
            return false;
        }

        if (!computeImpl(window->first, window->second)) {
            clearResult();
            return false;
        }

        return true;
    }

    /**
     * @brief Returns the polyline the last successful `compute()` produced.
     *
     * @return The points and their parameters; empty before the first successful run and after a failure.
     */
    [[nodiscard]] const CurvePolyline<T>& polyline() const noexcept
    {
        return result_;
    }

    /**
     * @brief Returns how many points the last successful `compute()` produced.
     *
     * @return The point count; 0 before the first successful run and after a failure.
     */
    [[nodiscard]] std::size_t pointCount() const noexcept
    {
        return result_.points.size();
    }

  protected:
    /**
     * @brief Runs the rule over an already resolved window, filling `resultBuffer()`.
     *
     * @details
     * - The base has checked the curve and the window by the time this is called; a subclass only has to judge its own
     *   parameters and fill the buffer. Answering false refuses the whole run, and the base clears the buffer.
     *
     * @param first Lower parameter of the resolved window.
     * @param last Upper parameter of the resolved window.
     * @return True when the buffer holds a complete polyline.
     */
    [[nodiscard]] virtual bool computeImpl(T first, T last) = 0;

    /**
     * @brief Returns the curve bound by `curve()`; only valid inside `computeImpl()`.
     *
     * @return The bound curve.
     */
    [[nodiscard]] const Curve<T>& curveRef() const noexcept
    {
        return *curve_;
    }

    /**
     * @brief Returns the buffer `computeImpl()` fills; empty at the start of every run.
     *
     * @return The result buffer.
     */
    [[nodiscard]] CurvePolyline<T>& resultBuffer() noexcept
    {
        return result_;
    }

    /**
     * @brief Drops the cached result; every setter calls this so a stale polyline cannot be read back.
     */
    void clearResult() noexcept
    {
        result_.points.clear();
        result_.parameters.clear();
    }

  private:
    const Curve<T>*                curve_{ nullptr };
    std::optional<std::pair<T, T>> range_{ std::nullopt };
    CurvePolyline<T>               result_{};
};

/**
 * @brief Cuts the window into a given number of chords, evenly spaced in the parameter.
 *
 * @details
 * - The point count is `segments + 1` and every chord covers the same parameter interval, so the points bunch up where
 *   the curve moves slowly and spread out where it moves fast.
 * - This is the cheapest rule and the only one that does not integrate the curve: one `evaluate()` per point.
 *
 * @warning
 * - Equal parameter does not mean equal arc length. Use `UniformLengthSampler` when the spacing has to be measured along
 *   the curve.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
class UniformParameterSampler final : public CurveSampler<T> {
  public:
    UniformParameterSampler() = default;

  public:
    /**
     * @brief Sets how many chords to produce and clears the previous result.
     *
     * @param segments Chord count; must be at least 1 for `compute()` to succeed.
     */
    void segments(int segments)
    {
        segments_ = segments;
        this->clearResult();
    }

    /**
     * @brief Returns the chord count this sampler will produce.
     *
     * @return The configured chord count.
     */
    [[nodiscard]] int segments() const noexcept
    {
        return segments_;
    }

  protected:
    [[nodiscard]] bool computeImpl(T first, T last) override
    {
        auto& result = this->resultBuffer();
        return geom::detail::sampleByParameter(this->curveRef(), first, last, segments_, result.parameters, result.points);
    }

  private:
    int segments_{ 8 };
};

/**
 * @brief Places points a fixed arc length apart along the curve.
 *
 * @details
 * - Each point is the answer of "walk `step` of length from the previous one", so the chords are equal in length rather
 *   than in parameter.
 * - The final chord is the remainder of the window and is therefore at most `step` long; a window no longer than `step`
 *   answers its two endpoints.
 *
 * @warning
 * - The cost is the point count times one local inverse solve, each of which integrates the speed several times.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
class UniformLengthSampler final : public CurveSampler<T> {
  public:
    UniformLengthSampler() = default;

  public:
    /**
     * @brief Sets the arc length between two consecutive points and clears the previous result.
     *
     * @param step Arc length per chord; must be positive and finite for `compute()` to succeed.
     */
    void step(T step)
    {
        step_ = step;
        this->clearResult();
    }

    /**
     * @brief Sets the absolute length error the inverse solves aim for and clears the previous result.
     *
     * @param tolerance Length tolerance; a non-positive value falls back to `EPS<T>()`.
     */
    void tolerance(T tolerance)
    {
        tolerance_ = tolerance;
        this->clearResult();
    }

    /**
     * @brief Sets how many points the run may produce and clears the previous result.
     *
     * @param max_points Point budget; must be at least 2. A run that would exceed it answers false.
     */
    void maxPoints(int max_points)
    {
        max_points_ = max_points;
        this->clearResult();
    }

    /**
     * @brief Returns the arc length this sampler places between points.
     *
     * @return The configured chord length.
     */
    [[nodiscard]] T step() const noexcept
    {
        return step_;
    }

    /**
     * @brief Returns the length tolerance the inverse solves aim for.
     *
     * @return The configured tolerance; 0 means `EPS<T>()`.
     */
    [[nodiscard]] T tolerance() const noexcept
    {
        return tolerance_;
    }

    /**
     * @brief Returns the point budget of one run.
     *
     * @return The configured maximum point count.
     */
    [[nodiscard]] int maxPoints() const noexcept
    {
        return max_points_;
    }

  protected:
    [[nodiscard]] bool computeImpl(T first, T last) override
    {
        auto& result = this->resultBuffer();
        return geom::detail::sampleByLength(this->curveRef(), first, last, step_, tolerance_, max_points_,
                                            result.parameters, result.points);
    }

  private:
    T   step_{ T(0) };
    T   tolerance_{ T(0) };
    int max_points_{ 100000 };
};

/**
 * @brief Places points so that no chord pulls away from the curve by more than a given amount.
 *
 * @details
 * - Every chord is accepted when the curve's point at the chord's parameter midpoint is within `deviation` of the chord's
 *   own midpoint; otherwise the chord is halved and both halves are judged the same way.
 * - The window is seeded with four chords before any test, because a single midpoint test cannot see an S-shaped curve
 *   whose midpoint lies on the chord joining its ends.
 * - The result is the coarsest polyline this rule can justify, not a uniform one: chords are long where the curve is
 *   straight and short where it bends.
 *
 * @warning
 * - `deviation` bounds the sagitta measured at each chord's parameter midpoint, not the segment's true maximum distance;
 *   a curve that bulges between midpoints can exceed it.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
class MaxDeflectionSampler final : public CurveSampler<T> {
  public:
    MaxDeflectionSampler() = default;

  public:
    /**
     * @brief Sets the largest sagitta a chord may have and clears the previous result.
     *
     * @param deviation Sagitta bound measured at the chord's parameter midpoint; must be positive for `compute()` to
     *                  succeed.
     */
    void deviation(T deviation)
    {
        deviation_ = deviation;
        this->clearResult();
    }

    /**
     * @brief Sets how many points the run may produce and clears the previous result.
     *
     * @param max_points Point budget; must be at least 2. A run that would exceed it answers false instead of returning a
     *                   coarser polyline.
     */
    void maxPoints(int max_points)
    {
        max_points_ = max_points;
        this->clearResult();
    }

    /**
     * @brief Returns the sagitta bound this sampler aims for.
     *
     * @return The configured deviation.
     */
    [[nodiscard]] T deviation() const noexcept
    {
        return deviation_;
    }

    /**
     * @brief Returns the point budget of one run.
     *
     * @return The configured maximum point count.
     */
    [[nodiscard]] int maxPoints() const noexcept
    {
        return max_points_;
    }

  protected:
    [[nodiscard]] bool computeImpl(T first, T last) override
    {
        auto& result = this->resultBuffer();
        return geom::detail::subdivideByDeflection(this->curveRef(), first, last, deviation_, max_points_,
                                                   result.parameters, result.points);
    }

  private:
    T   deviation_{ T(0) };
    int max_points_{ 100000 };
};

VN_GEOM_PLANAR_NS_END
