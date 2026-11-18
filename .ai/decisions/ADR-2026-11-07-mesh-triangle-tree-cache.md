# ADR-2026-11-07: a mesh carries its own triangle identity, and the triangle tree is a cache

> **Status**: Accepted
> **Date**: 2026-11-07
> **Scope**: `src/base/models/sdk/vine/models` (`Mesh.hpp`, `TriangleMesh.hpp`, `QuadMesh.hpp`, `MixedMesh.hpp`),
> `tests/test_geom_spatial/MeshBvhTest.cpp`, `.ai/design/graphics-geometry.md`, `.ai/design/geometry-bvh.md`

## Background

`geometry` can now build a static bounding-volume hierarchy (`Bvh` + `BvhBuilder`, see `ADR-2026-11-07-bvh-as-structure-plus-builder.md`).
To use it on a mesh, three questions have to be answered first:

1. **What is the tree's primitive?** The intersection function for triangles, `intersectTriangle`, is the unit of query, so the primitive must be a triangle.
2. **Who can state "which triangles does this mesh have"?** `Mesh` holds only vertex attributes and indices, and face topology is decided by the subclass (`TriangleMesh` three indices per face,
   `QuadMesh` four corners per face, `MixedMesh` splitting faces with offsets); before this only `TriangleMesh` had `triangleCount()`.
3. **Where does the tree live, and is it content or cache?** `Mesh` already had the precedent of `aabb_`.

## Decision

1. `Mesh` declares two virtual functions for triangle identity: `triangleCount()` (0 by default in the base class) and `triangleCorners(triangle, corners[3])`
   (false by default in the base class; returns three **vertex indices**). Each of the three concrete meshes implements them by the same rule as its own `toTriangleMesh()`
   (a triangle is three indices per face; a quad is split along the 1→3 corner diagonal into two; a polygon is fanned). `TriangleMesh::triangleCount()` becomes its override,
   with the signature unchanged.
2. `Mesh` caches one `Bvh<double>`: `bvh()` / `hasBvh()` / `setBvh()` / `buildBvh(builder)` / `clearBvh()`,
   the same shape as `aabb()` / `setAabb()` / `computeAabb()`. `buildBvh()` adds one primitive per triangle,
   so **primitive id == triangle index**.
3. Invalidation comes from the same rule as `aabb_` but covers a different set: `setPositions()` / `setIndices()` / `MixedMesh::setFaceOffsets()` drop the tree,
   while normals, texture coordinates and colours do not drop it; an unbuilt tree answers no candidates and does **not** degrade into a linear scan.
4. The tree is not built implicitly: `bvh()` does not build one because a query happened, and `buildBvh()` is an explicit one-shot action.

## Rationale

- **The primitive id must equal the triangle index**: that index is the identity of a picking result and is also what stays stable across `toTriangleMesh()`.
  If the two orders differed, a caller would need another mapping table (`G4`).
- **Triangle identity moves up to `Mesh` instead of staying in `TriangleMesh`**: `QuadMesh` / `MixedMesh` are consumers in their own right (the model viewer, meshio),
  and if the tree were built only on `TriangleMesh`, they would have to copy the whole mesh into a `TriangleMesh` before they could query (`G1`, `G10`).
- **Virtual functions rather than the caller walking the mesh**: a `MixedMesh` polygon has to be fanned and a `QuadMesh` split along a fixed diagonal - every consumer
  would rewrite that rule (`G7`). The two virtual functions put it on the type itself, and the base class has sensible defaults (a mesh that promises no faces answers 0).
- **`double` rather than `float`**: the tree is not geometry data but a guide structure for queries (`G13`). Query rays are `Rayd` in every consumer's hands;
  narrowing a ray to float for a box test loses the precision a small leaf box needs far from the origin and **misses** hits. Widening float vertices into a double
  box, on the other hand, is exact, so the cost is node memory, not precision.
- **It is a cache, not content**: it holds no identity, can be recomputed from the triangles, and can be dropped by any setter that changes a triangle - the same judgement
  as `aabb_` (`G1`, `G13`). That also answers "why not put it on `Scene`": the scene cares about nodes and instances, and a mesh itself can state which triangles it is made of.

## Rejected options

| Option | Rejection reason |
|---|---|
| Cache the tree only on `TriangleMesh` | The other two meshes would have to copy themselves through `toTriangleMesh()` before querying, splitting triangle identity into two (`G1`, `G10`). |
| The tree uses `float` (the vertices' precision) | A narrowed query ray misses hits far from the origin; widening the vertices is exact, and only node memory is at stake (`G4`, `G10`). |
| Make triangle identity a materialised `std::vector<std::array<uint32_t,3>>` table | Building the tree walks the mesh anyway, so materialising only adds a copy that must be invalidated in step with the mesh (`G10`, `G13`). |
| The tree is allocated lazily with `std::unique_ptr` | `bvh()` would then have to return a pointer or a nullable reference and every call site would check for null; an empty `Bvh` is just an empty vector (`G4`, `G7`). |
| Build the tree automatically on the first query | A query is `const` and required to be concurrent; an implicit build would make the first query allocate and would block other queries while building (`G10`, `G11`). |
| Build the tree on `Scene` / the render side | Every instance reuses one copy of the mesh data, and the tree is a property of **local space**; putting it on the scene side would duplicate it per instance (`G1`, `G5`). |

## Consequences

- `Mesh` gains two virtual functions (the vtable slot of `triangleCount()` moves up from `TriangleMesh`) and one empty `Bvh` member (three empty vectors).
  The vtable change is invisible outside: the `Mesh` family has always been the run-time polymorphism boundary.
- `TriangleMesh::triangleCount()` keeps its meaning; it only changes from "defined by this type alone" to "an override of the base-class contract".
- `QuadMesh` / `MixedMesh` gain `triangleCount()` / `triangleCorners()`, so they can be queried without copying the mesh.
- `MixedMesh::triangleCorners()` finds the face a triangle belongs to by binary search (the face's first triangle index, `offsets[f] - 2f`, is monotonic), with no per-triangle table.
- The picking side is not wired up yet: `graphics::RayIntersection` still scans triangles linearly, recorded as T24 of `../backlog/graphics.md`.
- The current design is recorded in 2.8 / 2.9 / F32 of `.ai/design/graphics-geometry.md`, and the tree itself in `.ai/design/geometry-bvh.md`.
