# geometry planar: the planar curve contract and the first value types

> **Status**: current
> **Last verified**: 2026-11-05
> **Scope**: `src/base/geom/sdk/vine/geom/planar` — the planar curve evaluation contract `Curve<T>`, the conic layer `ConicCurve<T>` and the nine types `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `BezierCurve` / `BSplineCurve` / `TrimmedCurve` / `OffsetCurve`, the six family algorithms, and the module's physical shape and naming rules. Not spatial curves and surfaces (see `geometry-kinds.md`), not polygons (no consumer).
> **Sources**: `src/base/geom/sdk/vine/geom/planar/Curve.hpp`, `ConicCurve.hpp`, `LineCurve.hpp`, `CircleCurve.hpp`, `EllipseCurve.hpp`, `HyperbolaCurve.hpp`, `ParabolaCurve.hpp`, `BezierCurve.hpp`, `BSplineCurve.hpp`, `TrimmedCurve.hpp`, `OffsetCurve.hpp`, `CurveProjection.hpp`, `CurveCurveIntersection.hpp`, `CurveLength.hpp`, `CurveCurvature.hpp`, `CurveCurveDistance.hpp`; every test file under `tests/test_geom_planar/`.
> **Backlog**: `../backlog/geom-planar.md`

## 1. Overview

### 1.1 Problem and background

Planar geometry needs a type of its own, and neither alternative holds: pressing the points into `Point3` with `z = 0` gives a planar curve a normal it does not have and a dimension that is always zero, so the 3D side then "accepts" a planar curve and interprets it as a spatial one; writing a second copy outside the module gives the concept "curve" a second definition with no contract. A `geom/planar` that holds only a namespace and one re-export of `Continuity` answers neither problem.

### 1.2 Goals

- One **independent** planar curve contract: item by item the same as `geom::spatial::Curve<T>` (evaluation, first and second derivatives, parameter domain, closedness and period, smoothness, validity, reversed parameter mapping), with points as `Point2` and vectors as `Vector2`.
- Nine types that can be verified by execution (line, circle, ellipse, hyperbola, parabola, Bézier, B-spline, a **generic trimmed view**, offset), turning the contract from a declaration into measurable behaviour.
- Six **family algorithms** (nearest point on a curve, intersection and nearest distance of two curves, arc length, parameter from arc length, curvature), proving that the eleven-item contract suffices to answer geometric questions on a curve without knowing its concrete type.
- The module's physical shape and naming rules stated once: one `vn::Geom`, two namespaces, one module global, names without a dimension marker, precision aliases still `d` / `f`.

### 1.3 Non-goals

- Do not move the whole 3D curve family over: types enter one by one under the triggers in `../backlog/geom-planar.md` (`G8`).
- Algorithms stay within "family-level queries and scalar measures": today nearest point, intersection, nearest distance between two curves, arc length, parameter from arc length and curvature; area, the control polygon and spline knot insertion are still not done.
- Do not borrow the 3D `ConicCurve` frame: a planar conic has no normal to store (2.4).
- No screen / pixel geometry: `Point2`, `Vector2` and `Rect2` belong to `math`, and this module has curves only.

### 1.4 One-paragraph conclusion

**A planar curve is a second contract, not a specialisation of the 3D one: the same eleven facts, `Point2` / `Vector2`, the same `geom::Continuity`; today it has the line, circle, ellipse, hyperbola, parabola, two splines, a generic trimmed view and an offset, plus the six family algorithms — nearest point, intersection, nearest distance, arc length, inverse and curvature.**

## 2. Design Points

### 2.1 An independent contract, not a generic specialisation of 3D

- **What**: `geom::planar::Curve<T>` declares its own eleven pure virtual functions plus a virtual destructor, with the same names and meaning as `geom::spatial::Curve<T>`; between the two there is no inheritance, no shared base class and no common template parameterised on the point type.
- **Why**: `G1`, `G5` — "planar curve" and "spatial curve" are two concepts, each with one definition and one owner. A generic `Curve<PointT>` contract turns every 3D consumer into a template and forces the 2D types to satisfy constraints only 3D needs; letting the 2D contract inherit the 3D one forces a planar curve to carry a constant normal and a dimension that is always zero, and 3D algorithms then accept it by mistake. The decision and the rejected options are in `../decisions/ADR-2026-11-05-planar-curve-contract.md`.
- **Cost**: the contract text exists twice, so a fact added on one side can silently drift from the other (`G6`). The mitigation does two things only and introduces no mechanism: the shared vocabulary lives in `math` (`Continuity`, `FP`, the future root finder), and `geom_global.hpp` re-exports `Continuity` as the same type so both sides write one name; the contract carries only **dimension-free** facts (2.5), and a new fact changes both documents together.

### 2.2 A name carries no dimension marker

- **What**: `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `BezierCurve` / `BSplineCurve` / `TrimmedCurve` / `OffsetCurve` / `Curve`, not `LineCurve2d` / `Curve2D`; the precision aliases stay `LineCurved` / `CircleCurvef` / `EllipseCurved` / `HyperbolaCurvef` / `ParabolaCurved` / `BezierCurved` / `BSplineCurved` / `TrimmedCurved` / `OffsetCurved`, identical to the `d` / `f` rule of `geom/spatial`.
- **Why**: `G4`, `G6` — the namespace is already the qualifier, so "2d" inside a type name is a repeated remark; a precision suffix says "which scalar", which has nothing to do with dimension, so it must stay as it is, or one concept reads differently in the two layers.
- **Boundary**: `geom::planar::LineCurve` and `geom::spatial::LineCurve` of the same name are two types. Code using both `using`s one and qualifies the other; no `Line2d`-style alias may be invented (D3).

### 2.3 The contract carries only what a curve necessarily answers

- **What**: eleven pure virtual items — (1) evaluation and derivatives: `evaluate` / `derivative` / `secondDerivative`; (2) parameter domain and period: `firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period`; (3) smoothness: `continuity` (and the non-virtual `isCN(n)` derived from it); (4) reversed parameter mapping: `reversedParameter`; (5) validity: `isValid`. Plus one pair of **non-pure capability members**: `maxDerivativeOrder()` (default 2) and `derivativeAt(u, order)` (answers the first and second order by default, `std::nullopt` beyond that).
- **Why**: `G4`, `G9` — this is the minimal complete set for "a parameterised curve", on the same test as the 3D side (`geometry-kinds.md` 2.7): a point, its rate of change, where it is valid, how smooth it is, how the parameter changes when walked backwards, and whether this state is usable. The planar side is not "a few items fewer" but the same group of facts with one dimension less.
- **What stays off the interface, item for item as on the 3D side**: `bounds()` (an unbounded curve can only return an infinite box), `length()` / arc-length parameterisation (an algorithm), the transform family (the coordinate system lives on a `Shape` node), the `curveType()` enum (the vtable already answers "what am I"), and derivative implementations of specific orders. The reason is written once in `geometry-kinds.md` 2.7 and D6, D7, and is not copied here.

### 2.4 The non-circular conics share one starting axis, and the circle stays out of that layer

