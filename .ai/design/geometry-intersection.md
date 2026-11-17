# geometry intersection pairings and result shapes

> **Status**: current
> **Last verified**: 2026-11-04
> **Scope**: the **entry convention** for "intersect two objects" in `geometry` (whose member it is, what it is called, what the result looks like) and the current list of pairings.
> Not the numeric algorithm inside each pairing (in the headers and `src/`), not "why the algorithm hangs on the intersected object" (`geometry-primitives.md` 2.8, `geometry-ray.md` 2.2), not the scene semantics of picking (`graphics-scene.md`).
> **Sources**: `intersectWith` / `intersects` / `intersectTriangle` of `Plane.hpp`, `LineCurve.hpp`, `Aabb.hpp`, `Obb.hpp`, `Ray.hpp`; `tests/test_geom_spatial/LinePlaneTest.cpp`, `RayTest.cpp`.
> **Backlog**: `../backlog/geometry.md` G11

## 1. Overview

### 1.1 Problem and background

There are nine intersection entries, spread over five value types and one free function. Their **algorithm position** is uniform (the intersected object owns the algorithm), but they do not read uniformly:

- three result shapes — `bool` + a `Point3` output, `bool` + a `RayHit` output, and returning an `Intersection` structure directly;
- two verbs — `intersectWith` (produces geometry) and `intersects` (decides only);
- "can the output be read when `false`" has no written rule, and the two entries answer differently: `Plane`'s ray entry does not write `hit` on `false`, while `Aabb × Aabb` writes `out` as an empty box from which the gap can be read.

So "how do two things intersect" has no predictable answer, and a new pairing has no test to pass.

### 1.2 Goals

- One predictable rule: **where the entry lives, what it is called, what the result looks like** — all three written here, not explained separately in each header.
- The nine existing entries satisfy the rule without a signature change (one shape exception, see 2.3).
- A new pairing needs no discussion of abstraction, only the convention of §5.

### 1.3 Non-goals

- No unified interface / virtual `Intersectable` tag (D1).
- No compile-time pairing matrix (D2).
- No filling in of the combinations missing from the pairing table (the rows marked "missing" in §3): no consumer today (`G8`), see `../backlog/geometry.md` G11.

### 1.4 One-paragraph conclusion

**What is abstracted is the vocabulary and the rules, not the dispatch: the algorithm stays on the intersected object, the result shape follows from "what dimension can the intersection have", and a missing pairing waits for a consumer.**

## 2. Design Points

### 2.1 Verbs: `intersectWith` produces geometry, `intersects` only decides

- **What**: `intersectWith(...)` returns `bool` and the intersection goes through the last output parameter; `intersects(...)` returns `bool` and produces no geometry (today only `Obb::intersects(const Obb&)`).
- **Why**: `G4` — a predicate and a query are named apart, so a caller need not build an unused result just to ask "did they touch"; a SAT decision needs no intersection at all.
- **Boundary**: a new pairing starts with `intersectWith`; add `intersects` only when a consumer **explicitly** wants a boolean (no result type) — adding both is two homes (`G1`).

### 2.2 The result shape follows from "what dimension can the intersection have"

| Intersection | Result type | Example |
|---|---|---|
| 0-dimensional, point only | `Point3<T>` output | `Plane × LineCurve` |
| 0-dimensional + the ray's arrival information | `RayHit<T>` (`t` / `point` / a `normal` facing the ray) | `Aabb` / `Obb` / `Plane` × `Ray`, `intersectTriangle` |
| 1-dimensional | `LineCurve<T>` output | `Plane × Plane` |
| 0-dimensional / empty / 1-dimensional, and classification needed | a named result structure | `Intersection` of `LineCurve × LineCurve` |

- **Why**: `G4` — the result type is the shape of the answer; `RayHit` carries two items a `Point3` cannot (distance, facing normal) that "where did the ray hit" should have, while a `Point3` cannot hold the intersection of two boxes at all.
- **Boundary**: `RayHit` belongs to the **ray family** vocabulary (the normal-orientation rule picking needs is in `geometry-ray.md` 2.3) and must not represent the result of a non-ray pairing. A new pairing may not introduce a fourth shape — no `std::optional`, no `std::variant`, no exception (D3).

### 2.3 The one shape exception: `LineCurve::Intersection`

