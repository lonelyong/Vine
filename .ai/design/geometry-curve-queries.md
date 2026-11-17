# geometry curve queries and measures: nearest point, arc length and curvature

> **Status**: current
> **Last verified**: 2026-11-05
> **Scope**: nearest point / distance from a point to **any** `Curve<T>` in `src/base/geom/sdk/vine/geom/spatial`, the nearest distance between two curves, arc length and its inverse, curvature, and the window rules and costs.
> Not curve × surface intersection (see `geometry-intersection.md`) and not point to surface (see `geometry-surface-queries.md`).
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/CurveProjection.hpp`, `CurveLength.hpp`, `CurveCurvature.hpp`; `tests/test_geom_spatial/CurveDistanceTest.cpp`, `CurveLengthTest.cpp`, `CurveCurvatureTest.cpp`.
> **Backlog**: `../backlog/geometry.md` G3 (solid intersection)

## 1. Overview

### 1.1 Problem and background

`Curve<T>` is an evaluation interface (`geometry-kinds.md` 2.7): a caller holding a `const Curve<T>&` can take a point, take the first / second derivatives and ask for the parameter domain, but **cannot ask "where is the closest point to this one"**. An entry per type would cover the line, the four conics, the trimmed view and the two splines — eight types — and a new curve would need it again (the counter-example to `G9`); and the surface side already has `closestPointOnSurface()`, so the curve side is the empty half of an asymmetric pair.

### 1.2 Goals

- One free-function entry, parameterised on the interface, requiring neither a concrete type nor an analytic form.
- When the parameter domain is unbounded the caller supplies a window and the function chooses no range — the same rule as curve × surface and surface projection.
- One result answers "where is the nearest point", "what is its parameter" and "how far is it".
- The same interface answers two scalar questions: the arc length over a window (and its inverse) and the curvature at a point.
- The nearest pair of points and the distance between two curves: the same interface and the same window rule.

### 1.3 Non-goals

- No equidistant reparameterisation (reparameterising a whole curve by arc length): today the answer is "parameter for a given length", producing no new curve type.
- No extension of the `Curve<T>` interface: no `closestPoint()`, no `bounds()`.

### 1.4 One-paragraph conclusion

**The nearest point is a stationary point of the squared distance, that is a zero of `f(u) = (C(u) - P) · C'(u)`; sampling finds the sign-changing intervals, `rootInBracket()` narrows them, and the result is compared with the window ends to take the smallest distance. Arc length is the integral of the speed and the inverse is that integral inverted; curvature is the length of the cross product of velocity and acceleration divided by the speed cubed.**

## 2. Design Points

### 2.1 A free function with a "derived from `Curve<T>`" constraint

- **What**: `closestPointOnCurve(curve, query, ...)` is a header template with `C` constrained by `requires std::is_base_of_v<Curve<T>, C>`.
- **Why**: `G1`, `G9` — the algorithm depends only on `evaluate` / `derivative` / the domain / `isValid`; as a free function rather than an interface virtual, "the relation between two objects" fits, and a hot path need not pay a vptr for it.
- **Why not a `const Curve<T>&` parameter**: `CircleCurve` derives indirectly through `ConicCurve<T>`, and template deduction cannot see through an indirect base; a constrained template inside a named concept is rejected by MSVC (C7605). Putting the constraint in the function's `requires` clause satisfies both (the same reason as `geometry-surface-queries.md` 2.1).

### 2.2 The caller supplies the window, under the same rule as intersection

- **What**: `std::optional<std::pair<T, T>> window` — an interval of curve parameters. When omitted a bounded curve uses its own domain; an unbounded curve (line, hyperbola, parabola) **must be given one**, and it must be finite and ordered.
- **Why**: `G4`, `G6` — exactly the rule of the curve window of `intersectCurveWithSurface()` (`geometry-intersection.md` C5), so a caller does not have to remember two conventions for two entries.
- **Cost**: an unbounded curve gains one parameter; a bounded curve is unaffected.

### 2.3 Root finding on the stationary points, not direct minimisation