- **What**: `CircleCurve` stores `center` and `radius`, with `P(u) = center + radius * (cos u, sin u)`; the parameter is radians, the domain `[0, 2π]`, counter-clockwise, the reversed mapping `2π - u`, and it has no frame. `ConicCurve<T>` holds `center` (which is the vertex for a parabola) + `x_direction` (normalised at construction) + the derived `yAxis()` (a +90° rotation) + `continuity() == CN`; `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` derive from it and declare only their own scalars.
- **Why**: `G1`, `G13`, `G8` — the 3D conic frame needs a normal because a curve in space must answer "in which plane, and from which axis is the angle measured"; in the plane there is no plane to choose, only a starting axis. The circle has not even a starting axis — counter-clockwise and +X are the only convention — so storing an axis that is forever `(1,0)` would be a field nothing can test. The three non-circular conics really do share one group of facts, so this layer has a real consumer rather than being an abstraction pre-built for one field.
- **Boundary**: the layer holds neither evaluation nor the parameter domain — the parameter means different things per type (eccentric angle, hyperbolic angle, offset along the directrix axis); a degenerate direction stores the zero vector, so every point query answers `center`, and `isValid()` is the only test. OCCT's `Geom2d_Circle` instead derives from `Geom2d_Conic` and carries a `gp_Ax2d` (a rotatable start); we do not follow that, see D5.
- **The line is the same story**: `LineCurve` stores `origin` and `direction`, the direction need not be unit length, and the parameter is a multiple of the direction — the same contract as the 3D `LineCurve` (`geometry-primitives.md` 2.3).

### 2.5 The planar tree is one sub-namespace of the module; shape and naming are in `geometry-module.md`

- **This tree declares two planar facts only**: (1) it is `vn::geom::planar`, side by side with `vn::geom::spatial`, with zero inheritance and no interchangeability; `Continuity` and `geom::detail` come from the enclosing namespace and this tree re-exports nothing. (2) A type name carries no dimension marker and the precision suffix is `f` / `d` (`LineCurved`, `CircleCurvef`).
- **The module shape (one library `vn::Geom`, one global, the shared `geom/detail` subtree, the five isolation gates, where `Continuity` lives, the three-step test for what goes where) is all in `geometry-module.md`** and is not repeated here. That page also records the rejected re-shuffles (flatten plus a `2` suffix, a suffix on both contracts).

### 2.6 A degenerate state answers, it does not fail

- **What**: `isValid()` is the only test, and it looks at **state** only, never at a parameter. A query in an invalid state **must be finite**, and what it answers is stated in the type's documentation: a zero-direction line answers `origin`; a zero-radius circle answers `center`, and a negative radius still traces the same circle; a conic with a zero main axis answers `center`, and with a zero semi-axis or focal distance it collapses onto a segment / line / vertex; a parabola with zero focal distance answers its vertex (it would divide by zero). A trimmed view over a reversed interval is invalid, and its queries still forward to the basis curve.
- **Why**: `G4`, `G11` — word for word the rule of the 3D side (`geometry-kinds.md` C12): a mistake becomes an **inspectable wrong answer** instead of undefined behaviour or an exception; the caller checks `isValid()` once at the entry, and the inner loop need not guard every parameter.

### 2.7 The value type keeps a path cheaper than the interface

- **What**: `LineCurve::pointAt(t)` is `constexpr`, and `CircleCurve::pointAt(angle)` / `EllipseCurve::pointAt(angle)` / `HyperbolaCurve::pointAt(u)` / `ParabolaCurve::pointAt(u)` are ordinary members on the type (trigonometric functions are not constant expressions); `evaluate()` forwards to them. A call on the concrete type can therefore inline, while a call through `Curve<T>&` must cross a vtable.
- **Why**: `G7`, `G10` — polymorphism is worth its cost only when the concrete type is unknown, and a hot path must be able to step around it; this path is **additive** (one more member), not complexity pushed onto the caller.
- **Boundary**: no symmetry for its own sake. `TrimmedCurve` has no `pointAt()` — it would only forward to the basis curve and save nothing; the `pointAt()` of the other five types equals their `evaluate()`, and the circle's matches the same-named 3D type.

### 2.8 The generic trimmed view: any basis curve plus an interval

- **What**: `TrimmedCurve<T>` stores a `std::shared_ptr<const Curve<T>>` plus two parameter bounds; evaluation, the first and second derivatives and the derivative capability all forward to the basis curve, `period()` answers the basis period only when its own `isPeriodic()` is true, and the reversed mapping reflects inside the interval. The factory `trimCurve(basis, first, last)` builds the handle from a concrete type.
- **Closedness is two things, "coverage + coincident ends"**: the interval must cover the basis curve's whole natural domain **and** the two ends must really land on the same point; `[0, 2π + 0.3]` covers the whole span but walks one corner further, the ends differ, so it answers false. Conversely, a full turn that does not start at 0 has coincident ends but does not cover the natural domain, and also answers false (the same rule as the 3D `TrimmedCurve`, see `Curve::isClosed()`).
- **Why**: `G1`, `G6`, `G9` — "a piece of a curve" has one expression: it replaces the former circle-only `ArcCurve`, so an elliptical arc, a trimmed spline or a clipped contour need not invent a type each; the shape is item for item the same as the 3D `TrimmedCurve` (same names, same members, same degenerate answers), so a caller need not remember two sets.
- **Cost**: one heap allocation per construction (type erasure can only go through `shared_ptr`), which makes `TrimmedCurve` the second exception to R2; the tight box of a piece is no longer answered by it either — the support function is erased with the basis curve, so a circle piece's box comes from `CircleCurve::bounds(first_parameter, last_parameter)` (2.10).

### 2.9 Family queries are free functions, and the caller supplies the window

- **What**: `closestPointOnCurve(curve, query, window, tolerance, sample_count)` is a header template with `C` constrained by `requires std::is_base_of_v<Curve<T>, C>`; the result `CurveProjection<T>{ point, parameter, distance }` answers "where is the nearest point, at which parameter, how far" with one extremum.
- **Why**: `G1`, `G9` — the query depends only on `evaluate` / `derivative` / the domain ends / `isValid`, and written as a free function rather than an interface virtual it can hold "the relation between two objects", and the nine implementations need not each write it. The shape and the rules are copied from the 3D `closestPointOnCurve()` (`geometry-curve-queries.md` 2.1–2.4), so a caller need not remember two conventions.
- **Window rule**: an unbounded curve (line, hyperbola, parabola) **must** be given a window; a bounded curve (circle, ellipse, trimmed piece, spline) uses its own domain when the window is omitted; without a finite window the result is `std::nullopt`, and the function never chooses a range itself. The measures and distances of §2.14–2.16 obey the same rule.
- **Boundary**: the module's algorithms all live in this family (§2.9, §2.14–§2.16), and the tension with `G8` is written here — they have no production consumer today, and their value is **testing the contract**: if the eleven items were not enough, these functions could not be written.

### 2.10 The bounding box is a bounded type's own query, not on the interface

- **What**: `CircleCurve` / `EllipseCurve` each have a `bounds()`; the circle also has `bounds(first_parameter, last_parameter)`, answering the tight box of an arc. `LineCurve` / `HyperbolaCurve` / `ParabolaCurve` have none. `BezierCurve` / `BSplineCurve` have a `bounds()` too, but that is the **pole box** (a convex-hull box, not a tight one), with the rule in 2.12.
- **Why**: `G4`, `G13` — an unbounded curve can only return an infinite box or a fake one, disguising "unbounded" as a rectangle, so the interface does not declare it (the same test as `geometry-kinds.md` 2.7). A planar box reuses `math::Rect2` directly; the module builds no second type for one box.
- **Boundary**: the box is the box of the **point set** traced by `evaluate()`: the circle is `center ± |r|`; the ellipse uses the support function `sqrt((a·X_i)² + (b·Y_i)²)`, not a loose `±a` box; an arc takes the two endpoints plus the four axial extrema inside the interval, and this query lives on the **circle** rather than on the trimmed view — `TrimmedCurve` erases the basis curve and cannot be asked for a support function. The direction of the interval does not change the point set, so a reversed interval answers the same box.

