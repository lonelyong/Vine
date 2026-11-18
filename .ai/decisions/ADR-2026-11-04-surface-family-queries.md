# ADR-2026-11-04: family-level surface queries use numeric iteration and do not widen the `Surface` interface

- Status: Accepted
- Date: 2026-11-04
- Related: `../design/geometry-surface-queries.md`, `../design/geometry-kinds.md` 2.10, `../design/geometry-intersection.md` C5; `src/base/geometry/sdk/vine/geometry/SurfaceProjection.hpp`, `RaySurfaceIntersection.hpp`, `SurfaceParameterWindow.hpp`; `tests/test_geometry/SurfaceQueryTest.cpp` <!-- drift-ok -->

## Background

`Surface<T>` is an evaluation interface: `evaluate`, the two first derivatives, the parameter domain, closure and periodicity, `isValid`. It cannot answer either "the closest point on the surface to a point" or "a ray against the surface", and those are the two questions a caller holding a `const Surface<T>&` asks most often. Analytic surfaces each have an exact entry (`Plane::intersectWith(Ray)`, `signedValue()`); spline and trimmed surfaces have none.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Add `inverse(point, u, v)` / `normalAt()` / `bounds()` to the interface | Each surface answers its own projection and algorithms stay thin | Thirteen implementations each write an iteration; the interface grows from "evaluation" to "evaluation plus solving"; the family contract sweep has three more items to verify (`G7`, `G8`) |
| B. Add closest-point and ray entries per type | Exact and optimisable | 13 x 2 entries, and a new surface repeats it (`G9` counter-example); trimmed views and spline surfaces still need their own |
| C. Stateless free functions: interface constraint plus a parameter window plus grid seeding plus Newton iteration | One set covers every implementation; the interface is untouched; analytic surfaces keep their own entries | Local solutions, a sampling cost, and known gaps at tangencies and singular points (`G10`) |

## Decision

Chose C:

1. `closestPointOnSurface()` runs a damped Gauss-Newton with a line search on `|S(u,v) - P|^2` (only the two first derivatives are needed).
2. `intersectRayWithSurface()` solves `S(u,v) = O + tD` in `(u, v, t)` with the Jacobian `[dS/du dS/dv -D]`.
3. The window is supplied by the caller (`SurfaceParameterWindow<T>`), and **an infinite end takes the surface's own bound** (so the default `naturalSurfaceWindow()` is the natural domain); `finiteParameterWindow()` is the single parsing point, with the same rules as curve against surface.
4. The ray result embeds `RayHit<T>`, the normal orientation rule has one home, and coincidence (the ray lying in the surface) uses a `coincident` flag rather than an enum.
5. The constraint is written as `requires std::is_base_of_v<Surface<T>, S>` on the function rather than as a named concept: `Plane` derives indirectly through a classification tag, `const Surface<T>&` cannot deduce `T`, and a constrained template `Surface<T>` inside a concept body is rejected by MSVC (C7605).
6. Curve against a non-analytic surface composes on the same projection: `intersectCurveWithSurfaceByProjection()` drives `detail::signedDistanceToSurface()` (distance plus the sign of the parameterised normal) through the **same** sweep as the analytic entry. The signed distance stays in `detail`, because its polarity belongs to the parameterisation while root finding only needs a sign change.

## Consequences

- Good: the interface stays "it only answers evaluation"; every present and future `Surface<T>` is covered directly; the repository no longer has a second normal convention; curve against any surface and ray against any surface share one projection instead of one each.
- Bad / cost: the result is a **local** minimum; tangencies can be missed; a fold the grid cannot resolve can be missed; each query costs `(seed_count+1)^2` evaluations of sampling. All of it is written in `geometry-surface-queries.md` section 6.
- Trigger (when to reopen): a hot-path consumer appears (picking that intersects spline surfaces every frame) or a global closest point is needed - then add that concrete type's own analytic or cached entry to the type, and do not widen the interface back.

## Evidence

- `tests/test_geometry/SurfaceQueryTest.cpp`: 19 cases covering plane / sphere / cylinder / torus / bilinear spline surfaces, both precisions, window constraints and the infinite-end rule, hits from behind, the coincidence classification and unusable input. <!-- drift-ok -->
- `CurveSurfaceProjection.*` in `tests/test_geometry/CurveSurfaceTest.cpp`: 7 cases covering curve against a spline surface, the same intersection point as the analytic entry, a trimmed window, a curve lying on the surface, a singular parameter line, unusable input and both precisions. <!-- drift-ok -->
- The family sweep of `GeometryKindsTest` is unchanged: the interface gained no virtual function.
