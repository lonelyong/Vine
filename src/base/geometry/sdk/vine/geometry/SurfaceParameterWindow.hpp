#pragma once

#include "geometry_global.hpp"

#include <cmath>
#include <limits>
#include <optional>

#include <vine/math/Types.hpp>

#include "Surface.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A finite rectangle in a surface's own parameter space.
 *
 * @details
 * - The four fields are the two parameter intervals a family-level query may visit: `[first_u, last_u]` x
 *   `[first_v, last_v]`. They are parameters, in the surface's own units, not spatial coordinates - a box in `u` / `v`
 *   that happens to have four numbers is not a `math::Rect2`.
 * - This is an **argument** of the surface algorithms, not part of `Surface<T>`: the interface reports the natural
 *   domain, and the caller narrows it when one direction is unbounded or when only a piece of a sheet is of interest.
 * - An infinite end means "leave this side as the surface's own domain". That is what makes the default window below the
 *   natural domain, and lets a caller narrow one direction (a cylinder's height) without spelling the other (its
 *   azimuth).
 *
 * @tparam T Floating-point scalar type.
 */
template <vn::math::FP T>
struct SurfaceParameterWindow {
    /// Lower end of the @p u interval.
    T first_u{ T(0) };

    /// Upper end of the @p u interval; must be greater than `first_u` once the infinite ends are resolved.
    T last_u{ T(0) };

    /// Lower end of the @p v interval.
    T first_v{ T(0) };

    /// Upper end of the @p v interval; must be greater than `first_v` once the infinite ends are resolved.
    T last_v{ T(0) };
};

/**
 * @brief Returns the window that means "the surface's own parameter domain".
 *
 * @details
 * - Every end is infinite, and an infinite end is resolved from the surface by `finiteParameterWindow()`. Naming the
 *   value beats defaulting to four zeros, which would be a zero-width window and mean the opposite.
 *
 * @return A window whose four ends all defer to the surface.
 *
 * @tparam T Floating-point scalar.
 */
template <vn::math::FP T>
[[nodiscard]] constexpr SurfaceParameterWindow<T> naturalSurfaceWindow() noexcept
{
    constexpr T infinity = std::numeric_limits<T>::infinity();
    return SurfaceParameterWindow<T>{ -infinity, infinity, -infinity, infinity };
}

/**
 * @brief Returns the finite parameter rectangle a family-level query will search, or nothing when there is none.
 *
 * @details
 * - An infinite end of @p window is replaced by the surface's own bound for that side; the rest is left as the caller
 *   wrote it. The default window is therefore the natural domain, and narrowing one direction is one assignment.
 * - The resolved rectangle must be finite and ordered, because the algorithms sample it - they never invent a range for
 *   a plane or a cylinder, the same rule `intersectCurveWithSurface()` applies to an unbounded curve
 *   (`geometry-intersection.md` C5).
 * - `std::nullopt` means the query has nothing to search: a surface whose `isValid()` is false, an unbounded direction
 *   that neither the caller nor the surface bounds, or a reversed / zero-width rectangle.
 *
 * @param surface The surface whose natural domain fills the infinite ends.
 * @param window  Rectangle to resolve; the default defers every end to @p surface.
 * @return The rectangle to search, or `std::nullopt` when no finite one exists.
 *
 * @tparam T Floating-point scalar, deduced from @p surface.
 */
template <vn::math::FP T>
[[nodiscard]] std::optional<SurfaceParameterWindow<T>> finiteParameterWindow(
    const Surface<T>& surface, SurfaceParameterWindow<T> window = naturalSurfaceWindow<T>())
{
    if (!surface.isValid()) {
        return std::nullopt;
    }

    if (!std::isfinite(window.first_u)) {
        window.first_u = surface.firstUParameter();
    }
    if (!std::isfinite(window.last_u)) {
        window.last_u = surface.lastUParameter();
    }
    if (!std::isfinite(window.first_v)) {
        window.first_v = surface.firstVParameter();
    }
    if (!std::isfinite(window.last_v)) {
        window.last_v = surface.lastVParameter();
    }

    if (!std::isfinite(window.first_u) || !std::isfinite(window.last_u) || !std::isfinite(window.first_v)
        || !std::isfinite(window.last_v)) {
        return std::nullopt;
    }
    if (!(window.last_u > window.first_u) || !(window.last_v > window.first_v)) {
        return std::nullopt;
    }
    return window;
}

VN_GEOMETRY_NS_END