### 2.11 Intersection of two curves: a free function with a three-valued result

- **What**: `intersectCurveWithCurve(first, second, first_window, second_window, tolerance, sample_count)` returns `CurveCurveIntersection<T>{ contact, hits }`: `contact` is `None` / `Isolated` / `Coincident`, and each hit carries a point and the parameter on each curve, ordered by the parameter of the first curve.
- **Why**: `G12`, `G4` — the relation between two objects belongs to neither, so it is a free function; the result must be three-valued, because "crosses at a point", "coincides over a stretch" and "does not meet" are three different answers, and splitting a coincident stretch into a list of points is both wrong and non-convergent (the same reason as `CurveSurfaceContact` in 3D).
- **How it is computed**: sample along the first curve, and at each sample take the perpendicular distance to the second with `closestPointOnCurve()` — one quantity that yields both the candidate (perpendicular distance within one step) and the coincidence decision (at least three consecutive samples within tolerance); candidates are then converged with a two-dimensional Newton on `C1(u) = C2(v)`, bracketed by the two windows.
- **Boundary**: a local method — tangency is missed by default (P9); two intersections closer than three sample steps are read as coincidence; the window rule is C5.

### 2.12 A spline is a curve that owns memory

- **What**: `BezierCurve` stores poles and nullable weights, with `degree()` derived as the pole count minus one; `BSplineCurve` stores distinct knots + multiplicities, poles and optional weights, with `continuity()` derived from the interior multiplicities (`geom::detail::knotContinuity`). Both evaluate through `geom::detail`: Bézier with de Casteljau and divided differences (pole / first / second differences scaled by `1` / `n` / `n(n-1)`), B-spline with a weighted sum of the basis functions (the `p+1` of the span, including the derivative layer).
- **Why**: `G1`, `G10` — the data model is the interface: no stored degree (two copies would contradict), an empty array instead of an `isRational` boolean (a boolean and the data can contradict each other); the blending kernels are in `math` and dimension-free, so this layer owns the data and the scaling and rewrites no evaluation.
- **Cost**: deep copies, no longer trivially copyable, and `evaluate` allocates a temporary array each call (`deCasteljau` takes the array by value) — which is why R2 is phrased as "no dynamic memory by default, exceptions stated"; a deliberate trade, the same as the same-named 3D types, and still cheaper than a virtual call when sampling a literal.
- **Boundary**: the `bounds()` of both is the **pole box** (the curve lies inside the convex hull of its poles), not a tight box; a tight box needs the roots of each coordinate's derivative, and there is no consumer today (P10). The rational denominator never reaches zero inside the domain (weights must be positive). A Bézier extrapolates polynomially outside `[0, 1]`; the B-spline domain runs between the first and last distinct knots, and parameters outside are clamped to the endpoints rather than extrapolated (as in the same-named OCCT type).

### 2.13 An offset holds a basis handle, and its derivative order follows the basis

- **What**: `OffsetCurve<T>` stores a `std::shared_ptr<const Curve<T>>` and a signed distance, with `O = C + d·N` where `N` is the left normal (`C'` rotated 90° counter-clockwise); all three numeric answers are exact, the second derivative using the basis curve's third derivative (`derivativeAt(u, 3)`). The factory `offsetCurve(basis, distance)` builds the handle from a concrete type.
- **Why**: `G1`, `G4`, `G9` — an offset is "another object plus one scalar", not "a specialisation of some basis curve", so the basis is a run-time handle rather than a template parameter (as in 3D); the need for a third derivative is now answered by the capability protocol rather than guaranteed by "the template parameter can only be a concrete type".
- **The capability decides the order**: the n-th derivative of an offset needs the (n+1)-th of the basis, so `maxDerivativeOrder()` reports `min(2, basis capability - 1)` — a basis answering 3 (every planar concrete type answers 3) gives an offset of 2, a basis answering 2 (another offset) gives an offset of 1 with `derivativeAt(u, 2)` empty. When the basis cannot supply a third derivative, `isValid()` is false and `secondDerivative()` answers the zero vector rather than a fabricated number.
- **Cost**: one heap allocation; no `bounds()`; the offset's second derivative costs one virtual `derivativeAt(u, 3)` rather than an inlined concrete call.
- **Boundary**: a positive distance is to the left of the direction of travel (so the offset of a counter-clockwise circle has radius `r - d`); at a cusp where the basis velocity is zero the normal is undefined, and the query falls back to the basis curve's own value without producing NaN; walking the same geometric curve backwards is "reversed basis + negated distance".

### 2.14 Arc length is an adaptive integral, and the inverse is that integral inverted

- **What**: `arcLength(curve, window, tolerance, max_depth)` integrates `|C'(u)|` with adaptive Simpson and returns the length over the window; `parameterAtLength(curve, arc_length, window, tolerance, max_depth)` inverts the same integral with a bisection-guarded Newton and returns the parameter reached by walking that length from the window start. The two numeric kernels are `geom::detail::adaptiveSimpson()` and `geom::detail::parameterReaching()`, and the two trees use the **same** implementation.
- **Why**: `G1`, `G9`, `G4` — length and "parameter for a given length" are one-dimensional measures of a curve and belong to no concrete type; written as free functions the nine types share one copy, and the interface needs no twelfth virtual. Both depend only on `derivative()` / `isValid()`, so they obey the same window rule as nearest point and intersection (C5). Why the kernels belong to `geom/detail` is in `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md` (`math` keeps vocabulary only, and the geometry numeric chain stays in one module).
- **Algorithm**: Simpson uses `(b - a) / 6 * (f(a) + 4 f(m) + f(b))` on each interval, compares the halved sum with the whole one, and stops when the Richardson error estimate is below tolerance; `max_depth` bounds the recursion at a cusp. The inverse uses `u <- u - (s(u) - L) / |C'(u)|`, falling back to bisection when the step leaves the bracket. Both sentences describe the kernels in `geom/detail`; the geometry side supplies only `|C'|` and `s([a, u])`.
- **Boundary**: `tolerance` is a **length** error, not a parameter error; the window must be finite; a negative `arc_length` or one exceeding the window total (beyond tolerance) answers `std::nullopt`. The closed forms `|r| * (b - a)` for an arc and `|direction| * (b - a)` for a line are the acceptance references.
- **Cost**: each inverse iteration integrates once more, so the cost is the iteration count times one integral; the module introduces no "arc-length parameterised curve type", and a caller inverts on demand.

### 2.15 Curvature is a signed scalar, positive when turning left

- **What**: `signedCurvature(curve, u)` returns `(C'(u) x C''(u)) / |C'(u)|^3`, with a positive sign meaning the centre of curvature is to the left of the direction of travel; the absolute value is the geometric curvature (the reciprocal of the radius of curvature).
- **Why**: `G4`, `G12` — curvature is how much the point bends, and the sign only says which way; choosing the left-normal convention keeps it consistent with the positive distance of `OffsetCurve`, so the two do not each set up their own convention. The parameterisation does not affect the result, because the speed has been divided out.
- **Boundary**: it depends on the first two derivatives only; where the speed is zero curvature is undefined and answers `std::nullopt` (the same "cannot answer" as "no finite window"); an invalid curve answers empty too. Analytic types and splines both answer exact values, because the first two derivatives themselves are exact.
- **Cost**: no extra type; a caller wanting the radius takes the reciprocal and a caller wanting the unsigned curvature takes the absolute value.