- **What**: `f(u) = (C(u) - P) · C'(u)` is the derivative of the squared distance; its zeros are the nearest point, the farthest point and the other stationary points. The window is scanned, each sign-changing interval is narrowed with `geom::detail::rootInBracket()`, and the result is compared with every sample point and the two window ends to take the smallest distance.
- **Why**: `G1`, `G10` — the root finder is already there (curve × surface uses it) and the stationary equation needs the first derivative only; the window ends must take part in the comparison, because when the foot falls outside the window `f` has no zero inside it and the minimum is on the boundary.
- **Why not golden-section / ternary search**: that needs a bracket containing the minimum first, which still comes from sampling; `rootInBracket()` already exists, is bracket-safe, and on an analytic curve converges to the exact foot (a segment and a circle both solve exactly) rather than to a simplex-search approximation. See §7 D2.
- **Boundary**: it calls only `evaluate()` and `derivative()`; it needs neither an analytic form nor the second derivative (the stationary equation is already first order).

### 2.4 The result has the same shape as on the surface side

- **What**: `CurveProjection<T>{ point, parameter, distance }`, whose fields correspond one to one with `SurfaceProjection<T>{ point, u, v, distance }`, with the two parameters replaced by one.
- **Why**: `G6`, `G4` — the answer to the same kind of question has the same shape in different families, so a caller does not remember two sets of field names; the distance and the nearest point are still two halves of one extremum and are not split into two entries (`G1`).
- **Boundary**: the parameter has the curve's own dimension (a distance for a line, an angle for a circle, a knot value for a spline) and is not normalised.

### 2.5 Cost and locality

- **What**: `sample_count + 1` samples (one `evaluate` and one `derivative` each), and at most 64 narrowing steps per sign-changing interval.
- **Why**: `G10`, `G11` — the same skeleton as surface projection: grid seeding plus bracket-safe narrowing; it allocates nothing and throws nothing.
- **Cost**: the answer is a **local** optimum; a flat minimum (`f` does not change sign) is only found when a sample lands on it; when the resolution is too low, raise `sample_count`. All of this is in §6.

### 2.6 Arc length and its inverse: two free functions shaped like the planar side

- **What**: `arcLength(curve, window, tolerance, max_depth)` integrates `|C'(u)|` with adaptive Simpson; `parameterAtLength(curve, arc_length, window, tolerance, max_depth)` inverts the same integral with a bisection-guarded Newton.
- **Why**: `G1`, `G9`, `G6` — length and "parameter for a given length" are one-dimensional measures of a curve and belong to no concrete type; the numeric kernels (`geom::detail::adaptiveSimpson()` and `geom::detail::parameterReaching()`) are one layer shared inside the module, and **2D and 3D use the same implementation**, with the two namespaces supplying only the two values "speed" and "distance already walked", so the two sides cannot drift in meaning (`../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`).
- **Boundary**: `tolerance` is a **length** error, not a parameter error; `arc_length` must be non-negative and not exceed the window total (beyond tolerance the answer is empty); the window rule is exactly that of `closestPointOnCurve()` (C1).
- **Cost**: each inverse iteration integrates once more, so the cost is the iteration count times one integral.

### 2.7 Curvature: a spatial curve has no sign

- **What**: `curvature(curve, u) = |C'(u) x C''(u)| / |C'(u)|^3`, non-negative; the radius of curvature is its reciprocal.
- **Why**: `G4` — a spatial curve has no natural "left / right" and the sign is undefined; the planar side has a plane orientation, so there it is called `signedCurvature()`. The names state this rather than leaving a convention for the caller to remember.
- **Boundary**: undefined where the speed is zero, answering `std::nullopt`; it depends on the first two derivatives only. Embedded in 3D, a planar curve's 3D curvature equals the absolute value of its planar signed curvature.
- **Cost**: no extra type; a direction comes from the planar signed version.

### 2.8 Nearest distance between two curves: sampling + projection on one side + two-dimensional Gauss-Newton

