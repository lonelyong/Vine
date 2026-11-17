# geometry parametric geometry and model data

> **Status**: current
> **Last verified**: 2026-11-04
> **Scope**: the membership and contract of parametric geometry (`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`) in `src/base/geom/sdk/vine/geom/spatial` as **modelling types**, and the boundary between `Primitive` and the geometry algorithms.
> Not the storage and revision of meshes / B-rep / point clouds (see `graphics-geometry.md`), not the classification of the curve and surface families (see `geometry-kinds.md`; the surfaces of revolution and the spline surfaces in `geometry-axis-surfaces.md`), not the ray–primitive contract (see `geometry-ray.md`).
> **Sources**: `src/base/models/sdk/vine/models/Primitive.hpp`, `src/base/models/sdk/vine/models/PrimitiveDispatch.hpp`, `src/base/models/src/Primitive.cpp`; `tests/test_geom_spatial`, `tests/test_brep`, `tests/test_robotics_io`.
> **Backlog**: `../backlog/geometry.md`

## 1. Overview

### 1.1 Problem and background

Parametric geometry is where two requirements meet, and they point in opposite directions:

- **Modelling** needs an object that can be placed in a scene, has identity and can be serialised, answering "what shape am I, how big, and where".
- **Algorithms** need a cheap value answering "where does this ray hit" and "what is the bounding box of this box".

Press the two into one type and the modelling object drags the algorithms in; split them into two sets of types and the parameters have two homes. The choice here is: **parametric geometry stays on the modelling side (subclasses of `Primitive`) and the algorithms stay on the geometry value side**, because such a shape has almost no algorithms of its own: ray–box is `Aabb::intersectWith(Ray)`, and ray–sphere belongs to the surface (`SphericalSurface`, see `geometry-kinds.md` 2.10).

### 1.2 Goals

- `Primitive` is the only base class of parametric geometry: the five subclasses carry their own sizes, `ShapeType` is handed to the base by the subclass at construction, and the tag cannot disagree with the class.
- One header per subclass: `models/Box.hpp` … `models/Ellipsoid.hpp`, each with a `.cpp`; `PrimitiveDispatch.hpp` holds the closed-set dispatch only.
- Geometry algorithms do not enter `Primitive`: a solid answers `bounds()`, `isValid()` and `hasVolume()` only.

### 1.3 Non-goals

- No value-typed parametric geometry: `Box` and friends are `Shape`s with identity and revision; a caller that wants "just compute with the dimensions" uses the value types `Aabb` / `Obb` / `Plane` / `SphericalSurface`.
- No **solid-to-solid** distance on parametric geometry: the nearest point / distance to a solid and ray intersection are answered by the family `Solid<T>` (`geometry-solid.md`); the distance between two solids still has no consumer (`G8`).
- Do not let `geometry` depend on OCCT: the backend vocabulary and the analytic construction stay in `brep`.

### 1.4 One-paragraph conclusion

**Parametric geometry is a modelling type (the five subclasses of `Primitive`, one header each); the base class holds no geometry and a subclass builds its solid on demand; geometry algorithms belong to the value types and the surfaces; `models` depends on `geometry`, never the reverse.**

## 2. Design Points

### 2.0 Why not value types

- **The rejected approach**: make the five dimensions identity-free value types, hold them in a `std::variant` inside `Primitive`, and dispatch at the consumer with `std::visit` / `as<T>()`.
- **Why it was rejected**: shapes such as `Box` / `Cone` provide no algorithms of their own (ray–box is the `Aabb` slab, ray–sphere belongs to the surface), so putting them on the "algorithm value" side empties the meaning of `Solid` and gives the radius / edge length two homes, one in the solid and one in the surface; and the modelling side needs identity, name and revision.
- **Cost (accepted)**: `Box` and friends carry a vptr and a reference count, so a copy is not a memcpy; adding a shape touches `ShapeType`, the two switches in `Shape.cpp`, the construction in `brep` and the reading / writing in `robotics::io`. Recorded here so the argument is not repeated.

### 2.1 `Primitive` is an abstract base class, one header per subclass

