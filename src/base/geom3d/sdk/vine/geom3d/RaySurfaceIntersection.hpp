#pragma once

#include "geom3d_global.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include "Ray.hpp"
#include "RayHit.hpp"
#include "Surface.hpp"
#include "SurfaceParameterWindow.hpp"
#include "SurfaceProjection.hpp"

VN_GEOM3D_NS_BEGIN

/**
 * @brief One crossing of a ray through a surface, with where on the surface it happened.
 *
 * @details
 * - `hit` carries the ray's own answers - distance `t`, the point, and the normal turned back towards the ray
 *   (`RayHit`'s rule).
 * - `u` / `v` are the surface parameters of the hit, so a caller that selects a face or trims a patch can keep using
 *   the surface's own coordinates instead of reconstructing them.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct RaySurfaceHit {
    /// Distance along the ray, the hit point, and the normal facing back towards the ray.
    RayHit<T> hit{};

    /// First surface parameter at the hit, in the surface's own units.
    T u{ T(0) };

    /// Second surface parameter at the hit, in the surface's own units.
    T v{ T(0) };
};

/**
 * @brief The crossings of a ray with a surface.
 *
 * @details
 * - `coincident` answers the case a point list cannot: the ray lies **in** the surface (a ray inside a plane, along a
 *   cylinder's generatrix), so the intersection is a stretch of the ray rather than a set of points. `hits` is then
 *   empty - the same classification `CurveSurfaceIntersection` uses for a curve lying on a surface.
 * - Otherwise `hits` holds the isolated crossings in ascending `hit.t`; an empty list means the ray missed.
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct RaySurfaceIntersection {
    /// True when the ray lies in the surface, so the intersection is not a point set.
    bool coincident{ false };

    /// The isolated crossings, ascending by `hit.t`; empty when `coincident` is true or nothing was hit.
    std::vector<RaySurfaceHit<T>> hits;
};

namespace detail
{

/**
 * @brief One Newton seed: a parameter pair and the ray distance of the surface point there.
 */
template <vn::math::FP T>
struct RaySurfaceSeed {
    T u{ T(0) };
    T v{ T(0) };
    T t{ T(0) };
};

/**
 * @brief Runs damped Newton on `S(u, v) = ray.pointAt(t)`, staying inside the window and on the forward ray.
 *
 * @details
 * - The system is three equations (the three components of `S - P`) in three unknowns `(u, v, t)`; its Jacobian is
 *   `[dS/du dS/dv -direction]`, so only the two first derivatives are needed and no implicit form is assumed.
 * - A singular Jacobian (the ray tangent to the surface, a singular parameter point) ends the iteration - that seed
 *   finds nothing, and the answer is a miss, as with the sign-change search's tangency rule (`P4`).
 * - The step is limited per iteration and halved up to eight times on overshoot; `u` / `v` are clamped into the window
 *   and `t` is not clamped, because the ray is unbounded and the caller filters by `t`.
 */
