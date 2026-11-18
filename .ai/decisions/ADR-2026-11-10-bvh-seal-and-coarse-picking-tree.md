# ADR-2026-11-10: the BVH can be sealed (result only), the picking tree gets coarse leaves of 64, and backend stream uploads copy nothing

> **Status**: Accepted
> **Date**: 2026-11-10
> **Scope**: `src/base/geom/sdk/vine/geom/spatial/Bvh.hpp`, `BvhBuilder.hpp`,
> `src/viz/graphics/sdk/vine/graphics/Geometry.hpp`, `src/viz/graphics/src/Geometry.cpp`,
> `src/plugins/gfx_backend_vsg/src/internal/StreamUploads.cpp` (and the header of the same name),
> `tests/test_geom_spatial/BvhQueryTest.cpp`, `tests/test_graphics/GraphicsTest.cpp`, `tests/test_vsg/StreamUploadsTest.cpp`,
> `.ai/design/geometry-bvh.md`, `.ai/design/graphics-geometry.md`, `.ai/design/vsg-backend.md`

## Background

When the model viewer opened `车身.stl` (704 MB, binary STL, 14,086,575 triangles, 42,259,725 corners), the process grew to several GB.
Costing it section by section (the table in 2.16 of `geometry-bvh.md` is the conclusion that measurement left behind):

| Resident item | Size | Note |
|---|---|---|
| The model (positions + normals + indices) | 1.18 GB | 42.26M × (12 + 12 + 4) B; a triangle soup shares no vertices, so the file's 50 B/triangle spreads out like this |
| The picking tree `Bvh<double>` | 1.36 GB | Primitive boxes 676 MB (14.09M × 48 B) + node layer 632 MB + order 56 MB |
| The backend's vertex/index copy | 1.18 GB | `StreamUploads` copied the floats the model already had into a `vsg::floatArray` again |

Three things were confirmed during that investigation, and they decide this ADR:

1. **Leaf granularity only affects the node count.** `BvhBuilder::leafSize` affects only the node array; `prim_bounds_` is `48 B × primitive count`, independent of granularity.
   Raising the leaf from 4 to 512 lowers the 1.36 GB ceiling only to 736 MB.
2. **The primitive boxes are not used on the picking path.** `RayIntersection`'s callback throws the second argument (the primitive box) away; `forEachRay` uses `node_bounds_` to cull
   only at the **leaf level**. Primitive boxes are build-time data (binned SAH needs them, leaf box merging needs them), not query results.
3. **`update()` / `refit()` have no production caller.** A rigid transform does not need them (the tree lives in local space, the transform is folded in at query time, and the key holds no
   world matrix); a vertex change goes through `Mesh` dropping and rebuilding the tree, and `Geometry::prepareBvh()` builds a new tree under a new key. They appear only in
   two cases of `BvhBuildTest`.

## Decision

1. **`Bvh` gains a sealed state**: `seal()` releases the per-primitive volumes (`vector().swap()`) and keeps `shape_` and `node_bounds_`;
   `isSealed()` is derived from the facts (built and the primitive boxes are empty), not a flag. After sealing, `add` / `addRange` / `update` / `refit` /
   `reserve` / `restoreTopology` / building throw `std::logic_error`; `primCount()` is answered from `shape_.order.size()`;
   `validate()` uses one primitive count (it only used the count anyway).
2. **The draw frame's picking tree defaults to leaf capacity 64** (`graphics::kPickTreeLeafSize`, expressed through `pickTreeBuilder()`), and
   `Geometry::prepareBvh(draw, builder)` opens the builder to the caller; `Geometry::prepareBvh()` seals as soon as it has built.
   `BvhBuilder`'s own default (4) does not move - that is the library's shape, not the engine's policy.
3. **The vsg backend's upload becomes an alias**: `StreamUploads::acquireVertex` / `acquireIndex` read the model's own bytes through `VsgBufferView` plus a real vsg array
   (`vsg::floatArray` / `vsg::uintArray`), and fall back to copying only when "there is no buffer handle" or "the span does not belong to that buffer".
   A vertex channel aliases a **segment**, an index channel aliases **the whole buffer**.

## Rationale

- `G10` (cost the real thing): primitive boxes are a build draft, yet they were resident as long-term state; and the only run-time consumer does not use them. Sealing separates
  "build cost" from "query cost": whoever wants mutability pays 48 B per primitive.
