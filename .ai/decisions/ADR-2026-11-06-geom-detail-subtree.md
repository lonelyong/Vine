# ADR-2026-11-06: a `detail` subtree inside `geom`, holding only dimension-independent shared implementations

- Status: Accepted
- Date: 2026-11-06
- Related: `../design/geometry-module.md` 2.2-2.3 / 3.1, `../design/geometry-planar.md` C10 / D3, `ADR-2026-11-06-merged-geom-module.md`, `ADR-2026-11-06-math-is-the-base-layer.md`, `ADR-2026-11-06-shared-numeric-kernels-in-math.md`

## Background

Merging `geom/planar` / `geom/spatial` solved "the library" and did not solve "where the shared code lives". After the merge a measurement showed the two trees still had line-for-line identical implementations, because the place that can see both sides does not exist inside the library: the garage held two trees and their own `detail`s.

| Pair | 2D lines | 3D lines | Differing lines | What differs |
|---|---|---|---|---|
| `CurveProjection.hpp` | 244 | 248 | 56 | `narrowCrossing` is identical word for word; `CurveScalar` / `CurveWindow` / `resolveCurveWindow` differ by one documentation sentence |
| `CurveCurveDistance.hpp` | 217 | 217 | 22 | `refineCurveCurveDistance` is identical line for line (purely generic) |
| `CurveLength.hpp` | 138 | 139 | 21 | the implementations of `arcLength` / `parameterAtLength` are identical line for line |

There was also one hidden coupling: the "scalar of a curve" alias `CurveScalar` lived in the **projection** header, so `CurveCurvature` / `CurveLength` / `CurveCurveDistance` / `CurveCurveIntersection` all included `CurveProjection.hpp` for one alias - a curvature query that needs no projection pulled the projector into the compile.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the duplication | No change; each tree is self-sufficient | Three copies of nearly 200 lines, and a change in one must be remembered in the other (the documentation has already drifted: one side says "the same kernel as the planar side", the other says nothing) |
| B. Move `math/numeric` and `math/spline` into `geom` as `detail` | The easiest to write: the geometry side stops looking for kernels across layers | `math` is the base layer, and after the move only geometry could answer "how do I integrate a function"; imaging / mesh / brep / robotics would either depend on the curve library for one integral or copy it again. `Quadrature` / `RootFinding` have their own unit tests and a live ADR, and demoting them to `detail` would make the tests test private code |
| C. Build a `detail` subtree inside `geom`, holding only dimension-independent shared geometry code | The duplication disappears, the layer direction is unchanged (`geom -> math` as before), the public names do not move | One more namespace level; the gate has to pin that `detail` may not name a dimension, or it becomes a third dimension tree |
| D. Add a `2`/`3` suffix to every type and merge the two namespaces into one | One namespace | 48 type names, 82 aliases, 62 headers, 489 qualified references, 15 targets; and the names would misreport the family relation (two contracts with zero inheritance, not interchangeable), while `BezierCurve3` already means "cubic" in CAD. `D3` rejected the same thing long ago |

## Decision

Chose **C**:

- The new subtree `src/base/geom/sdk/vine/geom/detail/`, include path `<vine/geom/detail/...>`, namespace `vn::geom::detail` (`geom_global.hpp` gains `VN_GEOM_NS_BEGIN` and `VN_GEOM_DETAIL_NS_BEGIN`).
- Its contents are only **dimension-independent**: `CurveTraits.hpp` (`CurveScalar` / `CurveWindow` / `resolveTolerance` / `resolveCurveWindow`), `CrossingSearch.hpp` (`narrowCrossing`), `CurveMeasure.hpp` (`arcLengthBetween` / `parameterCovering`), `CurvePairSearch.hpp` (`refineCurvePair`).
- **The public names do not move**: `geom::planar::arcLength` / `geom::spatial::arcLength` remain the only documented entries, each resolving its own window and tolerance and then forwarding (`G4`: the contract stays in its namespace, the implementation exists once).
- **The gate pins the direction**: `scripts/check_geom_layering.py` grows from "the two trees do not include each other" to three rules - `geom/planar` may not include `geom/spatial`, the reverse may not happen, and `geom/detail` may not include either (a helper that names a dimension is no longer shared).
- Whether code sinks is decided by the difference between the two copies: only type names and documentation differ -> sink; the difference is mathematical -> keep one each. So `signedCurvature` (signed in 2D) and `curvature` (a magnitude in 3D) stay two copies, and `closestPointOnCurve` does too (its result type carries `Point2` / `Point3`).

Criteria: `G1` (one copy of the shared code), `G2` (the shared code still lives on the layer that can look down; geometry does not sink into `math` and `math` does not float up into geometry), `G5` (a geometry-level change no longer means editing two places), `G7` (the complexity is absorbed in one `detail` instead of becoming a template parameter or a callback for the caller). The tension with `G8`: `geom/detail` had four headers with one implementation each, and nothing was pre-built for "might be shared later".

## Consequences

- Good: one copy each of `narrowCrossing`, the window rules, the tolerance rules, the two-curve refinement, the arc length and the inverse solve; `CurveCurvature` no longer pulls in the projector for an alias; `CurveLength` no longer includes `CurveProjection.hpp`.
- Cost: the qualifier `geom::detail::` is longer than `detail::`; `geom` now holds both a "tree-local `detail`" (`geom::planar::detail` / `geom::spatial::detail`, holding file-local helpers) and a "module-shared `detail`" (`vn::geom::detail`), told apart by the qualifier.
- Not done: moving `math`'s `numeric` / `spline` in (option B); adding `2`/`3` suffixes to types (option D, `D3` already covers it).
- Boundary: the names inside `geom/detail` are not a public interface and promise no ABI or stability; to publish a shared capability, first give it one public entry in each namespace.
- Follow-up (same day): option B was later adopted in full - the `spline` knot model and basis functions, the pure Bernstein evaluation, and `math/numeric`'s root finding and integration all moved into the `geom/detail` this ADR created, leaving `math` with vocabulary only. The two objections aimed at `numeric` ("the base layer must be able to answer an integral") still hold, and are now recorded explicitly as a cost: when a second consumer appears, promote it to its own module rather than settling for `geom`. See `ADR-2026-11-06-math-keeps-only-vocabulary.md`.

## Evidence

- New headers: the four files listed in `geometry-planar.md` 2.5; the three namespace macros in `geom_global.hpp` plus one `Continuity` re-export.
- Deleted: each tree lost `narrowCrossing` (with its documentation), `CurveScalar` / `CurveWindow` / `resolveCurveWindow`, `refineCurveCurveDistance`, and the bodies of `arcLength` / `parameterAtLength` - `CurveProjection.hpp` 244 -> 141 / 248 -> 145 lines, `CurveCurveDistance.hpp` 217 -> 150 lines (both sides), `CurveLength.hpp` 138 -> 120 / 139 -> 121 lines. Every remaining difference is documentation.
- Gate: `python scripts/check_geom_layering.py` -> `0 finding(s) in 66 file(s)`.
- Verified: the Debug build passes; `test_geom_planar` 138, `test_geom_spatial` 193 and `test_math` 56 green (behaviour unchanged, a pure move).