### 2.16 Nearest distance between two curves: sampling + projection on one side + two-dimensional Gauss-Newton

- **What**: `curveToCurveDistance(first, second, first_window, second_window, tolerance, sample_count)` returns `CurveCurveDistance<T>{ first_point, second_point, first_parameter, second_parameter, distance }`. The first curve is sampled, each sample is projected onto the second with `closestPointOnCurve()` (exact for the sample), and the pair is then refined with `(J^T J) [du, dv] = -J^T r` (`J = [C1', -C2']`), keeping the smallest distance.
- **Why**: `G12`, `G4`, `G9` — "the relation between two curves" belongs to neither, so it is a free function; the result carries both points, both parameters and the distance, so the caller does not evaluate again and `distance` cannot contradict the points. Reusing the nearest point and the window rule means the nine types need not each write it.
- **Boundary**: a local method — with several near approaches the answer depends on whether the sampling lands in each valley; where the pair is locally parallel (`J^T J` singular) the sampled pair is kept, and the sampled projection is still exact; the window rule is C5. When the curves touch or cross the distance may be 0 (the scan or the refinement lands on the contact).
- **Cost**: one nearest-point query per sample, so the cost is `sample_count ×` nearest point; no new type or interface item.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Planar curve contract | `Curve<T>` with eleven pure virtuals + `isCN()` + the capability pair `maxDerivativeOrder()` / `derivativeAt()`, with `Continuity` coming from `math` | landed (T1–T6) |
| F2 | Planar line | `LineCurve`: unbounded, not periodic, `CN`, a degenerate direction answers the origin | landed (T1–T4) |
| F3 | Planar circle | `CircleCurve`: radian parameter, closed period `2π`, counter-clockwise, a degenerate radius answers the centre | landed (T1–T5) |
| F4 | Planar ellipse | `EllipseCurve`: eccentric-angle parameter, closed period `2π`, carries its own main-axis directions, a degenerate axis answers the centre | landed (T1–T6) |
| F5 | Generic trimmed view | `TrimmedCurve<T>`: a `shared_ptr` basis curve + an interval; closed = the interval covers the basis natural domain and the two ends coincide, periodic = at least one period; the derivative capability forwards to the basis | landed (T1–T6) |
| F6 | Planar hyperbola and parabola | `HyperbolaCurve` / `ParabolaCurve`: hyperbolic angle and offset along the directrix axis, unbounded and not periodic, sharing the starting axis of `ConicCurve` | landed (T1–T6) |
| F7 | Nearest point on a curve | `closestPointOnCurve()`: roots of the stationary points of the squared distance + comparison with the window ends, returning point / parameter / distance | landed (T7) |
| F8 | Planar bounding box | `bounds()` of the circle / ellipse and `bounds(first, last)` of the circle, returning `math::Rect2`; an unbounded type has no such entry | landed (T8) |
| F9 | Planar curve intersection | `intersectCurveWithCurve()`: sampling + perpendicular-distance candidates + two-dimensional Newton, a three-valued result (no crossing / isolated points / coincident stretch) | landed (T9) |
| F10 | Planar splines | `BezierCurve` (poles + optional weights, de Casteljau) and `BSplineCurve` (distinct knots + multiplicities, basis sum, continuity from the multiplicities) | landed (T10, T11) |
| F11 | Family algorithms over a trimmed view | a spline piece or an offset piece goes straight into the six family algorithms (arc length counts the interval only, the nearest point lands inside it) | landed (T5, T12) |
| F12 | Planar offset | `OffsetCurve<T>`: a `shared_ptr` basis curve + left normal, signed distance, all three derivatives exact; order = basis capability − 1 | landed (T12) |
| F13 | Planar arc length and inverse | `arcLength()` / `parameterAtLength()`: adaptive Simpson over `|C'|`, the inverse by a bisection-guarded Newton | landed (T13) |
| F14 | Planar curvature | `signedCurvature()`: `(C' x C'') / |C'|^3`, positive when turning left, empty where the speed is zero | landed (T14) |
| F15 | Nearest distance between two curves | `curveToCurveDistance()`: sampling + projection on one side + two-dimensional Gauss-Newton, returning two points / two parameters / distance | landed (T15) |

### 3.1.1 F1 the planar curve contract

- **Behaviour**: through `const Curve<T>&` one can ask for the point and the first two derivatives at any parameter, and for the parameter domain / closedness / period / smoothness / reversed mapping / validity; `isCN(order)` follows from `continuity()`.
- **Constraint** (C1, C2, C3):
- **Requirement** (R1, R2):
- **Acceptance** (T1–T6):
- **Caveat** (N2, N3, P4):

### 3.1.2 F2–F6 the analytic curve family (line, circle, ellipse, hyperbola, parabola)

- **Behaviour**: line `origin + direction·t` (the whole real axis, never closed); circle `center + r·(cos u, sin u)` and ellipse `center + a·cos u·X + b·sin u·Y` (domain `[0, 2π]`, closed, period `2π`); hyperbola `center + a·cosh u·X + b·sinh u·Y`, only the `+X` branch, parabola `vertex + u²/(4f)·X + u·Y` (both unbounded and not periodic, `reversedParameter` is `-u`). All are `CN`.
- **Constraint** (C1–C3): each type accepts only the sizes it can use (direction, radius, semi-axes, focal distance); otherwise `isValid()` is false and point queries collapse onto the finite degenerate value stated in that type's documentation.
- **Requirement** (R1, R2): closed-form evaluation, no allocation; the circle's `derivative` has length equal to the radius, the ellipse's second derivative is `center - P(u)`, the hyperbola's acceleration is `P(u) - center`, the parabola's acceleration is the constant `X/(2f)`.
- **Acceptance** (T1–T6):
- **Caveat** (N1–N4, N10, P1–P3):

### 3.1.7 F7–F9 family queries and the bounding box

- **Behaviour**: the nearest point takes the zeros of the stationary points of the squared distance (narrowed by `geom::detail::rootInBracket()`) and compares them with the sample points and the window ends; the bounding box is the axis-aligned box of the point set traced by `evaluate()` inside the domain (circle `center ± |r|`, ellipse via the support function, a circle arc via the two endpoints plus the axial extrema inside the interval); intersection returns a three-valued result (`None` / `Isolated` with both parameters / `Coincident` without points).
- **Constraint** (C5–C7): a query window must be finite, and an unbounded curve is given one by the caller; the box exists only for a bounded type and must contain the point set; `Coincident` lists no points.
- **Requirement** (R1, R2): uses only `evaluate` / `derivative` / the domain ends / `isValid` and `closestPointOnCurve()`; throws nothing.
- **Acceptance** (T7–T9):
- **Caveat** (N5–N7, P7–P9):

### 3.1.10 F10 planar splines (Bézier and B-spline)

