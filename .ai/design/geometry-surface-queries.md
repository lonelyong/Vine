# geometry family-level surface queries: nearest point and ray intersection

> **Status**: current
> **Last verified**: 2026-11-04
> **Scope**: the two algorithms in `src/base/geom/sdk/vine/geom/spatial` that work on **any** `Surface<T>` — nearest point / distance from a point to a surface, and ray × surface.
> Not the analytic entry of a concrete type (`Plane::intersectWith(Ray)`, `signedValue()` and so on, see `geometry-ray.md` and `geometry-intersection.md`), not parametric geometry (see `geometry-parametric.md`), not the distance from a point to a curve and not solid operations (`../backlog/geometry.md` G3).
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/SurfaceParameterWindow.hpp`, `SurfaceProjection.hpp`, `RaySurfaceIntersection.hpp`; `tests/test_geom_spatial/SurfaceQueryTest.cpp`.
> **Backlog**: `../backlog/geometry.md` G3

## 1. Overview

### 1.1 Problem and background

`Surface<T>` is an evaluation interface (`geometry-kinds.md` 2.10): holding a `const Surface<T>&` a caller can take a point, take the two first derivatives and ask for the parameter domain, but **cannot ask any geometric question**. Each concrete type has an exact entry of its own — `Plane` has a ray intersection, the five analytic surfaces have `signedValue()` for curve root finding — while "a surface of unknown type" can only be evaluated.

Two consequences:

- **A trimmed or spline surface has no ray entry**. Ray × `BezierSurface` / `RectangularTrimmedSurface` has no answer today, and `intersectCurveWithSurface()` accepts only analytic surfaces with an implicit equation.
- **Nearest point is a blank**. The projection of a point onto a plane, the distance from a point to a sphere, the nearest point on a spline patch — none has an entry; and these are exactly the two most common questions about "an unknown surface".

Adding an entry per type is 13 surface types × 2 questions, and it would be repeated for every new surface (the counter-example to `G9`).

### 1.2 Goals

- One free-function entry per question, parameterised on the **interface**, requiring neither an implicit equation nor a concrete type.
- When the parameter domain is unbounded the caller supplies a window, and the function never invents a range.
- The results reuse the existing vocabulary: distance / nearest point / parameters, and the `t` / point / ray-facing normal of `RayHit`.
- No extension of the `Surface<T>` interface: no `inverse()`, no `normalAt()`, no `bounds()`.

### 1.3 Non-goals

- No distance from a point to a **curve** and no solid intersection: no consumer today (`G8`), left in backlog G3.
- No guarantee of a global optimum or of exactness: this is a numeric algorithm, and its properties and failure modes are in §5 / §6.
- No per-type special-case dispatch: a known type keeps using its analytic entry, and the family entry is the fallback, not a replacement.

### 1.4 One-paragraph conclusion

**Seed by sampling the parameter window and solve by Newton iteration — nearest point is a two-dimensional minimisation and a ray crossing is a three-variable system; the interface therefore keeps answering only "evaluation", with the cost and the locality written in the caveats.**

## 2. Design Points

### 2.1 A free function with a "derived from `Surface<T>`" constraint

- **What**: `closestPointOnSurface(surface, query, ...)` and `intersectRayWithSurface(ray, surface, ...)` are header templates with `S` constrained by `requires std::is_base_of_v<Surface<T>, S>`.
- **Why**: `G1`, `G9` — the algorithm depends only on five interface members (`evaluate`, the two first derivatives, the domain, `isValid`), which every implementation covers; it is a free function rather than a virtual because "the relation between two objects" does not fit a one-argument interface, and a hot path (`Plane`'s ray entry) should not pay a vptr for a family algorithm.
- **Why not a `const Surface<T>&` parameter**: `Plane` derives from `Surface<T>` indirectly through `ElementarySurface<T>`, and template deduction **does not see through** an indirect base; and when the concept is written as `SurfaceOf<T>`, MSVC rejects the constrained template `Surface<T>` inside the concept body (C7605). Putting the constraint in the function's `requires` clause satisfies both.
- **Boundary**: with a known concrete type the analytic entry stays (`Plane::intersectWith(Ray)`); the family entry neither forwards to it nor rewrites it (`G8`).

### 2.2 The parameter window belongs to the caller and is resolved once

- **What**: `SurfaceParameterWindow<T>{ first_u, last_u, first_v, last_v }` is a rectangle of **parameters**, not a spatial box; `finiteParameterWindow(surface, window)` answers "which rectangle does this search use" in one place — **an infinite end takes the surface's own bound**, the rest stays as the caller wrote it, and the four endpoints must then be finite with ordered intervals. The default window `naturalSurfaceWindow<T>()` has four infinite endpoints, so the default is the natural domain.
- **Why**: `G4`, `G6` — the same rule as the curve × surface window (`geometry-intersection.md` C5): an unbounded direction is narrowed by the caller or by the surface itself, and the function invents no range. Four zeros will not do as the default — that is a zero-width window, exactly the opposite meaning; an infinite end is an explicit, readable "use the natural domain". A `Rect2` would borrow the field names `xmin/ymin` and describe parameters as coordinates.
- **Cost**: the natural domain of a plane, a cylinder and a cone is itself unbounded, so a caller must give a window in that direction at least; the analytic entries are unaffected (the ray entry of `Plane` needs no window).

### 2.3 The two algorithms share the window and the seeding, not the iteration

- **What**: the nearest point does a **damped Gauss-Newton** on `|S(u,v) - P|²` (`JᵀJ`, using the two first derivatives only); a ray crossing solves `S(u,v) = O + t·D` in `(u, v, t)`, with the Jacobian `[dS/du dS/dv -D]` and a direct solve of the 3×3 linear system.
- **Why**: `G10`, `G1` — the two problems have different unknown dimensions (2 against 3); turning the ray into "project along the ray and find a root" makes one iteration an inner minimisation per step, while forcing the projection into a three-variable system needs an extra constraint. Only "window + grid seeding + step limiting + line search" is shared: the skeleton, not the algorithm.
- **Boundary**: both call only `evaluate()` and the two first derivatives; neither assumes an implicit equation, a normal orientation or a parameter meaning.

### 2.4 The result vocabulary: one projection result, one `RayHit` + parameters

- **What**: `SurfaceProjection<T>{ point, u, v, distance }` answers both "where is the nearest point" and "how far is it"; `RaySurfaceHit<T>{ hit, u, v }` embeds a `RayHit<T>`, and `RaySurfaceIntersection<T>{ coincident, hits }` is the whole answer on the ray side.
- **Why**: `G4`, `G6` — the distance and the nearest point are two halves of one minimum, and splitting them into two entries would make a caller compute twice; a ray hit must follow the orientation rule of `RayHit` (the normal dotted with the ray direction ≤ 0), or one scene would carry two normal conventions.
- **Boundary**: `coincident` is a `bool` and not a three-valued enum — a ray's answer is only "no crossing / finitely many crossings / the whole ray on the surface", the first two separated by whether `hits` is empty, and one more `None` value would only repeat that.

### 2.5 Numeric properties: local, convergent, with a cost

- **What**: the seed is a `(seed_count + 1)²` grid over the window; the nearest point refines only the 3 cheapest of them, while a ray tries every seed; each iteration runs at most 64 steps, the step is limited to 1/4 of the window, and the line search halves at most 8 times.
- **Why**: `G10`, `G11` — Gauss-Newton needs no second derivative; damping plus a line search guarantees the cost does not increase, and on failure it stops at "no better seed". The complexity is `(seed_count+1)²` evaluations plus the iterations per seed.
- **Cost**: the nearest point is a **local** minimum; ray intersection misses tangency (a singular Jacobian) and folds the grid cannot resolve; the hit normal at a singular parameter point (a pole) is zero. All of this is in §6.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Nearest point / distance from a point to a surface | `closestPointOnSurface(surface, query, ...)`: grid seeding + Gauss-Newton, returning the point, `(u, v)` and the distance | landed (T1–T4) |
| F2 | Ray × surface | `intersectRayWithSurface(ray, surface, ...)`: grid seeding + a three-variable Newton, returning ascending `RaySurfaceHit` | landed (T5–T9) |
| F3 | Window resolution and rejection | `finiteParameterWindow()`: an unbounded direction, a reversed interval or `isValid()` false are all rejected | landed (T1, T8) |
| F4 | A ray on the surface | three probes across the span all project onto the surface → `coincident`, no points listed | landed (T9) |
| F5 | Point-to-curve distance, solid intersection | the other half of the family algorithms | not done (G3) |

## 4. Public Interface

| Header | Content |
|---|---|
| `SurfaceParameterWindow.hpp` | `SurfaceParameterWindow<T>`, `naturalSurfaceWindow()` and `finiteParameterWindow()`; the single window-resolution point both algorithms share |
| `SurfaceProjection.hpp` | `SurfaceProjection<T>` and `closestPointOnSurface()` |
| `RaySurfaceIntersection.hpp` | `RaySurfaceHit<T>`, `RaySurfaceIntersection<T>` and `intersectRayWithSurface()` |

The full signatures and the meaning of every parameter are in the headers; the `t`, `point` and `normal` contract of `RaySurfaceHit::hit` is defined by `RayHit` and is not repeated here.

## 5. Constraints, Requirements and Invariants

- **C1**: the search rectangle must be finite. An infinite end of the window is completed from the surface's own bound; if it is still infinite afterwards, or the caller gave a reversed / zero-width interval, the answer is `std::nullopt` / an empty result, with no range chosen by the function.
- **C2**: only `evaluate` / `derivativeU` / `derivativeV` / the four domain endpoints / `isValid` of `Surface<T>` are used; no implicit equation, normal or concrete type is required.
- **C3**: a tolerance is a **geometric** quantity (a distance, a point displacement), not a parameter difference; a parameter difference is meaningful only on the scale of the window.
- **C4**: a ray hit is reported only for `t ≥ -tolerance`; `t` is a ray distance, because a `Ray` direction is always unit.
- **C5**: when `coincident` is true, `hits` must be empty — infinitely many points are not a point set.
- **R1**: nothing throws; unusable input returns an empty answer.
- **R2**: nothing is allocated except the `std::vector` of the ray result; the seeding grid and the iterations live on the stack.
- **R3**: parameters and results of the same `T`, with no mixed precision.
- **N1**: `SurfaceProjection::point == surface.evaluate(u, v)`; `distance == |point - query|`.
- **N2**: ray hits are ordered by `hit.t` and one crossing appears once.
- **N3**: `RaySurfaceHit::hit.normal` dotted with the ray direction is ≤ 0 (the rule of `RayHit`); at a singular parameter point it may be the zero vector.

## 6. Caveats and Traps

- **P1 Coincidence detection depends on the probes.** How it fails: a ray that only **grazes** the surface without lying on it, with all three probes happening to land on it (a constructed case), is reported as `coincident`; conversely, when the window covers only a small stretch the probes may land outside its spatial extent. How to detect: `coincident` is true while a neighbouring ray gets finite hits. How to prevent: the probes are the grid points' range along the ray (`span_low / mid / span_high`), spanning the whole surface rather than any three points.
- **P2 The nearest point is a local minimum.** How it fails: a torus or a folded spline patch has several local minima, the grid does not land on the global side, and `distance` is too large. How to detect: doubling `seed_count` makes the distance smaller. How to prevent: raise `seed_count` to the scale of the surface's undulation; when a global guarantee is needed, use the analytic entry.
- **P3 The normal at a pole is zero.** How it fails: a ray hitting a sphere exactly at a pole leaves `hit.normal` as the zero vector and `normal.dot(direction)` meaningless. How to detect: `normal.length() == 0` while `t` / `point` are normal. How to prevent: check the normal length; a pole normal is computed from the concrete type's own parameterisation.
- **P4 Missed tangency.** How it fails: a ray tangent to a sphere or a cylinder has a singular Jacobian, returns an empty result while the picture shows tangency. How to detect: the nearest distance is ≈ 0 but there is no hit. How to prevent: the same position as the curve × surface root finding (`geometry-intersection.md` P4) — when tangency matters, search for the extremum separately.
- **P5 A fold the grid cannot resolve.** How it fails: a spline patch folding back twice inside one grid cell puts both crossings in the same seed's basin and only one comes back. How to detect: doubling `seed_count` produces one more hit. How to prevent: set `seed_count` to the scale of the control net.
- **P6 Treating the window as a hint.** How it fails: a caller gives a window covering half the surface, expects "outside counts too", misses hits and blames the algorithm. How to detect: enlarging the window produces new hits. How to prevent: the window is a hard constraint (C1), not a hint; give the whole domain when the whole surface is wanted.

## 7. Rejected Options

- **D1** Adding `inverse(point, u, v)` / `normalAt()` / `bounds()` to `Surface<T>`: every implementation would have to write an iteration of its own, turning "a numeric algorithm" into an obligation for each surface author, and the family sweep in `GeometryKindsTest` would have three more items to verify (`G7`, `G8`); with the algorithm left in a free function an implementer answers evaluation only.
- **D2** Writing nearest point and ray special cases per type: 13 surface types × 2 questions, written again for every new surface (`G9`); the four types that already have an analytic entry keep it, and the family entry is the fallback.
- **D3** Making the nearest point an `optional<T>` distance entry plus a separate `project()`: one minimum computed twice, and a caller learns two names (`G1`, `G4`).
- **D4** Reusing the three-valued `CurveSurfaceContact` for the ray result: `Ray` is deliberately outside the curve family (`geometry-ray.md` 2.1) and borrowing it would write that fact wrongly; `bool coincident` plus an empty `hits` already expresses the three answers.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | plane projection and the window rules | `aPointProjectsOntoAPlaneAtItsFoot`, `anUnboundedDirectionNeedsAWindow`, `anInfiniteEndTakesTheSurfacesOwnBound` in `tests/test_geom_spatial/SurfaceQueryTest.cpp` |
| T2 | nearest point on a sphere / cylinder / torus | `aPointProjectsOntoASphereAlongItsRadius`, `aPointProjectsOntoACylinderThroughItsAxis`, `theNearestPointOnATorusIsTheBestLocalMinimum` in the same file |
| T3 | both precisions agree | `bothPrecisionsFindTheSameNearestPoint` in the same file |
| T4 | unusable input | the reversed / zero-width branches of `anUnboundedDirectionNeedsAWindow` in the same file |
| T5 | ray × plane agrees with the analytic entry | `aRayCrossesAPlaneLikeTheAnalyticEntry` in the same file |
| T6 | ray × sphere (two hits, normal orientation, self-consistent parameters) | `aRayThroughASphereIsHitTwice`, `aRayFromInsideASphereLeavesOnceWithAnInwardNormal` in the same file |
| T7 | ray × torus, four hits | `aRayThroughATorusIsHitFourTimes` in the same file (the same geometry cross-checked with `CurveSurfaceTest`) |
| T8 | the window is a constraint, a hit behind the origin does not count, unusable input | `theWindowIsAConstraintNotAHint`, `aHitBehindTheOriginIsNotACrossing`, `unusableInputsAnswerNothing`, `anUnboundedDirectionNeedsAWindow` in the same file |
| T9 | spline patch hits and coincidence classification | `aSplinePatchIsHitThroughItsOwnParameters`, `aRayLyingInAPlaneIsCoincidentNotAListOfPoints`, `aCoincidentRayIsAlsoSeenInFloat` in the same file (covering float / double together with `bothPrecisionsAgreeOnASphere`) |

## 9. Status and Evolution

- `closestPointOnSurface()` and `intersectRayWithSurface()` cover the five analytic surfaces, the two spline surfaces and the trimmed view (all through the interface only).
- The nearest point is also used by curve × surface as the implicitisation: `intersectCurveWithSurfaceByProjection()` roots the projected signed distance along the curve (`geometry-intersection.md` 2.9 / F9); that signed distance is `detail`, because its polarity belongs to the parameterisation.
- The same question on the curve side (nearest point / distance from a point to a curve) is in `geometry-curve-queries.md`: the same set of constraints — "finite window + sampled seeding + bracket-safe narrowing".
- Known gaps: the distance from a point to a curve and solid intersection (`../backlog/geometry.md` G3); the globality of the nearest point, tangent hits and the normal at a singular point are not provided, per the rules of §6 (P2–P4).
- Compatibility: purely additive — three new headers, new types, new free functions; `Surface<T>` and every value-type signature are unchanged.
- The cost is neither better nor worse: the analytic entry is still the hot path, and the family entry is the fallback for an unknown type.

## 10. References

- `geometry-kinds.md` 2.7 / 2.10 (the `Curve<T>` / `Surface<T>` interfaces and `isValid()`)
- `geometry-intersection.md` 2.7 / C5 (the window rule and root finder of curve × surface)
- `geometry-ray.md` 2.3 (the orientation rule of `RayHit`)
- `../decisions/ADR-2026-11-04-surface-family-queries.md`
- OCCT `GeomAPI_ProjectPointOnSurf` / `GeomAPI_IntCS` (the shape of the reference implementation: stateful algorithm classes; this repository uses stateless free functions)
