# ADR-2026-11-06: the B-spline knot model and `Continuity` move from `math` into `geom`

- Status: Partly superseded ("Bernstein evaluation stays in `math/spline`" and "`math/numeric` does not move" were superseded by `ADR-2026-11-06-math-keeps-only-vocabulary.md`; the move of the knot model and `Continuity` stands)
- Date: 2026-11-06
- Related: `../design/math-base-layer.md`, `../design/geometry-module.md` 2.3 / 7, `ADR-2026-11-06-geom-detail-subtree.md`, `ADR-2026-11-04-bspline-without-knot-breaks.md`

## Background

`math/spline/` had held two things since it was created: `BezierBlend.hpp` (`deCasteljau` / `differences`, Bernstein evaluation) and `BSplineBasis.hpp` (the knot model of distinct knots plus multiplicities, with the Cox-de Boor basis functions). Their only reason for being in `math` was "there was no other place at the time", and `ADR-2026-11-06-math-is-the-base-layer.md` redefined `math` as "the base layer, not only a maths library" to that end - **a page that has to defend a name is itself a signal of misplacement** (`G4`).

The criterion then changed: `geom/planar` / `geom/spatial` merged into one module with a `geom/detail` subtree, so "where does shared geometry code go" had an answer, while "shared mathematics" still had only `math`. Once separated, the question became clear:

| Header | Content | Does the signature mention our own data model? | Consumers |
|---|---|---|---|
| `BezierBlend.hpp` | A set of values plus a scalar, Bernstein blending and finite differences | No | `BezierCurve` / `BezierSurface` in both trees |
| `BSplineBasis.hpp` | `flatKnotCount` / `knotAt` / `knotSpan` / `isClampedKnotVector` / `knotContinuity` / `basisValue` | **Yes**: distinct knots plus multiplicities is the NURBS representation we chose, and `knotContinuity` answers the geometry interface's `Continuity` directly | `BSplineCurve` / `BSplineSurface` in both trees |

The same check applied to `math/Continuity.hpp`: the only files in the repository that include it directly are `geom/planar/Curve.hpp`, `geom/spatial/Curve.hpp` and `geom_global.hpp` - its only consumers are geometry too.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the status quo | No change | Keep using a page to defend the name `math`; the model behind `basisValue` lives in the "mathematics" layer, while `math` has no second consumer |
| B. Move the whole `spline/` directory into `geom/detail` | One rule explains everything | It also moves the purely mathematical Bernstein evaluation: animation or resampling later would have to depend on the curve library (a reversed `G2`) |
| C. Split by "does the signature mention our data model": `BSplineBasis` -> `geom/detail`, `BezierBlend` stays in `math/spline`; `Continuity` -> `geom/Continuity.hpp` | Each layer holds only what it understands; neither header needs its signature rewritten | `math/spline` keeps one header; `Continuity` changes its include path |
| D. Keep `math` purely mathematical: `basisValue` takes a **flat** knot vector and geometry flattens it | Model and evaluation are fully separated | Every evaluation would flatten once (an allocation), exactly the cost that choosing "distinct plus multiplicity" avoided (`G1` yielding to `G10`) |

## Decision

Chose **C**:

- `BSplineBasis.hpp` -> `src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp`, namespace `vn::geom::detail`. It is dimension-independent (it does not name `Point2` / `Point3`), which matches the existing rule for `geom/detail`; its consumers are inside the module only, so `detail` is an honest label.
- `Continuity.hpp` -> `src/base/geom/sdk/vine/geom/Continuity.hpp`, namespace `vn::geom` (the module root, **not** `detail`): it appears in the pure virtual signatures of `Curve<T>` / `Surface<T>`, so it is public vocabulary; `geom_global.hpp` re-exports it into both dimension namespaces (`geom/planar` / `geom/spatial` are **siblings** of `geom` rather than sub-namespaces, so the import is required and not a convenience).
- `BezierBlend.hpp` stays in `src/base/math/sdk/vine/math/spline/`: pure evaluation, no model; if a non-geometric Bernstein consumer appears later, this decision is confirmed. <!-- drift-ok -->
- `math/numeric` does not move: root finding and integration are processes where "the caller supplies the precision target", any module might need them, and `basisValue` is not that kind of thing.
- One implementation constraint: `Continuity.hpp` writes `VN_ROOT_NS_BEGIN namespace geom {` directly rather than going through `VN_GEOM_NS_BEGIN` - `geom_global.hpp` has to include it for the re-export, and using its own macro would create a cycle where neither side is declared when the other is included.

Criteria: `G1` (one owner per concept: the knot representation belongs to geometry), `G2` (pure mathematics stays in the base layer and geometry concepts no longer occupy it), `G4` (the names need no documentation defence), `G10` (no per-evaluation flattening), `G13` (the representation convention travels with the geometry type that stores it). The reverse check of `G8`: `math/spline` keeps one header, but it was not pre-built for the future - it is Bernstein evaluation, and it has a consumer today.

## Consequences

- Good: `math` holds no geometry concept any more; `geom` brings all its own geometry vocabulary (`Continuity`) and representation conventions (the knot model), so the module's self-sufficiency for spline representation no longer depends on "a coincidence of the base layer"; `knotContinuity` and the `continuity()` that uses it are in one module.
- Cost / migration: `<vine/math/spline/BSplineBasis.hpp>` -> `<vine/geom/detail/BSplineBasis.hpp>`, `math::spline::` -> `geom::detail::` (3 headers, 26 call sites); `<vine/math/Continuity.hpp>` -> `<vine/geom/Continuity.hpp>` (3 headers include it directly, the rest through `geom_global.hpp`); `vn::math::Continuity` -> `vn::geom::Continuity` (6 tests, 13 occurrences); `tests/test_math/BSplineBasisTest.cpp` -> `tests/test_geom_spatial/` (`test_math` links only `vn::Math` and no longer knows this header). <!-- drift-ok -->
- Reversal condition: a **non-geometric** consumer of the knot model appears (robot trajectory interpolation constructing a B-spline directly, for example) - then either give it another shared place, or promote it together with `vn::Nurbs`; when a second family of representation conventions appears, the `vn::Nurbs` trigger written in `ADR-2026-11-06-math-is-the-base-layer.md` still applies.

## Evidence

- `src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp`, `src/base/geom/sdk/vine/geom/Continuity.hpp`; `src/base/math/sdk/vine/math/spline/` keeps only `BezierBlend.hpp`. <!-- drift-ok -->
- `geom_global.hpp`: two `using geom::Continuity;` lines and the three module-level macros `VN_GEOM_NS_BEGIN` / `VN_GEOM_DETAIL_NS_BEGIN`.
- Tests: `test_math` 56 -> 49 (7 B-spline basis cases moved), `test_geom_spatial` 193 -> 200, `test_geom_planar` unchanged at 138; 357 green together.
- Gate: `check_geom_layering.py` reports 0 violations over 67 geometry headers (`geom/detail` includes neither dimension tree).
