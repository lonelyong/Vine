# geometry notes

- **An array alias must carry the element type** (`models/Array.hpp`): `Vec2fArray` / `Vec3dArray` / `Vec3ui32Array`;
  there is no `Vec2Array` defaulting to float; on the scalar side `Int8Array` … `UInt64Array` and `FloatArray` /
  `DoubleArray`; the names copy math's `Vec2f` / `Vec2i8` / `Vec2ui32`.

- **A header-only SDK class must be included by some TU in its module**: the member definitions of the modelling
  subclasses (`Box` / `Sphere` / …) and the geometry kinds (`BoxSolid` / `SphereSolid` / …) all live in headers,
  and the DLL instantiates and exports them only when a TU with the matching `_LIB` macro sees the class. Every
  modelling type now has its own `.cpp` (e.g. `models/src/Box.cpp`).
- **Modelling types are a separate module, `src/base/models`** (`sdk/vine/models/`, namespace `vn::models`,
  library `vn::Models`): `Shape` / `Primitive` / `Model` / the `Mesh` family / `BrepShape` / `PointCloud` / the
  `Material` family all live here, depending on `vn::Geometry` one way; includes are written
  `<vine/models/Xxx.hpp>`. The geometric value types (curves / surfaces / `Solid` / `Aabb` / `Obb` / `Ray`) stay
  in `src/base/geom/sdk/vine/geom/spatial`.
- **Parametric geometry is a modelling type and the base class holds no geometry**: `Primitive : Shape` has only
  the protected constructor `Primitive(ShapeType)` and the pure virtual `bounds()`, and **does not know
  `Solid`**; `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` (`final`, each in `models/Box.hpp` …
  `models/Ellipsoid.hpp`) each store **full-dimension** fields and use `solid()` to construct their concrete
  kind at the current size (`BoxSolidf` …, returned **by value**, not stored or cached). `isValid()` /
  `hasVolume()` / `bounds()` forward to `solid()` in the subclass. The only entry is
  `models/PrimitiveDispatch.hpp`: `visitPrimitive(primitive, visitor)` (a closed set; `false` when it is not one
  of the five); the visitor receives the concrete subclass and calls `solid()` to hand it to a geometry entry.
  There is **no** `Primitive::shape()` / `setSolid()`; `ShapeType` is still passed by the subclass constructor
  to the base. Rationale: `decisions/ADR-2026-11-05-primitive-is-a-modelling-base.md`.
- **The value types (`Aabb` / `Obb` / `LineCurve` / `Plane` / `Ray` and every curve and surface) carry `math::FP T`
  but the class carries no `VN_GEOMETRY_API`**: short queries are defined in the header (inlining happens in the
  caller; **do not** add `extern template`, it kills the measured gain in the picking loop).
  Exports come only from explicit instantiation, and **every type must be instantiated at both `float` and
  `double`**:
  - a type with a `src/` implementation does it in its own `.cpp` (`LineCurve.cpp` / `Obb.cpp` / `Plane.cpp`) —
    an explicit instantiation can only instantiate the member definitions that TU sees;
  - header-only types are collected in `src/Instantiations.cpp`, two lines per type (float, double); a new type
    that is forgotten has no symbols in the library.
  - **A new .cpp needs `cmake -S . -B build` again** (`vn_add_library` uses a glob without `CONFIGURE_DEPENDS`).
  - Exported `std::vector` / `std::shared_ptr` members report C4251, locally suppressed in `Instantiations.cpp`.
  Rationale and measurements: `design/geometry-primitives.md` 2.8,
  `decisions/ADR-2026-11-04-geometry-precision-and-instantiation.md`.
- A new header-only shape of the same kind must likewise be included by some `.cpp` in the module, or its
  members changed to out-of-line implementations.
- **Family-level surface queries**: `closestPointOnSurface()` (Gauss-Newton, returns the point / `(u,v)` /
  distance) and `intersectRayWithSurface()` (three-variable Newton, the result embeds `RayHit` + `u/v`) are both
  header templates.
  **Write the parameter constraint as `requires std::is_base_of_v<Surface<T>, S>` on the function**: `Plane`
  derives from `Surface<T>` indirectly through `ElementarySurface<T>`, and a `const Surface<T>&` parameter
  cannot deduce `T`; writing a named concept instead triggers MSVC C7605 (a constrained template `Surface<T>`
  inside a concept body).
  The window is `SurfaceParameterWindow<T>` (a parameter rectangle, not `Rect2`); an infinite side takes the
  surface's own bound, and the default `naturalSurfaceWindow()` is the natural domain.
  `finiteParameterWindow()` is the only place that resolves it.
  Known properties: the closest point is a local minimum; a tangent ray is missed (singular Jacobian); a fold
  the grid cannot resolve is missed; the normal at a sphere pole is zero; a ray lying on the surface is judged
  `coincident` with three probes spanning the range.
