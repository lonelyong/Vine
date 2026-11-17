# geometry kinds: Curve and Surface

> **Status**: current
> **Last verified**: 2026-11-04
> **Scope**: the two evaluation interfaces `Curve` / `Surface`, the membership of the concrete value types, and the minimum contract for a new type.
> Parametric geometry (`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`) **is not a family**: the modelling side has one modelling class `Primitive`, and the geometry side has the five kinds of the family `Solid<T>` (see `geometry-parametric.md`, `geometry-solid.md`).
> Not the full contract of a concrete type (see `geometry-primitives.md`, `geometry-parametric.md`, `geometry-ray.md` and the individual headers), not the intersection and distance algorithms.
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/Curve.hpp`, `Surface.hpp`; `src/base/geom/sdk/vine/geom/Continuity.hpp`; `tests/test_geom_spatial/GeometryKindsTest.cpp`.
> **Backlog**: `../backlog/geometry.md`

## 1. Overview

### 1.1 Problem and background

The geometry types have no classification relation: `LineCurve`, `Ray`, `Plane` and `Box` are unrelated classes, and "this is a curve / a surface / a solid" exists only in discussion. Writing a constraint over a family, putting a family into one container, or answering "how many dimensions does this type have" all rely on guessing from a name; the only run-time classification is `ShapeType`, which covers `Shape` subclasses only, so value types are not in it at all.

### 1.2 Goals

- Each of the two families has one name, and membership is declared at the type definition (`class CircleCurve : public ConicCurve`).
- Classification is queryable at compile time (`std::is_base_of_v<Curve<double>, CircleCurved>`) and by enum (`Surface::kParameterCount`).
- Geometry can be used as "a parameterised object of unknown type": for a curve evaluation, derivatives, parameter domain, smoothness and reversed mapping (2.7), for a surface evaluation, both first derivatives, the parameter domains and closed periods in both directions (2.10).

### 1.3 Non-goals

- The interface carries only what a parameterised object necessarily answers: a surface's normal, bounding box, second derivatives, reversal and transforms stay off it (2.10), and likewise for curves (2.7).
- No heterogeneous container: a "list of mixed curves" uses `std::variant` + `std::visit`; the interface answers about "one" curve / "one" surface (2.4).
- No family tag on parametric geometry: it belongs to neither the `Curve` nor the `Surface` tree; on the geometry side it is the family `Solid<T>` (ray–box is still the `Aabb` slab, and solid semantics belong to `Solid<T>::intersectWith()`); which shapes can form a model is answered by `ShapeType`.
- The family-level surface algorithms (nearest point, ray × surface) are in `geometry-surface-queries.md`, and the nearest point / distance on a curve in `geometry-curve-queries.md`; solid intersection still has no consumer (`G8`), see `../backlog/geometry.md` G3.

### 1.4 One-paragraph conclusion

**`Curve` and `Surface` are both evaluation interfaces, and a surface has one more classification tag; parametric geometry belongs to no family — it is only a modelling parameter.**

## 2. Design Points

### 2.1 Curve and Surface are both evaluation interfaces

- **What**: `Curve<T>` (the eleven items of 2.7) and `Surface<T>` (the fourteen of 2.10) are both **abstract interfaces**: a virtual destructor + evaluation and the first derivatives + parameter domain, closedness, period + smoothness + validity. `LineCurve` / the four conics / `TrimmedCurve` / `Plane` / `SphericalSurface` implement them.
- **Why**: `G4`, `G9` — "evaluate an unknown type and ask for its parameter domain" can only go through virtual functions. The two families ask the same group of facts, one with one parameter and one with two, so the interfaces correspond one to one (2.7 and 2.10).
- **Cost**: a type implementing the interface carries a vptr, is no longer trivially copyable, and its `sizeof` grows by one pointer. Curves are all cold types; `Plane` is copied in ray intersection, which is why it stores **no** in-plane frame (2.10).
- **`Ray` is not in the family**: it is copied by value per triangle in picking, and adding a vptr would give back the measured inline gain (`geometry-ray.md` 2.7); it keeps only `pointAt`.
- **`TrimmedCurve<T>`**: holds a `std::shared_ptr<const Curve<T>>` basis curve + a parameter interval, and `evaluate` forwards to the basis; closedness / periodicity are decided by the basis and the interval together (closed only when the interval spans the whole domain **and the two ends coincide**, periodic only when it spans one period).

### 2.2 The classification tag itself stays free

- **What**: `ElementarySurface<T>` / `BoundedSurface<T>` only classify, adding no state and no virtual function; the vptr of `Surface<T>` is the surface family's only polymorphism cost. `sizeof(Planed)` = two vectors + vptr, `sizeof(SphericalSurfaced)` = centre + radius + vptr, pinned by `static_assert`.
- **Why**: `G10`, `G4` — "deriving" should not make a type any bigger; one more member in the tag layer is one more piece of data on every copy.
- **Boundary**: we no longer claim "surfaces are free" — since 2026-11-04 a surface pays a vptr exactly as a curve does (`../decisions/ADR-2026-11-04-surface-evaluation-interface.md`); `Ray` is still outside the family, and a conic's frame stores three vectors once in `ConicCurve` (2.9).

### 2.3 Membership

| Family | Dimension | Members | Meaning of the parameter |
|---|---|---|---|
| `Curve` | 1 | **abstract interface**: `evaluate` / derivatives / parameter domain / closedness / period / smoothness / reversed mapping / validity (2.7). Implemented by `LineCurve`, `CircleCurve`, `EllipseCurve`, `HyperbolaCurve`, `ParabolaCurve`, `TrimmedCurve`, `BezierCurve`, `BSplineCurve`, `OffsetCurve`; abstract intermediate layers `ConicCurve`, `BoundedCurve` | `LineCurve` is a multiple of `direction`, circle / ellipse the eccentric angle, hyperbola the hyperbolic angle, parabola the offset along the directrix axis, the two splines their own knot representation |
| `Surface` | 2 | **abstract interface**: `evaluate` / both first derivatives / parameter domains / closedness and period in both directions / smoothness (2.10). Implemented by `Plane`, `SphericalSurface`, `CylindricalSurface`, `ConicalSurface`, `ToroidalSurface`, `RectangularTrimmedSurface` | sphere by azimuth and polar angle, plane by two in-plane coordinates |
| `ElementarySurface` | 2 | `Plane`, `SphericalSurface`, `CylindricalSurface`, `ConicalSurface`, `ToroidalSurface` (all implemented); each surface of revolution holds one `Placement` value (see `geometry-axis-surfaces.md` 2.1) | unbounded or semi-infinite analytic support surfaces |
| `BoundedSurface` | 2 | `RectangularTrimmedSurface` (a view), `BezierSurface`, `BSplineSurface` (both implemented, see `geometry-axis-surfaces.md` 2.4 / 2.5) | a finite parameter domain, so `bounds()` can always be answered |

- **Why**: `G1`, `G12` — the dimension decides only "how many parameters a point takes", and the concrete meaning stays on the type; lifting the parameter meaning into the base class would invent a convention every type has to reinterpret.
- **Default construction exists in two cases only**: (1) the default value is itself meaningful geometry (`Placement` is the unit frame, `CircleCurve` is the unit XY circle, `SphericalSurface` is the unit sphere); (2) the default is an **empty state `isValid()` recognises** (`BezierCurve` / `BSplineCurve` / `BezierSurface` / `BSplineSurface` have no data and `isValid()` is always false). The surfaces of revolution and the plane are neither — "a unit cylinder looks entirely legal" — so they declare no default constructor at all, and `static_assert(!std::is_default_constructible_v<CylindricalSurfaced>)` pins that.
- **Parametric geometry is not in the table**: `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` are **modelling types** (subclasses of `Primitive`, see `geometry-parametric.md`), not members of a geometry family. "Which geometries can form a model" is answered by the five subclasses of `Primitive`; computing geometry uses a value type (the solid value type is `Solid<T>`, see `geometry-solid.md`) or a surface.

### 2.4 Run-time polymorphism uses a variant

- **What**: `Solid<T>::Shape` (`std::variant<Box, Sphere, Cylinder, Cone, Ellipsoid>`, the five analytic solids) is the current case; the members and tessellation of `Solid<T>` dispatch through `std::visit`, so a new kind is a compile error. The modelling side has no variant: `Primitive` is a base class with identity, and `brep` and `robotics::io` dispatch by `shapeType()` + `obj_cast`.
- **Why**: `G4`, `G9` — a variant is a closed set, so tag and content cannot disagree; adding a member is a compile error naming every consumer, not a run-time silent fall-through to `default`.
- **Boundary**: the variant holds **values**, so it coexists with value semantics; holding pointers plus virtual functions would circle back to the rejected design.

### 2.5 A new type gets only a contract that can be verified

- **What**: `CircleCurve` offers `isValid()`, `pointAt(angle)`, `circumference()`, `bounds()`; ellipse / hyperbola / parabola offer `isValid()`, `pointAt(u)`, `bounds()` (ellipse only, closed and bounded); `SphericalSurface` offers `pointAt(azimuth, polar)` / `normalAt` / `bounds()`; `LineCurve` offers `pointAt(t)` / `closestPoint` / `intersectWith`.
- **A single triangle or quad gets no type**: `intersectTriangle(ray, a, b, c, hit)` takes three corner points directly and needs no type; the names `TriangleSurface` / `QuadSurface` are registered as reserved names in the backlog (`../backlog/geometry.md` G7), to be introduced when they grow queries of their own (ray–surface intersection, point-to-surface distance), and then with the `Surface` suffix — the repository already has `TriangleMesh` / `QuadMesh`, and a bare name would be read as a mesh element (`G4`).
- **Why**: `G8` — a capability with no consumer is not written: ray intersection, point-to-curve distance and surface intersection are all in the backlog waiting for their trigger. Those with consumers (family-level surface queries, point to curve, analytic solids) have been given consumer-side evidence under this test.
- **Boundary**: `PointCloud` is still a `Shape`, neither a curve nor a surface: it is a discrete point set with no parameterised point; `Box` and the other analytic solids are modelling types (subclasses of `Primitive`) or value types (`Solid<T>`), and belong to no family either.

### 2.6 Algorithms live on the value types, with no separate algorithm class

- **What**: a query is a **member of the object being intersected** — `Aabb::intersectWith(const Ray&, RayHit&)`, `Obb::intersectWith(...)`, `Plane::intersectWith(const LineCurve&, ...)`, `Solid<T>::closestPoint()` / `intersectWith(Ray)`; a triangle with no value type uses the free function `intersectTriangle`. There is no algorithm class such as `SphericalSurface` / `CylindricalSurface`.
- **Why**: `G1`, `G6`, `G12`, `G13` — the name and the data exist once (the radius is in `Solid<T>::Sphere`, and an algorithm class would not store it again); a caller need not look up "which class owns sphere intersection"; it matches the existing shape in `geometry-ray.md` 2.2; and an algorithm can only live on the geometry side (`G13`).
- **Boundary**:
  - An operation between two objects (boolean, nearest distance between solids, solid–solid intersection) belongs to neither → **free function**, in the module both share.
  - A family query (point to surface, ray × surface, curve × surface) has no "single concrete object that can answer it", so it is a free function plus a parameter window (`geometry-surface-queries.md`, `geometry-intersection.md` 2.9).
  - An operation needing a backend (STEP parsing, B-rep tessellation) → `brep`; the pure-maths tessellation of the analytic solids stays in `geometry` and, because it is float, is written as a free function (`geometry-tessellation.md` 2.1).
  - The analytic and trimmed surfaces of the surface family are all in `geometry` (the surfaces of revolution in `geometry-axis-surfaces.md`) and do not depend on OCCT; the `Geom_*` side of `brep` is another representation (reference-counted + mutable) and is not reimplemented here (`G1`).

### 2.7 The core properties of the curve interface

- **What**: the five groups of `Curve<T>`, all pure virtual — (1) evaluation and derivatives: `evaluate(u)`, `derivative(u)`, `secondDerivative(u)`; (2) parameter domain and period: `firstParameter()` / `lastParameter()` / `isClosed()` / `isPeriodic()` / `period()`; (3) smoothness: `continuity()` (and the non-virtual `isCN(n)` derived from it); (4) reversed parameter mapping: `reversedParameter(u)`; (5) validity: `isValid()` (whether the state can be evaluated, independent of the parameter). Plus one pair of **non-pure capability members**: `maxDerivativeOrder()` (default 2) and `derivativeAt(u, order)` (answers the first and second order by default, `std::nullopt` beyond).
- **Why**: `G4`, `G9` — these eleven items are the minimal complete set for the concept "parameterised curve": a point, its rate of change, where it is valid, how smooth it is, how the parameter changes when walked backwards, and whether this state is usable at all. Tangent / normal, discrete sampling, curvature and B-rep seam decisions all depend on these alone and need not know the concrete type.
- **`derivative` is the velocity vector, not a unit tangent**: `circle.derivative(u)` has the length of the radius, and a direction is normalised by the caller. This is written into the interface rather than left to a convention (`G4`); the same for `secondDerivative`, which equals the curvature vector only under an arc-length parameterisation.
- **`continuity()` uses a ladder enum**: `Continuity` is ordered by smoothness, and `>=` means "at least this smooth"; the analytic types (line, circle, plane) answer `CN`. `Polynomial` / `Rational` are not in it — they describe a representation (the nurbs form), not smoothness.
- **`period()` says only "does this object repeat"**: when `isPeriodic()` is false `period()` is always 0, even for an interval cut from a periodic curve. A trimmed view therefore does **not** forward the basis period: a quarter circle is "a short piece with two endpoints", and answering 2π would let callers of the form `if (period() > 0)` treat a short piece as a full turn (`G4`). The basis period is asked of the basis curve — `TrimmedCurve::basis_curve` is public.
- **`reversedParameter` instead of `reverse()`**: reversal is a property of the parameterisation, not an edit of the object. A curve is immutable and a basis curve may be shared by several trims, so a mutable reversal would make trims sharing one basis interfere; a reversed curve is built as its mirror (negate `direction` for a line, `normal` for a circle, swap the interval for a trim), and this function only answers "the parameter of the same point under the reversed parameterisation". A trim reflects **inside its own interval** (`first + last - u`), not on the basis curve.
- **`isValid()` is on the interface**: every implementation already answered "can this state be evaluated" (C12), but the same question could not be asked through `const Curve<T>&` — a caller had to know the concrete type. It is an answer about **state**, independent of the parameter, asked the same way as on `Shape` / `Primitive`; on the interface, family algorithms and sweep tests can guard in one place (see `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md`).
- **Boundary (what stays off the interface)**:
  - `bounds()`: only a finite curve can always answer, and a line can only return an infinite box, which disguises "unbounded" as a box (`G4`).
  - Arc length / `length()`: needs an algorithm and a parameterisation tolerance, so it is an algorithm, not a property.
  - The transform family (OCCT's `Transform` / `TransformedParameter`): the coordinate system lives on a `Shape` node, and a curve holds no placement.
  - The `curveType()` enum: `dynamic_cast` and the vtable already answer "what am I", and maintaining an enum too would be two homes (`G1`).
  - Derivative implementations of specific orders: the order is an **algorithm parameter and a type capability**, so the interface answers only "how far can you go" (`maxDerivativeOrder()`) and "what is this order" (`derivativeAt()`), with the closed form of the third derivative left on the types that can answer it; OCCT's `DN(order)` + `NotImplemented` is the counter-example (D10). See `../decisions/ADR-2026-11-05-curve-derivative-capability.md`.

### 2.8 The derivative ladder reaches the third order: splines and offsets both answer

- **What**: the eleven pure virtual items of `BezierCurve` / `BSplineCurve` / `OffsetCurve` are all implemented, and all three are **constructible value types**; the whole spatial curve family reports `maxDerivativeOrder() == 3` (a trimmed view forwards to its basis) and the offset reports 2:
  - `BezierCurve`: one de Casteljau algorithm used repeatedly — poles, first differences, second differences, third differences, with `P' = n * D1(u)`, `P'' = n * (n - 1) * D2(u)`, `P''' = n * (n - 1) * (n - 2) * D3(u)`; the rational form runs the same set in homogeneous coordinates, with the quotient rule substituting `P'` / `P''` / `P'''` in turn.
  - `BSplineCurve`: evaluate only the `degree + 1` basis functions of the span (Cox–de Boor with the derivative recursion), weight the poles and sum, and take the third derivative from the third layer of the same recursion; the rational form uses the quotient rule `P''' = (A''' - 3 P'' w' - 3 P' w'' - P w''') / w`; `isClosed()` compares the two endpoints, a geometric fact rather than a data fact.
  - `OffsetCurve`: `evaluate` / `derivative` use `N = reference_direction x p'` and `N' = reference_direction x p''`; `secondDerivative` adds one more layer `N'' = reference_direction x p'''`, and the second-order chain is written with `U = N/|N|`, `k = (N·N')/|N|²` as `U'' = N''/R - 2 N' k/R + U (k² - k')`, falling back to the basis curve itself when the cross product degenerates (no NaN).
- **Why the contract still has only eleven items**: the third derivative is a **capability**, not an obligation — a concrete type reports `maxDerivativeOrder()` as 3 and overrides `derivativeAt()`, `Curve<T>` gains no twelfth pure virtual, and a custom implementation that only reaches the second order writes nothing (2.7, `../decisions/ADR-2026-11-05-curve-derivative-capability.md`).
- **Why the offset reports 2 and not 3**: the n-th derivative of an offset needs the (n+1)-th of the basis, `P''` uses `p'''` and `P'''` would need `p''''`. So `OffsetCurve::maxDerivativeOrder()` is `min(2, basis capability − 1)`: when every spatial concrete type answers 3 the offset answers 2; an offset of an offset answers 1, `derivativeAt(u, 2)` answers empty and `isValid()` is false. The same rule as the planar side (`geometry-planar.md` 2.13). See `../decisions/ADR-2026-11-06-space-third-derivative-and-offset.md`.
- **Why the splines gained their numeric answers**: the data model and the type facts were defined before 2.8 (`G1`) and only the numeric functions were missing; they are the only curves in the repository that own memory, construction and copy semantics were already settled, and adding evaluation changes no contract.
- **Three trade-offs of the data model (unchanged)**:
  - knots store **distinct values + multiplicities**: the multiplicity is the only thing a caller asks for, and storing an expanded vector would mean regrouping on every query (`G4`). The two end multiplicities are degree + 1 (clamped) and an interior multiplicity is ≤ degree — at degree + 1 the two sides are completely independent and the parameter jumps, which is two curves rather than one (C13, `../decisions/ADR-2026-11-04-bspline-without-knot-breaks.md`).
  - weights use **an empty array for the polynomial case**, not an `is_rational` boolean: a flag and the data can contradict each other, an empty array cannot (`G1`).
  - `BezierCurve` stores no degree: the degree is the pole count − 1, and of two copies one goes stale.
- **Boundary**:
  - one query touches the poles of one span only; `BSplineCurve` uses `flatKnot()` to walk the flat index back to the distinct knots by linear scan — one fact, one scan, an explicit trade of `G1` for `G10` (recorded in the header's `@note`).
  - behaviour outside the domain differs: `BezierCurve` extrapolates polynomially, `BSplineCurve` clamps the parameter into the knot domain and takes the endpoint value outside (as OCCT's `Geom_BSplineCurve` does), both stated in the header (C10, P3).
  - the three are still the only curve types in the repository that own memory (`std::vector` fields), so they are "values in the deep-copy sense" and no longer trivially copyable; `LineCurve` / the conics / `Ray` still allocate nothing.

### 2.9 The frame and the smoothness the conics share live in `ConicCurve`

- **What**: `ConicCurve` holds the frame `center` / `normal` / `x_direction` (normalised at construction), offers the derived `yAxis()` / `planeNormal()`, and fixes `continuity()` at `CN`; the four conics declare only their own scalars (radius / two semi-axes / focal distance) and their numeric answers.
- **Why**: `G1`, `G6` — if the frame lived once in each of `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` (`yAxis()` / `planeNormal()` / `continuity()` four copies each), one change would mean four edits, and the normalisation rule could silently diverge in four places. OCCT's `Geom_Conic` likewise holds the frame in a `gp_Ax2` and fixes `Continuity()` at Cn; this only writes the same thing into our types.
- **Cost**: `CircleCurve` no longer derives its start axis from `normal` every time but stores `x_direction` once at construction, so it grows by 24 bytes (the size assertions in 2.2 follow); and the frame fields cannot be rewritten after construction — which turns "writing it silently moves the parameter start" into "writing it makes the three vectors contradict each other", the latter being what should be forbidden, as written in every conic's `@warning`.
- **A parabola's origin is its vertex**: `ParabolaCurve` stores no separate `vertex` and uses the inherited `center` (OCCT's `Geom_Conic::Location()` likewise returns the vertex for a parabola).
- **The answers of a degenerate frame live in `ConicCurve`**: with a zero `normal`, `yAxis()` is zero and the parameterisation collapses onto a segment along `x_direction` (circle, ellipse) or a ray (hyperbola, parabola); the `@return` of all four `pointAt()` points at this rule, and `isValid()` is the only test (C12). `CircleCurve::bounds()` and `EllipseCurve::bounds()` build the box from the same support function `sqrt((a·X_i)² + (b·Y_i)²)`, so a degenerate one gives a segment box rather than a radius cube (`G1`).
- **Boundary**: `isClosed()` / `isPeriodic()` / `period()` are not lifted into `ConicCurve` — a circle and an ellipse are closed and periodic, a hyperbola and a parabola unbounded and non-periodic, two lines each side, and inventing a "closed conic" layer would add one type for nothing (`G8`).

### 2.10 The core properties of the surface interface

- **What**: all fourteen items of `Surface<T>` are pure virtual — (1) evaluation and first derivatives: `evaluate(u, v)`, `derivativeU(u, v)`, `derivativeV(u, v)`; (2) parameter domain: `firstUParameter()` / `lastUParameter()` / `firstVParameter()` / `lastVParameter()`; (3) closedness and period in both directions: `isUClosed()` / `isVClosed()` / `isUPeriodic()` / `isVPeriodic()` / `uPeriod()` / `vPeriod()`; (4) smoothness: `continuity()` (and the non-virtual `isCN(n)`); (5) validity: `isValid()`.
- **Why**: `G4`, `G9` — this is the minimal complete set for "a parameterised surface", corresponding one to one with `Curve<T>` (one parameter becomes two), so one set of notes suffices: face traversal, sampling and the preconditions of intersection depend on these alone.
- **What was removed, and why**:
  - `normalAt(u, v)`: it is `derivativeU × derivativeV` normalised, one line to write; on the interface it would force a choice of orientation convention (outward for a sphere, which side for a plane?), turning a convention into a contract (`G4`). A concrete type still keeps its own `normalAt` (the outward normal of a sphere).
  - `bounds()` (bounding box): an unbounded surface can only return an infinite box, the same reason as for an unbounded curve (2.7).
  - Second derivative / curvature: the consumer that really needs it (curvature, G2 stitching) does not exist yet; a surface would need three groups (UU / UV / VV), the most expensive group in an interface (`G8`).
  - Reversal and the transform family (`uReversedParameter` / `transform`): as for curves — reversal is expressed by building a mirror, and transforms live on a `Shape` node.
- **`Plane` stores no in-plane frame**: the two in-plane axes are derived from `normal` each time (the projection of the world axis least parallel to the normal), so the plane does not grow because the interface needs one more piece of data, at the cost of one projection and cross product per evaluation — two new functions with no caller today. A zero normal is the plane's only degenerate input, reported by `isValid()` (`normal.length() > EPS`), the same rule as the conics' degenerate frame (P6).
- **`SphericalSurface`'s v direction is not closed**: `v` runs from 0 to π, and the two ends are the two poles rather than a seam; stitching and the parameterisation singularity at a pole are two different things, and `continuity()` still answers `CN`.
- **`SphericalSurface`'s v is measured from +Z, one shift away from OCCT (deliberate)**: OCCT's `Geom_SphericalSurface` measures from the equator with the domain `[-π/2, π/2]`; here standard spherical coordinates `[0, π]` are used (0 = the +Z pole), with `v_occt = π/2 - v`. The reason: a non-negative domain whose two ends are the poles, so reading `v` needs no sign check (`G4`); the cost is one conversion when taking parameters from OCCT.
- **Surfaces of revolution and trimming are in `geometry-axis-surfaces.md`**: the frame value of a surface of revolution (`Placement`), the shared "axis in a plane" rule (`Frame.hpp`), the current state of the rectangular trimmed surface and the spline surfaces, and the alignment with and divergence from OCCT are all in that page.
- **A trimmed view follows the same period rule as a curve**: `uPeriod()` / `vPeriod()` of `RectangularTrimmedSurface` answer the basis period only when `isUPeriodic()` / `isVPeriodic()` is true and 0 otherwise (`G6`: two trimmed views may not have two answers).

## 3. Features

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Family classification | two evaluation interfaces + the two classification tags of surfaces, `Surface::kParameterCount` = 2 | landed |
| F2 | Curve parameterisation | `pointAt` of `LineCurve` / `CircleCurve` (`Ray::pointAt` has the same shape, but `Ray` is not in the family, see 2.1) | landed |
| F3 | Sphere parameterisation | azimuth / polar angle and the normal of `SphericalSurface` | landed |
| F4 | Analytic bounding box | the `bounds()` of every new type returns an exact or tight `Aabb<T>` | landed |
| F5 | Core curve properties | evaluation and the first two derivatives, parameter domain and period, smoothness, reversed parameter mapping | landed |
| F6 | The conic family | evaluation, derivatives and parameter domain of the four analytic conics `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` | landed |
| F7 | Splines and offset | evaluation, first two derivatives and spline closedness of `BezierCurve` / `BSplineCurve`; point and first derivative of `OffsetCurve` | partial: the offset's second derivative is blocked |
| F8 | Surface interface | `evaluate` / both first derivatives, parameter domains and closed periods in both directions, smoothness; implemented by `Plane`, `SphericalSurface` | landed |

The features of the surfaces of revolution, the rectangular trim and the spline surfaces are in `geometry-axis-surfaces.md` §3 (F1–F3).

### 3.1 F1 family classification

- **Constraint** (C1): the tags carry no state and no virtual function; family membership is declared once, at the type definition.
- **Acceptance** (T1): the `static_assert`s in `GeometryKindsTest.cpp` cover membership, non-membership and size.

### 3.2 F2 curve parameterisation

- **Behaviour**: `CircleCurve::pointAt(angle)` walks an orthonormal basis derived from `normal`; `LineCurve::pointAt(t)` is a multiple of `direction`, and `Ray::pointAt(t)` is a distance.
- **Constraint** (C2, C12): a degenerate input produces no NaN — a zero-direction line is reported by `isValid()`, `closestPoint()` returns the origin and `intersectWith()` returns no flag (no division by zero); a conic with a degenerate frame collapses onto a segment / ray along `x_direction` and likewise returns no NaN.
- **Acceptance** (T2): one case each for the endpoints, the direction, the circumference and walking inside the plane.

### 3.3 F3 sphere parameterisation

- **Behaviour**: `SphericalSurface::pointAt(azimuth, polar)` measures the polar angle from +Z and the azimuth around +Z (as OCCT's sphere does); `normalAt` returns the unit vector pointing away from the centre.
- **Constraint** (C3): with a degenerate radius (≤ eps) `isValid()` is false and `normalAt` returns the zero vector rather than NaN.
- **Acceptance** (T3): corner correspondence, the centre point, the normal and a collinear degenerate case.

### 3.4 F4 analytic bounding box

- **Behaviour**: `LineCurve` has no `bounds()` (unbounded, see D5); `CircleCurve::bounds()` takes the half width per axis from the support function `radius * sqrt(X_i^2 + Y_i^2)` — the conics share this derivation, so a disc's box is exact and a degenerate frame gives a segment box; a triangle's box comes from `Aabb::compute(span)` over its three corners.
- **Constraint** (C4): `bounds()` is a box in **local** space; a world box multiplies by the node transform.

### 3.5 F5 core curve properties

- **Behaviour**: `LineCurve` answers a constant velocity `direction`, zero acceleration, `CN` and `-u`; `CircleCurve` answers velocity `radius * (-X sin u + Y cos u)`, acceleration `centre - P(u)`, `CN` and `period - u`; `TrimmedCurve` forwards all four items to its basis and reflects only the reversed mapping inside its own interval.
- **Constraint** (C5): a derivative returns a `Vector` (the same point space as `evaluate()`), and its length is the speed, not 1; `reversedParameter` is an involution.
- **Acceptance** (T5): the constant derivative of a line, the relation between a circle's velocity / acceleration and its radius, and the reflection and forwarding inside a trimmed interval.

### 3.6 F6 the conic family

- **Behaviour**: the four conics share one analytic form "plane + origin + in-plane axis", with different parameter meanings — the circle and ellipse use the eccentric angle (closed, period `2π`), the hyperbola the hyperbolic angle (unbounded, parameterising only the `+X` branch), the parabola the offset along the directrix axis (unbounded, `u = 0` at the vertex).
- **Constraint** (C6): the in-plane axis comes from the projection and normalisation of `Frame.hpp`'s `inPlaneAxis()` (shared by `ConicCurve`, `Plane` and `Placement`), falling back to "the world axis least parallel to the normal" when degenerate, so a degenerate input only produces a meaningless curve and never NaN; the `minor` axis is always derived as `normal × x_direction` and no third vector is stored.
- **Acceptance** (T6): the two axis endpoints of an ellipse, the relation between velocity and semi-axes, and the exact per-axis bounding box; the hyperbola's vertex, vertex velocity and acceleration direction; the parabola's "distance to the focus = distance to the directrix".

### 3.7 F7 splines and offset

- **Behaviour**: `BezierCurve::degree()` equals the pole count − 1 and evaluation is a chain of de Casteljau blends; `BSplineCurve::continuity()` reads the interior multiplicities and evaluation sums only the basis functions of the span; every parameter-domain answer of `OffsetCurve` comes from the basis curve, and the displacement direction is `reference_direction x tangent`.
- **Constraint** (C7, C10, C11): `OffsetCurve` is constructible (`static_assert(!std::is_abstract_v<...>)` pins it), but its basis must be able to answer a third derivative or `isValid()` is false; `BSplineCurve::isValid()` requires the clamped form (first and last multiplicity = `degree + 1`), strictly increasing distinct knots, and the sum of multiplicities = pole count + degree + 1; both splines' `isValid()` require the weights to be empty or equal to the pole count and strictly positive; an `OffsetCurve` basis may not be empty.
- **Acceptance** (T7): `aBezierCurveBlendsItsPoles` checks the endpoints, the midpoint, the derivatives at both ends and the constant second difference; `aRationalBezierCurveIsPulledTowardsItsHeavierPole` checks the quotient rule (the values of `P(u) = 3u / (1 + 2u)` and its derivative on both sides); `aBSplineCurveBlendsItsClampedSpan` checks the clamped endpoints, a single span being equivalent to a Bézier, and clamping outside the domain; `aRationalBSplineCurveTracesAQuarterCircle` checks the rational basis (the quarter circle); `anOffsetCurveInheritsTheBasisDomain` checks domain forwarding and the displacement geometry.

### 3.8 F8 the surface interface

- **Behaviour**: `Plane` answers `evaluate(u, v) = origin + u * X + v * Y` (both axes derived from `normal`), unbounded and not closed in both directions, `CN`; `SphericalSurface` answers `u` closed and periodic from 0 to `2π`, `v` neither closed nor periodic from 0 to `π`, `CN`.
- **Constraint** (C9): both implemented surfaces must answer all fourteen items through `const Surface<T>&`; `continuity()` is not broken by a pole or a seam (a pole is not a discontinuity).
- **Acceptance** (T8): `aPlaneAnswersTheSurfaceInterface`, `aSphericalSheetClosesOnlyAroundItsAxis`, both called through a base reference to prove virtual dispatch.

## 4. Public Interface

| Header | Content |
|---|---|
| `Curve.hpp` / `Surface.hpp` | the two interfaces `Curve` and `Surface`; `Surface::kParameterCount` |
| `Continuity.hpp` | the smoothness ladder enum, shared by curves and surfaces |
| `Curve.hpp` / `ConicCurve.hpp` / `BoundedCurve.hpp` | the curve family and its two intermediate layers; each concrete curve has its own header |
| `LineCurve.hpp` / `CircleCurve.hpp` / `Ray.hpp` / `SphericalSurface.hpp` | the implemented value types and the `*f` / `*d` aliases; `Ray` is not in the family (2.1) |
| `EllipseCurve.hpp` / `HyperbolaCurve.hpp` / `ParabolaCurve.hpp` | the other three analytic conics and their `*f` / `*d` aliases |
| `BezierCurve.hpp` / `BSplineCurve.hpp` | the two constructible spline value types: poles + weights (+ degree, knots, multiplicities) and the ten curve properties |
| `OffsetCurve.hpp` | the offset curve: a `shared_ptr` basis curve + distance + reference direction; the second derivative comes from the basis' third derivative (2.8) |
| `Surface.hpp` / `ElementarySurface.hpp` / `BoundedSurface.hpp` | the surface interface and its two classification tags |
| `Frame.hpp` | the shared "axis in a plane" rule (used by the conics, the plane and the surfaces of revolution) |
| `Placement.hpp` / `CylindricalSurface.hpp` / `ConicalSurface.hpp` / `ToroidalSurface.hpp` / `RectangularTrimmedSurface.hpp` | the frame value of the surfaces of revolution (`Placement`) and the trimmed surface, see `geometry-axis-surfaces.md` |
| `src/base/geom/src/Instantiations.cpp` | the explicit-instantiation list of the header-only geometry types (`float` + `double`) |

## 5. Constraints, Requirements and Invariants

- **C1**: a surface tag carries no state and declares no virtual function; the curve interface's virtual functions are the family's only polymorphism point (2.1, 2.7).
- **C2**: a degenerate input produces no NaN; `isValid()` is the only entry point for the decision.
- **C3**: family membership is declared once, at the type definition, and not repeated elsewhere.
- **C4**: every `bounds()` is a local-space box; a world box multiplies by the node transform.
- **C5**: `derivative` / `secondDerivative` return velocity and acceleration vectors, not unit tangents and curvature vectors; `reversedParameter` is an involution (`reversedParameter(reversedParameter(u)) == u`).
- **C6**: a conic's in-plane axis is projected and normalised by `Frame.hpp`'s `inPlaneAxis()` (one copy, shared with the plane and the surfaces of revolution), and the second axis is derived as `normal × x_direction`; a degenerate input yields a curve with `isValid() == false` and may never produce NaN.
- **C7**: `isCN(n)` is a query of `continuity()` and does not describe a curve on its own; `G1` / `G2` do not answer `isCN(1)` / `isCN(2)` (geometric continuity is not derivative continuity).
- **C8**: a conic's frame (`center` / `normal` / `x_direction`) is defined once in `ConicCurve` and normalised at construction; the four conics may not declare fields of the same names, nor rewrite the frame after construction.
- **C9**: all fourteen items of the surface interface must be answerable through `const Surface<T>&`; a pole or a seam is not "a discontinuity", and `continuity()` still answers `CN`; the classification tags may add neither state nor virtual functions.
- **C10**: the meaning of a parameter outside the domain is defined per type, not by convention — `BezierCurve` extrapolates polynomially and `BSplineCurve` clamps the parameter into the knot domain and takes the endpoint value; both are written in the header.
- **C11**: the weights of a rational curve must be strictly positive, or the denominator crosses zero inside the domain; `isValid()` of `BezierCurve` / `BSplineCurve` is the only check point.
- **C12**: `isValid()` covers four kinds of degenerate input: a line (zero direction), a composite type (an empty basis curve or a reversed interval on a trim, an empty basis curve or a zero reference direction on an offset), a rational spline (non-positive weights), a conic, a plane and a sphere (a degenerate frame / radius); a composite type reports whether its own inputs are usable and does not fold the basis validity into it. This answer is **pure virtual** on `Curve<T>` / `Surface<T>` (a caller holding `const Curve<T>&` can ask it), and a degenerate composite curve answers a degenerate value when queried (the origin, the zero vector, `C0`, false) instead of dereferencing an empty handle — that is crash-prevention behaviour, not a geometric fact.
- **C13**: a spline's interior knot multiplicity never exceeds the degree (both ends are still clamped at degree + 1). An interior multiplicity of degree + 1 is a gap, not a join, and `isValid()` rejects it; so `C0` from `continuity()` always means "the pieces meet at one point", and `degree - multiplicity` needs no clamping. A break that must be kept is split into several curves rather than handed to every algorithm as a hole.
- **N1**: the `t` of `LineCurve::pointAt(t)` multiplies the length of `direction`, and is a distance only for a unit direction; a `Ray` direction is always unit, so the two same-named parameters have different dimensions.
- **N2**: the default-construction admission rule matches 2.3: either the default value is meaningful geometry, or it is an empty state `isValid()` recognises; the surfaces of revolution and the plane are neither, so they declare no default constructor. `Plane` is the only surface without the `Surface` suffix (a plane has no second meaning, and OCCT does the same).
- **R1**: the zero-allocation, non-throwing scope is `pointAt` / `bounds` / `derivative` of `LineCurve` / the conics / `Aabb`; evaluation of `BSplineCurve` is zero-allocation too (local scalars only), while `BezierCurve` copies a temporary pole array per evaluation (de Casteljau needs `n` scratch values, and no mutable member is introduced for it).
- **R2**: the family classification adds no external dependency; a tag includes only `geom_global.hpp`, and the curve interface additionally includes only `Point3` / `Vector3` from `vine/math` and this directory's `Continuity.hpp`.

## 6. Caveats and Traps

- **P1**: adding a virtual function to a tag adds a vptr to every value type (`Ray` is copied by value in a hot loop). To share an algorithm, write a free function.
- **P2**: every `bounds()` is a box in **local** space, and a world box multiplies by the node transform; caching it as a world bounding box goes wrong at the scene level.
- **P3**: the two splines behave differently outside the domain (C10). How it fails: sample outside the domain with one idea of "extrapolation" and a B-spline gives a flat line while a Bézier gives a diverging curve. How to detect: whether points outside the domain jump to a constant, or the tangent suddenly becomes zero. How to prevent: check `firstParameter()` / `lastParameter()` before extrapolating, and do the extrapolation at the call site rather than in the curve.
- **P4**: the denominator of a rational curve is `Σ N_i(u) w_i`. How it fails: a zero or negative weight makes it cross zero inside the domain, and `evaluate()` returns infinity or a far point with a flipped sign. How to detect: `isValid()` is false, or the coordinates jump near the midpoint. How to prevent: guarantee all weights are positive before construction (C11).
- **P5**: `BSplineCurve` evaluation covers the clamped form only. How it fails: evaluate with unclamped knots (first and last multiplicity < `degree + 1`) and the basis functions do not sum to 1 at the domain ends. How to detect: `isValid()` is false. How to prevent: construct in the clamped form, which the multiplicity semantics of `continuity()` also depend on.
- **P6**: a conic with a degenerate frame is not "a circle of radius 0". How it fails: with a zero `normal`, `yAxis()` is zero, `evaluate()` walks a segment along the world X axis, and `bounds()` is just that segment; taking the result as a circle to pick points or compute a perimeter yields a string of collinear points. How to detect: `isValid()` is false. How to prevent: confirm `normal` is non-zero before construction and let `ConicCurve` normalise the frame in one place.
- **P7**: a line's "degenerate" has exactly one threshold — `|direction| > EPS` in `isValid()`. How it fails: write a second test `|direction|² < EPS` (equivalent to `|direction| < 1e-4`), which treats a legal short direction as a zero one and silently replaces the projection with the origin. How to detect: the two answers differ when the direction length is between `EPS` and `sqrt(EPS)`. How to prevent: take the test from `isValid()` only.
- **P8**: a trimmed view is not its basis curve / basis surface. How it fails: use `TrimmedCurve::period()` as the basis circle's 2π to normalise parameters, or assume `isClosed()` matches the basis, so a quarter circle is treated as a full turn for seam decisions. How to detect: `isPeriodic()` / `isClosed()` / `period()` of the trim are false or 0 while the basis answers true. How to prevent: ask the trim itself for closedness, period and period value (the period rule of 2.7, C12), and ask `basis_curve` / `basis` when the basis number is wanted.
- **P9**: a composite type whose `isValid()` is false can still be queried. How it fails: an empty-basis trim / offset answers the origin, the zero vector, `C0`, looking like "a curve degenerated to a point", and is then taken as a legal result in later computation. How to detect: `isValid()` is false. How to prevent: check `isValid()` on any `TrimmedCurve` / `OffsetCurve` / `RectangularTrimmedSurface` you receive — the degenerate answers exist only to avoid dereferencing an empty handle (C12).
- **P10**: treating a cut-up piece of data as one spline. How it fails: an interior multiplicity written as `degree + 1` makes the curve jump at the knot; evaluation still runs and the parameter interval is still continuous, so sampling produces points across the seam. How to detect: `isValid()` is false, or compute `continuity()` directly — with multiplicity equal to the degree it does answer `C0` (the pieces meet at one point), so the two no longer blur. How to prevent: split into several curves at the break when rebuilding (C13).

## 7. Rejected Options

- **D1** Making `Curve` / `Surface` virtual base classes: adds a vptr to every value type and kills inlining and trivial copyability (`G10`, `G8`). See `../decisions/ADR-2026-11-04-geometry-kinds.md`.
- **D2** Providing only concepts and no tags: compile-time questions such as `is_base_of` and the documentation home of "family" would disappear, and a run-time variant would still be needed to cover the same set (`G1`).
- **D3** Lifting the parameter meaning into the base class (a unified `pointAt(t)`): each curve's `t` means something different, and a unified signature only hides the difference in documentation (`G4`).
- **D4** Following OCCT and providing a mutable `reverse()`: a curve is immutable and a basis curve may be shared by several trims, so a mutable reversal would interfere; reversal is expressed as a constructed mirror plus `reversedParameter` (2.7).
- **D5** Putting `bounds()` on the curve interface: only a finite curve can always answer; a line can only return an infinite box, disguising "unbounded" as a box (`G4`).
- **D6** A `curveType()` enum, and `D3` / `DN`: `dynamic_cast` and the vtable already answer "what am I", so maintaining an enum too is two homes; a derivative above the third order has no consumer (`G1`, `G8`).
- **D7** Leaving surfaces as empty tags: a tag cannot answer "evaluate an unknown surface and ask for its parameter domains and periods in both directions", which is exactly what face traversal, sampling and the preconditions of intersection need (`G4`, `G9`). See `../decisions/ADR-2026-11-04-surface-evaluation-interface.md`.
- **D8** Putting `normalAt()` / `bounds()` / the second derivative on the surface interface: the first two are derived or fake values (an infinite box), and the third has no consumer (2.10).
- **D9** Adding a pure virtual `thirdDerivative()` to `Curve<T>` so that `OffsetCurve::secondDerivative()` can land: for one derivative of one curve, eight concrete curves would each take on a pure virtual obligation, and an implementer who only reaches the second order pays too (`G8`, `G4`); the actual answer is the capability protocol plus one non-virtual member on the types that can answer, and the contract stays at eleven items. See `../decisions/ADR-2026-11-05-curve-derivative-capability.md`.
- **D10** Following OCCT and putting an arbitrary-order `DN(order)` on the interface, throwing `NotImplemented` when unimplemented: the order is an algorithm parameter, not type identity, and going beyond the capability should be an **inspectable empty value** rather than an exception or a truncation (`G4`, `G10`). The capability protocol keeps "ask on demand" without bringing exception semantics into a geometric query. See `../decisions/ADR-2026-11-05-curve-derivative-capability.md`.

## 8. Tests and Acceptance

| # | Criterion | Case |
|---|---|---|
| T1 | membership, non-membership and zero cost | the `static_assert`s at the top of `tests/test_geom_spatial/GeometryKindsTest.cpp` |
| T2 | curve parameterisation and degenerate input | `aLineCurveAnswersThePointAtItsParameter`, `aCircleCurveWalksItsOwnPlane`, `aCircleCurveWithoutANormalIsInvalid` in the same file |
| T3 | sphere parameterisation and degenerate input | `aSphericalSheetPutsPolarZeroAtThePole`, `aCircleCurveWithoutANormalIsInvalid` in the same file |
| T4 | parameter domain and forwarding of the curve interface | `aCurveAnswersItsDomainAndPeriod`, `aTrimmedCurveForwardsToItsBasis` in the same file |
| T5 | core curve properties | `aLineCurveHasAConstantVelocity`, `aCircleCurveTurnsTowardsItsCentre`, `aTrimmedCurveMirrorsInsideItsOwnRange` in the same file |
| T6 | the conic family | `anEllipseCurveWalksItsTwoAxes`, `aHyperbolaBranchWalksAwayFromItsCentre`, `aParabolaIsEquidistantFromFocusAndDirectrix` in the same file |
| T7 | spline evaluation and offset | `aBezierCurveDescribesItself`, `aBezierCurveBlendsItsPoles`, `aRationalBezierCurveIsPulledTowardsItsHeavierPole`, `aBSplineCurveDerivesItsContinuityFromItsMultiplicities`, `aBSplineCurveBlendsItsClampedSpan`, `aRationalBSplineCurveTracesAQuarterCircle`, `anOffsetCurveInheritsTheBasisDomain`, `continuityAnswersTheDerivativeOrderQuestion` in the same file |
| T10 | third derivative and the offset's second derivative | `everySpaceCurveAnswersItsThirdDerivative` in the same file (the third derivative of the eight types agreeing with the central difference of the second, including both rational forms, cross-checked against `-P'` for circle / ellipse, `P'` for the hyperbola and zero for line / parabola), `theSpaceOffsetAnswersASecondDerivativeExactly`, `anOffsetOfAnOffsetStopsAtTheVelocity` |
| T8 | the surface interface | `aPlaneAnswersTheSurfaceInterface`, `aSphericalSheetClosesOnlyAroundItsAxis` in the same file; the acceptance of the surfaces of revolution and the rectangular trim is in `geometry-axis-surfaces.md` T1 |
| T9 | family contract sweep | `everyCurveKeepsTheInterfacePromises` / `everySurfaceKeepsTheInterfacePromises` and their `…InFloat` versions in the same file (**both precisions run once each**: the library explicitly instantiates and exports float / double): every implementation asked in turn for an ordered domain, `isValid()` true, closedness / period agreeing with the period value, `isCN` agreeing with `continuity()`, `reversedParameter` an involution, and finite derivatives on a finite domain; the two trim contracts additionally have `aTrimOfAPeriodicBasisReportsAPeriodOnlyWhenItRepeats`, `aTrimWithoutABasisAnswersInsteadOfDereferencingIt`, `anOffsetCurveWithoutABasisAnswersInsteadOfDereferencingIt`; the default-construction admission rule is pinned by the `static_assert`s at the top and by `anEmptyPatchIsRecognisablyEmpty`, and validity on the interface by `validityIsAskableThroughTheInterface` and two lines of `static_assert` |
## 9. Status and Evolution

- Both families, the split of surfaces into `ElementarySurface` / `BoundedSurface`, the four value types, and the membership and size assertions are all in place.
- The curve interface has eleven properties (2.7); `TrimmedCurve` is a composite curve, and `BezierCurve` / `BSplineCurve` are the two constructible splines (F7, 2.8).
- The surface interface has fourteen items (F8, 2.10), implemented by `Plane` and `SphericalSurface`; the two classification tags carry no state and no virtual function, but **a surface pays a vptr just as a curve does**.
- The four conics (F6) and the evaluation and first three derivatives of `BezierCurve` / `BSplineCurve` (F7, 2.8) are in place. **The spatial curve family answers the third derivative throughout**: `OffsetCurve` answers its second derivative from the basis' third derivative and is a constructible value type, and the order rule matches the planar side (D9, `../decisions/ADR-2026-11-06-space-third-derivative-and-offset.md`).
- The three analytic surfaces of revolution, the rectangular trimmed surface and the two spline surfaces are in `geometry-axis-surfaces.md`; the surface family has no placeholder type.
- Family algorithms exist on both sides: surfaces (nearest point / distance, ray × surface, see `geometry-surface-queries.md`) and curves (nearest point / distance, see `geometry-curve-queries.md`); all are free functions constrained with `requires std::is_base_of_v<…>`, adding no virtual function to an interface. Known gaps: solid intersection (backlog G3), the nearest distance between two curves.
- Compatibility: the surface interface changed from the non-template `Surface` tag to a template interface, a **breaking change** (`Surface` / `ElementarySurface` / `BoundedSurface` now all carry `<T>`); the only consumer in the repository is `tests`.
- Compatibility: `TrimmedCurve::period()` and `RectangularTrimmedSurface::uPeriod()` / `vPeriod()` return 0 when `isPeriodic()` is false instead of forwarding the basis period — a behaviour change with unchanged signatures (no caller in the repository); an empty-basis query on either trimmed view no longer dereferences an empty handle either (C12, P9). Both facts are pinned by the family contract sweep `everyCurveKeepsTheInterfacePromises` / `everySurfaceKeepsTheInterfacePromises` (T9).
- Compatibility (interface extension): `isValid()` is now a pure virtual of `Curve<T>` / `Surface<T>` (ten → eleven items, thirteen → fourteen), every implementation already had the method and only gained `override`; `Plane` therefore has a real validity test for the first time (a zero normal is false, where it used to be always usable). A custom implementer must implement it — there is no other implementer in the repository. See `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md`.

## 10. References

- `geometry-primitives.md` (membership and header rules of `Aabb` / `Obb` / `LineCurve` / `Plane`)
- `geometry-parametric.md` (parametric geometry and `Primitive`)
- `geometry-ray.md` (the `Ray` contract and why it is inline)
- `../decisions/ADR-2026-11-04-geometry-kinds.md`
- `../decisions/ADR-2026-11-04-curve-evaluation-interface.md` (the curve interface)
- `../decisions/ADR-2026-11-04-surface-evaluation-interface.md` (the surface interface)
- `../decisions/ADR-2026-11-05-curve-derivative-capability.md` (derivatives exposed by capability)
- `../decisions/ADR-2026-11-06-space-third-derivative-and-offset.md` (the spatial third derivative and the constructible offset)
- `../decisions/ADR-2026-11-04-axis-surface-frame.md` (the frame layer of the surfaces of revolution and the shared axis-in-a-plane rule)
- OCCT `Geom_Curve.hxx` / `Geom_Surface.hxx` / `Geom_Conic.hxx` / `Geom_BezierCurve.hxx` / `Geom_BSplineCurve.hxx` / `Geom_OffsetCurve.hxx` (source of the interface vocabulary and the numeric algorithms; the local source is in `E:\opensrc\OCCT\src\ModelingData\TKG3d\Geom`)