- **Behaviour**: `BezierCurve` evaluates by blending the poles with de Casteljau, the divided differences give the first two derivatives, the domain is `[0, 1]`, `isClosed()` looks at the first and last pole only, and reversal is `1 - u`; `BSplineCurve` sums the basis functions of the span, its domain runs between the first and last distinct knots, `continuity()` follows from the interior multiplicities, and `isClosed()` compares the two endpoints. Both become rational with optional weights (the quarter circle is the standard example).
- **Constraint** (C1, C3): poles and weights must be usable; the B-spline also requires a clamped knot vector — first and last multiplicity `= degree + 1`, interior multiplicity `<= degree`. When unusable it answers the first pole (the origin when there is no pole).
- **Requirement** (R1, R2): reuse the existing kernels (Bézier blending, the knot model and the basis are all in `geom::detail`); parameters outside the domain extrapolate for a Bézier and clamp to the endpoints for a B-spline.
- **Acceptance** (T10, T11):
- **Caveat** (N8, P10):

### 3.1.11 F12 the planar offset

- **Behaviour**: `OffsetCurve<T>` gives `C + d·N`, `C' + d·N'`, `C'' + d·N''`; the parameter domain and the reversed mapping forward to the basis curve, closedness requires the two endpoints to coincide (the basis being closed is not enough), the period follows the basis, and the order follows the basis capability.
- **Constraint** (C8): a positive distance is to the left of the direction of travel; a reversed basis needs a negated distance to trace the same geometric curve; where the basis velocity is zero the query falls back to the basis curve's own value.
- **Requirement** (R1, R2): the basis curve is held by value and allocates no extra memory; the third derivative is taken from the concrete type only (2.13).
- **Acceptance** (T12):
- **Caveat** (N9, P11):

### 3.1.12 F13–F15 scalar measures and the two-curve distance

- **Behaviour**: `arcLength()` integrates the speed over the window; `parameterAtLength()` inverts the same integral; `signedCurvature()` gives the signed curvature (positive when turning left); `curveToCurveDistance()` gives the nearest pair of points of two curves.
- **Constraint** (C5, C9): the window is finite and forward; the length tolerance is interpreted in length units; the arc length must be non-negative and not exceed the window total; curvature answers empty where the speed is zero; a distance has an answer only when both curves are valid.
- **Requirement** (R1, R2): depends only on `evaluate` / `derivative` (curvature also on `secondDerivative`) and `isValid`; throws nothing; `distance == |first_point - second_point|`.
- **Acceptance** (T13–T15):
- **Caveat** (N11–N13, P12–P14):

## 4. Public Interface

| Header | Content |
|---|---|
| `geom_global.hpp` | the module's only global: `VN_GEOM_API`, `VN_GEOM_PLANAR_NS_BEGIN` / `_END` and `VN_GEOM_SPATIAL_NS_BEGIN` / `_END`, and the two re-exports of `Continuity` |
| `Curve.hpp` | `Curve<T>` and `isCN()` |
| `ConicCurve.hpp` | `ConicCurve<T>`: `center` + `x_direction` + `yAxis()` + `CN` |
| `LineCurve.hpp` | `LineCurve<T>`, `LineCurvef` / `LineCurved` |
| `CircleCurve.hpp` | `CircleCurve<T>`, `CircleCurvef` / `CircleCurved` |
| `EllipseCurve.hpp` | `EllipseCurve<T>`, `EllipseCurvef` / `EllipseCurved` |
| `HyperbolaCurve.hpp` | `HyperbolaCurve<T>`, `HyperbolaCurvef` / `HyperbolaCurved` |
| `ParabolaCurve.hpp` | `ParabolaCurve<T>`, `ParabolaCurvef` / `ParabolaCurved` |
| `TrimmedCurve.hpp` | `TrimmedCurve<T>`, `TrimmedCurvef` / `TrimmedCurved`, `trimCurve()` |
| `BezierCurve.hpp` | `BezierCurve<T>`, `BezierCurvef` / `BezierCurved` |
| `BSplineCurve.hpp` | `BSplineCurve<T>`, `BSplineCurvef` / `BSplineCurved` |
| `OffsetCurve.hpp` | `OffsetCurve<T>`: a `shared_ptr` basis curve and a signed distance, `offsetCurve()` |
| `CurveProjection.hpp` | `CurveProjection<T>` and `closestPointOnCurve()` |
| `CurveCurveIntersection.hpp` | `CurveCurveContact`, `CurveCurveHit<T>`, `CurveCurveIntersection<T>` and `intersectCurveWithCurve()` |
| `CurveLength.hpp` | `arcLength()` and `parameterAtLength()` |
| `CurveCurvature.hpp` | `signedCurvature()` |
| `CurveCurveDistance.hpp` | `CurveCurveDistance<T>` and `curveToCurveDistance()` |

The full signatures and the meaning of every parameter are in the headers; this table states ownership only.

## 5. Constraints, Requirements and Invariants

