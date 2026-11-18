# ADR-2026-11-05: a planar arc is a value type, and a general trimmed view does not coexist with it

- Status: Superseded (by `ADR-2026-11-05-planar-trimmed-curve.md`: the trimmed view landed and `ArcCurve` was deleted)
- Date: 2026-11-05
- Related: `../design/geometry-planar.md`, `../design/geometry-kinds.md`

## Background

"one piece of a curve" had a different shape in each module: on the spatial side it is `TrimmedCurve<T>` - a `std::shared_ptr<const Curve<T>>` basis plus two parameter bounds, a view; on the planar side it was `ArcCurve` - a `CircleCurve` held by value plus a start and an end angle.

Once the planar module landed, the question had to be answered: add a general `TrimmedCurve<T>` so that elliptical arcs and trimmed splines can be expressed, or not?

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep `ArcCurve` only | Zero allocation, the domain rules implemented and tested in place, and the module's "a value type does not own memory" exception is down to the splines | Only circles can be trimmed; the interface shape differs from 3D's `TrimmedCurve` |
| B. Replace `ArcCurve` with `TrimmedCurve<CircleCurve>` | The same shape as 3D; any basis curve can be trimmed later | One heap allocation per construction (type erasure needs `shared_ptr`); the existing public surface of `ArcCurve` is deleted |
| C. Keep both | Both needs are met | "a piece of a circle" has two types, two rules and two sets of tests - one concept, two definitions |

## Decision

Chose A. `TrimmedCurve<T>` is **not** added next to `ArcCurve`. When a non-circular basis needs trimming (elliptical arcs, trimmed splines), land option B: introduce `TrimmedCurve<T>` and **delete** `ArcCurve`, never C.

The criteria are `G1` (one concept, one definition) and `G10` (count the real cost): when "circle plus interval" has only one expression, a general view buys no new capability and hands every arc one heap allocation and a new R2 exception; and `ArcCurve` already implements and tests the four rules of closure, periodicity, reversal and out-of-domain behaviour, which in 3D's trimmed view are corollaries of "basis plus interval" and in a value type have to be written out - the two shapes carry the same set of facts, stored differently.

`G8` also supports A: nothing today needs to trim an ellipse or a spline, so the benefit of option B is hypothetical.

## Consequences

- Good: `ArcCurve` allocates nothing, has a fixed size, and states its domain rules (for `isClosed` the interval must cover the circle's domain, and a shorter piece answers `period() == 0`) in its own documentation, so a reader need not derive "basis plus interval"; the R2 exception is only the two splines.
- Bad / cost: the planar side has no `TrimmedCurve<T>`, so "trim any curve" cannot be written today; two concepts of the same name have different interface shapes (a 2D value type against a 3D view), which matters when porting code.
- Trigger (when to reopen): the first consumer that needs to trim a non-circular basis (elliptical arcs, trimmed splines, truncating an imported profile). Then do B: create `TrimmedCurve<T>`, delete `ArcCurve`, and rewrite 2.8 of `geometry-planar.md` as "trimming is a view".

## Evidence (Historical record)

- `src/base/geom/sdk/vine/geom/planar/ArcCurve.hpp`: the basis circle held by value, with the four rules `isClosed` / `isPeriodic` / `period` / `reversedParameter` on the type (the file was deleted with this ADR's successor).
- `src/base/geom/sdk/vine/geom/spatial/TrimmedCurve.hpp`: a `shared_ptr` basis plus an interval, the view version of the same facts.
- `tests/test_geom_planar/ArcCurveTest.cpp`: seven cases covering closure, a half turn, reversal, evaluation outside the interval and a degenerate basis circle (migrated to `TrimmedCurveTest.cpp`).
- `../design/geometry-planar.md` 2.8 and D6.
