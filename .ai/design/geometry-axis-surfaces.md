# geometry surfaces of revolution and trimming

> **Status**: current
> **Last verified**: 2026-11-04
> **Scope**: the frame and parameterisation of the surfaces of revolution (cylinder / cone / torus), the rectangular trimmed surface, and the data model and evaluation of the two-parameter spline surfaces (Bézier / B-spline).
> Not the `Surface<T>` interface itself (see `geometry-kinds.md` 2.10), not the family membership (see 2.3 of the same page), not the intersection / distance / tessellation algorithms.
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/Placement.hpp`, `Frame.hpp`, `CylindricalSurface.hpp`, `ConicalSurface.hpp`, `ToroidalSurface.hpp`, `RectangularTrimmedSurface.hpp`, `BezierSurface.hpp`, `BSplineSurface.hpp`; `src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp`, `BezierBlend.hpp`; `tests/test_geom_spatial/GeometryKindsTest.cpp`
> **Backlog**: `../backlog/geometry.md`

## 1. Overview

### 1.1 Problem and background

The cylinder, the cone and the torus have in common not "unbounded" but that they all **turn around an arbitrary axis**: the design has to answer two questions first — where the placement is stored, and where the rule "project a preferred direction into the plane orthogonal to the normal" lives. The curve side and the plane side already each hold one such rule (`ConicCurve::inPlaneAxis` and `Plane::xAxis`).

### 1.2 Goals

- The three surfaces of revolution can be constructed and evaluated, and answer the thirteen items through a `const Surface<T>&`.
- The two spline surfaces can be constructed and evaluated, sharing one knot / basis implementation with `BSplineCurve`.
- The frame and `continuity()` have one home; the "axis in a plane" rule is shared with the curve family and the plane.
- The trimmed surface is a view and copies no basis surface.

### 1.3 Non-goals

- Do not move `SphericalSurface` into the frame: its axis only changes the parameterisation (2.2).
- Do not give any surface a `bounds()`: support on a rectangle needs solving, the pole-hull box is a conservative fake value, and there is no consumer today (`G8`).
- No intersection, tessellation, joining or wrapped knot vectors for the spline surfaces; only the clamped form is described.

### 1.4 One-paragraph conclusion

**The surfaces of revolution hand the frame to the value type `Placement` and the "axis in a plane" rule to `Frame.hpp`; the trimmed surface is a view of a basis surface; the two spline surfaces are value types over a row-major flat control net whose evaluation reuses the `BezierBlend` / `BSplineBasis` shared with the curves.**

## 2. Design Points

### 2.1 The frame is the value type `Placement`, not an inheritance layer

- **What**: `Placement<T>` (`Placement.hpp`) is "one right-handed coordinate system": `origin`, `axis` (the unit main direction) and `x_direction` (the start of `u`), with `yAxis() = axis × x_direction` derived and `hasUsableAxis()` reporting usability. Each of the three surfaces holds one `placement` field and keeps only its own scalars (radius / reference radius + half angle / major + minor radius).
- **Why**: `G1`, `G6`, `G12` — the frame's fields and derived rule have one home; and **adding an inheritance layer for one group of fields and one `return CN` is the over-design `G8` rejects**, so the frame is a value type and `continuity()` is one line each. The type tree therefore matches OCCT (`ElementarySurface` → concrete class, see 2.6).
- **Boundary**: `ElementarySurface` stays an empty tag (`C9`); `Placement` is not layered with the type tree and carries no virtual function; `dynamic_cast` is no longer needed — holding a concrete surface means reading `placement` directly.

### 2.2 Whether the placement is stored follows from "does it change the point set"

- A cylinder / cone / torus around another axis is another surface, so the placement must be stored; the sphere's axis only changes the parameterisation and not the point set, so `SphericalSurface` keeps the world Z axis and holds no `Placement` (`G1`, `G8`).
- **Cost**: the sphere's parameter start differs from OCCT's sphere (noted in the header); each of the three surfaces of revolution gains three vectors, and the size assertions in `geometry-kinds.md` 2.2 follow.

### 2.3 "Axis in a plane" has one implementation

- `inPlaneAxis(preferred, normal)` in `Frame.hpp`: project the preferred direction into the plane orthogonal to the normal and normalise; when the projection is zero, take the world axis least parallel to the normal — a deterministic rule that needs no stored state.
- The conic curves, the plane and the surfaces of revolution share this one implementation (`G1`); `ConicCurve` has no `inPlaneAxis` of its own, which was a source-level break (no caller in the repository).

### 2.4 The trimmed surface is a view

- `RectangularTrimmedSurface<T>` holds a `std::shared_ptr<const Surface<T>>` and four parameter bounds and forwards all thirteen items; closedness / period are decided jointly by "the basis surface is closed / periodic in that direction" and "the rectangle covers the whole span / at least one period" (`G7`, `G10`). Closedness additionally requires that **the two ends really land on the same boundary**: on a periodic basis surface the span must be an integer number of periods, so a rectangle that covers the whole span but walks half a period further must answer false; on a non-periodic closed basis surface (a spline patch) there is no repeatable period, so the rectangle must use exactly the basis surface's own two natural ends, and a rectangle extrapolated beyond them answers false (the same rule as `TrimmedCurve`'s coincident endpoints; both require "coverage + both ends on the same point / boundary").

### 2.5 The spline surfaces reuse the curve-side blending and basis, and write no second copy

- **What**: `BezierSurface` / `BSplineSurface` are both `BoundedSurface<T>` value types: a row-major flat pole array + `u_pole_count` / `v_pole_count` (index `iu + u_pole_count * iv`, `u` contiguous), with an optional equal-length weight array (empty = polynomial). Evaluation is the outer product of two one-dimensional passes: first blend every row into one point along `u`, then blend those points along `v`; the rational form does the same double blend in four-dimensional homogeneous coordinates and divides by the weight at the end.
- **The two shared pieces of infrastructure** (both in `geom::detail`, one copy for the two geometry modules): `BSplineBasis.hpp` (distinct knots + multiplicities → flat index, `knotSpan`, `basisValue`, `isClampedKnotVector`, and `knotContinuity` for the "multiplicity → `Continuity` ladder") and `BezierBlend.hpp` (de Casteljau and finite differences, taking only "a group of values + a scalar"). The surface side holds no second knot scan, basis recursion or continuity ladder (`G1`, C3).
- **Degree and counts have one home**: `BezierSurface`'s degree is the count minus one and is therefore not stored; `BSplineSurface` stores two degrees, because the knot vector does not imply them.
- **Closedness is a data fact**: the **homogeneous** poles of the first and last row / column being equal point by point in both directions is closedness. `BSplineSurface` decides from the data too — in the clamped form the boundary curve at `v = v_min` is the curve of the first pole row, and the clamped basis is linearly independent, so the curves are equal if and only if the poles are equal, with no evaluation and comparison needed (`G10`).
- **`continuity()`**: Bézier is always `CN` (a polynomial within the patch); a B-spline takes the **worse** of `C(degree − largest interior multiplicity)` in the two directions, with a side that has no interior knot counting as `CN` — which is exactly why a B-spline surface is one type (`G1`).
- **Cost**: each evaluation allocates a small temporary array sized by the control count (`deCasteljau` takes the array by value, `differences` returns a new one), unlike the allocation-free recursion of `BSplineCurve`; optimise when the point count makes it a hot path, see `../backlog/geometry.md` G10.

### 2.6 Alignment with OCCT, and where we differ

- **OCCT has no `Placement` class and no `AxisSurface`**: it puts the placement `gp_Ax3 pos` into `Geom_ElementarySurface`, all five elementary surfaces (plane, cylinder, cone, sphere, torus) carry it, and it exposes `Axis()` / `Location()` / `Position()` / `SetPosition()` uniformly; the other branch that carries a frame is `Geom_SweptSurface` (`basisCurve` + `gp_Dir direction`).
- **What aligns**: `placement.origin` = OCCT's `Location()` (a point on the axis), `placement.axis` = the main direction of `Axis()`, `placement.x_direction` = `gp_Ax3.XDirection`; the parameterisation, parameter domain, closedness / period in both directions, `CN`, and the convention "the normal comes from `D1U × D1V`" align item by item (see `ElSLib::CylinderValue` / `ConeValue` / `TorusValue` and their `Bounds`).
- **Where we differ, and why**:
  - The frame is not in the tag: `C9` requires `ElementarySurface` to be stateless, so it is a value-type member (2.1); OCCT can put it in the base class because its surfaces are reference-counted heap objects where three extra vectors do not change the copy cost, while ours are value types (`G10`).
  - The cost is losing OCCT's uniform "ask any elementary surface for `Position()`": holding the concrete type means reading `placement`, holding `ElementarySurface<T>&` does not. When such a consumer really appears, consider `dynamic_cast` first (consistent with D6, "the vtable answers what I am"), then talk about widening the interface.
  - The spline surfaces describe the **clamped form** only (first and last multiplicity = degree + 1 in both directions): OCCT's `Geom_BSplineSurface` also accepts unclamped and periodic knots, which is a deliberate narrowing (`G8`: no consumer today), and `isValid()` rejects the unclamped form.
  - `Plane` stores no placement: it is copied by value on the picking hot path and derives its two axes from `normal` each time (`geometry-kinds.md` 2.10); OCCT's `Geom_Plane` stores a `gp_Ax3`, a deliberate difference (`G10`).
  - `SphericalSurface` keeps the world Z axis and measures `v` from +Z (`[0, π]`), where OCCT uses `gp_Ax3` and measures `v` from the equator (`[-π/2, π/2]`); the reason is in 2.2 and `geometry-kinds.md` 2.10.
  - No mutable API (`SetPosition` / `UReverse` / `VReverse`): a value type is immutable, and reversal is expressed by constructing a mirror (`geometry-kinds.md` 2.7 / D4).

## 3. Features

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Analytic surfaces of revolution | evaluation, both first derivatives, parameter domain and closed period in both directions, outward normal for `CylindricalSurface` / `ConicalSurface` / `ToroidalSurface` | landed |
| F2 | Rectangular trimmed surface | forwards to the basis surface; closedness / period decided by basis and rectangle together; `isValid()` rejects an empty basis and a reversed interval | landed |
| F3 | Two-parameter spline surfaces | evaluation, both first derivatives, parameter domain, closedness in both directions and `continuity()` for `BezierSurface` / `BSplineSurface` | landed |

### 3.1 F1 Analytic surfaces of revolution

- **Behaviour**: all three share the shape "radial vector × scalar + axial vector × scalar", differing only in how the radius varies with `v`: cylinder `r = R`; cone `r = R + v·sinα` with axial `v·cosα`; torus `r = R + r₀·cos v` with axial `r₀·sin v`. The parameter convention follows OCCT: for cylinder / cone `u` is the azimuth and `v` is the axial distance and the **generatrix arc length** respectively; both torus parameters are angles.
- **Constraint** (C1): `u` is closed and periodic in `[0, 2π]` for all three; cylinder / cone `v` is unbounded and not closed, torus `v` is also closed and periodic; a degenerate axis collapses the parameterisation onto a segment along `x_direction` and makes `isValid()` false (the same rule as `ConicCurve`).
- **Acceptance** (T1): `aCylindricalSurfaceWrapsItsAxis`, `aConicalSurfaceWalksItsGeneratrix`, `aToroidalSurfaceClosesInBothDirections`.

### 3.2 F2 Rectangular trimmed surface

- **Behaviour**: the four parameter bounds answer directly; `isUClosed()` / `isVClosed()` require the basis surface to be closed in that direction, the rectangle to cover its whole span, and the two ends to land on the same boundary (on a periodic basis surface = the span is an integer number of periods; on a non-periodic one = exactly its two natural ends); `isUPeriodic()` / `isVPeriodic()` require the rectangle to cover at least one period. The two are independent facts: a rectangle spanning 1.5 periods is periodic but not closed.
- **Constraint** (C2): an empty basis surface and a reversed rectangle are both reported by `isValid()`; `evaluate()` outside the rectangle forwards to the basis surface and does not clamp.
- **Acceptance** (T1): `aRectangularTrimmedSurfaceForwardsToItsBasis`.

### 3.3 F3 Two-parameter spline surfaces

- **Behaviour**: `BezierSurface` evaluates a tensor-product Bézier on `[0, 1]²`, `BSplineSurface` sums over the clamped knot vector in both directions; both first blend every row into one point along `u` and then blend along `v` (2.5). `derivativeU` / `derivativeV` use the same row / column differences, and the rational form evaluates in four-dimensional homogeneous coordinates and differentiates by the quotient rule. `continuity()`: Bézier is always `CN`, a B-spline takes the worse of the two directions.
- **Constraint** (C3, C4): blending and basis come from `BezierBlend.hpp` / `BSplineBasis.hpp`, the surface side rewrites neither; the control net is row-major flat; outside the domain Bézier extrapolates and B-spline clamps to the endpoint (the same rule as their curves); when `isValid()` is false, `evaluate()` returns the first pole instead of reporting an error.
- **Acceptance** (T3): `aBezierPatchBlendsItsNet`, `aRationalBezierPatchPullsTowardsItsHeavierCorner`, `aBSplinePatchSumsItsTwoBasisFamilies`.

## 4. Public Interface

| Header | Content |
|---|---|
| `Placement.hpp` | the right-handed frame value type: `origin` / `axis` / `x_direction`, `yAxis()`, `hasUsableAxis()` |
| `CylindricalSurface.hpp` / `ConicalSurface.hpp` / `ToroidalSurface.hpp` | the three analytic surfaces (one `Placement` each), their `pointAt` / `normalAt` / `isValid` and the `*f` / `*d` aliases |
| `RectangularTrimmedSurface.hpp` | basis-surface handle + four parameter bounds and the `*f` / `*d` aliases |
| `BezierSurface.hpp` / `BSplineSurface.hpp` | the two spline surfaces: row-major flat control net + optional equal-length weights, `degreeU()` / `degreeV()`, `poleAt(iu, iv)`, `isValid()` and the `*f` / `*d` aliases |
| `BezierBlend.hpp` / `BSplineBasis.hpp` | the two pieces of infrastructure shared with the curves: de Casteljau blending + finite differences; distinct knots + multiplicities → `knotSpan` / `basisValue` / `isClampedKnotVector` / `knotContinuity` |
| `Frame.hpp` | the shared `inPlaneAxis(preferred, normal)` (used by the curve family, the plane and the surfaces of revolution) |

## 5. Constraints, Requirements and Invariants

- **C1**: the frame is defined once in `Placement` and normalised at construction; each of the three surfaces holds one `placement` field, may not declare `origin` / `axis` / `x_direction` fields of its own, and may not rewrite it after construction.
- **C2**: closedness / period of a trimmed surface is a fact of "basis surface + rectangle" together and may not be read from one side only; closedness additionally requires "both ends on the same boundary", which on a periodic basis surface means the span is an integer number of periods and on a non-periodic one means exactly its two natural ends. An empty basis and a reversed rectangle are reported by `isValid()`.
- **C3**: evaluating a spline surface must reuse the curve-side shared implementation (the knot scan / basis of `BSplineBasis.hpp`, the de Casteljau and differences of `BezierBlend.hpp`) and may not be rewritten on the surface side.
- **C4**: the control net is a row-major flat array indexed `iu + u_pole_count * iv`; the two counts, the net length and the weight length must agree, checked by `isValid()` rather than left to the caller's discipline.
- **R1**: `evaluate` / `derivativeU` / `derivativeV` / `isValid` of the three analytic surfaces allocate nothing, throw nothing and do scalar arithmetic only.
- **R2**: `evaluate` and the two first derivatives of a spline surface throw nothing, but each call allocates a temporary array sized by the control count (the cost in 2.5); `isValid()` allocates nothing.
- **N1**: `axis` and `x_direction` stay unit length and mutually orthogonal after construction, so `yAxis()` is a unit vector too; rewriting the three fields afterwards makes them contradict each other (the reading of C1).
- **N2**: `pointAt(u, v)` and `evaluate(u, v)` are always the same value; `normalAt` always points away from the axis (for the torus, away from the tube centre), i.e. along `derivativeU × derivativeV`, and does not flip with the parameters.
- **N3**: when `isValid()` of a spline surface is true, both counts are ≥ 2, the net length is the product of the counts, the weights are empty or of equal length and all positive, and for `BSplineSurface` the first and last multiplicity is degree + 1 in both directions with strictly increasing distinct knots; when it is false, geometric queries follow the degenerate rule (`evaluate()` returns the first pole, `isUClosed()` / `isVClosed()` are false) and nothing throws.

## 6. Caveats and Traps

- **P1 The `v` of a cylinder / cone and the `v` of a torus are not the same quantity.** How it fails: treat `v` as "height" everywhere, so a cone's `v` becomes an axial length and a torus's `v` a radius. How to detect: `derivativeV().length()` is 1 on a cylinder (axial) and 1 on a cone (generatrix arc length), and the minor radius on a torus. How to prevent: pass parameters in the dimension the header states, and do not guess from a field name.
- **P2 The closedness of a trimmed surface is neither the closedness of the basis surface nor "covers the whole span".** How it fails: because the basis is a cylinder, assume the trimmed patch is closed too and treat a quarter cylinder as a full turn for seam decisions; or because the rectangle covers the whole span but walks half a period more, assume the two boundary edges coincide. How to detect: `isUClosed()` returns false while the basis returns true, or corresponding points on the two u edges are not at distance zero. How to prevent: ask the trim itself for closedness / period (C2); do not derive a conclusion from "basis closed + covers the whole span".
- **P3 Getting the row-major index direction backwards.** How it fails: treat the net as column-major (`iv + v_pole_count * iu`) and get the transposed surface; a square net looks perfectly right, a `2×3` silently becomes `3×2`, and a single row / column hides it. How to detect: whether `poleAt(0, 1)` equals `poles[u_pole_count]`. How to prevent: read the net only through `poleAt(iu, iv)`, and read the private `offset()` before indexing by hand (C4).
- **P4 The `uPeriod()` / `vPeriod()` of a rectangular trim is not the basis surface's period.** How it fails: because the basis is a cylinder, assume the trimmed patch is 2π periodic too and normalise parameters with it; a quarter cylinder then becomes a full turn. How to detect: `isUPeriodic()` / `isVPeriodic()` false while `uPeriod()` / `vPeriod()` are 0 (the basis surface's answers are true / 2π). How to prevent: ask the trim itself for the period, and read the public `basis` when the basis surface's number is wanted (the same rule as `TrimmedCurve` on the curve side, `geometry-kinds.md` 2.7 / P8).

## 7. Rejected Options

- **D1** Making the frame an inheritance layer (`AxisSurface`) or stuffing it into the `ElementarySurface` tag: the first adds an inheritance layer for one group of fields and one `return CN`, the second violates `C9`; nor does `SphericalSurface` migrate — the sphere's axis only changes the parameterisation (2.2). See `../decisions/ADR-2026-11-04-surface-placement-value-type.md` (supersedes `ADR-2026-11-04-axis-surface-frame.md`).
- **D2** Storing a spline surface's control net as `vector<vector<Point>>`: one layer of pointers buys no expressiveness, and the row / column index either costs an extra indirection or implies a length convention (`G10`, `G12`); the net is row-major and flat (C4).
- **D3** Deciding `BSplineSurface` closedness by "evaluate and compare the two boundary curves": in the clamped form the boundary curve at `v = v_min` is the curve of the first pole row, and the clamped basis is linearly independent, so equal homogeneous poles and equal curves are equivalent (2.5); comparing by evaluation is only slower and brings a tolerance question into equality (`G10`).

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | parameterisation, derivatives, domain and closedness, degenerate input of the three surfaces of revolution; forwarding, four invalid inputs and integer-period closedness of the trim | `aCylindricalSurfaceWrapsItsAxis`, `aConicalSurfaceWalksItsGeneratrix`, `aToroidalSurfaceClosesInBothDirections`, `aRectangularTrimmedSurfaceForwardsToItsBasis`, `aTrimmedSurfaceClosesOnlyWhenItsEdgesMeet` in `tests/test_geom_spatial/GeometryKindsTest.cpp` |
| T2 | membership and size | the `static_assert`s at the top of the same file (`sizeof(CylindricalSurfaced)` and two more, `sizeof(Placement<double>) == 3 * sizeof(Point3d)`, both spline surfaces are `BoundedSurface` and not abstract) and `aPlacementCompletesAndNormalisesItsFrame` |
| T3 | evaluation, rational form, `continuity()` and invalid nets of the spline surfaces | `aBezierPatchBlendsItsNet` (bilinear net with midpoint `(0.5, 0.5, 0.5)`, corner interpolation, `derivativeU` / `derivativeV`), `aRationalBezierPatchPullsTowardsItsHeavierCorner` (`2×2` weights `{1,1,1,3}`, midpoint = `(2/3, 2/3)`), `aBSplinePatchSumsItsTwoBasisFamilies` (a bilinear B-spline equals the Bézier of the same net, `derivativeU(0,0) = (2,2,0)` on a quadratic `apron`, an interior multiplicity of 2 drops `continuity()` to `C0`) |
| T4 | family-level surface contract, period and empty-basis behaviour of the trim | `everySurfaceKeepsTheInterfacePromises` (each of the eight surfaces asked in turn for an ordered domain, period and period value agreeing and repeating by the period, `isCN` agreeing with `continuity()`, both first derivatives finite on a finite rectangle) and the `uPeriod()` and empty-basis assertions inside `aRectangularTrimmedSurfaceForwardsToItsBasis` |

## 9. Status and Evolution

- The three surfaces of revolution, the rectangular trimmed surface and the two spline surfaces are all present; the frame is the value type `Placement`, one per surface of revolution.
- Shared infrastructure: `BSplineBasis.hpp` also provides `knotContinuity()`, so the multiplicity → `Continuity` ladder exists once and both curves and surfaces call it (an extension of C3).
- Known gaps: spline evaluation allocates a temporary array (R2, trigger in `../backlog/geometry.md` G10), only clamped knot vectors are supported, and there is no `bounds()`.
- Compatibility: there is no `AxisSurface` layer, the frame is the `Placement` value type; there is no `ConicCurve::inPlaneAxis` (a source-level break, no caller in the repository); `BSplineCurve`'s private `basisValue` / `findSpan` / flat index scan live in `BSplineBasis.hpp` (private interface, not a break).

## 10. References

- `geometry-kinds.md` 2.3 (family membership), 2.10 (the `Surface<T>` interface), 2.9 (the isomorphic approach of `ConicCurve`)
- `../decisions/ADR-2026-11-04-surface-placement-value-type.md` (the frame is a value type)
- `../decisions/ADR-2026-11-04-axis-surface-frame.md` (superseded)
- OCCT `Geom_CylindricalSurface` / `Geom_ConicalSurface` / `Geom_ToroidalSurface` / `Geom_RectangularTrimmedSurface` / `Geom_BezierSurface` / `Geom_BSplineSurface` and `ElSLib::CylinderValue` / `ConeValue` / `TorusValue` (source of the parameter conventions)