- **C1**: the parameter uses the type's own dimension (a multiple of the direction for a line, radians for a circle, an ellipse and a circle arc, the hyperbolic angle for a hyperbola, the offset along the directrix axis for a parabola). The parameter domain is a property of the curve, not a precondition of `evaluate`; what a parameter outside the domain means is stated in the type's documentation.
- **C2**: `period()` is meaningful only when `isPeriodic()` is true, and is 0 otherwise; a bounded piece does not forward the period of the whole turn (the same rule as the 3D `TrimmedCurve`).
- **C3**: `isValid()` looks at state only and is independent of the parameter; in an invalid state every query returns the degenerate value stated in the type's documentation and may not produce NaN or infinity.
- **C4**: `evaluate` / `derivative` / `secondDerivative` of the analytic types (line, conics) are constant-time closed-form evaluations and allocate nothing; the two splines reuse the kernels of `geom::detail` (the basis recursion of `BSplineCurve` allocates nothing, the de Casteljau and divided differences of `BezierCurve` take arrays by value, the exception in R2), with a cost growing with degree and pole count. The trimmed and offset views forward through one virtual call. Every implementation throws nothing.
- **C5**: the parameter window of a family query or measure must be finite: an unbounded curve must be given a window by the caller, and a missing, reversed or zero-width one returns `std::nullopt`, with the function never choosing a range (the same rule as `geometry-curve-queries.md` C1; arc length, the inverse and the two-curve distance follow it too).
- **C6**: a bounding box is offered only on a bounded type and answered only by `bounds()`; the interface does not declare `bounds()`, and an unbounded type has no such entry.
- **C7**: the intersection result is three-valued: `Coincident` means "a coincident stretch" and lists no points; only `Isolated` carries `hits`, and each hit's point lies on both curves.
- **C8**: the sign of an offset follows the direction of travel: a positive distance is on the left; to trace the same geometric curve from a reversed basis the distance must be **negated**; at a cusp where the basis velocity is zero the normal is undefined and the query falls back to the basis curve's own value.
- **C9**: the dimension of a scalar measure matches the curve: `arcLength` / `parameterAtLength` speak of a **length**, `signedCurvature` is the **reciprocal of a length**, and `CurveCurveDistance::distance` is a **length**; a tolerance parameter is interpreted in the same dimension, not as a parameter difference.
- **C10**: the shared implementation lives only in `sdk/vine/geom/detail/` and must be dimension-free: it depends only on `C::value_type` and `evaluate` / `derivative`, never names `Point2` / `Point3`, and includes neither dimension tree (checked by `scripts/check_geom_layering.py`). The decision is in 2.5: move it down when the only difference is a type name, keep one each when the difference is mathematical.
- **R1**: a planar curve depends only on `Point2`, `Vector2`, scalars and the existing facilities of `math`; no 3D concept (normal, frame, z component) may be introduced.
- **R2**: a value type holds **no** dynamic memory by default. `BezierCurve` and `BSplineCurve` are the stated exceptions: the poles and optional weights of the former, and the poles / distinct knots / multiplicities / optional weights of the latter, are `std::vector`, which means deep copies and no longer trivially copyable (the same trade as the same-named 3D types); any newly introduced type that holds memory must state the same exception and cost here.
- **N1**: on a type offering `pointAt()` (line, circle, ellipse, hyperbola, parabola) it equals `evaluate(u)`.
- **N2**: `reversedParameter` is an involution and swaps the two ends of the domain (`reversedParameter(firstParameter()) == lastParameter()`). It gives the parameter of the same point on the **reversed** curve — the reversed curve is the original mirrored about the mid-domain axis (the starting axis for a periodic curve), so `evaluate(reversedParameter(u))` does not equal `evaluate(u)`.
- **N3**: `derivative` is the velocity vector, not a unit tangent (equal to the radius for a circle, smallest at the vertex and growing towards both ends for a hyperbola); `secondDerivative` equals the curvature vector only under an arc-length parameterisation (and is constant for a parabola).
- **N4**: for a circle and an ellipse `period() == lastParameter() - firstParameter() == 2π`, and a parameter outside the domain wraps by the period; a trimmed piece answers `2π` only when the interval spans the full basis period and 0 for a shorter piece (C2).
- **N5**: `CurveProjection::point == curve.evaluate(parameter)` and `distance == |point - query|`; with several equidistant solutions one is reported (every point of a circle is equidistant from the centre, and the answer depends on where the scan and the root finding land).
- **N6**: `bounds()` contains every point `evaluate(u)` produces inside the curve's domain (including the endpoints and the axial extrema inside the interval); the ellipse's box is the tight box the support function gives, not a loose box built from the semi-axes.
- **N7**: `CurveCurveHit::point` equals both `first.evaluate(first_parameter)` and `second.evaluate(second_parameter)`; `hits` is ordered by `first_parameter` and one crossing is reported once.
- **N8**: `BezierCurve::degree() == poles.size() - 1`; `isRational() == !weights.empty()`; every `evaluate` of a rational curve lies inside the convex hull of its poles.
- **N9**: `OffsetCurve::evaluate(u) == basis.evaluate(u) + distance * normalAt(u)`, where `normalAt(u)` is the unit +90° rotation of the basis tangent; both of its derivatives are closed-form exact values, not difference approximations.
- **N10**: `isClosed()` true **implies** `evaluate(firstParameter()) == evaluate(lastParameter())`, but not the other way round: a trimmed piece must also cover the basis curve's natural domain. So a `[0, 2π]` piece of a circle is closed, while `[0, 2π + 0.3]` (different ends) and `[0.3, 0.3 + 2π]` (same ends but not covering the natural domain) are not. `isPeriodic()` and `isClosed()` are independent: a piece spanning 1.5 periods is periodic but not closed.
- **N11**: `arcLength()` is a **path length** (backtracking included), not the distance between the two ends; `parameterAtLength()` measures from the window start, and `arcLength(curve, {a, u})` equals the requested length within tolerance.
- **N12**: the absolute value of `signedCurvature()` is the geometric curvature; a positive sign means the centre of curvature is to the left of the direction of travel (the same side as the positive distance of `OffsetCurve`). Walking the same point set backwards flips the sign.
- **N13**: the three equalities of `CurveCurveDistance` hold together: `first_point == first.evaluate(first_parameter)`, `second_point == second.evaluate(second_parameter)`, `distance == |first_point - second_point|`.
- **N14**: `maxDerivativeOrder()` reports the highest order that **can be answered**, not a promise of the contract: `derivativeAt(u, order)` equals the corresponding derivative for `1 <= order <= maxDerivativeOrder()` (the first and second agreeing with `derivative` / `secondDerivative`), and answers `std::nullopt` for an order of 0, a negative order or one beyond the capability. Of the nine types, all report 3 except `OffsetCurve`, which reports 2.

## 6. Caveats and Traps

- **P1 Mixing parameter dimensions.** How it fails: pass an arc length to a circle where an angle is expected and get a point that "looks like it is on the circle but is not where you wanted", with no error. How to detect: recover the parameter from a known point, or change the radius and see the result not scale. How to prevent: read the type's `@param` (a circle always says "radians"); across types, put the dimension in the parameter name, as in `arc_length` / `angle`.
- **P2 Treating `derivative` as a unit tangent.** How it fails: a line's direction is not unit length and a circle's tangent has the length of the radius, so using it as a direction before normalising scales everything wrong. How to detect: the tangent length differs at two points of the same curve. How to prevent: `normalized()` before use (N3).
- **P3 A degenerate state answers something "reasonable".** How it fails: a zero-radius circle answers the centre for every parameter, an ellipse / hyperbola / parabola with a zero main axis answers the centre, a zero-direction line answers the origin for every parameter, and a later algorithm treats it as a real curve. How to detect: the result does not depend on the parameter. How to prevent: test `isValid()` at the entry (C3).
- **P4 Comparing `2π` in `float`.** How it fails: `CircleCurvef::period() == math::PI_TWO` is false and an `isPeriodic()` branch is decided wrongly. How to detect: the same case passes in double and fails in float. How to prevent: compare parameters with a tolerance (`EPS<T>()` or one scaled to the parameter), and compare exactly only for boolean facts.
- **P5 A virtual call through `Curve<T>&`.** How it fails: batch sampling hangs off a base reference, crossing the vtable every time, and the compiler cannot inline. How to detect: disassembly or timing shows the concrete type is clearly faster. How to prevent: hold the concrete type in a hot loop (2.7).
- **P6 Reading the reversed mapping as "the same point".** How it fails: assume `evaluate(reversedParameter(u)) == evaluate(u)` and get the mirror point about the mid-domain axis (the starting axis for a circle or an ellipse), so two curves are used as one. How to detect: the reversed point and the original are symmetric about `u = mid-domain`, the ends swap and the middle points do not coincide. How to prevent: use `reversedParameter` only for "the same point on the reversed curve" (N2); when the same point is wanted, call `evaluate(u)`.
- **P7 The nearest point is a local minimum.** How it fails: a curve approaching the query point repeatedly has the sampling find only one of the minima and the distance comes out too large; a flat minimum where `f` touches zero has no bracket to narrow. How to detect: doubling `sample_count` makes the distance smaller. How to prevent: raise `sample_count` to the scale of the curve's undulation; the full version of the same analysis (open intervals, tangency, sampling aliasing) is in `geometry-curve-queries.md` §6.
- **P8 Building a bounding box from the semi-axes.** How it fails: a rotated ellipse boxed at `±major_radius` is one size too big, and clipping and picking test useless regions. How to detect: the box corners are not on the curve, and the box width is twice the semi-axis. How to prevent: use `bounds()` (the support function) and build no box from the semi-axes by hand (2.10).
- **P9 Tangency and coincidence differ by one sample step.** How it fails: two curves that touch have no transverse root and are missed by default; two intersections closer than three sample steps are read as `Coincident`. How to detect: densifying `sample_count` changes the crossing relation, or shrinking the tolerance makes the coincidence disappear. How to prevent: know that this is a decision about the **shape of the intersection set**, not a point-by-point test; to tell them apart, densify the sampling and set the tolerance far below the radius of curvature.
- **P10 Treating the pole box as a tight box.** How it fails: clipping or sizing with `BezierCurve::bounds()` gives the box of the pole hull, larger than the curve. How to detect: the box corners are far from the curve, and the box size depends only on the pole count. How to prevent: tighten a tight box by sampling; the pole box is only a coarse box for rejection tests.
- **P11 Treating an offset as "differentiable everywhere".** How it fails: taking the offset's second derivative of a basis curve with a cusp (coincident first poles, a zero-length direction) still answers the basis curve's `continuity()`, while the normal does not exist there. How to detect: `derivative().length()` is zero at that parameter. How to prevent: check that the speed is non-zero before using the second derivative; at a cusp, compute once on each side.
- **P12 Treating an arc length as a parameter.** How it fails: handing the result of `arcLength()` straight to `evaluate()` puts the point somewhere unexpected, and changing the parameterisation changes the result again. How to detect: the parameter recovered from a known point disagrees; changing the length of a line's direction scales the result. How to prevent: use `parameterAtLength()` when a parameter is wanted; for a circle arc, `r * θ` can be computed directly.
- **P13 Using a signed curvature as an unsigned one.** How it fails: when the curve bends the other way the curvature turns negative, and the "radius of curvature" taken as its reciprocal is negative too. How to detect: walking the same point set backwards flips the sign. How to prevent: take `abs` for the geometric curvature; the sign is only for the direction of bending.
- **P14 Treating the nearest distance as global.** How it fails: two curves approaching in several places have the sampling and refinement land in only one valley and the distance comes out too large. How to detect: raising `sample_count` makes the distance smaller. How to prevent: sample to the scale of the curve's undulation; when a strict global answer is needed, split into monotone stretches first and take the smallest.

