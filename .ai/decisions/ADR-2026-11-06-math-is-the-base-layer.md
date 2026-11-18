# ADR-2026-11-06: `math` is the base layer, not only a maths library

- Status: Superseded (the `spline/` knot model and `Continuity` have moved into `geom`, see `ADR-2026-11-06-spline-model-moves-to-geom.md`; the `numeric` judgement is unaffected)
- Date: 2026-11-06
- Related: `ADR-2026-11-06-shared-numeric-kernels-in-math.md`, `../design/geometry-axis-surfaces.md` 2.5, `../design/geometry-primitives.md` 2.7

## Background

`vn::math` held three kinds of thing whose common property was not "mathematics":

1. **mathematical objects and elementary operations**: `Point2/3`, `Vector*`, `Matrix*`, `Quaternion`, `Transform*`, `Rect2`, `EPS`/`PI`, the elementary functions of `Math.hpp`.
2. **mathematical evaluation (one computation, no tolerance)**: `spline::deCasteljau` / `differences` (Bernstein blending and finite differences), `spline::basisValue` (Cox-de Boor basis functions and their derivatives), `symmetricEigen`. This is real mathematics: given numbers it produces numbers, and there is no "converge to a precision".
3. **CAD representation conventions (not mathematics)**: `spline::flatKnotCount` / `knotAt` / `knotSpan` (the flat index of distinct knots plus multiplicities), `isClampedKnotVector` (clamped-form validation), `knotContinuity` (multiplicity -> the `Continuity` ladder), and `Continuity` itself. These are the vocabulary of a NURBS data model - with a different representation (OCCT's expanded knot vector, say) they would have to be rewritten, so they are not mathematical facts.

Putting the third kind into `math` felt "odd", because it pushed a representation convention into a maths library. But the code had no better place: `geom/planar` and `geom/spatial` are **sibling modules**, both need it, and the repository had no common geometry layer below them (under `src/base`, `math` is the only thing before both). So the position was forced by the dependency graph, and the problem was **how the name and the duty are stated**, not the layering.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the status quo but state `math`'s duty as "the base layer" | No risk; consistent with the `numeric` judgement that "two headers do not justify a module" | The name is still `math`, and a new reader is puzzled once more |
| B. Promote `spline/` to the module `vn::Nurbs`, between `math` and the two geometry modules | The most accurate semantics: one layer of "mathematics", one of "representation", one of "geometry types" | A module for two headers, contradicting the `numeric` judgement; and `math/Continuity.hpp` would have to move with it, dragging in the `Curve` / `Surface` interfaces |
| C. Split the third kind into its own sub-namespace (`spline` keeps the basis functions, `knots` holds the conventions) | The boundary is visible inside one module | The two kinds are always used together (locate the span before evaluating), and splitting them makes the caller write two prefixes |

## Decision

Chose **A**: the position does not move, and two things are written into the documentation.

- `vn::math`'s duty is **the base layer plus mathematics**: any module may depend on it and it depends on no module (except `global`/`core`). Mathematical vocabulary goes in the root layer, mathematical **processes** go to `numeric` (the criterion is in `ADR-2026-11-06-shared-numeric-kernels-in-math.md`), and table-lookup **representation conventions** go to `spline`.
- `math/spline` was already two halves, and the header marks each item: `deCasteljau` / `differences` / `basisValue` are mathematical evaluation; `flatKnotCount` / `knotAt` / `knotSpan` / `isClampedKnotVector` / `knotContinuity` are NURBS representation conventions (multiplicity, clamped form, the continuity ladder). `Continuity` likewise: it is the smoothness ladder, shared by the geometry interfaces and the spline conventions, which is why it lives in `math` rather than `geom/spatial`.

**Escalation trigger (when a `vn::Nurbs` module should be created)**: a second family of "representation conventions" needs the same place - NURBS parameterisation (knot insertion, degree elevation, normalising rational weights), trim boundary conventions, shared vocabulary for tolerance and approximation. Then promote `spline/` together with those new families to an independent module and evaluate `Continuity` with them (it is shared in three places: interface smoothness, spline multiplicity, mesh continuity). With only one family nothing moves: that would be extracting one edge of `math` into an empty shell.

## Consequences

- Good: the position has a written reason, so "why is it in math" need not be re-derived; the criterion for the three kinds is consistent (evaluation -> root layer / process -> `numeric` / representation convention -> `spline`) and no longer wavers on "does it look like an algorithm".
- Bad / cost: the name `math` still understates its duty; a new member has to read this ADR to learn that "`math` is also the base layer".
- Not done: renaming `vn::math` (a breaking change across every module for nothing but the name).

## Evidence

- `src/base/math/sdk/vine/math/spline/BSplineBasis.hpp`: `basisValue` (mathematical evaluation) shares a header with `knotSpan` / `isClampedKnotVector` / `knotContinuity` (representation conventions), and each function's `@brief` says which half it belongs to. <!-- drift-ok -->
- `src/base/math/sdk/vine/math/spline/BezierBlend.hpp`: `deCasteljau` and `differences`, pure evaluation, no tolerance. <!-- drift-ok -->
- Consumers: `BezierCurve` / `BSplineCurve` / `BezierSurface` / `BSplineSurface` in `geom/planar` and `geom/spatial` (6 headers) - both sibling modules use it, which is the only reason it is shared.
- The `src/base` directory list: there is no common geometry module between `geom/planar` and `geom/spatial`, and `math` is the only layer below.
