# Transparent sorting uses the world-matrix origin as the point that stands for the object, so large or offset geometry sorts wrongly

- **Status**: Fixed (2026-11-13). The order key is now `detail::SortKey` (`DrawListBake.hpp`): the representative point is the world box centre (the matrix origin when there is no box) and the depth is a quantised camera-space depth.
- **Found**: 2026-11-02
- **Module**: `src/viz/graphics`
- **Files**: `detail::sortCollected` / `detail::makeSortKey` (`src/viz/graphics/src/DrawListBake.{hpp,cpp}`)

## Symptom and impact

Opaque batches sort near to far and transparent batches far to near, and the order key was
`(model_matrix * (0,0,0) - eye).length2()` (`Scene.cpp:746`) - the position of the **node's local origin** in world space.

For meshes whose local vertices are far from the origin (CAD parts, models that bake world coordinates straight into the
vertices, importer data that keeps the source coordinates) that point may not lie on the object at all, or may lie on its
far side: two transparent objects then sort by the wrong near/far relation and blend wrongly (foreground covered by
background). Near-to-far in the opaque batch is only an early-Z efficiency question and does not change the picture;
transparent order is a correctness question.

Using the view distance instead of a camera-space depth is a secondary symptom of the same root cause: at the edge of the
field of view it compares differently from the rasterised depth order.

## Root cause

Sorting needs "where is this object", and the only thing at hand was a matrix; the implementation took the zero-cost answer,
the matrix origin, instead of the geometric extent it already knew (`subtreeBounds` / `localBounds` are computed on the
collection path and reused through `BoundsCache`).

## Suggested fix

1. Change the order key to the squared distance from the eye to the **world-space bounding box** representative point:
   prefer the centre of `BoundsCache::worldBound(node, node_world)` (this frame already computes it, so the traversal cost
   does not grow), and fall back to the matrix origin when it is unavailable.
2. Use a camera-space depth (the z after the `view` transform) instead of the euclidean distance, so the order agrees with
   the rasterised depth; the two disagree about "who is in front" at the edge of the field of view.
3. State and record that this is an **object-level approximation**: it does not settle self-intersection, non-convex
   shapes or several layers around one centre. Content that needs a stronger order uses `Node::setDrawOrder` (already
   supported), splits its geometry, or gets its own pass for independently sorted transparency (see backlog).
4. After changing the key, look at sort stability and cost together: compute the representative point inside the existing
   per-command loop, and do not introduce a new allocation per frame.

## Verification plan

- Unit test: two transparent quads whose geometry sits far from the origin (for example all vertices in `x∈[100,101]`),
  with the camera looking at a front/back relation along y; assert that the command order matches their real box distances
  (it fails today, because the order follows the origin).
- Unit test: two transparent objects with the same centre and different sizes, verifying that the order after taking the
  box centre is the expected one.
- Visual regression: a `PrimitiveDrawTest`-style pixel case with two overlapping transparent faces, where the blend result
  matches the far-to-near order.

## Design constraints

The finding touches **G4** ("what the order key is" is not stated at the interface, so a caller cannot predict it),
**G11** (the error only shows in pixels and is hard to trace back to the order key) and **G10** (a zero-cost
approximation replaced correctness and nobody wrote the cost down).

## Fix (2026-11-13, F11)

- **Representative point**: the world box centre (the row already carries `world_box`, computed once by the fold), and only
  a row without a box falls back to the command's own matrix origin.
- **Depth**: camera-space depth (the -z of `view * point`) instead of the euclidean distance, quantised by
  `SortKey::kDepthStep` (1/4096 world units), so the order agrees with the rasterised depth and is reproducible across
  machines.
- **Total order**: the key is drawOrder, then layer (the smallest layer number), then opaque/transparent, then the
  quantised depth, then the row/primitive index (`tie`). `tie` makes every permutation sort identically, so
  `sortCollected` uses `std::sort` and needs no stability (`R17` / `T9`).
- **Tests**: `SelectTest.TheSortKeyIsATotalOrder`, `SelectTest.TheKeyReadsDrawOrderThenLayerThenSegmentThenDepth`,
  `SelectTest.TheKeySortsByTheBoxCentreNotTheNodeOrigin` (the node origin is deliberately placed at the farthest point) and
  `SelectTest.ACommandWithNoBoxFallsBackToItsOrigin`. The pixel-level regression passes through the existing `test_vsg`.
- **The boundary is unchanged**: still an object-level approximation (see "Design constraints" and
  `graphics-scene.md` N10).