## 7. Rejected Options

- **D1** One generic `Curve<PointT>` contract, or letting `geom::planar::Curve` inherit the 3D contract: `G1`, `G5` — the two dimensions are two concepts, and merging them forces the 2D types to carry a normal and a z component while 3D algorithms accept planar curves by mistake. See `../decisions/ADR-2026-11-05-planar-curve-contract.md`.
- **D2** Borrowing the 3D `ConicCurve` frame (`normal` + `x_direction`) for a planar circle: `G1`, `G8` — the plane has no storable direction, so the two fields can neither be given a value nor checked, and they only cost every circle 24 bytes.
- **D3** Putting a dimension marker in the name (`Curve2d` / `Circle2d`, or the shorter `Curve2` / `Curve3` across all 48 types) or adding `*2d` aliases: `G4`, `G6` — the namespace is already the qualifier and an alias gives one concept two names; a suffix would also misreport the family relation (the two contracts have zero inheritance and are not interchangeable), and `BezierCurve3` already means "cubic" in CAD.
- **D4** Copying the whole 3D curve family over now: `G8` — a type with no consumer only drifts on both sides at once; types enter one by one under the triggers in `../backlog/geom-planar.md`.
- **D5** Following OCCT and giving a planar circle a starting axis too (`Geom2d_Circle` derives from `Geom2d_Conic` and holds a `gp_Ax2d`): `G8`, `G4` — circles of the same radius with a rotatable start are one geometric object, and one more writable state only adds another implicit convention about "where parameter 0 is"; no consumer has ever asked for a rotated circle's parameter origin.
- **D6** Keeping a circle-only trimmed type next to the planar arc (the former `ArcCurve` is deleted): `G1`, `G8` — "a piece of a curve" should have one expression, and a circle-only type answering the same concept as the generic view would drift in two places. See `../decisions/ADR-2026-11-05-planar-trimmed-curve.md`.
- **D7** Adding a twelfth pure virtual `thirdDerivative()` to `Curve<T>` (or approximating the offset's second derivative by differences): `G4`, `G10` — one derived type's need may not raise the whole contract, and an approximation replaces the meaning "a derivative is exact". The actual answer is the capability protocol, see `../decisions/ADR-2026-11-05-curve-derivative-capability.md`.
- **D9** Following OCCT and putting `DN(order)` on the interface together with "throws `NotImplemented` when unimplemented": `G4`, `G10` — the order is an algorithm parameter, not type identity, and a query beyond the capability should be an inspectable empty value rather than an exception or a truncation. The capability protocol keeps "ask on demand" without paying for exceptions.
- **D8** Making arc length, curvature or the two-curve distance a virtual of `Curve<T>`: `G1`, `G8` — they are "a measure of a curve" or "the relation between two curves", not the identity of one class; on the interface the nine implementations would each write them and every curve type would be forced to support what it does not need. They stay free functions (§2.14–2.16).
- **The module-level rejections** (`math` keeping root finding / integration and the spline conventions, the two trees moved down into `geom/planar` + `geom/spatial`, flattening with a `2` suffix on 2D, a suffix on both trees) are outside this page, see `geometry-module.md` §7 and `../decisions/ADR-2026-11-06-geom-tree-layout-and-naming.md`, `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | parameterisation: a line walks along its direction, a circle walks its angle | `WalksAlongItsDirection`, `WalksItsAngle` in `tests/test_geom_planar/CurveBasicsTest.cpp` |
| T2 | derivative semantics: constant velocity on a line, velocity and centripetal acceleration on a circle | `HasAConstantVelocity`, `TurnsTowardsItsCentre` in the same file |
| T3 | domain / closedness / period / smoothness | `IsUnboundedAndNeverClosed`, `IsClosedPeriodicAndSmooth` in the same file; `ARangeThatOvershootsTheCircleIsNotClosed` in `TrimmedCurveTest.cpp` (covers the whole span but walks one corner further, so not closed) |
| T4 | a degenerate state answers a degenerate value, not NaN | `AZeroDirectionIsNotALine`, `ANonPositiveRadiusIsNotACircle` in `CurveBasicsTest.cpp`; the degenerate cases in `EllipseCurveTest.cpp` and `ConicCurvesTest.cpp` |
| T5 | the reversed mapping is an involution that swaps the domain ends, and is usable through `Curve<T>&` and `unique_ptr<const Curve<T>>` | `ReversesByNegatingItsParameter`, `ReversesByReflectingInsideThePeriod`, `AnswersThroughTheBaseInterface` in `CurveBasicsTest.cpp`; the reversal and polymorphism cases in `EllipseCurveTest.cpp` and `TrimmedCurveTest.cpp` |
| T6 | contract sweep: ordered domain, reversal involution, coincident ends of a closed curve, a periodic curve repeating by `period()`, `period() == 0` on a non-periodic curve, the capability pair agreeing with the named derivatives and answering empty beyond it, nine types × float / double; plus a derivative-chain check (each derivative agreeing with the central difference of the previous one, including both rational forms) and a check that "the capability stays visible after type erasure" | `EveryPlanarCurveKeepsItsPromisesInDouble` / `…InFloat` / `EveryCurveDifferentiatesConsistently` / `TheDerivativeCapabilityReportsWhatEachTypeCanAnswer` in `tests/test_geom_planar/CurveContractTest.cpp` |
| T7 | nearest point: exact foot on a line, an unbounded curve must be given a window, a foot outside the window falls on an end, feet on a circle / ellipse / arc, the equidistant solutions from the centre, invalid curves, both precisions | `tests/test_geom_planar/CurveProjectionTest.cpp` |
| T8 | bounding box: negative / zero radius of a circle, the support function of a rotated and a 45° ellipse, the extrema of a circle arc inside an interval, a reversed interval, the box containing every sample point, both precisions | `tests/test_geom_planar/BoundsTest.cpp` |
| T9 | intersection: line × line, parallel lines, line × circle (two points ordered by the first curve's parameter), two circles, a trimmed piece clipped by the window, a coincident arc and a coincident circle, an unbounded curve without a window, invalid curves, both precisions | `tests/test_geom_planar/CurveIntersectionTest.cpp` |
| T10 | Bézier: walking the poles, divided-difference derivatives, the rational quarter circle, invalid poles / weights, closedness and reversal, the pole box, the family algorithms working unchanged (nearest point and intersection), both precisions | `tests/test_geom_planar/BezierCurveTest.cpp` |
| T11 | B-spline: a clamped quadratic equal to a Bézier, a polyline knot vector, multiplicities deciding `C0` / `C1`, the rational quarter circle, clamping outside the domain, an inconsistent knot vector, closedness and reversal, the pole box, the family algorithms, both precisions | `tests/test_geom_planar/BSplineCurveTest.cpp` |
| T12 | offset: a line shifted left, a counter-clockwise circle moving inwards, the ellipse and Bézier second derivative agreeing with differences, a spline third derivative, a reversed basis with a negated distance, zero distance giving the basis, no NaN at a cusp, the family algorithms usable | `tests/test_geom_planar/OffsetCurveTest.cpp` |
| T13 | arc length and inverse: a line by its speed, a circle arc by its radius, Bézier agreeing with a dense polyline, an offset circle keeping its length, the rational quarter circle, a cusp still finite, an unbounded curve without a window / reversed / zero-width rejected, the interior inverse and both precisions | `tests/test_geom_planar/CurveLengthTest.cpp` |
| T14 | curvature: 0 on a line and a line-like spline, 1/r on a circle (including float), 1 on the rational quarter circle, `a/b²` and `b/a²` at the two axis ends of an ellipse, the sign at a parabola's vertex, agreeing with a difference of tangent angles, empty at a cusp and on an invalid curve | `tests/test_geom_planar/CurveCurvatureTest.cpp` |
| T15 | two-curve distance: parallel lines, line × circle, two circles, 0 when they cross, spline × line, the window pushing the optimum onto an end, an offset curve taking part, the two parameters agreeing with the points, an unbounded curve without a window, invalid curves, both precisions | `tests/test_geom_planar/CurveDistanceTest.cpp` |
| T16 | family-algorithm sweep: all nine types accept the nearest point, arc length, inverse, curvature and two-curve distance through `Curve<T>` alone, `arcLength` and `parameterAtLength` agree with each other, run once in float and once in double | `EveryPlanarCurveWorksWithTheFamilyAlgorithmsInDouble` / `…InFloat` in `tests/test_geom_planar/CurveContractTest.cpp` |

## 9. Status and Evolution

### 9.1 Current state

Sixteen headers (`Curve` / `ConicCurve` / `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `BezierCurve` / `BSplineCurve` / `TrimmedCurve` / `OffsetCurve` / `CurveProjection` / `CurveCurveIntersection` / `CurveLength` / `CurveCurvature` / `CurveCurveDistance`) + `test_geom_planar` (138 cases, including the contract sweep of the nine types × float / double, the derivative chain and capability checks, the two-precision comparison of the six family algorithms and the "nine types × every algorithm" sweep); `vn::Geom2D` is an INTERFACE target exporting no symbol; the module has no production consumer — the only user today is the test.

