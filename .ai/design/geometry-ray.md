# geometry rays and primitive intersection

> **Status**: current (the value type and the four primitive intersections; `graphics` picking goes through it)
> **Last verified**: 2026-11-04
> **Scope**: the ray value type of `src/base/geom/sdk/vine/geom/spatial` and the membership and contract of the ray–AABB / OBB / plane / triangle queries.
> Not scene traversal, picking identity and selection state (see `graphics-scene.md`), not ray–mesh traversal and acceleration structures (see §9.2).
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/Ray.hpp`, `RayHit.hpp`, `intersectWith(Ray, RayHit&)` of `Aabb.hpp` / `Obb.hpp` / `Plane.hpp`, `tests/test_geom_spatial`.
> **Backlog**: `../backlog/graphics.md` (open items such as ray–mesh traversal)

## 1. Overview

### 1.1 Problem and background

The ray lives in `viz`: `vn::graphics::Ray` (`Vec3d` origin + unit direction) shares its module with `vn::graphics::RayIntersection`, and the latter mixes two things:

- **primitive intersection** — the slab (box), Möller–Trumbore (triangle): depends on geometric data only;
- **scene traversal** — node tree, world matrices, instance stream, picking identity: belongs to the rendering side only.

There are three consequences:

- A non-rendering ray consumer (the selected face in `brep`, robot LiDAR simulation, collision queries) must either depend on `viz` or copy a `Ray` of its own; `brep` is in `base`, and depending on `viz` is a clear back edge.
- One slab / triangle intersection can only be implemented inside graphics, and no other layer can reach it.
- The `Aabb` / `Obb` / `Plane` of `geometry` have no ray entry, so ray queries and the contract of these types are split across two modules.

### 1.2 Goals

- `Ray` has one definition, on the `base` side, on the same layer as `Aabb` / `Obb` / `LineCurve` / `Plane`.
- The four primitive intersections share a module with the intersected types, named after the existing convention of `Plane::intersectWith(LineCurve, Point3&)`.
- `graphics` keeps scene semantics only: screen → world, world → local, nearest hit and picking identity.

### 1.3 Non-goals

- No ray–mesh traversal and no acceleration structure (BVH / octree): this page defines "ray × a single primitive" only; the membership and contract of the acceleration structure are in
  `geometry-bvh.md` (`D5` is superseded by `ADR-2026-11-07-bvh-as-structure-plus-builder.md`).
- No scene query and no selection state: picking is a mechanism, selection is business (`graphics-scene.md`).
- No ray entry on **value types** such as `SphericalSurface` / capsule / conical surface: no consumer; add one under the same convention when one appears. Ray intersection of the analytic solids (`Solid<T>`) exists already, see `geometry-solid.md`.

### 1.4 One-paragraph conclusion

**A ray is a spatial entity and belongs to `geometry`; an intersection hangs on the intersected object; scene traversal and picking identity stay in `graphics`; `math` is not touched.**

## 2. Design Points

### 2.1 `Ray` and `LineCurve` are two semantics, not merged

- **What**: `Ray<T>{ origin, direction }` with a **unit** direction, where `t` is a distance in model units; `LineCurve<T>` is an infinite line, the direction need not be unit, and `t` is tied to `|direction|`.
- **Why**: `G4`, `G12` — merging the two into one type with a `bool is_half_line` or "the direction length is a convention" makes every reader re-derive the dimension of `t`.
- **Boundary**: `Ray` does not enter the `Shape` hierarchy (the same position as the four value types of 2.4); the existing contract of `LineCurve` is untouched.

### 2.2 The intersection hangs on the intersected object

- **What**: `Aabb<T>::intersectWith(const Ray<T>&, RayHit<T>&)`, the same signature on `Obb<T>` and on `Plane<T>`; a triangle has no value type and therefore becomes the free function `intersectTriangle(ray, a, b, c, hit)`.
- **Why**: `G1`, `G6` — the same shape as `Plane::intersectWith(LineCurve, Point3&)`: the intersected object owns the algorithm, and the caller need not know the slab or Möller–Trumbore.
- **Boundary**: `Obb` transforms the ray into local space and reuses `Aabb`, writing no second slab (`G1`, `G10`).
- **Boundary**: `Aabb` / `Obb` report the **nearest hit** only (boundary semantics); `Solid<T>::intersectWith()` reports every crossing of the solid (entry / exit, volume semantics). Both share `preparedRayBoxInterval()` in `detail/RaySlab.hpp` (a box query first prepares once with `PreparedRay::from(ray)`), writing no second slab.

### 2.3 The result is a `RayHit`, not an `optional`

- **What**: `RayHit<T>{ t, point, normal }`; a query returns `bool`, and on `false` `hit` is not written.
- **Why**: `G4` — picking needs exactly these three; "hit / miss" is a value, neither an exception nor an `optional` wrapper (every existing intersection entry in the repository is `bool` + output parameter).
- **Boundary**: `point == ray.pointAt(t)` is a contract invariant; `normal` always **faces the ray's origin** (dot product with `direction` ≤ 0), the same rule for box, OBB, plane and triangle — including a ray starting inside a box (it sees the back of the exit face).

### 2.4 Precision follows the type, and the camera path uses `Rayd`

- **What**: the `Ray<T>` template with the `Rayf` / `Rayd` aliases, consistent with `Aabb` / `Obb` / `LineCurve` / `Plane`; `Camera::screenToWorldRay()` returns a `Rayd`.
- **Why**: `G10` — recovering world coordinates from screen pixels in float loses precision; mesh vertices are still float, and the intersection evaluates per triangle in `Rayd`, so there is one code path only.
- **Boundary**: no implicit conversion from `Rayd` to `Rayf`; a float version is constructed explicitly.

### 2.5 Scene semantics stay in `graphics`

- **What**: `RayIntersection` keeps `intersect()` / `intersectScene()` / `intersectSceneAll()` and `RayIntersectionResult{ hit, point, normal, distance, geometry, primitiveIndex, instanceIndex }`, and internally uses `geom::spatial::RayHit<double>` plus its own identity fields.
- **Why**: `G2` — the node tree, world matrices, instance stream and visibility are graphics vocabulary; once the geometry side knew them, `brep` and robotics would depend on `viz` in reverse.
- **Boundary**: there is no `graphics::Ray` and no alias (the same position as `D2`, "no `math::Rect3` compatibility alias"); every call site is changed in one pass.

### 2.6 No new module, and not into `math`

- **What**: the ray and the four intersection entries all go into `geometry`.
- **Why**: `G8` — one value type plus four queries is not a layer; `math` holds numbers and transforms only, and putting a spatial entity there is exactly what the test in `geometry-primitives.md` 2.1 exists to prevent.
- **Boundary**: `math` provides only the points / vectors / transforms it already has; the ray introduces no new `math` type.

### 2.7 Ray–triangle intersection is defined in the header

- **What**: the members of `Ray<T>` and `intersectTriangle` are all defined in `Ray.hpp`, the class no longer has a separate implementation file, and the `VN_GEOMETRY_API` explicit instantiation goes with it.
- **Why**: `G7`, `G10` — `RayIntersection::traverseMesh` in `graphics` calls this per triangle, and a cross-DLL call cannot inline; measured 7.09 → 5.99 ns on hit and 3.96 → 2.71 ns on the parallel rejection.
- **The whole class moved, not one function**: putting only `intersectTriangle` in the header and leaving `pointAt()` in the `.cpp` drops the gain from 1.10 to 0.39 ns/tri — `hit.point = ray.pointAt(t)` inside the body would still be an external call.
- **Boundary**: the same rule as the other value types (`geometry-primitives.md` 2.8); the only call site is still `graphics`, and `C1`–`C3` and the signatures of §4 are unchanged.

## 3. Features

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Ray value type | `Ray<T>`: origin + unit direction; `pointAt()` / `distanceToPoint()` | landed |
| F2 | Ray–AABB | slab method, returning the first forward hit and the exit-face normal | landed |
| F3 | Ray–OBB | transform into local space and reuse AABB, with `t` still a world distance | landed |
| F4 | Ray–plane / triangle | plane: the `n·d` parallel test; triangle: Möller–Trumbore, two-sided | landed |
| F5 | graphics migration | `Camera` returns `geom::spatial::Rayd`; `RayIntersection` keeps only scene traversal and identity | landed |

### 3.1 F1 the ray value type

- **Behaviour**: the direction is normalised at construction; a zero-vector `direction` constructs legally but **every query returns false** (no NaN).
- **Constraint** (C1): `t` is a distance in model units; `pointAt(t) == origin + direction * t`.
- **Acceptance** (T1): `RayTest` covers normalisation, `pointAt`, the zero direction and `distanceToPoint`.

### 3.2 F2 ray–AABB

- **Behaviour**: the slab method; with the origin outside the box the entry-face normal is taken; with the origin inside the box `t = 0` and the normal is the face it sees (the back of the exit face, keeping the dot product with `direction` ≤ 0).
- **Constraint** (C2): `t >= 0`; `normal` faces the ray's origin.
- **Caveat** (N1): a degenerate box (zero size) still goes through the same formula, and the hit face may be any pair of parallel faces.
- **Acceptance** (T2): five cases — hit, miss, origin inside, parallel to a face, grazing the edge.

### 3.3 F3 ray–OBB

- **Behaviour**: transform the ray into local space with the `Obb` orientation and half extents, call the AABB query, then transform the normal back to world.
- **Constraint** (C3): the `Obb` transform is orthonormal, so `t` is unchanged by it and is directly the world distance.
- **Acceptance** (T3): a rotated box hits exactly as the `Aabb` version does (same shape, same ray).

### 3.4 F4 ray–plane / triangle

- **Behaviour**: the plane returns false when `|n·d| <= eps` (parallel); the triangle hits on both sides, with the normal adjusted by the winding and the ray direction.
- **Constraint** (C4): the hit result on collinear vertices / a degenerate triangle is up to the implementation, but may never return NaN.
- **Acceptance** (T4): a back-face hit, a parallel case, a degenerate triangle, and a hit point outside the triangle.

### 3.5 F5 the graphics migration

- **Behaviour**: there is no `graphics::Ray`; `Camera::screenToWorldRay()` returns `geom::spatial::Rayd`; `RayIntersection` assembles its existing result from a `RayHit`.
- **Requirement** (R4): `model_viewer`, `OrbitCameraManipulator` and `RayIntersectionTest` are changed together, and `graphics::Ray` no longer appears anywhere in the repository.
- **Acceptance** (T5): `RayIntersectionTest` is green and does not depend on `graphics::Ray`.

## 4. Public Interface

| Type / file | Content |
|---|---|
| `Ray.hpp` | `Ray<T>`, `Rayf` / `Rayd`; `pointAt()` / `distanceToPoint()` |
| `RayHit.hpp` | `RayHit<T>{ t, point, normal }` |
| `Aabb.hpp` / `Obb.hpp` / `Plane.hpp` | each one's `intersectWith(const Ray<T>&, RayHit<T>&)` |
| free function | `intersectTriangle(const Ray<T>&, a, b, c, RayHit<T>&)` |

The full signatures and comments are in the headers; this page records membership and the reasons only.

## 5. Constraints, Requirements and Invariants

- **C1**: `t` is a distance in model units; the direction is always a unit vector (except the zero direction, see C3).
- **C2**: only hits with `t >= 0` are accepted; `normal` faces the ray's origin.
- **C3**: a zero-direction ray produces no hit and no NaN.
- **C4**: degenerate input (collinear triangle, parallel plane) returns false and throws nothing.
- **R1**: an intersection is a pure function: it modifies neither object and allocates nothing.
- **R2**: `Obb` reuses the `Aabb` implementation and writes no second slab.
- **R3**: a query is as precise as the types being intersected: `Rayd` × `Aabbd` in double, `Rayf` × `Aabbf` in float, never mixed implicitly.
- **R4**: `graphics` defines no ray type of its own; it consumes geometry's only.
- **N1**: `point == ray.pointAt(t)` always holds; a caller need not recompute it.
- **N2**: an origin exactly on the surface counts as a hit (`t = 0`), and a picker that must distinguish "starting inside" reads `t` itself.
- **N3**: `eps` is only the tolerance for "direction counts as zero"; the slab's parallel test uses **exact zero**, or a near-axis ray is taken as parallel and misreported as a miss; the triangle's determinant tolerance is **relative** (divided by the two edge lengths), or a small triangle is never hit because its area is small.

## 6. Caveats and Traps

- **P1**: using a `Ray` as a `LineCurve` gives the wrong dimension — a `LineCurve`'s `t` depends on `|direction|`, a `Ray`'s `t` is a distance. How it fails: the hit distance differs from the displayed one by the direction length. How to detect: `pointAt(t)` does not match the expected point. How to prevent: take `t` through `intersectWith` only.
- **P2**: a pick whose origin is inside the box gets `t = 0` and the reverse normal of the exit face, looking like a hit "flush against the face". How to detect: `t` is 0. How to prevent: use `intersectSceneAll()` or test containment first when the inner nearest face is wanted.
- **P3**: grazing (a ray through an edge or a vertex) may hit two adjacent faces in floating point. How to detect: two distances are almost equal. How to prevent: do not rely on "the unique hit"; sort by distance and take the first.
- **P4**: multiplying float mesh vertices by a `Rayd` loses precision once. How to detect: the hit point on a distant object jitters. How to prevent: promote the triangle vertices to double first (the implementation rule of F5).

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Putting `Ray` into `math` | `math` holds numbers and transforms only; a spatial entity belongs to `geometry` (`geometry-primitives.md` 2.1, `G1`). |
| D2 | Leaving it in `graphics` with every other layer copying it | one concept with two or three implementations inevitably drifts (`G1`); `brep` depending on `viz` is a back edge (`G2`). |
| D3 | `Ray` reusing `LineCurve` (with a boolean or a direction-length convention) | two dimensions in one type make the meaning of `t` something to derive from the documentation (`G4`). |
| D4 | Writing intersection as a single overload set of the free function `intersect(ray, target)` | separated from the intersected type, `Obb` reusing `Aabb` and the plane special case would all be rewritten outside; the member form matches the existing `Plane::intersectWith` (`G6`). |
| D5 | Doing ray–mesh and BVH ahead of time | without performance evidence, an acceleration structure introduces new data organisation and invalidation rules (`G8`, `G10`). **Superseded**: provided once batch ray consumers appeared, see
  `ADR-2026-11-07-bvh-as-structure-plus-builder.md` and `geometry-bvh.md`. |
| D6 | Keeping a `graphics::Ray` compatibility alias | the same position as `geometry-primitives.md` D2; one type with two names inevitably drifts (`G4`). |

## 8. Tests and Acceptance

| # | Criterion | Case |
|---|---|---|
| T1 | normalisation, `pointAt`, a zero direction producing no NaN, `distanceToPoint` | `tests/test_geom_spatial/RayTest.cpp` |
| T2 | box: hit, miss, origin inside, parallel, grazing | `RayTest.cpp` |
| T3 | OBB: a rotated box and the equivalent AABB answer alike | `RayTest.cpp` |
| T4 | plane and triangle: back face, parallel, degenerate, hit point outside the triangle | `RayTest.cpp` |
| T5 | after the migration `RayIntersectionTest` is green and does not depend on `graphics::Ray` | `tests/test_graphics` (`HitsTriangle` / `MissesTriangle` are currently blocked by an existing crash, see backlog) |

## 9. Status and Evolution

### 9.1 Current state

Geometry holds `geom::spatial::Ray` / `RayHit`, the ray overloads of `Aabb` / `Obb` / `Plane` and `intersectTriangle`; `graphics` has no `Ray` of its own, `Camera::screenToWorldRay()` returns `geom::spatial::Rayd`, and `RayIntersection` keeps only scene traversal, world → local and picking identity.

### 9.2 Known gaps

- Ray–mesh traversal and an acceleration structure (BVH): membership, structure and build boundary are in `geometry-bvh.md`;
  the ray query entry is `Bvh::forEachRay()`, which reuses this page's `PreparedRay` and slab implementation.
- A few cases in `tests/test_graphics` that go through `geometryFromShape()` (`GeometryTest.MeshCounts`, `RayIntersectionTest.HitsTriangle` / `MissesTriangle`, `SceneTest.BoundingBoxAggregates`) crash on this machine and did so before this work too; recorded in `../backlog/graphics.md`.

### 9.3 Compatibility / migration

There is no `graphics::Ray` and no alias; the migration points were `Camera.cpp`, `RayIntersection.cpp`, `OrbitCameraManipulator.cpp`, `model_viewer` and `RayIntersectionTest`.

## 10. References

- Code: `src/base/geom/sdk/vine/geom/spatial/Ray.hpp`, `RayHit.hpp`, `Aabb.hpp`, `Obb.hpp`, `Plane.hpp`; `src/viz/graphics/sdk/vine/graphics/Camera.hpp`, `RayIntersection.hpp`.
- Design: `geometry-primitives.md` (the boundary test for spatial value types), `graphics-scene.md` (picking is a mechanism), `geometry-intersection.md` (the table of every pairing and result shape).
- Tests: `tests/test_geom_spatial/RayTest.cpp`, `tests/test_graphics` (camera and picking).
- External material: Möller–Trumbore ray–triangle intersection; the Kay–Kajiya slab method.