template <vn::math::FP T>
[[nodiscard]] std::optional<RaySurfaceHit<T>> refineRaySurfaceSeed(const Ray<T>&                    ray,
                                                                   const Surface<T>&                surface,
                                                                   const SurfaceParameterWindow<T>& window,
                                                                   T                                tolerance,
                                                                   T                                distance_limit,
                                                                   RaySurfaceSeed<T>                seed)
{
    const T u_limit = (window.last_u - window.first_u) * T(0.25);
    const T v_limit = (window.last_v - window.first_v) * T(0.25);

    for (int iteration = 0; iteration < 64; ++iteration) {
        const auto point     = surface.evaluate(seed.u, seed.v);
        const auto residual  = point - ray.pointAt(seed.t);
        const auto tangent_u = surface.derivativeU(seed.u, seed.v);
        const auto tangent_v = surface.derivativeV(seed.u, seed.v);
        const auto direction = -ray.direction;

        if (residual.length() <= tolerance) {
            auto    normal = tangent_u.cross(tangent_v);
            const T length = normal.length();
            normal         = (length > vn::math::EPS<T>()) ? normal.normalized() : vn::math::Vector3<T>(T(0), T(0), T(0));
            if (normal.dot(ray.direction) > T(0)) {
                normal = -normal;
            }
            if (seed.t < -tolerance) {
                return std::nullopt;
            }
            RaySurfaceHit<T> hit{};
            hit.hit.t      = seed.t;
            hit.hit.point  = point;
            hit.hit.normal = normal;
            hit.u          = seed.u;
            hit.v          = seed.v;
            return hit;
        }

        const T determinant = tangent_u.dot(tangent_v.cross(direction));
        if (!(std::abs(determinant) > std::numeric_limits<T>::epsilon() * tangent_u.length() * tangent_v.length())) {
            break;
        }

        const auto target = -residual;
        T          step_u = target.dot(tangent_v.cross(direction)) / determinant;
        T          step_v = tangent_u.dot(target.cross(direction)) / determinant;
        T          step_t = tangent_u.dot(tangent_v.cross(target)) / determinant;
        if (!std::isfinite(step_u) || !std::isfinite(step_v) || !std::isfinite(step_t)) {
            break;
        }
        step_u = clampToWindow(step_u, -u_limit, u_limit);
        step_v = clampToWindow(step_v, -v_limit, v_limit);
        step_t = clampToWindow(step_t, -distance_limit, distance_limit);

        bool accepted = false;
        T    cost     = residual.length2();
        T    scale    = T(1);
        for (int trial = 0; trial < 8; ++trial) {
            const T next_u    = clampToWindow(seed.u + scale * step_u, window.first_u, window.last_u);
            const T next_v    = clampToWindow(seed.v + scale * step_v, window.first_v, window.last_v);
            const T next_t    = seed.t + scale * step_t;
            const T next_cost = (surface.evaluate(next_u, next_v) - ray.pointAt(next_t)).length2();
            if (next_cost < cost) {
                seed.u   = next_u;
                seed.v   = next_v;
                seed.t   = next_t;
                accepted = true;
                break;
            }
            scale *= T(0.5);
        }

        if (!accepted) {
            break;
        }
    }

    return std::nullopt;
}

} // namespace detail

/**
 * @brief Returns every crossing of a ray with a surface, by Newton iteration on the surface's parametric form.
 *
 * @details
 * - The problem is solved as it is written: `S(u, v) = origin + t * direction` in the three unknowns `(u, v, t)`, with
 *   the Jacobian `[dS/du dS/dv -direction]`. A grid over the parameter window supplies the starting guesses, each
 *   guess is refined, duplicates are dropped and the hits come back in ascending `t`. Only `evaluate()` and the two
 *   first derivatives are used, so **any** `Surface<T>` - analytic sheets, trimmed pieces and spline patches - is
 *   covered without a per-type entry.
 * - The window is the caller's, and its infinite ends become the surface's own bounds; the default is therefore the
 *   natural domain. **A direction that neither the caller nor the surface bounds is refused** rather than searched
 *   over an invented range (`C1`), for the same reason `intersectCurveWithSurface()` demands a window for a line.
 * - A ray lying **in** the surface (inside a plane, along a cylinder's generatrix) has infinitely many crossings. It is
 *   reported by `coincident` with an empty `hits`, detected by projecting three points spread across the piece's own
 *   ray span onto the surface (`P1`).
 * - A ray that only **touches** the surface (grazing) has a singular Jacobian and is reported as a miss, matching the
 *   sign-change search's documented rule (`P2`).
 * - Hits behind the origin are not crossings of a ray and are dropped; a hit exactly at the origin is kept.
 *
 * @param ray       The ray to cast; an aimless ray (`direction` zero) hits nothing.
 * @param surface   The surface to hit; must satisfy `isValid()`.
 * @param window    Parameter rectangle to search; infinite ends take the surface's own bound.
 * @param tolerance Geometric tolerance: a refined point this close to the ray is a hit.
 * @param seed_count Number of grid intervals per direction; more finds more crossings and costs more iterations.
 * @return The crossings, ascending by `hit.t`, or the coincident classification.
 *
 * @warning
 * - The result is only as complete as the seed grid: a patch folded more finely than the grid can resolve a crossing
 *   into the same cell will lose it; raise `seed_count` for those.
 * - A hit at a singular parameter point (a sphere's pole) carries a **zero** `hit.normal`, because the two tangents are
 *   parallel there; the point and `t` are still correct.
 *
 * @note
 * - This is the family-level entry. A caller that knows the concrete surface keeps its analytic entry
 *   (`Plane::intersectWith(Ray)`), which is cheaper and exact.
 *
 * @tparam T Floating-point scalar, deduced from @p ray.
 * @tparam S Concrete surface type; must derive from `Surface<T>`.
 */
