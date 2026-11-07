#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Surface.hpp"
#include "SurfaceParameterWindow.hpp"

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief The nearest point of a surface to a query point, with the parameters that produce it.
 *
 * @details
 * - `point` is `surface.evaluate(u, v)`; it is kept here so a caller does not evaluate the surface a second time.
 * - `distance` is `|point - query|`, so this one result answers both "where is the nearest point" and "how far is the
 *   point from the surface" - there is no second entry that recomputes the same minimum.
 * - The parameters are the surface's own: an azimuth and a height on a cylinder, two in-plane coordinates on a plane.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct SurfaceProjection {
    /// The nearest point on the surface.
    vn::math::Point3<T> point{};

    /// First parameter of @p point, in the surface's own units.
    T u{ T(0) };

    /// Second parameter of @p point, in the surface's own units.
    T v{ T(0) };

    /// Distance from the query point to @p point; never negative.
    T distance{ T(0) };
};

namespace detail
{

/**
 * @brief One sampled parameter pair with its squared distance to the query, used as a starting guess.
 */
template <vn::math::FP T>
struct SurfaceProjectionSeed {
    T u{ T(0) };
    T v{ T(0) };
    T cost{ std::numeric_limits<T>::infinity() };
};

/**
 * @brief Keeps the three cheapest seeds seen so far, cheapest first.
 */
template <vn::math::FP T>
void keepCheapestSeed(std::array<SurfaceProjectionSeed<T>, 3>& seeds, const SurfaceProjectionSeed<T>& candidate)
{
    if (!(candidate.cost < seeds[2].cost)) {
        return;
    }

    if (candidate.cost < seeds[0].cost) {
        seeds[2] = seeds[1];
        seeds[1] = seeds[0];
        seeds[0] = candidate;
    }
    else if (candidate.cost < seeds[1].cost) {
        seeds[2] = seeds[1];
        seeds[1] = candidate;
    }
    else {
        seeds[2] = candidate;
    }
}

/**
 * @brief Clamps a parameter into a window, so an iteration cannot leave the piece the caller asked about.
 */
template <vn::math::FP T>
[[nodiscard]] T clampToWindow(T value, T lower, T upper)
{
    if (value < lower) {
        return lower;
    }
    if (value > upper) {
        return upper;
    }
    return value;
}

/**
 * @brief Runs damped Gauss-Newton on `|S(u, v) - query|^2` from one seed, staying inside the window.
 *
 * @details
 * - Only the two first derivatives are used: the Gauss-Newton approximation `J^T J` of the Hessian needs nothing the
 *   `Surface<T>` interface does not report.
 * - A rank-deficient tangent frame (a pole, a collapsed patch) or a step that does not lower the cost ends the
 *   iteration; the caller compares the seeds it has.
 * - The step is limited to a quarter of the window per iteration and halved up to eight times when it overshoots, so a
 *   bad seed moves towards the minimum instead of jumping across the sheet.
 */
template <vn::math::FP T>
[[nodiscard]] SurfaceProjectionSeed<T> refineProjectionSeed(const Surface<T>&                surface,
                                                            const vn::math::Point3<T>&       query,
                                                            const SurfaceParameterWindow<T>& window,
                                                            T                                tolerance,
                                                            SurfaceProjectionSeed<T>         seed)
{
    const T u_limit = (window.last_u - window.first_u) * T(0.25);
    const T v_limit = (window.last_v - window.first_v) * T(0.25);

    for (int iteration = 0; iteration < 64; ++iteration) {
        const auto point     = surface.evaluate(seed.u, seed.v);
        const auto residual  = point - query;
        const auto tangent_u = surface.derivativeU(seed.u, seed.v);
        const auto tangent_v = surface.derivativeV(seed.u, seed.v);

        const T a           = tangent_u.dot(tangent_u);
        const T b           = tangent_u.dot(tangent_v);
        const T c           = tangent_v.dot(tangent_v);
        const T g           = tangent_u.dot(residual);
        const T h           = tangent_v.dot(residual);
        const T determinant = a * c - b * b;

        // A singular tangent frame (a sphere's pole, a collapsed patch) has no Gauss-Newton step; another seed may.
        if (!(std::abs(determinant) > std::numeric_limits<T>::epsilon() * (a * c + b * b))) {
            break;
        }

        T step_u = (-c * g + b * h) / determinant;
        T step_v = (b * g - a * h) / determinant;
        if (!std::isfinite(step_u) || !std::isfinite(step_v)) {
            break;
        }
        step_u = clampToWindow(step_u, -u_limit, u_limit);
        step_v = clampToWindow(step_v, -v_limit, v_limit);

        bool accepted = false;
        T    scale    = T(1);
        for (int trial = 0; trial < 8; ++trial) {
            const T    next_u     = clampToWindow(seed.u + scale * step_u, window.first_u, window.last_u);
            const T    next_v     = clampToWindow(seed.v + scale * step_v, window.first_v, window.last_v);
            const auto next_point = surface.evaluate(next_u, next_v);
            const T    next_cost  = (next_point - query).length2();
            if (next_cost < seed.cost) {
                const T moved = (next_point - point).length();
                seed.u        = next_u;
                seed.v        = next_v;
                seed.cost     = next_cost;
                accepted      = true;
                if (moved <= tolerance) {
                    return seed;
                }
                break;
            }
            scale *= T(0.5);
        }

        if (!accepted) {
            break;
        }
    }

    return seed;
}

} // namespace detail