- **Curve × non-analytic surface**: `intersectCurveWithSurfaceByProjection()` shares
  `detail::scanCurveAgainstSurface` with the analytic entry and only swaps the value for
  `detail::signedDistanceToSurface` (projection distance + the sign of `dS/du × dS/dv`).
  **That signed distance deliberately stays in `detail`**: polarity belongs to the parametrisation (the sphere's
  cross product points inward, the sign opposite to `signedValue()`), and root finding only needs the sign
  change. A singular parameter line (sphere pole) returns NaN = “no information”.
  After narrowing a root the scan verifies `|f| ≤ tolerance` once more, filtering spurious sign changes caused by
  a closest-point branch jump (`geometry-intersection.md` P8).
  Design: `design/geometry-surface-queries.md`; tests: `tests/test_geom_spatial/SurfaceQueryTest.cpp`.
- **Closest point / distance from a point to a curve**: `closestPointOnCurve()` (`CurveProjection.hpp`) scans
  the stationary points of the squared distance, `f(u) = (C(u) - P)·C'(u)`, as zeros, narrows with
  `rootInBracket()`, and **treats the window ends as candidates too** (when the foot point lies outside the
  window the minimum is on the boundary). It calls only `evaluate` / `derivative`.
  The window is an `optional<pair>`: a bounded curve may omit it, an unbounded curve (line / hyperbola /
  parabola) must give it. Local minima and “flat minima (f does not change sign)” are separated by
  `sample_count`, see `design/geometry-curve-queries.md`.
