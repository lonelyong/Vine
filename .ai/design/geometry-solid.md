# geometry analytic solids

> **Status**: current
> **Last verified**: 2026-11-09
> **Scope**: the `Solid<T>` family (five analytic bodies: `BoxSolid` / `SphereSolid` / `CylinderSolid` / `ConeSolid` / `EllipsoidSolid`), its two queries (closest point, ray intersection), and how the model side provides that solid (each modelling subclass' `solid()`, `visitPrimitive()`).
> Not tessellation (see `geometry-tessellation.md`), not operations between solids (booleans, distance between solids, see `../backlog/geometry.md` G3), not the surface-family queries (see `geometry-surface-queries.md`).
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/Solid.hpp`, `BoxSolid.hpp` … `EllipsoidSolid.hpp`, `src/base/models/sdk/vine/models/PrimitiveDispatch.hpp`; `tests/test_geom_spatial/SolidTest.cpp`, `SolidDistanceTest.cpp`, `SolidIntersectionTest.cpp`.
> **Backlog**: `../backlog/geometry.md`

## 1. Overview

### 1.1 Problem and background

`Primitive` is a **modelling** type: it carries identity, name and revision, and it answers "what is the geometry of this link", not "compute on this geometry".
The geometric algorithms were originally hung directly on it - tessellation, ray intersection and point-to-solid distance were each a free function whose only entry was `shapeType()`.
There are three consequences: the formulas for one shape are spread across three files; consumers of the model (serialization, scene tree) are forced to know a geometry entry; and each new capability means writing the tag dispatch once more.

The value-type side also lacks the word "solid": `Aabb` / `Obb` are boxes, the five surfaces are unlidded, interior-less shells, and `closestPointOnSurface()` cannot answer the solid semantics of "distance from a point to the solid" (zero inside).

### 1.2 Goals

- One **family**: `const Solid<T>&` accepts any one solid and can be asked about it - which is what a consumer whose "parameter must be a concrete type" needs (an export signature, a backend interface).
- One **closed value set**: the five kinds are all by-value types, with no pointer, no heap and no shared ownership; an entry that must answer per kind gets one overload per kind.
- The model side keeps a single conversion: each modelling subclass constructs its own kind from its own dimensions (`Box::solid()`), and `visitPrimitive()` hands the concrete subclass to the caller; geometry does not know `ShapeType`.
- When a kind is added, the compiler lists which consumers must handle it (a missing branch / a missing overload in the visitor).

### 1.3 Non-goals

- Not offering booleans between solids and the nearest distance between solids: the result type does not fit in `geometry` and there is no consumer (G3, goes through `brep` + OCCT).
- Not offering a represented solid: NURBS bodies, mesh bodies and boolean results are `BrepShape`, never `Solid` (`G13`).
- Not offering a tessellation member on `Solid<T>`: `TriangleMesh` is float, and making it a member would let `Solid<double>` lose precision implicitly (`geometry-tessellation.md` 2.1).
- Not offering an implicit conversion between double and single precision: a caller that wants `double` constructs `BoxSolidd` / `SphereSolidd` / … explicitly (2.6).

### 1.4 One-paragraph conclusion

**`Solid<T>` is the family interface (five virtual queries) and the five kinds are its values; a consumer that does not know the kind asks the family interface, while a consumer that must answer per kind uses `visitPrimitive()` to reach the concrete modelling class and call the matching geometry entry (there is no "variant of the five kinds" type).**

## 2. Design Points

### 2.1 Two layers, two questions

- **What it is**: `Solid<T>` (abstract, five pure virtuals: `isValid` / `hasVolume` / `bounds` / `closestPoint` / `intersectWith`) answers "how is this solid"; `BoxSolid<T>` … `EllipsoidSolid<T>` are its values.
- **Why**: `G13`, `G1` - some parameters have to be concrete types rather than templates: an export signature or a backend interface can only write `const Solid<float>&`.
  The interface layer hands out exactly that type; a consumer that must answer per kind takes the concrete kind (an overload or `visitPrimitive()`), and each of the two layers answers one real question.
- **Boundary**: the model's entry is the subclass' `solid()` (returns the value of a concrete kind) and the free function `visitPrimitive()`; the geometry side does not know `Primitive`.

### 2.2 A closed set is expressed by overloads, not by a variant

- **What it is**: each of the five kinds is a value type; an entry that must answer per kind (`tessellateSolid()`, `brep`'s `solidOf()`) writes five overloads, and the caller hands the concrete subclass in with `visitPrimitive()`.
- **Why**: `G8`, `G1` - the variant has no storage consumer today (after `ADR-2026-11-05-primitive-is-a-modelling-base` the model stores dimensions only); keeping a "variant of the five kinds" would only give the caller a second spelling.
  The overload set and `visitPrimitive()` are two faces of the same closed set: adding a kind leaves one overload missing in the visitor, and the compile error still appears.
- **Boundary**: an arbitrary solid (mesh, NURBS, boolean result) is not a member here, and its exit is `BrepShape`; `Solid<T>` does not know `Shape` / `Primitive`.

### 2.3 Visit and virtual calls each own one segment

- **What it is**: tessellation, the B-rep bridge and the description-file writer dispatch per kind (one overload per kind, or `visitPrimitive()`); `Primitive::isValid()`/`hasVolume()`/`bounds()` and consumers that ask a single question go through the virtual functions.
- **Why**: `G9`, `G11` - a place that needs "one answer per kind" (a mesh, a `BRepPrimAPI` call, an XML element) must be exhaustive, and a missing kind in `std::visit` is a compile error; a place that needs only "one answer" (a bounding box, validity, a ray hit) uses the interface, and the call site does not care about the kind.
- **Boundary**: a visitor used only once is a lambda; one reused is a named struct (for example the tessellation's `TessellateVisitor`).

### 2.4 Canonical local space and half extents

- **What it is**: box / sphere / ellipsoid are centred on the origin; the cylinder / cone axis is +Z and spans `[-half_height, +half_height]`, with the cone's base at `-half_height`.
  `BoxSolid` stores **half** the edge length, and the cylinder and the cone store **half** the height.
- **Why**: `G4`, `G12` - the formulas in that space are written in half extents throughout (the box is a three-axis clamp, the cylinder a radial plus an axial clamp), and the field names `half_x` / `half_height` say which half themselves; full extents appear only on the model description-file side.
- **Boundary**: placement belongs to the node transform holding the shape; `Solid` has no coordinate system, no placement and no "rotated solid".

### 2.5 Result shapes of the two queries and the default tolerance

- **What it is**: `closestPoint(query, tolerance = EPS)` returns `std::optional<SolidProjection<T>>`; `intersectWith(ray, eps = EPS)` returns an ascending, de-duplicated `std::vector<RayHit<T>>`.
- **Why**: `G4`, `G6` - a single-valued answer is an optional and a variable-length answer is a collection, consistent with `closestPointOnSurface()` / `intersectCurveWithSurface()`; the default tolerance is written in **two places, the interface and each kind**, because a derived declaration hides the base's default arguments (P5).
- **Boundary**: `Aabb::intersectWith(Ray)` is still `bool` plus an out parameter - that is the "one hit" question, and the shape follows the question.

### 2.6 Dimensions are float, higher precision is an explicit choice

- **What it is**: the model stores `float`; `BoxSolidd` / `SphereSolidd` / … are the `double` kinds, and a caller that needs them constructs them itself.
- **Why**: `G4` - what the description file reads is float, and a mesh is float too (`TriangleMesh`); with no "implicitly treat a float solid as double" conversion there is no "thought I had double dimensions" misunderstanding.
- **Cost**: crossing precision is written explicitly (constructing `BoxSolidd{...}`); changing precision is not a one-line cast but an explicit type choice.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Family interface and five kinds | `Solid<T>` five pure virtuals; `BoxSolid` / `SphereSolid` / `CylinderSolid` / `ConeSolid` / `EllipsoidSolid` each hold extents and formulas | landed (T1, T2) |
| F2 | Closest point / distance from a point to a solid | `closestPoint(query, tolerance)`: solid semantics, zero inside | landed (T3–T6) |
| F3 | Ray × solid | `intersectWith(ray, eps)`: ascending, at most two, normal turned toward the ray | landed (T4, T7) |
| F4 | The model provides the solid | each modelling subclass' `solid()` (value) and `visitPrimitive()` (hands over the concrete subclass) | landed (T2, T8) |
| F5 | Tessellation | `tessellateSolid(concrete kind, quality)` | see `geometry-tessellation.md` |

### 3.2 F1 Interface and kinds

- **Behaviour**: each of the five kinds is `final` and holds half extents plus its own formulas; each can be default-constructed (all-zero extents) and is rejected by `isValid()`.
- **Constraints** (C1, C2): space and extents are in 2.4; a non-positive extent makes the solid unusable.
- **Acceptance** (T1): `theFamilyIsAnInterfaceAndTheKindsAreValues` and `theBoundsAreTheAnalyticExtentOfEachKind` in `SolidTest.cpp`.

### 3.3 F2 Closest point and distance

- **Behaviour**: the box clamps on three axes; the sphere scales radially; the ellipsoid scales and then runs a one-dimensional Lagrange root find; the cylinder clamps radially × axially; the cone takes the nearer of two candidates, the base disc and the generatrix profile segment.
  A query inside the solid returns the query point itself with distance 0.
- **Constraints** (C3): an unusable solid answers `std::nullopt`.
- **Requirements** (R1): no allocation beyond the return value; the ellipsoid is the only iterative kind, capped at 256 bisections.
- **Acceptance** (T3–T6): one case per kind of the four, plus "unusable input" and "the two precisions agree".

### 3.4 F3 Ray × solid

- **Behaviour**: the box uses slabs; the sphere / ellipsoid a quadratic (the ellipsoid scaled into unit space first); the cylinder is the lateral quadratic filtered by `|z| <= half_height`, plus two end discs; the cone is the lateral quadratic with an axial filter, plus the base disc.
  Results are ascending in `t` and de-duplicated within `eps`.
- **Constraints** (C4, C5): only boundary hits with `t ≥ -eps` are reported; the normal is the outward normal turned toward the ray.
- **Note** (N4): a hit on the cone apex has a zero normal.
- **Acceptance** (T7): the nine cases in `SolidIntersectionTest.cpp`.

### 3.5 F4 The model owns its dimensions

- **Behaviour**: a modelling subclass constructs the concrete kind from the current dimensions (`Box::solid()`); `visitPrimitive()` hands the concrete subclass to the visitor, and the geometry entry receives it through a per-kind overload.
  The model side stores no geometric value.
- **Constraints** (C6): `Solid<T>` does not know `Shape` / `Primitive`; the model side has no code that dispatches geometry on `ShapeType`.
- **Acceptance** (T2, T8): `visitPrimitiveReachesTheConcreteSubclass`, `thePrimitiveBaseHoldsNoGeometry`.

## 4. Public Interface

### 4.1 Family and kinds

| Interface | Contract | Notes |
|---|---|---|
| `Solid<T>` | five pure virtuals: `isValid()`, `hasVolume(T)`, `bounds()`, `closestPoint(query, tolerance)`, `intersectWith(ray, eps)` | the full signature is in `Solid.hpp` |
| `BoxSolid<T>` / `SphereSolid<T>` / `CylinderSolid<T>` / `ConeSolid<T>` / `EllipsoidSolid<T>` | `final` implementers holding half extents | public fields `half_x`, `radius`, `half_height`, `base_radius`, `radius_x/y/z` |
| `Solidf` / `Solidd`, `BoxSolidf` / `BoxSolidd`, … | the two precisions' aliases | the same naming scheme as `Aabbf` / `Aabbd` |

### 4.2 Model side

| Interface | Contract | Notes |
|---|---|---|
| `Box::solid()` … `Ellipsoid::solid()` | the value of the concrete kind | the subclass constructs it from its own dimensions |
| `visitPrimitive(primitive, visitor)` | `bool` | hands the concrete modelling class to the visitor; `false` when it is not one of the five |
| `Primitive::isValid()` / `hasVolume(double)` / `bounds()` | the subclass forwards to `solid()` | recomputes no formula |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- **C1**: every extent is in canonical local space; placement belongs to the node transform, and the family has no coordinate system.
- **C2**: `BoxSolid` stores half edge lengths, `CylinderSolid` / `ConeSolid` store half heights.
- **C3**: an unusable solid (any half extent ≤ 0) answers `std::nullopt` / an empty result, never throws and never divides by zero.
- **C4**: ray results contain only hits with `t ≥ -eps`, ascending and de-duplicated within `eps`.
- **C5**: the normal is the outward normal turned toward the ray (`RayHit`'s rule); a singular point (the cone apex) gets a zero vector.
- **C6**: `Solid<T>` does not know `Shape` / `Primitive`; `Primitive` is the only type that owns both sides.

### 5.2 Requirements (R#)

- **R1**: `isValid()` / `hasVolume()` / `bounds()` / `closestPoint()` do not allocate; `intersectWith()` allocates only the result `vector`.
- **R2**: no dependency on OCCT and none on `brep` (`G2`).
- **R3**: header-only templates; both precisions are instantiated in `test_geom_spatial`.

### 5.3 Invariants (N#)

- **N1**: there is no "empty solid" value: `visitPrimitive()` returns `true` for the five subclasses and `false` otherwise; every geometry entry returns an empty result for an unusable solid.
- **N2**: `bounds()` still returns a well-formed box (min ≤ max) for a degenerate solid.
- **N3**: a ray starting inside reports at most one hit; a tangency (including a double root) reports one.
- **N4**: `Primitive::shapeType()` is given by the subclass' construction chain and corresponds one-to-one with `visitPrimitive()`'s dispatch.

## 6. Caveats and Traps

### 6.1 Solid semantics versus surface semantics

- **How it goes wrong**: using `closestPoint()` for "distance from a point to the sphere surface" gives 0 rather than the distance to the shell.
- **How to detect**: the distance is 0 when the query point is inside the solid.
- **How to prevent**: surface semantics means `SphericalSurface` / `closestPointOnSurface()`; the two are not the same question (the criteria table in 2.2).

### 6.2 The zero normal at the cone apex

- **How it goes wrong**: when the ray passes through the apex `hit.normal` is a zero vector, and lighting computed from it comes out completely black.
- **How to detect**: `t` / `point` look normal while `normal.length() == 0`.
- **How to prevent**: C5; when an apex normal is needed, the caller computes it itself from the lateral limit.

### 6.3 A ray lying in a face

- **How it goes wrong**: expecting "lying on the face" to report infinitely many points or one coincident marker.
- **How to detect**: the result holds only the two end points.
- **How to prevent**: a solid's intersection is an interval, and what is reported is its two end points (C4).

### 6.4 Dimension precision

- **How it goes wrong**: expecting `Primitive::solid()` to give `double`.
- **How to detect**: the type is `const Solid<float>&`, visible at compile time.
- **How to prevent**: 2.6; construct `BoxSolidd` / `SphereSolidd` / … explicitly for higher precision.

### 6.5 A derived class hides the base's default arguments

- **How it goes wrong**: writing `= EPS<T>()` only on `Solid<T>` and then calling `closestPoint(query)` through `BoxSolidf` reports "too few arguments".
- **How to detect**: the call through the concrete kind does not compile while the call through `const Solidf&` is fine.
- **How to prevent**: write the default once on the interface and once on each kind's declaration (2.5); a call site does not repeat `EPS` itself.

## 7. Rejected Options

- **D1** `Solid<T>` is directly an alias of `std::variant` (no interface layer): it loses the "parameter is a concrete type" ability, and every consumer that asks a single question has to visit itself (`G7`). See `../decisions/ADR-2026-11-05-solid-family-and-variant.md`.
- **D2** family interface + `Primitive` holding it by pointer: one heap allocation per model construction and one dereference per `bounds()`, and it introduces a "model and geometry share ownership" path (`G10`, `G13`). The same ADR.
- **D3** one model subclass per shape (`BoxSolid(Primitive)` …): `ShapeType` and the class can drift, the formulas of one shape spread over several places, and model consumers know the algorithm entries (`G1`, `G12`). The same ADR.
- **D4** making tessellation a member of `Solid<T>`: `TriangleMesh` is float, so `Solid<double>::tessellate()` could only output reduced precision (`G4`). See `geometry-tessellation.md` D5.
- **D5** implicit conversion between float and double solids: it would hide "the dimensions only have float resolution" inside the call chain (`G4`). See 2.6.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | the interface is abstract, the five kinds are values, the two precisions' kinds do not convert into each other | `theFamilyIsAnInterfaceAndTheKindsAreValues` in `SolidTest.cpp` |
| T2 | `visitPrimitive()` hands over the concrete subclass; a primitive that is not one of the five gets `false` | `visitPrimitiveReachesTheConcreteSubclass`, `thePrimitiveBaseHoldsNoGeometry` in the same file |
| T3 | the box's faces / corners / interior | `aBoxClampsToItsFacesAndCorners` in `SolidDistanceTest.cpp` |
| T4 | a sphere's inside and outside, two hits / tangency | `aSphereProjectsAlongItsRadius`, `aTangentialRayTouchesASphereOnce` in `SolidIntersectionTest.cpp` |
| T5 | the ellipsoid's three axes + the Lagrange condition | `anEllipsoidProjectsOntoItsSurfaceAlongTheNormal` in `SolidDistanceTest.cpp` |
| T6 | a cylinder's radial / axial / edge cases, the cone's choice between its base and its generatrix | `aCylinderClampsRadiallyAndAxially`, `aConeChoosesBetweenItsBaseAndItsSide` |
| T7 | a ray from inside reports only the exit, end-cap hits, de-duplication | `aRayFromInsideASolidReportsOnlyTheExit`, `aCylinderIsHitOnItsSideAndOnItsCaps` in `SolidIntersectionTest.cpp` |
| T8 | unusable solids, degenerate extents, the per-kind volume criterion | `anUnusablePrimitiveStillAnswersItsBounds`, `everySubclassReportsItsOwnTag`, `theDefaultSubclassIsTheUnitShape` in `SolidTest.cpp` |

Command: `cmake --build build --config Debug --target test_geom_spatial`, then run `build/bin/Debug/test_geom_spatial.exe`.

## 9. Status and Evolution

### 9.1 Current status

- The family interface and the five kinds exist; the model stores no geometric value (a subclass stores dimensions only, `solid()` constructs on demand); tessellation is geom's `tessellateSolid()`, which the model side's `meshFromTriangulation()` assembles into a mesh (see `geometry-tessellation.md`); `brep` and the description-file writer both dispatch per kind (`visitPrimitive()` + overloads).

### 9.2 Known gaps / backlog

- Operations between solids (booleans, the nearest distance between solids) have no consumer, see `../backlog/geometry.md` G3.
- A new kind (torus / capsule / prism): add one `XxxSolid` header + one `ShapeType` + one `visitPrimitive()` branch, and fill in the tessellation and `brep` overloads; the compiler reports at the visitor's instantiation and at overload resolution. Represented solids do not come in here; they go through `BrepShape`.
- Tessellation exists in both precisions; only the model mesh is float, so assembling a `Triangulationd` into a mesh converts to float explicitly (`geometry-tessellation.md` 9.2).

### 9.3 Compatibility / migration

- Model side: `Primitive` is the only class; construct with a subclass (`Box` / `Sphere` / …), read with `solid()` (the concrete kind's value) or `visitPrimitive()` (the concrete subclass).
- There is no tessellation / ray / distance entry that takes a model as its parameter; the replacements are `tessellateSolid(visitPrimitive(...))` and `p.solid().closestPoint(...)` / `p.solid().intersectWith(...)`.

## 10. References

### 10.1 Code

- `src/base/geom/sdk/vine/geom/spatial/Solid.hpp`, `BoxSolid.hpp` … `EllipsoidSolid.hpp`
- `src/base/models/sdk/vine/models/Primitive.hpp`, `Box.hpp` … `Ellipsoid.hpp`, `PrimitiveDispatch.hpp`
- `tests/test_geom_spatial/SolidTest.cpp`, `SolidDistanceTest.cpp`, `SolidIntersectionTest.cpp`

### 10.2 Docs and ADRs

- `geometry-parametric.md` (the modelling type's contract)
- `geometry-kinds.md` 2.6 (algorithms live on the geometry side)
- `geometry-tessellation.md` (the mesh form of the same solids)
- `../decisions/ADR-2026-11-05-solid-family-and-variant.md`