- The intersection of two lines may be **a point, the empty set (skew) or the whole line (collinear)**, and it must also answer parallel and the nearest distance; four states cannot be expressed by `bool + output` — hence a named result (`is_intersected` / `is_parallel` / `is_collinear` / the two parameters / distance).
- This is not a leftover shape but the other half of the rule in 2.2: **when the dimension of the intersection is not fixed and the classification is itself part of the answer, return a structure**. A future pairing of the same kind (for instance `Plane × Plane` if it also had to separate coplanar / parallel / intersecting) follows the same method.
- **Cost**: a caller reads a structure rather than one output parameter (the reverse cost of `G7`: squeezing the classification into `bool` plus two extra outputs reads worse).

### 2.4 What an output means on `false` is stated per entry, no longer guessed

- **Rule**: a geometric result is **read only when `true` is returned** (every `RayHit`-style entry follows this: on `false` `hit` keeps the caller's value).
- **The one exception**: `Aabb::intersectWith(const Aabb&, Aabb<T>& out)` writes `out` on `false` as an empty box from which the **gap** can be read — information its consumer (distance / rejection) needs, stated in the header.
- **Why**: `G4`, `G11` — "is the output defined" must be readable at the entry, or a caller either writes an extra `if` or reads the leftover value of the previous query.

### 2.5 Whose member the entry is

- **The ray family belongs to the intersected object**: `Aabb` / `Obb` / `Plane` / triangle (a free function, because it has no type). The reason is that picking walks along the intersected object (`geometry-ray.md` 2.2).
- **Symmetric with no dominant side**: a free function, in the module both share — a future `Aabb × Obb` and `Obb × LineCurve` fall under this rule.
- **A symmetric pairing needing a four-state result**: a member function (`LineCurve × LineCurve`), because hanging it on either side works equally and a member is chosen only so that `line.intersectWith(other)` can be found (`G12`).

### 2.7 Curve × surface: implicit root finding, not one special case per pair

- **What**: any curve × any **analytic** surface is solved by one template entry: `intersectCurveWithSurface(curve, surface, window, tolerance, interval_count)`. The surface's **signed distance** is sampled along the curve as `f(u) = signedValue(surface, curve.evaluate(u))`, and the sign-changing intervals are narrowed with `geom::detail::rootInBracket()`; a zero is a crossing.
- **Why**: `G9`, `G1` — a special case per pair would be a matrix of 8 curve types × 5 surface types; and "is the point on the surface" is already answered by each surface's implicit equation, so the root finder needs only a one-dimensional `f(u)`. A new curve changes nothing here, and a new surface only adds a `signedValue()`.
- **Cost**: it holds only for a surface **with an implicit equation** (all five analytic surfaces have one); a spline or trimmed surface does not, and that half is covered by projection in `intersectCurveWithSurfaceByProjection()` (2.9).
- **Why the root finder lives in `geom/detail`**: `G1`, `G5` — `rootInBracket()` knows only "a continuous scalar function + a bracket", but its only consumer today is geometry, and the geometry numeric chain (root finding → tolerance conversion → integration / inverse) is cheaper kept together than split in two layers; `math` therefore keeps mathematical vocabulary only. The cost and the reversal condition (promote it to a module when a second consumer appears) are in `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.
- **Why a safe bracketing false position rather than pure Newton**: Newton cannot tell which root it will land on and diverges on a flat stretch; false position with a bisection fallback (bisect when the estimate falls inside 10% of either bracket end) guarantees convergence while keeping the fast first steps. Tangency is still missed (P4), and that is written in the header rather than left for the caller to guess.

### 2.8 Precision: one `T`, no mixed precision

- Both sides of every entry have the same type parameter; `Aabb::intersectWith(const Ray<U>&, ...)` states this explicitly with `requires std::is_same_v<T, U>`.
- **Why**: `G4` — a mixed-precision crossing silently picks a scale for the tolerance; the camera ray and the vertices of `graphics` are the same precision anyway (`geometry-ray.md` 2.4).

### 2.9 Curve × non-analytic surface: projection as the implicit function

- **What**: `intersectCurveWithSurfaceByProjection(curve, surface, curve_window, surface_window, tolerance, interval_count)` shares the scan of the analytic entry (`detail::scanCurveAgainstSurface`) and only replaces `f(u)` with the signed distance obtained by projection (`detail::signedDistanceToSurface`): the magnitude is the nearest-point distance and the sign comes from `dS/du × dS/dv`.
- **Why**: `G9`, `G1` — a spline or trimmed surface has no implicit equation, but **both can be evaluated**; "projection + parametric normal" is an implicitisation any evaluable surface can answer. The scan, the coincidence classification, the duplicate removal and the parameter tolerance are extracted from the analytic entry into one shared implementation, and the two paths differ only in the value they read.
- **Boundary**: the **polarity of the sign belongs to the parameterisation** (for a sphere the `u`/`v` cross product points inwards, so the sign is the opposite of `signedValue()`), which is why it exists only as `detail` — root finding cares about a sign change only, and the polarity is irrelevant; a concrete number comes from the concrete type's `signedValue()`. When the nearest point lands on a singular parameter line (a pole) there is no normal, the value is NaN, and the scan treats it as "no information".
- **Cost**: every sample is a full projection (seeding grid + Newton), far more expensive than `signedValue()`; when the nearest point jumps between two branches it creates a spurious sign change, so after `rootInBracket()` the point is accepted only if `|f| ≤ tolerance` (P8).

## 3. Features

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Line × plane | `Plane::intersectWith(const LineCurve<T>&, Point3<T>&, eps)`: a 0-dimensional point, parallel → `false` | landed (T1) |
| F2 | Plane × plane | `Plane::intersectWith(const Plane<T>&, LineCurve<T>&, eps)`: a 1-dimensional line; both parallel and **coplanar** → `false` (N1) | landed (T2) |
| F3 | Ray × plane | `Plane::intersectWith(const Ray<T>&, RayHit<T>&, eps)`: a hit behind the ray → `false` | landed (T3) |
| F4 | Line × line | `LineCurve::intersectWith(const LineCurve&, eps)` → the four states of `Intersection` (intersecting / parallel / collinear / skew) + the nearest distance | landed (T1) |
| F5 | Box × box | `Aabb::intersectWith(const Aabb<T>&, Aabb<T>&)`: on `false`, `out` is the **gap box** (the one exception in 2.4) | landed (T4) |
| F6 | Oriented box × oriented box | `Obb::intersects(const Obb<T>&)`: SAT, producing no intersection | landed (T5) |
| F7 | Ray × box / oriented box / triangle | `RayHit`: origin inside the box → `t = 0` with the normal facing the ray; `Obb` transforms into local space and reuses `Aabb` | landed (T3) |
| F8 | The combinations missing from the pairing table | ray–line, line–box, plane–box, oriented box–box | not done (`G11`) |
| F9 | Curve × **any** surface (family level) | `intersectCurveWithSurfaceByProjection(curve, surface, curve_window, surface_window, tolerance, interval_count)`: roots of the projected signed distance along the curve; shares the scan and coincidence classification of the analytic entry | landed (T10) |
| F10 | A unified intersection interface | a virtual `Intersectable` / a template pairing matrix | not done (D1, D2) |
| F11 | Curve × analytic surface | `intersectCurveWithSurface(curve, surface, window, tolerance)`: roots of the surface's signed distance `signedValue()` along the curve, giving every crossing | landed (T6) |
| F12 | Curve lying on the surface | as above; three consecutive samples lying on the surface → `CurveSurfaceContact::Coincident`, no points listed | landed (T7) |
| F13 | Ray × **any** surface (family level) | `intersectRayWithSurface(ray, surface, window, ...)`: grid seeding + a three-variable Newton, the result embedding a `RayHit`; the algorithm and cost are in `geometry-surface-queries.md` | landed (T9) |

### 3.1 The entry list and the result shapes

- **What F1 / F2 / F3 have in common**: a point, a line and a ray hit are the first three rows of the table in 2.2; all three can be obtained from the same plane equations, but the **result types differ**, so they cannot share one entry.
- **Why F4 returns a structure**: the result dimension is not fixed and the classification is itself part of the answer (2.3).
- **Why F5 produces a box and F6 gives only a boolean**: the intersection of two boxes is still a box (it fits in `Aabb`), while the intersection of two oriented boxes is generally a polyhedron and `geometry` has no type to hold it — rather than return an approximate box, it answers only "did they touch".

| Intersected | Object | Entry | Result | Note |
|---|---|---|---|---|
| `Plane` | `LineCurve` | `Plane::intersectWith(const LineCurve<T>&, Point3<T>&, eps)` | 0-dimensional point | F1 |
| `Plane` | `Plane` | `Plane::intersectWith(const Plane<T>&, LineCurve<T>&, eps)` | 1-dimensional line | F2 |
| `Plane` | `Ray` | `Plane::intersectWith(const Ray<T>&, RayHit<T>&, eps)` | `RayHit` | F3 |
| `LineCurve` | `LineCurve` | `LineCurve::intersectWith(const LineCurve&, eps)` | `Intersection` | F4 |
| `Aabb` | `Aabb` | `Aabb::intersectWith(const Aabb<T>&, Aabb<T>&)` | box | F5 |
| `Aabb` | `Ray` | `Aabb::intersectWith(const Ray<U>&, RayHit<U>&, eps)` (`U == T`) | `RayHit` | F7 |
| `Obb` | `Obb` | `Obb::intersects(const Obb<T>&)` | `bool` | F6 |
| `Obb` | `Ray` | `Obb::intersectWith(const Ray<T>&, RayHit<T>&, eps)` | `RayHit` | F7 |
| triangle | `Ray` | `intersectTriangle(ray, a, b, c, hit)` | `RayHit` | F7 |
| any curve | the five analytic surfaces | `intersectCurveWithSurface(curve, surface, window, tolerance)` | `CurveSurfaceIntersection` (classification + hit set) | F11 / F12 |
| any curve | any `Surface<T>` | `intersectCurveWithSurfaceByProjection(curve, surface, curve_window, surface_window, ...)` | `CurveSurfaceIntersection` (classification + hit set) | F9 |
| `Ray` | any `Surface<T>` | `intersectRayWithSurface(ray, surface, window, ...)` | `RaySurfaceIntersection` (`coincident` + ascending `RaySurfaceHit`) | F13 |
| `Ray` | `LineCurve` | — | — | F8 |
| `LineCurve` | `Aabb` / `Obb` | — | — | F8 |
| `Plane` | `Aabb` / `Obb` | — | — | F8 |
| `Aabb` | `Obb` | — | — | F8 |
| curve / surface family | any | — | — | F9 |

## 4. Public Interface

| Header | Content |
|---|---|
| `Plane.hpp` | three entries: × line (a point), × plane (a line), × ray (`RayHit`) |
| `LineCurve.hpp` | × line → `Intersection` (four states + two parameters + nearest distance) |
| `Aabb.hpp` / `Obb.hpp` | × box / × ray; `Obb::intersects` is the only decision-only entry |
| `Ray.hpp` | `intersectTriangle` (a free function, inlined in the header, see `geometry-primitives.md` 2.8) |
| `Plane.hpp` / `SphericalSurface.hpp` / `CylindricalSurface.hpp` / `ConicalSurface.hpp` / `ToroidalSurface.hpp` | one `signedValue(surface, point)` overload each: the signed distance, zero on the surface |
| `CurveSurfaceIntersection.hpp` | `CurveSurfaceContact`, `CurveSurfaceHit<T>`, `CurveSurfaceIntersection<T>` and the two header templates: `intersectCurveWithSurface` (implicit equation) and `intersectCurveWithSurfaceByProjection` (any evaluable surface) |
| `geom/detail/RootFinding.hpp` | `rootInBracket(function, lower, upper, tolerance)`: the root in a bracket, safe false position + bisection fallback; `parameterReaching(value, slope, …)`: the parameter at which a monotone quantity reaches the target |

## 5. Constraints, Requirements and Invariants

- **C1**: there are two verbs only: producing geometry → `intersectWith` (`bool` + output), deciding only → `intersects`.
- **C2**: the result type is decided by the intersection dimension and the need for classification (2.2); no fourth shape, no `optional`, no `variant`, no exception.
- **C3**: a geometric result is read only when `true` is returned, and a `RayHit`-style entry must not write its output on `false`; the gap box of `Aabb × Aabb` is the exception stated in the header.
- **C4**: both sides of one entry have the same precision; no implicit cross-precision entry.
- **R1**: every entry allocates nothing and throws nothing; the result comes from scalar arithmetic only (long algorithms in `src/`, short queries in the headers, see `geometry-primitives.md` 2.8).
- **N1**: `Plane × Plane` returns `false` when coplanar rather than "returning the whole plane" — a 2-dimensional intersection has no result type (2.2), and a caller decides from the normals and the distance when it needs to.
- **C5**: the window of a curve × surface entry is given by the caller: a bounded curve may omit it (its own domain is used), an **unbounded curve must be given one**, and the window must be finite; a missing window returns `None`, with no range chosen by the function.
- **C6**: tangency (no sign change) is not reported; `None` means "no crossing was found", which includes the grazing case (P4).
- **C7**: a stretch of curve lying on the surface returns `Coincident` with `hits` cleared — infinitely many points are not a point set and cannot be listed.
- **C8**: the geometric tolerance and the parameter tolerance differ by a factor `|dP/du|`; when `|dP/du| ≈ 0` (a cusp, a pole) a fraction of the sample step is used instead, never a direct division.
- **C9**: a projected sample may have no sign (a singular parameter line → NaN); NaN is not a sign change and does not count as "on the surface", it only means "no information here".

## 6. Caveats and Traps

- **P1**: treating `RayHit` as a general "intersection". How it fails: using it for the result of `Plane × LineCurve` applies the normal-orientation rule of `geometry-ray.md` 2.3 to a scene with no ray. How to detect: the result type carries `t` but there is no ray. How to prevent: the table in 2.2 — a non-ray pairing uses `Point3`.
- **P2**: reading a `RayHit` on `false`. How it fails: the leftover hit of the previous query is used for a normal decision. How to detect: `hit.t` disagrees with the return value that was ignored. How to prevent: C3 — look at the return value first.
- **P3**: assuming the `out` of `Aabb × Aabb` is meaningful only when intersecting. How it fails: on `false` it is discarded as uninitialised and the gap box is lost. How to detect: the header says "on disjoint it is the gap between the two boxes". How to prevent: the exception is written in 2.4.
- **P4**: missing tangency (curve × surface). How it fails: a curve grazing a surface (sphere tangency, cylinder tangency) does not change the sign of `signedValue()`, so `None` is reported while the picture shows a crossing. How to detect: the nearest distance is ≈ 0 but there is no hit. How to prevent: when tangency matters, find the extrema of `f` (`f'(u) = 0`) and test `|f| ≤ tol`, which is another algorithm; not doing it by default is stated in the header's `@warning`.
- **P5**: sampling aliasing. How it fails: the curve oscillates faster than the scan interval, some crossings fall in one interval and their sign changes cancel, so roots are missed. How to detect: doubling `interval_count` produces more crossings. How to prevent: set the interval to the scale of the curve's undulation, and do not handle a highly oscillating curve with the default.
- **P6**: choosing the wrong window. How it fails: too small a window on an unbounded curve means roots outside it are absent from the result, presenting as "no crossing". How to detect: doubling the window produces new roots. How to prevent: estimate a window first from the ray or the bounding box (3.4 / C5).
- **P7**: the parameter tolerance failing at a singularity. How it fails: near a cone apex or a pole `|dP/du| → 0`, so `tol / |dP/du|` explodes or divides by zero and the bisection "converges" to a wrong parameter. How to detect: the iteration count hits the cap, or the crossing is off. How to prevent: the fallback branch of C8 — a fraction of the sample step as the parameter tolerance.
- **P8**: the projection's sign jumps at a branch of the nearest point. How it fails: when the curve passes behind a trim boundary the nearest point jumps from one boundary to the other, `f` changes sign discontinuously, and `rootInBracket()` narrows to the jump and reports a crossing that is not on the surface. How to detect: the reported `point` is farther from the surface than `tolerance`. How to prevent: the scan checks `|f| ≤ tolerance` again after narrowing before recording (implemented), so a jump is dropped rather than becoming a false crossing.
- **P9**: the projection's sign has no value on a singular parameter line. How it fails: a curve passing exactly through the sphere's axis has its outer point nearest the pole, `dS/du × dS/dv` is zero, the value is NaN, and a crossing through the pole is missed. How to detect: only the curve passing exactly through the axis misses; a curve off the axis is fine. How to prevent: use the analytic entry for a surface with an implicit equation; when a trimmed or spline surface is crossed at a singular parameter line, narrow the window to a region without the singularity before solving.

## 7. Rejected Options

- **D1** A virtual `Intersectable` / tag plus a single `intersect(const Intersectable&)`: a pairing is a **binary relation** a one-argument interface cannot express; it also adds a hot-path vptr to `Aabb` / `Obb` / `Ray` (`G10`) and forces a caller to learn a second dispatch style (`G7`).
- **D2** A template `intersects<A, B>` concept matrix: long algorithms such as `Plane::intersectWith(Plane, LineCurve&)` live in the DLL's `src/` (`geometry-primitives.md` 2.8), a template cannot cross the DLL boundary, and all that would remain is a thin forward plus an N×M compile-time matrix (`G10`, `G7`).
- **D3** Returning `std::optional<Result>` everywhere / throwing: every existing entry in the repository is `bool` + output (`geometry-ray.md` 2.3), and one more shape means a caller learns two at once (`G6`, `G4`).
- **D4** Filling in every pairing marked "missing" in §3 now: no consumer (`G8`); write it down and wait for the trigger (G11).
- **D5** Adding `inverse()` to `Surface<T>` / making the implicit equation a precondition of family-level intersection: the first writes iteration into every implementation (the same reason as D1), the second excludes the spline and trimmed surfaces, which are exactly what this half has to cover (`G9`). See `../decisions/ADR-2026-11-04-surface-family-queries.md`.

## 8. Tests and Acceptance

| # | Criterion | Case |
|---|---|
| T1 | line × line / line × plane | `skewLineCurvesReportTheClosestPair`, `parallelAndCollinearFlags`, `lineCrossesThePlaneAtOnePoint`, `parallelLineCurveHasNoIntersection` in `tests/test_geom_spatial/LinePlaneTest.cpp` |
| T2 | plane × plane (including parallel and coplanar) | `twoPlanesMeetAlongALineCurve`, `coincidentPlanesReportNoIntersectionLineCurve` in the same file |
| T3 | ray × plane / box / triangle (including origin inside, parallel, back face, degenerate) | `tests/test_geom_spatial/RayTest.cpp` |
| T4 | box × box (including the gap box of a non-intersection) | `intersectWith` in `tests/test_geom_spatial/AabbTest.cpp` |
| T5 | oriented box × oriented box (SAT, including touching and separation) | `IntersectsIncludingTouchingAndRejectsSeparatedBoxes` in `tests/test_geom_spatial/ObbTest.cpp` |
| T6 | curve × analytic surface | `aLineCrossesAPlaneWhereTheAnalyticAnswerSays` (aligned point by point with the analytic entry), `aCircleCrossesAnOffCentreSphereTwice`, `aLineThroughATorusMeetsItFourTimes` (both precisions), `anUnboundedCurveNeedsAWindow`, `unusableInputsAnswerNothingInsteadOfNaN` in `tests/test_geom_spatial/CurveSurfaceTest.cpp` |
| T7 | coincidence and tangency | `aCurveLyingOnTheSurfaceIsCoincidentNotAListOfPoints`, `aTouchWithoutCrossingIsReportedAsNoCrossing` (pinning the default behaviour of P4) in the same file |
| T8 | the root finder itself | `tests/test_geom_spatial/RootFindingTest.cpp` (bracket, no sign change, NaN, oblique bracket convergence, iteration cap, single precision) |
| T9 | ray × any surface (family level) | the `RaySurface.*` cases in `tests/test_geom_spatial/SurfaceQueryTest.cpp`; the algorithm and item-by-item acceptance are in `geometry-surface-queries.md` §8 |
| T10 | curve × non-analytic surface (spline patch, trimmed patch, curve on the surface, unusable input, both precisions) | the `CurveSurfaceProjection.*` cases in `tests/test_geom_spatial/CurveSurfaceTest.cpp` |
## 9. Status and Evolution

- The nine point / box / ray entries and the root-finding entry of "curve × analytic surface" are in place; the root rule of curve × surface is in 2.9.
- The family-level ray × surface is F13, with the algorithm, window rules and cost in `geometry-surface-queries.md`; this page records only its place in the pairing table.
- Curve × **any** surface is F9: the projected signed distance is rooted along the curve and shares the scan with the analytic entry.
- Known gaps: the box / ray pairings marked "missing" in §3 (G11); tangent crossings are not done (P4).
- Compatibility: purely additive — free functions and header templates only, with no existing signature changed; `signedValue()` and `CurveSurfaceIntersection.hpp` are new.

## 10. References

- `geometry-ray.md` (the ray value type, the orientation rule of `RayHit`, and why the entry is in the header)
- `geometry-primitives.md` 2.8 (the test for "short query in the header, long algorithm in `src/`")
- `geometry-kinds.md` 2.7 / 2.10 (the `Curve<T>` / `Surface<T>` interfaces and `isValid()`)
- `geometry-surface-queries.md` (family-level nearest point and ray × surface: the numeric algorithms and the window rules)
- `../backlog/geometry.md` G3 / G11
- OCCT `IntCurvesFace_ShapeIntersector` / `GeomAPI_IntCS` / `BRepAlgoAPI_*` (the shape of the reference implementation: stateful intersection algorithm classes; `geometry-kinds.md` 2.6 explains why this repository does not take that road)
