# ADR-2026-11-07: the BVH splits into a structure plus a builder, and `Obb` volumes are rejected after measurement

> **Status**: Accepted
> **Date**: 2026-11-07
> **Scope**: `src/base/geom/sdk/vine/geom/spatial` (new `Bvh.hpp`, `BvhBuilder.hpp`, `detail/`),
> `src/base/geom/src/Instantiations.cpp`, `scripts/check_geom_layering.py`, `tests/test_geom_spatial`,
> `.ai/design/geometry-bvh.md`

## Background

`D5` of `geometry-ray.md` had listed "ray-mesh and the BVH" as not done: at the time there was no performance evidence, and there was a worry about introducing a second data organisation.
The consumers have appeared since (picking, and batch rays for LiDAR simulation), and a reusable acceleration structure is needed. Three things were settled during the design:

1. The structure goes into `geometry` (the same layer as `Aabb` / `Ray`), not into `graphics` or `brep` - both sides need it, and leaving it on either side is a back edge (`G2`);
2. Structure and build algorithm must be separated: adding one heuristic must not edit the core type (`G9`);
3. The plan was to support both `Aabb` and `Obb` volumes (a `VolumeTraits` adaptation).

## Decision

1. `Bvh<T, Volume = Aabb>` is the **structure**: primitive bounds, a flat node array, primitive order, two traversals, `refit`, `validate`, quality statistics.
   It does not know what a primitive is, nor how the tree was built.
2. `BvhBuilder` is the **builder**: chained `leafSize` / `maxDepth` / `quality` configuration plus `build()` / `buildWith()`.
   A little state, no inheritance, no virtual functions.
3. Between them there is exactly one contract, `detail::BuildJob<T, Volume>` (can read a box, can reorder, can append a node).
   `Bvh.hpp` must not include `BvhBuilder.hpp`, enforced by rule 6 of `check_geom_layering.py`.
4. `VolumeTraits<V>` provides the nine operations the tree needs; this round implements only `VolumeTraits<Aabb<T>>`.
   **`Obb` volumes are not delivered**, for the reason below.
5. `D5` of `geometry-ray.md` is recorded as superseded by this ADR: the condition (a batch-ray consumer appears) has been met and the acceleration structure landed.

## Rationale

- The structure / builder separation has an executable criterion: with `BvhBuilder.hpp` and `BvhBuilders.hpp` deleted, `Bvh.hpp` must still compile on its own.
  That criterion can be written as a gate, whereas "clear responsibility" cannot (`G1`, `G2`, `G12`).
- Building is occasional, single-threaded and may be slow; querying is frequent, may be concurrent and must not allocate. Putting both rhythms into one type forces the query path
  to carry build state (`G5`, `G10`).
- Adapt through traits instead of changing `Aabb` / `Obb`: the tree's requirement list is written on the tree's side, and an existing type does not grow members for "might be used by the tree" (`G13`).
- Node layout: `parent + 1` as the left child, `offset` holding the right child: 32 bytes per node (`float` + `Aabb`), one index fewer for an interior node (`G10`).
- Querying uses a callback instead of returning a container: a nearest hit needs one result, and allocating an array is pure loss (`G7`).

## Why `Obb` volumes were rejected

`VolumeTraits<Obb<T>>` was implemented as a conservative merge that fits one PCA box over the sixteen corners of the two boxes, and tests were written; two things were measured:

1. **Worse quality**: in a scene of long thin parts, the sum of leaf surface areas of the `Obb` tree is roughly **2x** that of the `Aabb` tree (22977 vs 11293, over the same set of 200 long thin boxes). The reason is that one fitted orientation cannot explain the orientation of both children, so every merge inflates the box.
2. **Containment does not hold strictly**: a corner of the fitted box lands one rounding step outside the box just fitted, so I1 of `validate()` fails and a tolerance has to be added.

A volume that is "slower, looser and still needs a tolerance" is not worth delivering. What is needed is a merge that **keeps both children's orientations** (a two-orientation node, say, or a multi-box representation of the union), which is another design and outside this round (`G8`). The template parameter of `VolumeTraits` stays,
and the `Obb` adapter, together with its rejection reason, is recorded as `D2` of `geometry-bvh.md`.

## Rejected options

| Option | Rejection reason |
|---|---|
| Make the build algorithm a `switch` inside `Bvh::build(BvhBuildQuality)` | Adding an algorithm edits the core type; a foreign algorithm cannot be injected; `Bvh.hpp` is forced to carry every build kernel (`G9`, `G5`). |
| Polymorphic builder (`IBvhBuilder` + virtual functions) | Building is an occasional one-shot action; swapping the algorithm at run time has no consumer (`G8`). |
| `buildWith` takes a `std::function` | One type erasure per build; a template parameter costs nothing (`G10`). |
| Pre-allocate a node window of `2·⌈n/leaf⌉+1` | The leaf count depends on how the split falls (5 primitives become 2+3), so that bound is out of range. Changed to `appendNode()` growth plus keeping the capacity after the build. |
| The query returns `std::vector<PrimId>` | A single-result query would allocate too; a callback can stop early (`G7`). |
| Build with an explicit stack on the heap | `max_depth = 64` already gives a recursion-depth bound; an explicit stack only trades that bound for one heap allocation (`G10`). |
| Support dynamic insert / remove | The consumer is "rebuild after the model changed"; a dynamically balanced tree has no matching benefit (`G8`); `update` + `refit` cover movement. |

## Consequences

- `geometry` gains two public headers (`spatial/Bvh.hpp`, `spatial/BvhBuilder.hpp`) and four `detail` headers; purely additive, no compatibility impact.
- `Instantiations.cpp` gains two explicit instantiations, `Bvh<float>` / `Bvh<double>`.
- `check_geom_layering.py` gains rule 6: the structure must not depend on the builder.
- The implementation of `Aabb::rayBoxInterval` moves into `detail/RaySlab.hpp` and `Aabb` delegates to it - the slab exists once (`geometry-ray.md` R2).
- `D5` and the §9.2 gap item of `geometry-ray.md` now point at `geometry-bvh.md`.
- The current design is recorded in `.ai/design/geometry-bvh.md`.
