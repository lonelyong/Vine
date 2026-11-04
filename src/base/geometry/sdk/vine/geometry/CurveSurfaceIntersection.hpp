#pragma once

#include "geometry_global.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/RootFinding.hpp>
#include <vine/math/Types.hpp>

#include "Curve.hpp"
#include "Surface.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief What the intersection of a curve with a surface turned out to be.
 *
 * @details
 * - `Isolated` and `Coincident` are different answers, not different amounts of the same one: a curve lying *in* a
 *   plane or on a sphere's great circle shares infinitely many points with the surface, and listing them would be both
 *   wrong and unbounded, so the stretch is reported as one fact instead.
 * - A curve that crosses the surface an even number of times reports `Isolated` with an even number of hits; `None` is
 *   only "no crossing was found", which includes the grazing case (`CurveSurfaceIntersection.hpp` P1).
 */
enum class CurveSurfaceContact : std::uint8_t {
    None,      ///< No point of the curve lies on the surface, or nothing usable was handed in.
    Isolated,  ///< Finitely many crossings; `hits` holds them in ascending curve parameter.
    Coincident ///< A stretch of the curve lies on the surface, so the intersection is not a point set.
};

/**
 * @brief One crossing of a curve through a surface.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
struct CurveSurfaceHit {
    /// Curve parameter of the crossing, in the curve's own units.
    T curve_parameter{ T(0) };

    /// The point on the surface; equal to `curve.evaluate(curve_parameter)`.
    vn::math::Point3<T> point;
};

/**
 * @brief The result of intersecting a curve with a surface.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
struct CurveSurfaceIntersection {
    /// Which of the three answers this is.
    CurveSurfaceContact contact{ CurveSurfaceContact::None };

    /// The crossings, ascending, one per crossing; empty unless `contact` is `Isolated`.
    std::vector<CurveSurfaceHit<T>> hits;
};

/**
 * @brief Returns the crossings of a curve with an analytic surface, by root-finding its signed distance along the curve.
 *
 * @details
 * - The search samples `f(u) = signedValue(surface, curve.evaluate(u))` across the parameter window, keeps the samples
 *   that are already on the surface, and narrows every sign change with `vn::math::refineRoot()`. Because `f` is a
 *   signed distance, a root is exactly a crossing - the surface's own implicit form does the work, so this works for
 *   any curve and any of the five analytic surfaces that provide a `signedValue()` overload.
 * - `contact` is `Coincident` when three consecutive samples are on the surface: that is the signature of a curve
 *   running *along* the surface (a line in a plane, a circle on a sphere), where the crossings are not a point set. In
 *   that case `hits` is cleared - listing an unbounded number of them would be the wrong answer.
 * - Duplicates are removed by point distance, which also collapses the two ends of a periodic curve onto one crossing.
 * - A parameter tolerance is derived from the geometric one round by round (`tolerance / |dP/du|`), because a distance
 *   and a parameter only coincide for an arc-length parameterisation. Where the curve stands still (a cone's apex, a
 *   sphere's pole) the fallback is a fraction of the sampling step.
 * - Nothing is transformed: the curve and the surface must live in the same space.
 *
 * @param curve          Curve to walk; must be usable (`isValid()`).
 * @param surface        Analytic surface to cross; must be usable and provide `signedValue()`.
 * @param window         Parameter window to search. Omitted for a bounded curve, which searches its own domain;
 *                       **required** for an unbounded one (a line, a hyperbola, a parabola), whose domain is infinite.
 * @param tolerance      Geometric tolerance: how close to the surface a point still counts as on it.
 * @param interval_count Number of scan intervals; more finds finer features and costs more evaluations.
 * @return The crossings, or `contact == None` when the inputs are unusable, the window is missing or infinite, or no
 *         crossing was found.
 *
 * @warning
 * - A **tangency** (the curve touching the surface without crossing it) does not change the sign of `f` and is therefore
 *   reported as `contact == None`. This is deliberate and documented in `geometry-intersection.md` P4; a caller that
 *   needs grazing contacts has to search for `f`'s extrema, which is a different algorithm.
 *
 * @note
 * - `Coefficient` of the cost: `interval_count + 1` evaluations of the curve and the surface, plus the bisection steps
 *   per crossing. Sampling is uniform, so a curve that oscillates faster than the scan can resolve will alias - raise
 *   `interval_count` for those.
 *
 * @tparam T           Floating-point scalar, deduced from @p curve.
 * @tparam SurfaceLike Concrete analytic surface type providing `signedValue(surface, point)`.
 */
template <vn::math::FP T, typename SurfaceLike>
[[nodiscard]] CurveSurfaceIntersection<T> intersectCurveWithSurface(
    const Curve<T>& curve, const SurfaceLike& surface, std::optional<std::pair<T, T>> window = std::nullopt,
    T tolerance = vn::math::EPS<T>(), int interval_count = 64)
{
    CurveSurfaceIntersection<T> result;
    if (!curve.isValid() || !surface.isValid()) {
        return result;
    }

    T first = curve.firstParameter();
    T last  = curve.lastParameter();
    if (window) {
        first = window->first;
        last  = window->second;
    }
    if (!std::isfinite(first) || !std::isfinite(last) || !(last > first)) {
        return result;
    }

    const T value_tolerance = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    const T on_surface      = value_tolerance;
    const int steps         = interval_count > 3 ? interval_count : 4;
    const T   step          = (last - first) / static_cast<T>(steps);

    const auto value_at = [&](T parameter) { return signedValue(surface, curve.evaluate(parameter)); };
    const auto record   = [&](T parameter) {
        const auto point = curve.evaluate(parameter);
        for (const auto& hit : result.hits) {
            if ((hit.point - point).length() <= on_surface) {
                return;
            }
        }
        result.hits.push_back(CurveSurfaceHit<T>{ parameter, point });
    };

    T previous_parameter = first;
    T previous_value     = value_at(first);
    int near_surface_run = vn::math::isZero(previous_value, on_surface) ? 1 : 0;
    if (near_surface_run > 0) {
        record(first);
    }

    for (int index = 1; index <= steps; ++index) {
        const T parameter = (index == steps) ? last : first + step * static_cast<T>(index);
        const T value     = value_at(parameter);

        near_surface_run = vn::math::isZero(value, on_surface) ? near_surface_run + 1 : 0;
        if (near_surface_run >= 3) {
            result.contact = CurveSurfaceContact::Coincident;
            result.hits.clear();
            return result;
        }

        const bool crossed
            = (previous_value < T(0) && value > T(0)) || (previous_value > T(0) && value < T(0));
        if (crossed) {
            const T middle = previous_parameter + (parameter - previous_parameter) * T(0.5);
            const T speed  = curve.derivative(middle).length();
            const T parameter_tolerance
                = speed > vn::math::EPS<T>() ? value_tolerance / speed : step * T(1e-6);
            const auto root = vn::math::refineRoot<T>(value_at, previous_parameter, parameter,
                                                      parameter_tolerance > T(0) ? parameter_tolerance
                                                                                 : step * T(1e-6));
            if (root) {
                record(*root);
            }
        } else if (vn::math::isZero(value, on_surface)) {
            record(parameter);
        }

        previous_parameter = parameter;
        previous_value     = value;
    }

    if (!result.hits.empty()) {
        result.contact = CurveSurfaceContact::Isolated;
    }
    return result;
}

VN_GEOMETRY_NS_END
