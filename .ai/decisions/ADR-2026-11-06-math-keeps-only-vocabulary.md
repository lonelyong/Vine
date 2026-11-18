# ADR-2026-11-06: `math` keeps only mathematical vocabulary; root finding, integration and Bernstein evaluation move to `geom/detail`

- Status: Accepted (supersedes `ADR-2026-11-06-shared-numeric-kernels-in-math.md`; the "Bernstein stays in `math`" half of `ADR-2026-11-06-spline-model-moves-to-geom.md` is superseded by this ADR as well)
- Date: 2026-11-06
- Related: `../design/math-base-layer.md`, `../design/geometry-module.md` 2.6 / 7, `ADR-2026-11-06-geom-detail-subtree.md`, `ADR-2026-11-06-spline-model-moves-to-geom.md`

## Background

Earlier the same day, the B-spline knot model and `Continuity` moved from `math` into the geometry module, on the criterion "does the signature mention our own data model". That move left two remnants:

- `math/numeric/` (`adaptiveSimpson`, `rootInBracket`, `parameterReaching`): the signatures mention **no** geometry model, so by the previous criterion they should stay in `math`.
- `math/spline/BezierBlend.hpp` (`deCasteljau`, `differences`): likewise no model.

So `math` became "18 vocabulary headers plus two subdirectories used only by geometry": no geometry model, but no second consumer either - `brep`, `robotics`, `meshio`, `graphics`, `models` and `loaders` contain no root finding, integration or Bernstein evaluation at all.

**The judgement had moved to a different axis here**: not "does the signature mention our model" (`numeric` and `BezierBlend` do not), but **"who consumes this capability today, and whose concept does it serve"**. Root finding and integration really are general mathematics, but in this repository their only use is "geometry tolerance -> parameter tolerance" and "integrate speed along a curve"; keeping the whole geometry numeric chain inside one module is worth more than keeping three functions in the base layer, at the price of the base layer losing a capability.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep `math/numeric` and `math/spline` | The general capability stays in the base layer, usable by any future module | `math` always has two subdirectories "used only by geometry"; a reader has to read the ADR to learn why they are there |
| B. Move everything into `geom/detail` | `math` keeps only vocabulary, so name and content agree; the geometry numeric chain (root finding -> tolerance conversion -> arc length / closest point / intersection) is all inside one module, and changes need not cross modules | Any future non-geometric consumer (a new solver, trajectory interpolation, resampling) would depend on `vn::Geom` for one integral; `Quadrature` / `RootFinding` drop from public API to `detail` |
| C. Create an independent `vn::Numeric` module (between `math` and `geom`) | General processes get their own layer reachable by both consumers | A module for three headers; one more level between `math` and `geom`, and `geom` would link two libraries |

## Decision

Chose **B**:

- `Quadrature.hpp`, `RootFinding.hpp`, `BezierBlend.hpp` -> `src/base/geom/sdk/vine/geom/detail/`, namespace `vn::geom::detail`.
- `math` keeps 18 root-layer headers (value types, constants and predicates, concepts, `EigenDecomposition`, `math_global`) and **no subdirectories at all**.
- All three are treated as `detail`: their consumers are the geometry curves and surfaces plus `geom/detail`'s own helpers (`narrowCrossing` uses `rootInBracket`, `arcLengthBetween` / `parameterCovering` use `adaptiveSimpson` / `parameterReaching`, Bezier curves and surfaces use `deCasteljau` / `differences`).
- Migration: `tests/test_math/{QuadratureTest,RootFindingTest}.cpp` -> `tests/test_geom_spatial/` (`test_math` links only `vn::Math` and no longer knows these headers).

Criteria: `G1` (one home per capability), `G5` (changes to the geometry numeric chain land inside the geometry module), `G6` (`math` and `geom` each hold one kind of content: vocabulary / geometry), `G4` (the names no longer need a documentation defence). The tension with `G2` is **recorded honestly and written below as a cost**: this moves a general capability up out of the base layer, reversing the judgement of `ADR-2026-11-06-shared-numeric-kernels-in-math.md` from earlier the same day. B was chosen because "there is no second consumer today, and two homes cost more than one", not because "generality does not matter".

## Consequences

- Good: the name and the content of `math` finally agree; the geometry numeric chain no longer crosses modules (root finding, tolerance conversion, integration and the inverse solve all live in `geom/detail`); `narrowCrossing` and its kernel are in the same namespace, so call sites need no qualifier.
- **Cost (explicitly accepted)**: any non-geometry module that wants integration or root finding has exactly three roads - depend on `vn::Geom`, copy it, or promote the kernels at that point (option C). The third is the natural reversal path of this decision: **when a second consumer appears, promote `Quadrature` / `RootFinding` to an independent module instead of settling for `geom`**.
- Cost (label): the three headers are `detail` now and promise no stability. They still have their own headers and tests, but "publicly dependable" no longer holds.
- Migration list: `<vine/math/numeric/*.hpp>`, `<vine/math/spline/BezierBlend.hpp>` -> `<vine/geom/detail/*.hpp>`; `vn::math::numeric::` / `math::spline::` -> `geom::detail::` (9 geometry headers, 83 occurrences); two test files change suite.
- Reversal condition: any non-geometric consumer of root finding, integration or Bernstein evaluation. Then follow option C, and the `geom` side only changes its includes, not its signatures.

## Evidence

- `src/base/geom/sdk/vine/geom/detail/`: `Quadrature.hpp`, `RootFinding.hpp`, `BezierBlend.hpp` (new) plus the 5 earlier headers, 8 in total; `src/base/math/sdk/vine/math/` holds 18 headers and no subdirectory.
- Consumer count for `math`: the non-geometry references of `adaptiveSimpson` / `rootInBracket` / `parameterReaching` / `deCasteljau` / `differences` number **0**.
- Tests: `test_math` 49 -> 36 (7 files), `test_geom_spatial` 200 -> 213, `test_geom_planar` unchanged at 138; 387 green together.
- Gate: `check_geom_layering.py` reports 0 violations over 70 geometry headers (`geom/detail` includes neither dimension tree).