### 9.2 Known gaps

- A tight box of a piece exists only for the circle (`CircleCurve::bounds(first, last)`): a trimmed view over any other basis curve erases the support function and has no tight box; not filled before a consumer appears.
- The family algorithms cover nearest point, intersection, nearest distance between two curves, arc length and its inverse, and curvature; the control polygon, area and spline knot insertion are not done (no consumer).

### 9.3 Compatibility / migration

Purely additive: the interfaces and types of `geom/spatial` are unchanged and the two modules do not reference each other. A new planar type affects this module and `tests/test_geom_planar` only; and because the target is INTERFACE, there is no ABI compatibility to maintain.

## 10. References

### 10.1 Code

- `src/base/geom/sdk/vine/geom/planar/Curve.hpp`, `ConicCurve.hpp`, `LineCurve.hpp`, `CircleCurve.hpp`, `EllipseCurve.hpp`, `HyperbolaCurve.hpp`, `ParabolaCurve.hpp`, `BezierCurve.hpp`, `BSplineCurve.hpp`, `TrimmedCurve.hpp`, `OffsetCurve.hpp`, `CurveProjection.hpp`, `CurveCurveIntersection.hpp`, `CurveLength.hpp`, `CurveCurvature.hpp`, `CurveCurveDistance.hpp`, `geom_global.hpp`
- The planar box type: `src/base/math/sdk/vine/math/Rect2.hpp` (this module builds no box of its own)
- Spline and numeric kernels (all dimension-free): `src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp` (knot model and basis), `BezierBlend.hpp` (Bernstein blending), `Quadrature.hpp` / `RootFinding.hpp` (root finding and integration); the search invariant of `knotSpan` is in `../bugs/spline-knotspan-hang.md`
- `tests/test_geom_planar/CurveBasicsTest.cpp`, `EllipseCurveTest.cpp`, `ConicCurvesTest.cpp`, `TrimmedCurveTest.cpp`, `BezierCurveTest.cpp`, `BSplineCurveTest.cpp`, `OffsetCurveTest.cpp`, `CurveContractTest.cpp`, `CurveProjectionTest.cpp`, `BoundsTest.cpp`, `CurveIntersectionTest.cpp`, `CurveLengthTest.cpp`, `CurveCurvatureTest.cpp`, `CurveDistanceTest.cpp`
- The 3D counterparts: `src/base/geom/sdk/vine/geom/spatial/Curve.hpp`, `ConicCurve.hpp`, `EllipseCurve.hpp`, `HyperbolaCurve.hpp`, `ParabolaCurve.hpp`, `BezierCurve.hpp`, `BSplineCurve.hpp`, `OffsetCurve.hpp`, `TrimmedCurve.hpp`, `CurveProjection.hpp`, `CurveSurfaceIntersection.hpp`

### 10.2 Documents and ADRs

- `geometry-kinds.md` 2.7 — the eleven curve interface items and the facts kept off the interface (this module follows them item by item).
- `geometry-curve-queries.md` — the 3D nearest point (the same stationary-point root finding and window rule, followed item by item in 2.9 here), the nearest distance between two curves (2.8, the same shape as 2.16 with `Point3`), and arc length / curvature (2.6–2.7, the same shape as 2.14–2.15; 3D curvature is unsigned).
- `geometry-primitives.md` 2.3, 2.8 — the naming and precision-instantiation rules of the value types.
- `../decisions/ADR-2026-11-05-planar-curve-contract.md` — why this is a second contract.
- `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md` — `isValid()` on the interface.

### 10.3 External material

- OpenCASCADE `TKG2d/Geom2d` (`Geom2d_Curve`, `Geom2d_Conic`, the five analytic curves, `Bezier` / `BSpline` / `Trimmed` / `Offset`) serves only as a reference for the **capability list and the parameter conventions**: the `gp_Ax2d` of a conic (origin + starting axis) is isomorphic to this module's `ConicCurve`, and the meaning of the eccentric angle / hyperbolic angle / focal distance matches OCCT. The API shape is not copied: OCCT's objects are mutable and reference-counted, with a transform family and an in-place `Reverse()`; this module is value types + `reversedParameter` (D5, `geometry-kinds.md` 2.7).