- **The analytic solid `Solid<T>` is a family interface and the five kinds are values**: in `Solid.hpp`, `Solid<T>`
  is an abstract base class (`isValid` / `hasVolume` / `bounds` / `closestPoint` / `intersectWith`, five pure
  virtuals) and each of the five solids has its own header (`BoxSolid.hpp` … `EllipsoidSolid.hpp`). **There is no
  variant** (the `SolidShape` variant was deleted): to answer per kind, write one overload per kind and let the
  caller obtain the concrete subclass through `visitPrimitive()`; a modelling subclass constructs its concrete
  kind by value with `solid()` at the current size. **The default tolerance must be written on both the base class
  and every kind** (a derived declaration shadows the base's default argument). Design: `design/geometry-solid.md`;
  tests: `tests/test_geom_spatial/SolidTest.cpp`.
- **Tessellating an analytic solid**: `tessellateSolid()` (`src/base/geom/sdk/vine/geom/spatial/SolidTessellation.hpp`
  + `src/base/geom/src/SolidTessellation.cpp`) has one overload per kind (`BoxSolid<T>` … `EllipsoidSolid<T>`) and
  returns a **value** `Triangulation<T>` (public fields `positions` / `normals` / `indices`, aliases `Triangulationf`
  / `Triangulationd`); triangle counts: box 12, sphere / ellipsoid `2·around·(along−1)`, cylinder `4·around`
  (side 2·around + one around per cap), cone `2·around` (side fan + bottom cap).
  **Both precisions are explicitly instantiated and exported** (the declaration carries no macro;
  `template VN_GEOM_API ...` lives in the .cpp); a new .cpp needs `cmake -S . -B build` again.
  **Model-side assembly** is `vn::models::meshFromTriangulation(Triangulationf)` (`models/SolidTessellation.hpp`):
  it moves the three arrays into a `TriangleMesh` and computes the AABB; meshes are float only, so a caller that
  must assemble a `Triangulationd` narrows it explicitly. The geometry side **does not** return a `TriangleMesh`
  (a geom → models back edge) and **does not** return a `Buffer` (`Mesh::setPositions` takes only `std::vector`,
  which would add a copy).
  **Vertices are duplicated where the normal is discontinuous** (box edges, cylinder caps, cone apex per segment),
  so the vertex count is not minimal; closedness must be counted by **position**, not by index. The result writes
  positions / normals / indices only, no UVs.
  Design: `design/geometry-tessellation.md`; tests: `tests/test_geom_spatial/SolidTessellationTest.cpp`.
- **Ray × analytic solid**: `Solid<T>::intersectWith(ray, eps)` goes through each kind's own analytic formula: box
  slab, sphere / ellipsoid quadratic, cylinder side + both caps, cone side + base. The result holds at most two
  hits (one when tangent), reports only `t ≥ -eps`, and **from an interior origin reports the exit only**; the
  normal is turned toward the ray as `RayHit` expects, and a hit at the cone apex has a zero normal. `T` in
  `Solid<T>` is the computation precision; the solid's dimensions themselves are float.
- **Closest point / distance from a point to an analytic solid**: `Solid<T>::closestPoint(query, tolerance)` uses
  **solid** semantics — inside (boundary included) it returns the query point itself with distance 0; outside it
  returns the closest point on the boundary. Box clamps, sphere is radial, ellipsoid is a one-dimensional
  bisection on the Lagrange condition, cylinder clamps radially × axially, cone takes the nearer of “base disc vs
  generatrix profile”. `tolerance` only controls the ellipsoid root finding; the inside / outside test is an exact
  comparison.
- **`Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` are header-only classes** (not exported): the members and
  `desc()` are in the header and every consumer compiles them;
  their descriptors use the inline `VN_OBJECT_META`, and `Type::obtain()` guarantees one per process, see `core.md`.
- Local space of the five primitives: `BoxSolid` / `SphereSolid` / `EllipsoidSolid` are centred on the origin;
  `CylinderSolid` / `ConeSolid` span `[-h/2, +h/2]` along Z, with the cone's base at `-h/2`.
- **Surfaces are the same**: `Surface<T>` is a **template interface** (thirteen items: `evaluate(u,v)`,
  `derivativeU/V`, four parameter bounds, closed/periodic in both directions, `continuity()`), and
  `ElementarySurface<T>` / `BoundedSurface<T>` are only classification tags (stateless, no virtual functions).
  `Surface` / `ElementarySurface` / `BoundedSurface` **all take `<T>` now** (the old non-template spellings become
  `Surface<double>`).
  `Plane` stores no in-plane frame (both axes are derived from `normal` each time, because it is copied);
  `SphericalSurface`'s `u` (azimuth) is closed and periodic, `v` (polar angle) is not closed. The interface has
  **no** `normalAt` / `bounds` / second derivative.
- **The three surfaces of revolution are implemented** (`CylindricalSurface` / `ConicalSurface` /
  `ToroidalSurface`): each holds a `Placement<T>` value (`origin` / `axis` / `x_direction` + `yAxis()`, always
  right-handed) and `continuity()` returns `CN` in each; the rule for an in-plane axis exists once, in
  `Frame.hpp`'s `inPlaneAxis()` (`ConicCurve` / `Plane::xAxis` / `Placement` all use it).
  Placement criterion: **store it if it changes the point set** (cylinder / cone / torus), **derive it if it only
  changes the parametrisation** (the sphere still uses the world Z axis).
  Parameter conventions follow OCCT: cylinder `v` is the axial distance, cone `v` is the **generatrix arc length**
  (`dP/dv` is a unit vector), torus `v` is the **equatorial angle**.
  `RectangularTrimmedSurface` is a view (basis surface `shared_ptr` + four parameter bounds), and closed/periodic
  needs both “basis surface and rectangle”.
  `BezierSurface` / `BSplineSurface` **are evaluable now**: a row-major flat control net (index
  `iu + u_pole_count * iv`; reading the net uses only `poleAt(iu, iv)`) + optional equal-length weights (empty =
  polynomial); tensor-product evaluation is “blend each row along u, then blend along v”, and the rational form
  divides by the weight in four-dimensional homogeneous coordinates. Both share the curve-side infrastructure:
  `BezierBlend.hpp` (`deCasteljau` / `differences`) and `BSplineBasis.hpp` (`knotSpan<T>` / `basisValue<T>` /
  `isClampedKnotVector` / `knotContinuity`, **note that a call site must spell `<T>` explicitly**, `std::span`
  cannot deduce it). `BSplineSurface` accepts clamped form only; closedness is decided by comparing homogeneous
  poles (the clamped basis is linearly independent, which is equivalent to comparing evaluations).
  Cost: evaluation allocates a temporary array each time (`backlog/geometry.md` G10). The full surface-side design
  is in `design/geometry-axis-surfaces.md`.
- **The contract of the trimmed views (`TrimmedCurve` / `RectangularTrimmedSurface`)**: ① `period()` / `uPeriod()`
  / `vPeriod()` answer the basis curve's / surface's period only when their own `isPeriodic()` is true, and 0
  otherwise (they used to forward the basis period, so a quarter circle answered 2π); for the basis numbers read
  the public `basis_curve` / `basis` directly.
  ② An empty basis is no longer dereferenced: queries degrade to `Point()`, a zero vector, `C0`, `false`
  (`isValid()` is the only test). `OffsetCurve` is handled the same way (its `isValid()` also requires a non-zero
  reference direction). ③ A trimmed curve's `isClosed()` also needs the two ends to coincide: `[0, 2π + 0.3]`
  covers the whole span but walks one extra corner, so it must answer false (2D `TrimmedCurve` follows the same
  rule as 3D); a full turn whose start is not 0 has coincident ends but does not cover the natural domain, and
  also answers false.
  Rectangle trimming is the same: on a periodic basis the span must be an integer number of periods
  (`isUClosed()` is false for `[0, 2π+0.3]`); a non-periodic closed basis (a spline patch) must use exactly its
  two natural ends, and an extrapolated rectangle also answers false. Such cross-type contracts are swept by
  `everyCurveKeepsTheInterfacePromises` / `everySurfaceKeepsTheInterfacePromises`; run them first when changing
  the interface.
- **Spline multiplicity rules**: `degree + 1` at both ends (clamped), **at most `degree` inside**; an interior
  multiplicity of `degree + 1` is a gap, which `isValid()` rejects outright —
  so `continuity()`'s `C0` really always means “meets at one point”. An interior multiplicity of `degree` → `C0`
  (a kink, but still connected). See `decisions/ADR-2026-11-04-bspline-without-knot-breaks.md`.
- **`isValid()` is a pure virtual of `Curve<T>` / `Surface<T>`** (11 items for a curve, 14 for a surface): a new
  curve or surface **must** implement it, or it does not compile. `Plane::isValid()` is `normal.length() > EPS` (a
  zero normal is no longer "always usable"). See
  `decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md`.
- **`math` keeps only mathematical vocabulary** (`decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`; current
  design in `design/math-base-layer.md`): 18 headers, no subdirectories, no procedures. Root finding / integration
  (`adaptiveSimpson` / `rootInBracket` / `parameterReaching`), Bernstein evaluation (`deCasteljau` /
  `differences`), the knot model and `Continuity` are **all in `geom`**: the first three in
  `geom/detail/{Quadrature,RootFinding,BezierBlend}.hpp`, the knot model in `geom/detail/BSplineBasis.hpp`, and
  `Continuity` in `geom/Continuity.hpp` (the module root, re-exported by both namespaces).
  Two dividing criteria: ① does the signature mention our own data model; ② is there a non-geometric consumer
  today. The cost: a non-geometric module that wants fast integration must depend on `vn::Geom`; when a second
  consumer appears, promote it to its own module (the ADR's reversal path). `symmetricEigen` stays at the `math`
  root — its budget is internal and the caller gives no precision target.
- **Directories and naming (current, `decisions/ADR-2026-11-06-geom-contracts-nested-under-module.md`, design in
  `design/geometry-module.md`)**: `src/base/geom/sdk/vine/geom/{planar,spatial,detail}` + `geom_global.hpp` /
  `Continuity.hpp` at the cross-cutting block root; namespaces `vn::geom::{planar,spatial,detail}`.
  **Type names carry no dimension marker** (`LineCurve`, not `Curve2`/`CurveCurved`), and the precision suffix stays
  `f` / `d`.
  Two points: ① `Continuity` needs no re-export (the closed namespace finds it); ② **a shared piece must be spelled
  `geom::detail::` in full** — six contract headers declare a same-named `planar::detail`/`spatial::detail`, and a
  bare `detail::` resolves to that level (measured C2039).
  Do not propose “flat + `2` suffix” or “mark 2D only” again: 4 pairs of same-named public functions (two pairs
  cannot overload) + precision-suffix collisions (the still-valid part of the earlier layout decision, whose record is in git).
- **Template parameter constraints**: geometry families are always `template <vn::math::FP T>` (including `Plane`,
  which used to be an exception); `Aabb` / `Obb` deliberately keep `typename T`
  so that `Aabbi = Aabb<int32_t>` is allowed. A new type inside a family uses `FP`; do not copy the value-type rule.
- **Test precision**: `GeometryKindsTest`'s family-level sweeps `expect*Invariants` / `sweepCurves` / `sweepSurfaces`
  are **templated**, and run once for double and once for float (`…InFloat`); adding a curve or surface to `sweep*()`
  gets both precisions at once. Older tests (Ray/Plane/curve series)
  are mostly still double-only; the `Aabb` / `Obb` unit tests use float.
- **`Curve<T>` is a pure virtual interface (eleven items)**: a new curve must implement `evaluate` / `derivative` /
  `secondDerivative` / `firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period` / `continuity` /
  `reversedParameter` in one go.
  Traps: `derivative` is the **velocity vector** (a circle's length is the radius), not a unit tangent;
  `TrimmedCurve`'s `reversedParameter` reflects inside its own interval (`first + last - u`) and must not forward to
  the basis curve. `Ray` deliberately does not derive from `Curve` (hot paths copy it by value). See
  `design/geometry-kinds.md` 2.7.
- **Derivatives are exposed by capability**: besides the eleven pure virtuals, `Curve<T>` (2D and 3D have the same
  shape) has two non-pure members — `maxDerivativeOrder()` (default 2)
  and `derivativeAt(u, order)` (1/2 forwards to `derivative` / `secondDerivative`; 0, negative or out of range
  answers `std::nullopt`). Types with a closed-form third derivative
  (line / circle / conic / both splines) override it to 3; `OffsetCurve` reports `min(2, basis capability − 1)` (a
  basis that answers 3 ⇒ reports 2;
  an offset of an offset ⇒ reports 1 and `isValid()` is false). A trimmed view forwards the capability to its basis
  unchanged. The spatial and planar **families have exactly the same shape**. A new curve that only goes to the
  second derivative **needs to write nothing**. See `decisions/ADR-2026-11-05-curve-derivative-capability.md`.
- **Curve × analytic surface intersection works now**: `intersectCurveWithSurface(curve, surface, window, tolerance,
  interval_count)` (`CurveSurfaceIntersection.hpp`)
  finds the zeros of the surface's signed distance `signedValue(surface, point)` along the curve
  (`geom::detail::rootInBracket`, false position with a bisection fallback). The five analytic surfaces each have a
  `signedValue` overload;
  **an unbounded curve must be given a window** (`LineCurve` / hyperbola / parabola); tangency is missed by default
  (P4); a stretch lying on the surface reports `Coincident` and lists no points.
  Spline and trimmed surfaces need parameter inversion and are not done (backlog G12).
- **All four conics are implemented** (`CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve`); **the
  frame and `continuity()`
  = `CN` live in `ConicCurve`** (`center` / `normal` / `x_direction` + `yAxis()`), and the four subtypes keep only
  their own scalars (the parabola uses the inherited
  `center` as its vertex; there is no separate `vertex` field). Changing `ConicCurve`'s frame = changing the size of
  four types (`CircleCurved` is now 3 vectors + double).
  `BezierCurve` / `BSplineCurve` **are evaluable now** (de Casteljau reused for three orders / Cox–de Boor basis +
  quotient rule), both constructible and dispatchable through `Curve<T>&`;
  **every spatial curve type reports `maxDerivativeOrder() == 3`**, each with a non-virtual `thirdDerivative(u)`
  (zero for line/parabola, `-P'` for circle and ellipse,
  `P'` for hyperbola, third-order differences / a third basis layer + quotient rule for splines); `OffsetCurve` is
  therefore **constructible** too, and `secondDerivative` is evaluated exactly from the basis curve's third
  derivative.
  Note: `OffsetCurve`'s `secondDerivative` relies on `basis->derivativeAt(u, 3)`; for an offset of an offset the
  basis answers only 2 — then `maxDerivativeOrder()` reports 1,
  `isValid()` is false and the second derivative answers a zero vector
  (`decisions/ADR-2026-11-06-space-third-derivative-and-offset.md`).
  `BSplineCurve::isValid()` requires **clamped form** (first and last multiplicity = `degree + 1`, distinct knots
  increasing); both splines require all weights positive.
  Degenerate input has exactly one test — `isValid()`: a line (zero direction, `closestPoint()` returns the origin,
  `intersectWith()` returns no flags, no division by zero),
  a trim (empty basis curve / reversed interval), a conic (`normal` zero ⇒ it collapses to a segment or ray along
  `x_direction`, not “a circle of radius 0”).
  `CircleCurve::bounds()` and `EllipseCurve::bounds()` use the same support function `sqrt((a·X_i)² + (b·Y_i)²)`,
  giving a segment box when degenerate.
  When writing a derived class: **anything that depends on the base class must be qualified** — write a base static
  function as `ConicCurve<T>::inPlaneAxis(...)`, and declare inherited data members / functions
  with `using ConicCurve<T>::center;` or similar, otherwise MSVC reports C3861/C2065. `inPlaneAxis` is now a free
  function in `Frame.hpp`
  (`vn::geom::spatial::inPlaneAxis`), and `ConicCurve` no longer has a protected static of that name.
- **`geom/planar` is a second contract, not a generic specialisation of 3D**: `geom::planar::Curve<T>` and
  `geom::spatial::Curve<T>` have the same name and meaning and neither derives from the other
  (`decisions/ADR-2026-11-05-planar-curve-contract.md`); a planar circle is `center` + `radius` and does not share
  the `ConicCurve` frame (its start point is fixed by the plane).
  The module is header-only templates but lives in **the same library as the spatial side** (`vn::Geom`,
  `VN_GEOM_LIB`) with the same global (`vine/geom/geom_global.hpp`),
  so 2D no longer has a separate INTERFACE target; 2D/3D isolation is enforced by `scripts/check_geom_layering.py`
  instead (the compiler cannot stop it inside one target), five rules:
  `geom/planar` must not include `geom/spatial`, and vice versa, `geom/detail` must not include either,
  `geom/detail` must not **name** `Point2`/`Point3`/`Vector2`/`Vector3`,
  an angle-bracket include in a geometry header may only point at `vine/geom/` or `vine/math/` (quotes only at the
  same directory). After adding a rule, verify it once with a temporary violating header.
  Dimension-independent shared implementation lives in `sdk/vine/geom/detail/` (8 headers: traits / crossing search /
  arc length / two-curve refinement / knot model / Bernstein / root finding / integration),
  reached as `geom::detail::` (the tree-private `detail` exists in 6 files, and an unqualified name resolves there);
  it is not public interface, and the public names stay `geom::planar::arcLength` / `geom::spatial::arcLength`.
  Names carry no dimension marker and the precision suffix stays `d` / `f`. Design: `design/geometry-module.md`
  (shape and naming) and `design/geometry-planar.md` (the planar contract).
- **Planar geometry (`geom/planar`) has nine types**: `LineCurve` / `CircleCurve` / `EllipseCurve` /
  `HyperbolaCurve` / `ParabolaCurve` / `BezierCurve` / `BSplineCurve` / `TrimmedCurve` / `OffsetCurve`. The ellipse
  carries its own `x_direction` (major axis, `yAxis()` derived by a +90° rotation), the circle does not — its start
  point is fixed by the plane, so there is **no** planar `ConicCurve` layer.
  **`TrimmedCurve<T>` is the only trim expression** (`ArcCurve` is deleted): a `shared_ptr<const Curve<T>>` basis
  curve + two parameter bounds, with the factory `trimCurve(basis, a, b)`; closed needs the interval to cover the
  whole basis domain **and the ends to coincide**, periodic needs at least one period, otherwise `period() == 0`;
  hence `[0, 2π+0.3]` covers the whole span but is not closed; a trim does not reparametrise, and its derivative is
  the basis curve's velocity at the same parameter.
  **`reversedParameter` does not return the same point**: it gives the same point on the reversed curve (the
  original mirrored about the domain-midpoint axis / the start axis), guaranteeing only involution and swapped ends;
  `evaluate(reversedParameter(u)) != evaluate(u)` (for a circle y flips sign; for an arc x and y swap). The contract
  is watched by the float / double sweep in `tests/test_geom_planar/CurveContractTest.cpp`.
- **Planar conics share `ConicCurve<T>`** (`geom/planar`): the layer holds only `center` + `x_direction`
  (normalised at construction) + `yAxis()` (+90° rotation) + `continuity() == CN`, with no evaluation or parameter
  domain.
  `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` derive from it; **the circle does not** (its start point is
  fixed by the plane, and storing an axis that is always (1,0) would be a dead field).
  The parameters mean different things: ellipse = eccentric angle, hyperbola = hyperbolic angle (only the
  `+x_direction` branch; the other branch is the reversed direction), parabola = the offset along the transverse
  axis (`P = vertex + u²/(4f)·X + u·Y`).
  A zero direction stores a zero vector ⇒ a point query answers `center`, and `isValid()` is the only test; all
  three curves are unbounded and non-periodic, and `reversedParameter` is `-u` for all.
- **Planar closest point `closestPointOnCurve()`** (`geom/planar/CurveProjection.hpp`): the same skeleton as the 3D
  function of the same name — root finding on the stationary points of the squared distance
  `f(u) = (C(u)-Q)·C'(u)` plus a comparison of sample points / window ends;
  an unbounded curve (line / hyperbola / parabola) **must** be given a window, otherwise `nullopt`. Two things to
  note: with several equidistant solutions (the centre of a circle) the answer is “one of them”, so do not assert
  the parameter is 0;
  the closest point of a hyperbola to a point on its axis is usually **not at the vertex** (e.g. from (3,0) to
  `x=2cosh, y=sinh` the minimum is at `5cosh u = 6`, distance √0.8), so solve the stationary-point equation first
  when writing a test.
- **Planar bounding boxes**: `bounds()` exists only on bounded types (`CircleCurve` / `EllipseCurve`, returning
  `math::Rect2<T>`), and the interface does not declare it — an unbounded curve could only return a fake box.
  A circle also has `bounds(first_parameter, last_parameter)`: the **tight box** of an arc lives here (the interval
  direction does not change the point set; a reversed interval gives the same box) —
  `TrimmedCurve` erases the basis curve, so the support function cannot be asked and a segment's box cannot live on
  the trimmed view.
  The ellipse uses the support function on the world axes, `sqrt((a·X_i)² + (b·Y_i)²)`, **not** `±major_radius`
  (which is loose once rotated); the circle uses `|radius|`.
- **An invalid state must answer finite values**: a parabola with zero focal length used to divide by zero
  (`u²/(4·0)` → NaN / inf); now `pointAt` answers the vertex and both derivatives a zero vector
  (`hasFocalLength()`);
  the other degenerate forms: a zero-radius circle → its centre, a zero-semi-axis ellipse → a segment, a
  zero-semi-axis hyperbola → the line through the centre, a zero-direction conic → center.
- **Planar curve × curve intersection `intersectCurveWithCurve()`** (`geom/planar/CurveCurveIntersection.hpp`): a
  three-valued result `CurveCurveContact{None, Isolated, Coincident}` + `hits` (point and both parameters, ordered by
  the first curve's parameter), mirroring the shape of 3D's `CurveSurfaceContact`.
  The algorithm is dual: sample along the first curve and take each point's perpendicular distance to the second
  with `closestPointOnCurve()`. **That distance answers two things at once** — three or more consecutive samples
  within tolerance = a coincident stretch; a distance ≤ one step length (`step·|C1'|`) = a candidate, then a 2D
  Newton converges `C1(u)=C2(v)`.
  The first version took “sample distance ≤ tolerance” as the candidate test, and almost every crossing was missed
  (a sample almost never lands exactly on the other curve).
  Known boundaries: tangency is missed by default (with a singular Jacobian a hit is accepted only when the residual
  is already within tolerance); two intersections closer than three steps are read as coincident.
- **Deducing `T` in geometry templates**: a family-level algorithm's `T` appears only in default arguments, so
  `template <FP T, ...>` cannot deduce it — always use `template <typename C...>` + `geom::detail::CurveScalar<C>`
  (= `C::value_type`, in `sdk/vine/geom/detail/CurveTraits.hpp`) and `geom::detail::CurveWindow<C>`, with
  `geom::detail::resolveCurveWindow()` for the window and `geom::detail::resolveTolerance()` for the tolerance.
  Those aliases are **no longer** in `CurveProjection.hpp`, so curvature / arc length / two-curve distance do not
  have to pull in the projector for one alias.
- **Planar `BezierCurve`**: the data model is the interface — `poles` + a nullable `weights`,
  `degree() = poles.size()-1`, `isRational()` is “weights non-empty”. Evaluation and the first two derivatives come
  from one pass of `geom::detail::deCasteljau` (poles, first differences, second differences → scaled by 1 / n /
  n(n-1)); the rational form is computed in homogeneous coordinates and divided by the weight.
  This is the module's **first curve that owns memory** (R2's written exception: deep copy, not trivially copyable,
  one temporary array allocated per evaluation). `bounds()` is the **pole box** (convex-hull property: contains,
  not tight); a tight box needs the roots of the derivative and is not done today.
  Family-level algorithms (closest point, intersection, contract sweeps) accept it without any change — which is
  exactly what the eleven-item contract is there to prove.
- **Planar `BSplineCurve`**: distinct knots + multiplicities (not expanded), poles, optional weights; the domain is
  `[first knot, last knot]`, and a parameter **outside the domain is clamped to the end** (unlike Bezier
  extrapolation); `continuity()` comes from `knotContinuity(degree, multiplicities)` (an interior multiplicity
  decides `C(degree − multiplicity)`); evaluation is the weighted sum of the `p+1` `basisValue`s of the containing
  span.
  `bounds()` is the pole box like Bezier. `isValid()` = `isClampedKnotVector` (first/last multiplicity = degree + 1,
  interior multiplicity ≤ degree, pole count consistent) + all weights positive.
- **`geom::detail::knotSpan()` once had an infinite loop** (fixed 2026-11-05): `high` was initialised to `last_span`
  instead of `last_span+1`, so when the bracket was down to one cell `mid` stopped advancing — a spline with
  interior knots hung when evaluated in the second span. Regression case
  `tests/test_geom_spatial/BSplineBasisTest.cpp` (then in `test_math`); postmortem
  `.ai/bugs/spline-knotspan-hang.md`.
- **Planar offset `OffsetCurve<T>`**: holds a `std::shared_ptr<const Curve<T>>` basis curve + a signed distance
  (factory `offsetCurve(basis, d)`), the same shape as 3D;
  **order = `min(2, basis capability − 1)`**: a basis that answers 3 ⇒ the offset answers 2; a basis that answers 2
  (another offset) ⇒ the offset answers 1, `derivativeAt(u,2)` answers empty and `isValid()` is false.
  Sign convention: a positive distance is on the **left of the direction of travel** (`-C'.y, C'.x` normalised), so
  a counter-clockwise circle's offset radius is `r - d`. To follow the same geometric curve from a reversed basis,
  the distance must be **negated**.
  At a cusp (zero velocity) the normal is the zero vector and a query falls back to the basis curve's own value;
  `continuity()` forwards the basis curve and does not promise differentiability at a cusp; there is no `bounds()`.
- **Planar family-level measures**: `arcLength()` / `parameterAtLength()` (`CurveLength.hpp`: adaptive Simpson +
  Newton with a bisection guard; `tolerance` is a **length** error),
  with the numeric kernels in `geom::detail` (`adaptiveSimpson()` / `parameterReaching()`; `rootInBracket()` lives
  in the same directory), and **one copy shared by both trees** (arc length ×2, closest point ×2, 3D curve ×
  surface), so the sharing is a fact rather than a pre-built abstraction;
  the “geometric tolerance → parameter tolerance” conversion (`value_tolerance / |dP/du|`) stays on the geometry
  side, and the whole module has **one** `geom::detail::narrowCrossing()`
  (`sdk/vine/geom/detail/CrossingSearch.hpp`), so the three call sites no longer repeat that preamble line by line;
  arc length and its inverse also exist once
  (`arcLengthBetween` / `parameterCovering` in `geom/detail/CurveMeasure.hpp`), and the public entries of both trees
  only resolve window and tolerance and forward
  (`decisions/ADR-2026-11-06-shared-numeric-kernels-in-math.md`,
  `decisions/ADR-2026-11-06-geom-detail-subtree.md`; the kernels' own tests are in `tests/test_math/`).
  `signedCurvature()` (`CurveCurvature.hpp`: `(C'×C'')/|C'|³`, positive for a left turn, `nullopt` at zero
  velocity), `curveToCurveDistance()`
  (`CurveCurveDistance.hpp`: sampling + closest-point projection + 2D Gauss-Newton, returning two points / two
  parameters / the distance). All three obey the C5 window rule;
  the curvature sign and the offset share the same left-normal convention. The 3D side has the same names in
  `geom/spatial/CurveLength.hpp` and `CurveCurvature.hpp`,
  with the same algorithm and window rules but an unsigned curvature (`curvature()`; 3D has no natural left/right).
  The 3D two-curve closest distance has also landed (`geom/spatial/CurveCurveDistance.hpp`,
  result points as `Point3`); backlog G3 now only holds “solid intersection”. The sampling / Gauss-Newton of
  projection and two-curve distance **are still two copies** (the glue ratio is high; re-evaluate under the ADR's
  trigger condition).

- **The BVH is two types, “structure + builder”** (`spatial/Bvh.hpp`, `spatial/BvhBuilder.hpp`): `Bvh<T, Volume = Aabb>` holds only the primitive
  bounds and the flat node tree, `BvhBuilder` does the building, and between them there is exactly one contract, `detail::BuildJob`. `check_geom_layering.py` rule 6
  forbids `Bvh.hpp` from including `BvhBuilder.hpp` — with the builder deleted, `Bvh.hpp` must still compile on its own, which is the executable test of “separated responsibility”.
  An internal node's `offset` is the **right** child index; the left child is always `parent + 1` (writing `node.offset - 1` walks into itself or out of bounds).
  The node array grows by appending through `appendNode()` — do not pre-allocate `2·⌈n/leaf⌉+1`, since the leaf count depends on the split (5 primitives split into 2+3).
  A query callback receives the leaf's **conservative merged box**, so the result is a superset and exact testing is the caller's job. An `Obb` volume was measured and rejected
  (PCA-fitted corner merging: a leaf's surface area is about 2× an `Aabb` tree's, and a corner falls one rounding step outside the fitted box); do not rebuild it that way.
  Design and rationale: `design/geometry-bvh.md`, `decisions/ADR-2026-11-07-bvh-as-structure-plus-builder.md`.

- **The triangle tree has exactly one numbering rule**: `spatial/TriangleBvh.hpp`'s `TriangleSource` (`positions()` + `triangleCount()` +
  `triangleCorners()`) plus `registerTriangles()` / `buildTriangleBvh()` / `restoreTriangleBvh()`. `Mesh` and the draw frame's
  `TriangleStream` each carry a `static_assert` pinning that they satisfy the concept — do not hand-write “walk triangles → compute box → add” on either side again.
  The rule does not throw: a bad corner or a non-finite coordinate returns `false` (the already-registered prefix may stand; the caller discards that tree).
  The shape has two forms: `BvhTopology` (owning) and `BvhTopologyView` (borrowed, from `view()` or an implicit conversion), and `RestoreTopology` holds only
  the latter. See `design/geometry-bvh.md` 2.14 / 2.15 and F9.

- **A mesh carries its own triangle identity and tree**: `Mesh::triangleCount()` / `Mesh::triangleCorners(i, corners[3])` are virtual and every concrete mesh
  implements them by the same rule as its own `toTriangleMesh()` (three indices per triangle / a quad along the 1→3 diagonal / a polygon as a triangle fan), so triangle numbering is stable across conversions
  — it is both the tree's primitive id and the picking identity. `Mesh` caches a `Bvh<double>` (`buildBvh(builder)` / `bvh()` / `hasBvh()` /
  `setBvh()` / `clearBvh()` / `sharedBvh()`): one primitive per triangle; `setPositions` / `setIndices` /
  `MixedMesh::setFaceOffsets` drop the tree, normals/UVs/colours do not. `setBvh()` returns `bool`: it accepts only a built tree with `primCount() == triangleCount()`
  (the same admission rule as `Geometry::adoptBvh()`). The tree is deliberately double: query rays are `Rayd`, and narrowing to float misses hits far from
  the origin. The tree is published as `std::shared_ptr<const Bvh>` and is **immutable once published** (a rebuild swaps in a new object; `clearBvh()` only stops publishing),
  so another frame can share the same tree without copying. An unbuilt tree answers no candidates and does **not** degrade to a linear scan.
  See `design/graphics-geometry.md` 2.9 / F32 and `decisions/ADR-2026-11-07-mesh-triangle-tree-cache.md`.

- **Do not mix the two smart pointers**: `Buffer<T>` is `RefCounted` → use `intrusive_ptr` (the count lives in the object; `Mesh`'s five channel handles are `intrusive_ptr<const Buffer<T>>`, and a mesh only replaces a channel, never writes bytes in place); `Bvh` is a value type → sharing it can only use `shared_ptr<const Bvh>` (and building it must still be able to sit on the stack). Criterion: **count in the object → intrusive_ptr; value type to share → shared_ptr**. Their sharing contracts differ too: one is “shared mutable bytes, writes go through a `Buffer::edit()` scope (announced at the end)”, the other is “immutable after publication”.

- **A tree can be sealed once built**: `Bvh::seal()` releases the per-primitive boxes (`Aabb<double>` is **48 B/primitive**), and keeps only `shape_` + `node_bounds_`;
  `isSealed()` is derived from “built and the primitive boxes are empty”, with no flag. After sealing, `add`/`addRange`/`update`/`refit`/`reserve`/
  `restoreTopology`/building throw `logic_error`, `primCount()` answers from `shape_.order.size()`, and the query callback's second argument becomes the **leaf box**.
  Leaf granularity (`BvhBuilder::leafSize`) changes only the **node count**, not the primitive boxes — lowering memory needs both.
  `update`/`refit` have **no production caller today** (tests only): a rigid transform does not touch the tree (the tree is in local space and the key holds no world matrix),
  and a vertex change drops the tree and rebuilds. See `design/geometry-bvh.md` 2.16 / F10 / P10 and
  `decisions/ADR-2026-11-10-bvh-seal-and-coarse-picking-tree.md`.