- **What**: `curveToCurveDistance(first, second, first_window, second_window, tolerance, sample_count)` returns `CurveCurveDistance<T>{ first_point, second_point, first_parameter, second_parameter, distance }`. The first curve is sampled, each sample is projected onto the second with `closestPointOnCurve()` (exact for the sample), and the pair is refined with `(J^T J) [du, dv] = -J^T r` (`J = [C1', -C2']`), keeping the smallest distance.
- **Why**: `G12`, `G4`, `G9` — "the relation between two curves" belongs to neither, so it is a free function; the result carries both points, both parameters and the distance, so the caller does not evaluate again and `distance` cannot contradict the points. Reusing the nearest point and the window rule means the eight types need not each write it.
- **Boundary**: a local method — with several near approaches the answer depends on whether the sampling lands in each valley; where the pair is locally parallel (`J^T J` singular) the sampled pair is kept, and the sampled projection is still exact; the window rule is C1. When the curves touch or cross the distance may be 0 (the scan or the refinement lands on the contact).
- **Cost**: one nearest-point query per sample, so the cost is `sample_count ×` nearest point; no new interface item.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Nearest point / distance from a point to a curve | `closestPointOnCurve(curve, query, window, tolerance, sample_count)`: stationary-point root finding + comparison with the ends, returning the point, the parameter and the distance | landed (T1–T4) |
| F2 | The window is a hard constraint | when the foot falls outside the window the answer is the nearer end, with no extrapolation | landed (T2) |
| F3 | Nearest distance between two curves | `curveToCurveDistance()`: sampling + projection on one side + two-dimensional Gauss-Newton, returning two points / two parameters / the distance | landed (T7) |
| F4 | Arc length and its inverse | `arcLength()` / `parameterAtLength()`: adaptive Simpson over |C'| (`geom::detail::adaptiveSimpson`) + a bisection-guarded Newton (`geom::detail::parameterReaching`) | landed (T5) |
| F5 | Curvature | `curvature()`: `|C' x C''| / |C'|^3`, non-negative, empty where the speed is zero | landed (T6) |

## 4. Public Interface

| Header | Content |
|---|---|
| `CurveProjection.hpp` | `CurveProjection<T>` and `closestPointOnCurve()` |
| `CurveLength.hpp` | `arcLength()` and `parameterAtLength()` |
| `CurveCurvature.hpp` | `curvature()` |
| `CurveCurveDistance.hpp` | `CurveCurveDistance<T>` and `curveToCurveDistance()` |

The full signatures and the meaning of every parameter are in the headers.

## 5. Constraints, Requirements and Invariants

- **C1**: the search interval must be finite. An unbounded curve must be given a window by the caller; a missing, reversed or zero-width one returns `std::nullopt`, with no range chosen by the function.
- **C2**: a tolerance is a **geometric** quantity; when converted to a parameter tolerance it uses `tolerance / |dP/du|`, falling back to a fraction of the sample step where the curve comes to rest.
- **R1**: nothing is allocated and nothing throws; unusable input returns an empty answer.
- **R2**: only `evaluate` / `derivative` / the two domain endpoints / `isValid` of `Curve<T>` are used.
- **N1**: `CurveProjection::point == curve.evaluate(parameter)`; `distance == |point - query|`.
- **N2**: among candidates at the same distance the smallest parameter wins (for instance every point of a circle is equidistant from the centre, and the first sample is reported).
- **N3**: `arcLength()` is a **path length** (backtracking included); `parameterAtLength()` measures from the window start, and `arcLength(curve, {a, u})` equals the requested length within tolerance.
- **N4**: `curvature()` is non-negative; a circle answers the same value at two points, and a spatial curve has no "left / right".
- **N5**: the three equalities of `CurveCurveDistance` hold together: `first_point == first.evaluate(first_parameter)`, `second_point == second.evaluate(second_parameter)`, `distance == |first_point - second_point|`.

## 6. Caveats and Traps

- **P1 A window that is too narrow.** How it fails: the foot lies outside the window, the answer falls on an end and is taken as "the nearest point". How to detect: the parameter equals a window end and `f` still has the same sign beyond it. How to prevent: estimate the window first from bounds or geometric intuition (C1).
- **P2 A local minimum.** How it fails: the curve approaches the query point repeatedly and the sampling finds only one of the minima, so the distance is too large. How to detect: doubling `sample_count` makes the distance smaller. How to prevent: raise `sample_count` to the scale of the curve's undulation.
- **P3 A flat minimum has no bracket.** How it fails: `f` touches zero at the minimum without changing sign, `rootInBracket()` has no interval to narrow, and the minimum is found only if a sample lands on it. How to detect: the distance barely changes near a sample while the parameter is imprecise. How to prevent: sample densely; when an exact parameter is needed, run a separate minimum search on that stretch.
- **P4 Sampling aliasing.** How it fails: the curve oscillates faster than the scan interval, two stationary points fall into one cell and their sign changes cancel. How to detect: doubling `sample_count` produces a smaller distance. How to prevent: set the scan density to the scale of the undulation.
- **P5 Treating an arc length as a parameter.** How it fails: handing the result of `arcLength()` straight to `evaluate()`. How to detect: the point is in the wrong place, and changing the parameterisation changes the result again. How to prevent: use `parameterAtLength()` when a parameter is wanted.
- **P6 Assuming a 3D curvature has a sign.** How it fails: using the sign of `curvature()` to judge the direction of bending. How to detect: every input returns non-negative, and walking the same point set backwards does not change the value. How to prevent: a direction comes from the planar `signedCurvature()` (which needs a plane orientation), or from comparing the acceleration with the normal yourself.
- **P7 Treating the two-curve nearest distance as global.** How it fails: two curves approaching in several places have the sampling and refinement land in only one valley and the distance comes out too large. How to detect: raising `sample_count` makes the distance smaller. How to prevent: sample to the scale of the undulation; when a strict global answer is needed, split into stretches first and take the smallest.