- `G8` (stay restrained): `update`/`refit` are kept but outside sealing - neither deleting a capability a real, future consumer (deforming meshes, shape-edit previews) will want,
  nor letting it keep charging read-only consumers.
- `G1` / `G7`: the only difference between the two states is "are the primitive boxes still there"; making it two types would duplicate the single nearest-first pruning traversal (`D20`).
- The cost of coarse leaves is quantifiable: the exact tests of one click = leaves visited × leaf capacity. For a picking tree over 14M triangles at leaf 64, one ray crosses
  a dozen to a few dozen leaves, and the exact tests are in the thousands at 20-50 ns each, that is sub-millisecond; what it buys is 16x fewer nodes and an order of magnitude less
  build time.
- Zero copy already has a precedent in this repository (`VsgBufferView.hpp` and the texcoord binding), and it solves the "the backend outlives the model" lifetime problem at the
  same time: the array holds the view and the view holds the buffer.

## Rejected options

| Option | Rejection reason |
|---|---|
| Only adjust leaf granularity | It only cuts the node layer: 1.36 GB → 736 MB, while the primitive boxes (676 MB) do not move at all. |
| Make sealing a separate type `SealedBvh` / `BvhView` | The two states differ by one storage only, and a separate type would duplicate the single nearest-first pruning traversal or make it a template; both are heavier than "no flag, state derived from the data" (`D20`, `G7`). |
| Delete `update` / `refit` / `qualityReport` (per `G8`) | It would push a real future consumer (a fixed primitive set with boxes that change each time) back to "rebuild everything"; sealing already stops them charging each other, so deleting brings no extra benefit. `F2` states that they have no production caller today. |
| Change `BvhBuilder`'s default leaf capacity to 64 | The library provides shape, not policy; existing tests and documents measure quality against "the library default". Policy goes in the consumer (`kPickTreeLeafSize`). |
| Change primitive boxes to `Aabb<float>` (24 B) | Lossless (the box values are float coordinates' min/max already), but it is the **next** lever and independent of sealing; doing sealing first lets the two be measured as two steps. Left in the `backlog`. |
| No picking tree (scan 14M triangles linearly) | A click costs 0.2-0.5 s, and the capability of "picking a large mesh" is lost as such; after sealing the tree is already an order of magnitude smaller than the model. |
| Build lazily (build the tree on the first click) | The build time at open turns from "one perceptible stall" into "a stall on the first click", and the memory gain has already been eaten by sealing (the node layer at leaf 64 is only a few tens of MB). |

## Consequences

- **Memory** (the picking tree over 14.09M triangles): `1.36 GB → 88 MB` (leaf 64 + sealing). The backend saves another 1.18 GB (vertices and indices are no longer copied).
- **Semantics**: a query callback on a sealed tree receives the **leaf box** rather than the primitive box (a superset, `R7` / `P10` of `geometry-bvh.md`). Picking never used that
  parameter, so behaviour is unchanged; any new consumer that needs an exact per-primitive box must not seal.
- **ABI**: `Bvh`'s class layout is unchanged (no new members) and everything added is a header-inline function; the only breaking surface is that six write interfaces on a sealed tree throw
  `logic_error` - and only tests call them today.
- **Tests**: five sealing cases in `BvhQueryTest`, two in `GraphicsTest` (sealing and coarse leaves observable, the caller's granularity replaceable),
  three in `StreamUploadsTest` (an array aliasing the model's address, the index aliasing a whole segment, the lifetime semantics of the entry and of the bind respectively).
  `AHandBuiltDrawPreparesItsOwnTree` and `APreparedTreeChangesTheCostAndNotTheAnswer` now pass a fine leaf explicitly - they measure the pruning mechanism, not the engine's granularity policy.
- **Documents**: `geometry-bvh.md` 2.16 / F10 / C13 / R7 / N3 / P10 / D20 / T12; `graphics-geometry.md` F33 / C12 / N18;
  `vsg-backend.md` F16.1. Also corrected `geometry-bvh.md` 2.4, which wrote `Aabb<double>` as 24 bytes (it is 48 bytes).

## Follow-up

- Change primitive / node boxes to `Aabb<float>`: halves the box memory again (lossless), and needs a statement of "storage scalar ≠ compute scalar" for `Bvh`'s volume type.
  See `../backlog/geometry.md`.
- Box quantisation (relative to the parent, 16 bit per axis) is a further step, to be taken only with sustained evidence from a large-model consumer.