- **What**: `class Primitive : public Shape` has only the protected constructor `Primitive(ShapeType)` and one pure virtual `bounds()`, and **holds no geometric value**; `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` each have a header (`models/Box.hpp` … `models/Ellipsoid.hpp`), each stores its dimensions (full extents) and each provides its `solid()`.
- **Directory boundary**: types with identity (`Shape` / `Primitive` / `Model` / meshes / `BrepShape` / `PointCloud` / `Material`) live in `src/base/models` (`sdk/vine/models/`), and the geometry value types (curves, surfaces, `Solid`, `Aabb` / `Obb` / `Ray`) live in `src/base/geom/sdk/vine/geom/spatial`. `models` depends on `geometry` in one direction.
- **Why**: `G1`, `G12` — one type per file, and a consumer includes only the shape it writes about; `PrimitiveDispatch.hpp` carries the single entry point of the closed set (`visitPrimitive()`). Each subclass's `.cpp` includes its own header and the DLL export is generated by that TU, with no reliance on "some TU remembers to include every type".
- **Boundary**: `Primitive` itself declares no geometric query (no `pointAt`, no `intersectWith`, no `solid()`). "Whether it is a solid" is a property of the subclass: a future profile or swept surface derives directly without changing the base class (`Profile` in `SolidTest.cpp` is the acceptance of this test).

### 2.2 The subclass hands the tag to the base class

- **What**: each subclass's constructor delegates to a call such as `Primitive(ShapeType::Box)`, and `shape_type_` is written once, in the base constructor.
- **Why**: `G11` — the alternative, each of the five constructors writing `shape_type_ = ...` itself, exposes a mistake only far from its cause; with the base class taking it, "a class can only report its own type" is guaranteed by the construction chain.
- **Boundary**: a subclass can still be derived and report a non-existent type (`StrayPrimitive` in `ConstructionTest` tests exactly this); a consumer validates with `switch (shapeType())` + `obj_cast` together, as `brep` and `robotics::io` do.

### 2.3 The canonical local space

- **What**: the box and the ellipsoid are centred on the origin; the cylinder and the cone have their axis on +Z, span `[-height/2, +height/2]`, and the cone's base is at `-height/2`.
- **Why**: `G4`, `G12` — this is the space `brep` construction, the robot IO and the existing tests all assume; placement belongs to the node transform, and the shape itself carries no coordinate system.

### 2.4 The algorithms are not on `Primitive`

- **What**: the model answers only `bounds()`, `isValid()` and `hasVolume(eps)`, and all three are **forwarded** by the subclass to one `solid()` construction; when geometry is needed, each subclass's `solid()` gives a value of the concrete kind and `visitPrimitive(primitive, visitor)` hands the concrete subclass to a visitor. Tessellation hands the result of `solid()` to the matching `tessellateSolid()` overload (`geometry-tessellation.md`).
- **Why**: `G13`, `G1`, `G8`, `G12` — a modelling type's job is identity / name / revision and a geometry type's job is the formula; hanging a geometric query on the model would scatter one shape's formula over several places and make model consumers aware of algorithm entry points. `G13` does not ask whether a new consumer exists: putting an existing capability on the side that owns it is itself a constraint.
- **Boundary**: the modelling side has no code dispatching geometry by `ShapeType`; `Solid<T>` does not know `Shape` / `Primitive`, and the two meet in exactly one place, `Primitive` (`geometry-solid.md` 2.1).

### 2.5 OCCT does STEP parsing and tessellation only

- **What**: `brep`'s long-term job is "read STEP / IGES into a `BrepShape` and tessellate it into a `TriangleMesh`"; `brep::makeBrep(Primitive)` is the **transitional bridge** that turns a parametric shape into an analytic solid with `BRepPrimAPI`.
- **Why**: `G2`, `G5` — the backend vocabulary does not enter `geometry`; without a backend `hasBackend()` is false and `makeBrep` returns an empty handle, with the contract unchanged.

