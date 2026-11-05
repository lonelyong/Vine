#pragma once

#include "math_global.hpp"

#include <cstdint>

VN_MATH_NS_BEGIN

/**
 * @brief How smooth a curve or surface is inside its domain: the highest derivative that stays continuous.
 *
 * @details
 * - The enumerators are **ordered by smoothness**, so `>=` answers "at least this smooth": a curve answering `C2`
 *   also satisfies a query for `C1`. C-continuity and geometric continuity are interleaved where they belong, since
 *   `G1` (same tangent *direction*, arbitrary magnitude) is stronger than `C0` and weaker than `C1`.
 * - `CN` is what the analytic types answer: a line, a circle and a plane have derivatives of every order inside
 *   their domain.
 * - The value describes the interior of the domain only. A closed or periodic curve's seam is not part of that
 *   domain and is reported by `isClosed()` / `isPeriodic()` on the curve itself, not here.
 * - `Polynomial` and `Rational` are deliberately absent: they classify a *representation* (nurbs form), not a
 *   smoothness, and a consumer asking for smoothness would have to special-case them.
 *
 * @note
 * - The enumerators are a ladder: compare them with `>=` or `<=` and never with `==` unless the exact value is what
 *   is being asked for.
 */
enum class Continuity : std::uint8_t {
    C0, ///< Position continuous: the pieces meet at a point.
    G1, ///< Tangent direction continuous: a common tangent with possibly different magnitudes.
    C1, ///< First derivative continuous.
    G2, ///< Curvature continuous: a common curvature with possibly different parameterisations.
    C2, ///< Second derivative continuous.
    C3, ///< Third derivative continuous.
    CN, ///< Infinitely differentiable: an analytic curve or surface.
};

VN_MATH_NS_END
