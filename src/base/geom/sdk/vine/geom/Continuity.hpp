#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstdint>

// The namespace macro comes from `geom_global.hpp`, which includes this header for the module root's vocabulary. This
// header therefore never reaches back for it: `geom_global.hpp` declares its macros before including anything of its
// own, so both include orders work.
VN_GEOM_NS_BEGIN

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
 * - It lives at the module root, not in one dimension tree, because a planar curve answers the same ladder as a space
 *   curve; both namespaces re-export it (`geom_global.hpp`), so `geom2d::Continuity` and `geom3d::Continuity` name
 *   this one type. Its only consumer is geometry - the curve and surface contracts plus the B-spline knot ladder.
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

VN_GEOM_NS_END