/**
 * @brief Returns the point of a surface nearest to @p query, and its distance.
 *
 * @details
 * - The surface is sampled on a `(seed_count + 1) x (seed_count + 1)` grid of its parameter window, the three cheapest
 *   samples start a damped Gauss-Newton refinement, and the cheapest result wins. Only `evaluate()` and the two first
 *   derivatives are used, so any `Surface<T>` works - analytic sheets, trims and the two spline patches alike.
 * - The window is the caller's, and its infinite ends become the surface's own bounds; the default is therefore the
 *   natural domain. **A direction that neither the caller nor the surface bounds is refused** rather than sampled
 *   over an invented range (C1).
 * - A surface with several local minima (a torus, a folded patch) answers the best one the grid found; raise
 *   `seed_count` when the sheet has features finer than the default grid (P2).
 *
 * @param surface   The surface to project onto; must satisfy `isValid()`.
 * @param query     The point to measure from.
 * @param window    Parameter rectangle to search; infinite ends take the surface's own bound.
 * @param tolerance Geometric resolution: the refinement stops when a step moves the surface point by no more than this.
 * @param seed_count Number of grid intervals per direction; more finds finer features and costs more evaluations.
 * @return The nearest point found and its parameters, or `std::nullopt` when there is no finite window to search.
 *
 * @warning
 * - This is a numerical minimisation: the answer is a **local** minimum of the distance inside the window, not a
 *   guaranteed global one (P2), and a point lying exactly on a singular parameter line can converge slowly.
 *
 * @note
 * - A point-to-surface distance and the nearest point are one answer, so this is the only entry: read `distance` for
 *   the distance and `point` / `u` / `v` for where it is.
 *
 * @tparam T Floating-point scalar, deduced from @p query.
 * @tparam S Concrete surface type; must derive from `Surface<T>`.
 */