// clang-format off
// The declaration is kept on one line so the documentation gate can attach this block to it; LLVM style would move the
// return type onto its own line, which the gate reads as an undocumented declaration.
template <vn::math::FP T, typename S>
[[nodiscard]] RaySurfaceIntersection<T> intersectRayWithSurface(
    const Ray<T>& ray, const S& surface, SurfaceParameterWindow<T> window = naturalSurfaceWindow<T>(),
    T tolerance = vn::math::EPS<T>(), int seed_count = 4)
    requires std::is_base_of_v<Surface<T>, S>
{
    // clang-format on
    RaySurfaceIntersection<T> result;
    const auto                rectangle = finiteParameterWindow(surface, window);
    if (!rectangle) {
        return result;
    }
    if (!(ray.direction.length2() > T(0))) {
        return result;
    }

    const T   value_tolerance = (tolerance > T(0)) ? tolerance : vn::math::EPS<T>();
    const int divisions       = (seed_count < 1) ? 1 : ((seed_count > 16) ? 16 : seed_count);
    const T   u_width         = rectangle->last_u - rectangle->first_u;
    const T   v_width         = rectangle->last_v - rectangle->first_v;

    // One pass over the grid: the ray span of the piece, used for the coincidence probes and the step limit.
    T span_low  = std::numeric_limits<T>::infinity();
    T span_high = -std::numeric_limits<T>::infinity();
    for (int i = 0; i <= divisions; ++i) {
        const T u = rectangle->first_u + u_width * static_cast<T>(i) / static_cast<T>(divisions);
        for (int j = 0; j <= divisions; ++j) {
            const T    v  = rectangle->first_v + v_width * static_cast<T>(j) / static_cast<T>(divisions);
            const auto at = (surface.evaluate(u, v) - ray.origin).dot(ray.direction);
            span_low      = std::min(span_low, at);
            span_high     = std::max(span_high, at);
        }
    }

    const T span           = span_high - span_low;
    const T distance_limit = std::max({ span * T(0.5), std::abs(span_low), std::abs(span_high), T(1) });

    // Three points spread across the piece's ray span: on the surface at all three means the ray lies in it, not
    // through it. Distances are geometric, so the tolerance is the same one the crossings use.
    if (span > value_tolerance) {
        const T probes[3]  = { span_low, span_low + span * T(0.5), span_high };
        int     on_surface = 0;
        for (const T probe : probes) {
            const auto projection = closestPointOnSurface<T>(surface, ray.pointAt(probe), *rectangle, value_tolerance, seed_count);
            if (projection && projection->distance <= value_tolerance) {
                ++on_surface;
            }
        }
        if (on_surface == 3) {
            result.coincident = true;
            return result;
        }
    }

    for (int i = 0; i <= divisions; ++i) {
        const T u = rectangle->first_u + u_width * static_cast<T>(i) / static_cast<T>(divisions);
        for (int j = 0; j <= divisions; ++j) {
            const T    v    = rectangle->first_v + v_width * static_cast<T>(j) / static_cast<T>(divisions);
            const auto seed = detail::RaySurfaceSeed<T>{ u, v, (surface.evaluate(u, v) - ray.origin).dot(ray.direction) };
            const auto hit  = detail::refineRaySurfaceSeed(ray, surface, *rectangle, value_tolerance, distance_limit, seed);
            if (!hit) {
                continue;
            }

            const bool duplicate = std::any_of(result.hits.begin(), result.hits.end(), [&](const RaySurfaceHit<T>& existing) {
                return std::abs(existing.hit.t - hit->hit.t) <= value_tolerance;
            });
            if (!duplicate) {
                result.hits.push_back(*hit);
            }
        }
    }

    std::sort(result.hits.begin(), result.hits.end(), [](const RaySurfaceHit<T>& left, const RaySurfaceHit<T>& right) { return left.hit.t < right.hit.t; });
    return result;
}

VN_GEOM3D_NS_END