## 7. Rejected Options

- **D1** Adding `closestPoint()` / `bounds()` to `Curve<T>`: every implementation writes its own iteration and the interface grows from "evaluation" to "evaluation + solving" (`G7`, `G8`); with the algorithm left in a free function an implementer answers evaluation only.
- **D2** Golden-section / ternary search minimising the distance directly: it needs a bracket containing the minimum first (still from sampling), converges only linearly, and on an analytic curve still gives an approximation; stationary-point root finding reuses the existing `rootInBracket()` and gives the exact foot on a segment and a circle (`G1`, `G10`).
- **D3** Splitting the distance and the nearest point into two entries (`distanceToCurve()` + `projectOnCurve()`): one extremum computed twice, and a caller learns two names (`G1`, `G4`).
- **D4** Newton on the second derivative: `f'(u) = |C'|² + (C - P)·C''` is available, but Newton is not guaranteed to stay inside the interval and has no ready bracket protection; `rootInBracket()` already meets the need, and the second derivative is left to the consumers that really need curvature.
- **D5** Making arc length or curvature a virtual of `Curve<T>`: the same reason as D8 on the planar side (`G1`, `G8`) — they are measures of a curve, not the identity of one class, and on the interface every implementation would write them again.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | line foot and unusable input | `aPointProjectsOntoALineCurveAtItsFoot`, `unusableInputsAnswerNothing` in `tests/test_geom_spatial/CurveDistanceTest.cpp` |
| T2 | an unbounded curve must be given a window, a foot outside it falls on an end | `anUnboundedCurveNeedsAWindow`, `aFootOutsideTheWindowFallsOnTheNearerEnd` in the same file |
| T3 | nearest point on a circle and a spline | `aPointProjectsOntoACircle`, `aPointProjectsOntoASplineCurve` in the same file |
| T4 | equidistant solutions and both precisions | `theCentreOfACircleReportsOneOfItsEquidistantPoints`, `bothPrecisionsFindTheSameFoot` in the same file |
| T5 | arc length and its inverse: a line by its speed, a circle by its radius, a spline agreeing with a dense polyline, an unbounded curve without a window rejected, the interior inverse and both precisions | `tests/test_geom_spatial/CurveLengthTest.cpp` |
| T6 | curvature: 0 on a line, 1/r on a circle (including float), the two axis ends of an ellipse, empty at a cusp and on an invalid curve, non-negativity | `tests/test_geom_spatial/CurveCurvatureTest.cpp` |
| T7 | two-curve distance: parallel lines, line × circle, two circles, 0 when they cross, the two parameters agreeing with the points, an unbounded curve without a window, invalid curves, both precisions | `tests/test_geom_spatial/CurveCurveDistanceTest.cpp` |

## 9. Status and Evolution

- `closestPointOnCurve()`, `curveToCurveDistance()`, `arcLength()`, `parameterAtLength()` and `curvature()` cover the line, the conics, the splines and the trimmed view (all through the interface only).
- Same names and shapes as the planar side: arc length and its inverse are the same, curvature is unsigned in 3D (the planar side has `signedCurvature()`), and the two-curve distance only swaps the point for a `Point3`.
- Known gaps: the nearest point and the nearest distance are both **local** searches, so a curve with several valleys needs dense enough sampling (P2, P7); a flat minimum follows §6 (P3).
- Compatibility: purely additive — four new headers, two new types and four new free functions; `Curve<T>` and every value-type signature are unchanged.

## 10. References

- `geometry-kinds.md` 2.7 (the `Curve<T>` interface and `isValid()`)
- `geometry-intersection.md` 2.7 / C5 (the window rule and the use of `rootInBracket()`)
- `geometry-surface-queries.md` (the same problem on the surface side and the same shape of constraints)
- `../decisions/ADR-2026-11-04-surface-family-queries.md` (the decision for free functions + constraints + numeric solving)