## 3. Features

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Modelling type | `Primitive : Shape` + five subclasses (one header each), `shapeType()` fixed by the construction chain | landed |
| F2 | Shape contract | `isValid()` / `hasVolume(eps)` / `bounds()` (canonical local space) | landed |
| F3 | B-rep bridge | `brep::makeBrep(Primitive)` dispatches by type, an empty handle without a backend | landed (the local build has the OCCT backend) |
| F4 | Serialisation | `robotics::io` reads and writes the five geometric elements by `ShapeType` | landed |
| F5 | Tessellation | `tessellateSolid(concrete kind, quality)`: the five analytic solids become a closed, outward-facing `Triangulation<T>` with exact normals; the modelling side assembles it into a `TriangleMesh` with `meshFromTriangulation()` (see `geometry-tessellation.md`) | landed |
| F6 | Model → geometry | each subclass's `solid()` (the concrete kind built from the current dimensions) and `visitPrimitive()` (hands the concrete subclass of the closed set) | landed |
| F7 | Solid queries | `Solid<T>::closestPoint()` / `intersectWith(Ray)`: nearest point / distance and ray hits with solid semantics, with the model taking no part (see `geometry-solid.md`) | landed |

### 3.1 F1 the modelling type

- **Constraint** (C1): a subclass may report its `ShapeType` only through the protected constructor; `Primitive` cannot be instantiated directly.
- **Acceptance** (T1): `aPrimitiveOutsideTheFiveProducesNoSolid` in `tests/test_brep/ConstructionTest.cpp`.

### 3.2 F2 the shape contract

- **Behaviour**: default construction gives unit dimensions; `bounds()` returns a local-space `Aabbf` (the box is derived by `Solid<T>` and is not recomputed in the subclass, see `geometry-solid.md` 2.1); an illegal dimension is reported by `isValid()` rather than rejected at construction.
- **Constraint** (C2): `Cone::bounds()` takes the bounding cylinder of the base.
- **Acceptance** (T2): `everyPrimitiveFillsItsAnalyticBounds` checks the canonical space of the five shapes against the bounding box after tessellation.

### 3.3 F3 the B-rep bridge

- **Behaviour**: `brep::makeBrep(Primitive)` dispatches by `shapeType()` to `BRepPrimAPI` and constructs the analytic solid; without a backend `hasBackend()` is false and the handle is empty; a solid with illegal dimensions constructs no solid.
- **Constraint** (C4): `geometry` does not know OCCT — the bridge lives in `brep`, and the direction of `R2` is unchanged.
- **Acceptance** (T3): `aBuildWithoutABackendProducesNoSolid`, `anInvalidPrimitiveProducesNoSolid` and `theSolidCarriesThePrimitiveName` in `tests/test_brep/ConstructionTest.cpp` (the last two return early in a build without a backend).

### 3.4 F4 serialisation

- **Behaviour**: `robotics::io` constructs the subclass by tag when reading URDF / custom XML, dispatches by `ShapeType` when writing back, and checks that `obj_cast` succeeds.
- **Acceptance** (T4): `tests/test_robotics_io/DeviceIOTest.cpp`, `PkgIOTest.cpp`.

## 4. Public Interface

| Header | Content |
|---|---|
| `models/Primitive.hpp` | `Primitive` (protected constructor taking a `ShapeType`) |
| `models/Box.hpp` … `models/Ellipsoid.hpp` | the five subclasses, one header and one source file per type |
| `models/PrimitiveDispatch.hpp` | `visitPrimitive()` |
| `models/SolidTessellation.hpp` | `meshFromTriangulation()`: packs the geometry-side `Triangulation` into a `TriangleMesh` (tessellation itself is in `geometry-tessellation.md`) |
| `geometry/Solid.hpp` | the `Solid<T>` interface and `Solidf` / `Solidd` (see `geometry-solid.md`) |
| `models/Shape.hpp` | `ShapeType`, `isValid()`, `hasVolume()`, `revision()` |

## 5. Constraints, Requirements and Invariants

