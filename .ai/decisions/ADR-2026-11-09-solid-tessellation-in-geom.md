# ADR-2026-11-09: solid tessellation returns to geom, and the result is a vertex array rather than a mesh

> **Status**: Accepted
> **Date**: 2026-11-09
> **Scope**: `src/base/geom/sdk/vine/geom/spatial/SolidTessellation.hpp` (new), `src/base/geom/src/SolidTessellation.cpp` (new),
> `src/base/models/sdk/vine/models/SolidTessellation.hpp`, `src/viz/graphics/src/Geometry.cpp`,
> `tests/test_geom_spatial/SolidTessellationTest.cpp`

## Background

`tessellateSolid()` was born in `src/base/geometry/src/` (commit `79058532`). `9be3b6f4` ("modelling leaves geometry") moved it <!-- drift-ok -->
into `src/base/models` together with `TriangleMesh`, so 2.1 of `geometry-tessellation.md` still says "implemented in geometry's `src/`"
today, contradicting the actual path.

After the move, the function's **only** modelling dependency was four lines inside the private `MeshBuilder::finish()`: construct a `TriangleMesh`, set the three arrays into it,
call `computeAabb()`. The 250 lines before that touch only `SolidShapef`, the five kinds, `Vec3f` and `std::vector`. In other words, it lived in `models` because
its **return type** is a modelling type, not because the algorithm needs modelling.

At the same time, the only production consumer, `graphics::geometryFromShape()`, repacks the vertices into a `Geometry`'s `Buffer<float>` as soon as it has the mesh,
so the `TriangleMesh` in between is just a stopover on that path.

## Decision

1. Tessellation returns to `geom`: `vn::geom::spatial::tessellateSolid()`, in the same directory as the five kinds
   (`geom/spatial/SolidTessellation.hpp`), implemented in `src/base/geom/src/SolidTessellation.cpp`, one overload per kind.
2. The result is the geometry value `vn::geom::spatial::Triangulation<T>`: three public `std::vector` fields, `positions` / `normals` / `indices`,
   plus `vertexCount()` / `triangleCount()` / `empty()`, with the aliases `Triangulationf` / `Triangulationd`. The precision is the template parameter `T`,
   following the solid; both precisions are explicitly instantiated and exported in the same `.cpp`, the same rule as the other geometry value types. An unusable solid returns an empty value.
3. `TessellationQuality` moves up with it. The model side keeps only assembly:
   `vn::models::meshFromTriangulation(Triangulationf)` moves the three arrays into a `TriangleMesh` and computes the bounds, and an empty value returns an empty mesh.
4. The call chain becomes `meshFromTriangulation(tessellateSolid(shape.solid()))` (`shape` comes from `visitPrimitive()`), and the `graphics` side is semantically unchanged.

## Rationale

- **`G2`**: a geometry algorithm depends only on geometry and core (`Geom` links `Math` + `Core`, and `Buffer` / `intrusive_ptr` live in core). Writing
  `TriangleMesh` into the return type would create a back edge between `geom` and `models`, and `TriangleMesh` is a modelling type, so it cannot move down.
- **`G8` / `G10`**: why not return a `Buffer`. `Mesh::setPositions()` takes only a `std::vector`, so returning
  `intrusive_ptr<Buffer<T>>` would give the model side one more copy; and no second consumer needs the Buffer form today (the single `graphics`
  path repacks anyway). A value type lets the model side `std::move` with zero copies.
- **`G1` / `G13`**: the result value holds no identity, revision or face topology; it describes only "which triangles this run produced", apart from the solid's modelling identity.
- **`G6`**: `brep::tessellate()` still returns a `TriangleMesh` - it sits above `models` and may name a model mesh; the two tessellation results
  differing is a difference of layer, not two spellings of one thing.

## Rejected options

| Option | Rejection reason |
|---|---|
| Keep the status quo (`models::tessellateSolid` returning a `TriangleMesh`) | Documentation and code already disagree; and it makes a purely mathematical algorithm carry the whole modelling module for its return type. |
| Move to geom and still return a `TriangleMesh` | A `geom → models` back edge (`G2`). |
| Move to geom and return three `intrusive_ptr<Buffer<T>>` | `Mesh::setPositions()` on the model side takes only a `std::vector`, so this costs one more copy; and no consumer wants the Buffer form today (`G8`, `G10`). |
| Move to geom and use three `std::vector&` out-parameters | Both the name and the ownership would rest on convention, which fails `G4`; the representation of an empty result is unclear too. |

## Consequences

- Public signature change: `TessellationQuality` / `Triangulation<T>` move from `vn::models` to `vn::geom::spatial`;
  `models::tessellateSolid()` is replaced by `geom::spatial::tessellateSolid()` + `models::meshFromTriangulation()`.
  The only call sites in the repository were `graphics::geometryFromShape()` and one test file, both updated.
- The entry points are overloaded by kind and templated on precision: the removal of the variant argument is recorded in
  `ADR-2026-11-09-solid-closed-set-without-variant.md`.
- `geom` gains one `.cpp` (`vn_add_library` uses a glob, so `cmake -S . -B build` has to be re-run).
- The tests now assert on `Triangulation`'s three arrays, with a further case covering the model-side assembly.
- If the render path later wants to save the one copy in `packAttribute`, `geometryFromShape()` should take a `Triangulation` directly,
  not have the Buffer form pushed back into a geometry entry point.
