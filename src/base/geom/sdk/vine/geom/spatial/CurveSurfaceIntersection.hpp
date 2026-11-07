#pragma once

#include <vine/geom/geom_global.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/geom/detail/CrossingSearch.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>

#include "Curve.hpp"
#include "Surface.hpp"
#include "SurfaceParameterWindow.hpp"
#include "SurfaceProjection.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

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
enum class CurveSurfaceContact : std::uint8_t
{
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

namespace detail
{

/**
 * @brief Walks a curve over a finite window, turning sign changes of @p value_at into crossings.
 *
 * @details
 * - This is the half both curve x surface entries share: sampling, the coincidence run, duplicate removal and the
 *   parameter tolerance derived from the curve's speed. The two entries differ only in how @p value_at obtains a
 *   **signed** value (`signedValue()` from an implicit form, or a projection onto a surface).
 * - A NaN value carries no sign: it breaks a near-surface run and never forms a crossing, which is what makes a
 *   degenerate normal a gap rather than a false answer. `rootInBracket()` treats the same NaN as no root.
 * - The caller owns the surface's usability and window; this function owns the curve's.
 *
 * @param curve          Curve to walk; must satisfy `isValid()`.
 * @param window         Curve parameter window to search, or nothing for the curve's own domain.
 * @param tolerance      Geometric tolerance: how close to the surface a point still counts as on it.
 * @param interval_count Number of scan intervals; at least four.
 * @param value_at       Callable answering the signed distance of the curve at a parameter.
 * @return The crossings, the coincidence classification, or an empty `None` result.
 */
template <vn::math::FP T, typename ValueAt>
[[nodiscard]] CurveSurfaceIntersection<T>
scanCurveAgainstSurface(const Curve<T>& curve, std::optional<std::pair<T, T>> window, T tolerance, int interval_count, ValueAt&& value_at)
{
    CurveSurfaceIntersection<T> result;
    if (!curve.isValid()) {
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

    const T   value_tolerance = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    const T   on_surface      = value_tolerance;
    const int steps           = interval_count > 3 ? interval_count : 4;
    const T   step            = (last - first) / static_cast<T>(steps);

    const auto record = [&](T parameter) {
        const auto point = curve.evaluate(parameter);
        for (const auto& hit : result.hits) {
            if ((hit.point - point).length() <= on_surface) {
                return;
            }
        }
        result.hits.push_back(CurveSurfaceHit<T>{ parameter, point });
    };

    T   previous_parameter = first;
    T   previous_value     = value_at(first);
    int near_surface_run   = vn::math::isZero(previous_value, on_surface) ? 1 : 0;
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

        const bool crossed = (previous_value < T(0) && value > T(0)) || (previous_value > T(0) && value < T(0));
        if (crossed) {
            const auto root = geom::detail::narrowCrossing(curve, value_at, previous_parameter, parameter,
                                                           value_tolerance, step * T(1e-6));
            if (root) {
                record(*root);
            }
        }
        else if (vn::math::isZero(value, on_surface)) {
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

} // namespace detail

/**
 * @brief Returns the crossings of a curve with an analytic surface, by root-finding its signed distance along the curve.
 *
 * @details
 * - The search samples `f(u) = signedValue(surface, curve.evaluate(u))` across the parameter window, keeps the samples
 *   that are already on the surface, and narrows every sign change with `vn::geom::detail::rootInBracket()`. Because
 *   `f` is a
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
[[nodiscard]] CurveSurfaceIntersection<T> intersectCurveWithSurface(const Curve<T>&                curve,
                                                                    const SurfaceLike&             surface,
                                                                    std::optional<std::pair<T, T>> window         = std::nullopt,
                                                                    T                              tolerance      = vn::math::EPS<T>(),
                                                                    int                            interval_count = 64)
{
    if (!surface.isValid()) {
        return {};
    }

    return detail::scanCurveAgainstSurface<T>(curve, window, tolerance, interval_count, [&](T parameter) {
        return signedValue(surface, curve.evaluate(parameter));
    });
}

/**
 * @brief Returns crossings of a curve with **any** surface, by root-finding the signed distance a projection gives.
 *
 * @details
 * - The analytic entry needs a surface with an implicit form (`signedValue()`). A spline patch or a trim view has none,
 *   so this entry builds the same kind of function differently: `signedDistanceToSurface()` projects the curve point
 *   onto the surface and takes the side from the surface's own normal. The scan, the coincidence classification,
 *   duplicate removal and the parameter tolerance are the analytic entry's - the two differ only in how a value is
 *   obtained (`geometry-intersection.md` 2.9).
 * - The surface window is the search rectangle for the projection; its infinite ends take the surface's own bounds, so
 *   a bounded patch needs nothing and a plane, cylinder or cone needs its unbounded direction narrowed (`C5`).
 * - A sample whose normal is degenerate (a sphere's pole) yields a NaN distance; NaN carries no sign, so it neither
 *   counts as on the surface nor forms a crossing.
 *
 * @param curve          Curve to walk; must be usable (`isValid()`).
 * @param surface        Surface to cross; must satisfy `isValid()`.
 * @param curve_window   Curve parameter window to search. Omitted for a bounded curve; **required** for an unbounded
 *                       one (a line, a hyperbola, a parabola), whose domain is infinite.
 * @param surface_window Parameter rectangle the projection searches; infinite ends take the surface's own bound.
 * @param tolerance      Geometric tolerance: how close to the surface a point still counts as on it.
 * @param interval_count Number of scan intervals; more finds finer features and costs more projections.
 * @return The crossings, or `contact == None` when the inputs are unusable, a window is missing or infinite, or no
 *         crossing was found.
 *
 * @warning
 * - A **tangency** does not change the sign of the projected distance and is reported as `None`, exactly as the
 *   analytic entry does (`P4`).
 * - The sign is only as good as the projection: a local minimum that is not the nearest point (`P2`) can put the curve
 *   point on the wrong side far from the surface. Near the crossing - where the sign decides - the nearest point is
 *   unambiguous.
 * - Cost: every sample is a full projection (a seed grid plus Newton steps), so this is far more expensive than a
 *   `signedValue()` call; raise `interval_count` only when the curve really needs it.
 * - A curve point whose nearest point is on a **singular parameter line** (a sphere's pole, seen from the axis) has no
 *   normal, so its value is NaN and a crossing that lands exactly there is not reported. A surface with an implicit
 *   form does not have this gap - use its entry.
 *
 * @note
 * - Prefer `intersectCurveWithSurface()` whenever the surface provides `signedValue()`: it is exact and needs no
 *   surface window.
 *
 * @tparam T Floating-point scalar, deduced from @p curve.
 * @tparam S Concrete surface type; must derive from `Surface<T>`.
 */
// clang-format off
// The declaration is kept on one line so the documentation gate can attach this block to it; LLVM style would move the
// return type onto its own line, which the gate reads as an undocumented declaration.
template <vn::math::FP T, typename S>
[[nodiscard]] CurveSurfaceIntersection<T> intersectCurveWithSurfaceByProjection(
    const Curve<T>& curve, const S& surface, std::optional<std::pair<T, T>> curve_window = std::nullopt,
    SurfaceParameterWindow<T> surface_window = naturalSurfaceWindow<T>(), T tolerance = vn::math::EPS<T>(),
    int interval_count = 64)
    requires std::is_base_of_v<Surface<T>, S>
{
    // clang-format on
    const auto rectangle = finiteParameterWindow(surface, surface_window);
    if (!rectangle) {
        return {};
    }

    const T value_tolerance = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    return detail::scanCurveAgainstSurface<T>(curve, curve_window, value_tolerance, interval_count, [&](T parameter) {
        return detail::signedDistanceToSurface<T>(surface, curve.evaluate(parameter), *rectangle, value_tolerance);
    });
}

VN_GEOM_SPATIAL_NS_END