- **C1**: `Primitive` cannot be instantiated directly; a subclass's `shape_type_` can only come from its own construction chain.
- **C2**: every `bounds()` is a box in canonical local space; a world box multiplies by the node transform.
- **C3**: a non-positive dimension is illegal; an illegal shape constructs no solid, but its `bounds()` is still a well-formed box.
- **N1**: the default tolerance of `Shape::hasVolume()` is `double`, and a subclass gives its default for float dimensions; an explicit `eps` is the caller's.
- **N2**: all five subclasses are `final`, so a further derivation reporting a non-existent `ShapeType` is no longer possible; `brep` / `robotics::io` dispatch by the closed set with `visitPrimitive`, and a primitive outside the five returns `false`.
- **N3**: a subclass stores no solid and caches no `solid()`: each call constructs a value from the current dimensions, so a changed dimension takes effect on the next query with no invalidation point; conversely, a caller may not keep a reference to the value `solid()` returned.
- **R1**: `isValid()` / `hasVolume()` / `bounds()` allocate nothing and throw nothing.
- **R2**: `brep` may include `geometry`, and `geometry` does not include `brep` (`G2`).

## 6. Caveats and Traps

- **P1**: `bounds()` is a box in **local** space; a world box multiplies by the node transform (`graphics-geometry.md` F15/F21).
- **P2**: `setWidth()` and friends do not validate: set the edge length to 0 and the shape is still there, reported only by `isValid()`.

## 7. Rejected Options

- **D1** Making the five dimensions identity-free value types and holding them in a `std::variant` inside `Primitive`: `Box` / `Cone` have no algorithms of their own, so the "value" side gains nothing and the radius / edge length ends up with two homes, one in the solid and one in the surface (`G1`). See `../decisions/ADR-2026-11-04-primitive-is-a-shape.md`.
- **D2** Letting `geometry` depend on OCCT and construct analytic solids directly: the backend vocabulary enters the core module (`G2`), and a build without a backend would lose every contract.
- **D3** Exposing geometric queries on `Primitive` (a virtual `intersectWith`): the algorithm surface hangs on the modelling type and a consumer must remember whom to ask; geometry belongs to the family interface `Solid<T>` and its five kinds (`G1`, `G8`).
- **D4** Writing geometry free functions parameterised on the model (the `f(primitive)` form): each capability writes its own tag dispatch and one shape's formula scatters over several places; once the subclass gives the geometry value through `solid()`, the geometry side has one algorithm only (`G1`, `G12`).
- **D5** Letting `Primitive` hold one solid value by value (an intermediate shape from 2026-11-05, withdrawn): the base class then assumes "a primitive is necessarily a solid", and a profile or swept surface has nowhere to sit (`G1`). See `../decisions/ADR-2026-11-05-primitive-is-a-modelling-base.md`.

## 8. Tests and Acceptance

| # | Criterion | Case |
|---|---|---|
| T1 | a shape outside the five produces no solid | `aPrimitiveOutsideTheFiveProducesNoSolid` in `tests/test_brep/ConstructionTest.cpp` |
| T2 | canonical bounding box | `everyPrimitiveFillsItsAnalyticBounds` in the same file |
| T3 | B-rep bridge | `aBuildWithoutABackendProducesNoSolid`, `anInvalidPrimitiveProducesNoSolid`, `theSolidCarriesThePrimitiveName` in the same file |
| T4 | serialisation round trip | `tests/test_robotics_io/DeviceIOTest.cpp`, `PkgIOTest.cpp` |

## 9. Status and Evolution

- The five subclasses, the protected constructor and the one-header-per-type layout are in place; `brep` and `robotics::io` dispatch by the task type.
- The local build has the OCCT backend (`VN_HAS_OCCT=TRUE`, vcpkg's `opencascade`), `test_brep` builds and passes, and the solid-construction branch of F3 therefore really runs; a build without a backend still takes the `hasBackend()` false path.
- Related: the curve and surface families have no placeholder type, see `geometry-kinds.md` 2.10 and `geometry-axis-surfaces.md`; tessellation, ray intersection and nearest point / distance all land on the family `Solid<T>` (`geometry-solid.md`, `geometry-tessellation.md`, F5–F7), and this topic has no open item.

## 10. References

- `geometry-kinds.md` (the `Curve` / `Surface` families and the surface tree), `geometry-primitives.md` (membership and header rules of the value types), `geometry-solid.md` (the value types of the analytic solids: nearest point and ray intersection), `geometry-tessellation.md` (tessellation)
- `graphics-geometry.md` (mesh storage and the rendering boundary)
- `../decisions/ADR-2026-11-04-primitive-is-a-shape.md`, `../decisions/ADR-2026-11-05-primitive-is-a-modelling-base.md`