// clang-format off
// The declaration is kept on one line so the documentation gate can attach this block to it; LLVM style would move the
// return type onto its own line, which the gate reads as an undocumented declaration.
template <vn::math::FP T, typename S>
[[nodiscard]] std::optional<SurfaceProjection<T>> closestPointOnSurface(
    const S& surface, const vn::math::Point3<T>& query, SurfaceParameterWindow<T> window = naturalSurfaceWindow<T>(),
    T tolerance = vn::math::EPS<T>(), int seed_count = 4)
    requires std::is_base_of_v<Surface<T>, S>
{
    // clang-format on
    const auto rectangle = finiteParameterWindow(surface, window);
    if (!rectangle) {
        return std::nullopt;
    }

    const T   value_tolerance = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    const int divisions       = (seed_count < 1) ? 1 : ((seed_count > 16) ? 16 : seed_count);
    const T   u_width         = rectangle->last_u - rectangle->first_u;
    const T   v_width         = rectangle->last_v - rectangle->first_v;

    std::array<detail::SurfaceProjectionSeed<T>, 3> seeds{};
    for (int i = 0; i <= divisions; ++i) {
        const T u = rectangle->first_u + u_width * static_cast<T>(i) / static_cast<T>(divisions);
        for (int j = 0; j <= divisions; ++j) {
            const T    v     = rectangle->first_v + v_width * static_cast<T>(j) / static_cast<T>(divisions);
            const auto point = surface.evaluate(u, v);
            detail::keepCheapestSeed(seeds, detail::SurfaceProjectionSeed<T>{ u, v, (point - query).length2() });
        }
    }

    auto best = seeds[0];
    for (int start = 0; start < 3; ++start) {
        if (!std::isfinite(seeds[start].cost)) {
            continue;
        }
        const auto refined = detail::refineProjectionSeed(surface, query, *rectangle, value_tolerance, seeds[start]);
        if (refined.cost < best.cost) {
            best = refined;
        }
    }

    if (!std::isfinite(best.cost)) {
        return std::nullopt;
    }

    const auto point = surface.evaluate(best.u, best.v);
    return SurfaceProjection<T>{ point, best.u, best.v, std::sqrt(best.cost) };
}

namespace detail
{

/**
 * @brief Returns a signed distance to a surface, with the sign taken from the parameterisation's own normal.
 *
 * @details
 * - The magnitude is `closestPointOnSurface().distance`; the sign is the side of `dS/du x dS/dv` the point is on.
 *   The **polarity therefore belongs to the parameterisation**, not to a convention: a sphere's `u` / `v` cross points
 *   inward, so its value is negative outside. That is why this stays internal - a root finder only needs the sign
 *   changes, which are polarity-independent, while a caller comparing numbers needs the analytic `signedValue()` of a
 *   concrete surface.
 * - A point within @p tolerance of the surface answers 0: there is no side to report on the surface itself.
 * - A NaN means no side can be told: no projection (unusable surface or window) or a degenerate normal at the nearest
 *   point (a sphere's pole, a collapsed patch). A caller must read it as "no information", as `rootInBracket()` does.
 *
 * @param surface   The surface to measure from; must satisfy `isValid()`.
 * @param point     The point to measure.
 * @param window    Parameter rectangle the nearest point is searched in; infinite ends take the surface's own bound.
 * @param tolerance Geometric tolerance at which the answer is 0 and the projection refinement stops.
 * @param seed_count Number of grid intervals per direction handed to `closestPointOnSurface()`.
 * @return The signed distance, 0 on the surface, or a quiet NaN when no side can be told.
 *
 * @tparam T Floating-point scalar, deduced from @p point.
 * @tparam S Concrete surface type; must derive from `Surface<T>`.
 */
template <vn::math::FP T, typename S>
requires std::is_base_of_v<Surface<T>, S>
[[nodiscard]] T signedDistanceToSurface(const S&                   surface,
                                        const vn::math::Point3<T>& point,
                                        SurfaceParameterWindow<T>  window     = naturalSurfaceWindow<T>(),
                                        T                          tolerance  = vn::math::EPS<T>(),
                                        int                        seed_count = 4)
{
    const auto projection = closestPointOnSurface<T>(surface, point, window, tolerance, seed_count);
    if (!projection) {
        return std::numeric_limits<T>::quiet_NaN();
    }

    const T value_tolerance = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    if (projection->distance <= value_tolerance) {
        return T(0);
    }

    const auto normal = surface.derivativeU(projection->u, projection->v).cross(surface.derivativeV(projection->u, projection->v));
    if (!(normal.length() > vn::math::EPS<T>())) {
        return std::numeric_limits<T>::quiet_NaN();
    }

    return ((point - projection->point).dot(normal) < T(0)) ? -projection->distance : projection->distance;
}

} // namespace detail

VN_GEOM_SPATIAL_NS_END
