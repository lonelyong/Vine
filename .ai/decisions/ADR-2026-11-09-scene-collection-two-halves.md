# ADR-2026-11-09: scene collection splits into a bake half and a selection half

- Status: Accepted (the first half landed; the second half waits for measurement)
- Date: 2026-11-09
- Related: `../design/graphics-scene.md`, `../backlog/graphics.md` T24, `../../src/viz/graphics/src/Scene.cpp`

## Background

`Scene::collectRenderCommands` answered two things with one walk: the **graph** (who is drawn, the world matrices, the folding of opacity/layers/drawOrder/state)
and the **view** (frustum culling, sorting). The memo was keyed on the view, so as soon as the camera moved the graph half was dropped and recomputed too - measured (Debug,
`SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera`) at **13797 µs/frame** for 2000 drawables with a moving camera, against 1 µs for a memo hit.
Orbit and zoom are the viewer's everyday interaction, so this is not a "optimise later".

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A Keep the status quo | No change | Every view pays one full recomputation |
| B Re-walk dirty subtrees only | Sounds cheapest | The result has no subtree locality (global sorting, opacity grouping, distance keys, folds across subtrees) - `G1`: the cache granularity must match the result's dependencies; rejected |
| C **Bake (graph half) + select (view half)** | Cut along the dependency boundary; reuse the existing `contentEpoch` announcement; the public API and the counter meanings stay | One more O(nodes) side table |
| D Hand objects a global stable id | Solves it together with the "keyed by address" family | Unrelated to this problem (the `PipelineKey`/`StreamKey` keys are not this bottleneck), and already rejected in ADR-2026-11-09-address-keys-hold-the-object |

## Decision

Chose **C**, inside `Scene` (the public surface gains only one diagnostic counter):

- **Bake** (key = `root_->contentEpoch()`): store, in traversal order, each node's **world matrix** and the three folds (`opacity` / `layers` /
  `draw_order`), and for a leaf also the resolved state/material/program plus its primitive span. The array index is the identity (both walks are driven by
  the same code), and `end` lets "cut away a whole subtree" still skip in one step.
- **Select** (key = the view): iterate the side table, prune on the layer union, run the frustum test, push commands, sort. The counter `contentCollectCount`
  keeps its meaning ("this view collected once"), so every existing test's numbers still hold.
- **View-dependent subtrees are not baked**: `Node::viewDependence()` is new (`AutoTransform` declares the widest tier its own switch allows), and
  such a run is handed to the original walk, `collectNodeCommands`, during selection - so AutoTransform's positioning logic is not rewritten.

## Lessons measured (written down so they are not repeated)

1. **World boxes cannot be baked.** The first version stored each node's world box in the side table too, and two cost-witness cases went red at once:
   `CollectCommandsAsksEachLeafBoundOnce`, `TheLocalDataBoxIsComputedOnceAndRecomputedWhenTheDataChanges`. The reason is not that
   the cases are stale but that **a box is keyed on a stream's revision, not on the node's epoch**: `Buffer::edit()` does not disturb the graph, so a baked box would
   be used until some unrelated node change. The correction: selection still asks `BoundsCache` for the box (one transform per node, and the local box is already cached
   on the node per stream revision). **Only facts decided by the graph alone can be baked.**
2. **The gain is smaller than expected**: in Debug, 2000 drawables with a moving camera go **13797 → 11113 µs (−19%)**, and 200 drawables 1267 → 1040 µs
   (−18%). So the bulk of the 13.8 ms is not the matrix products and folds but the per-view `BoundsCache` hash table and the construction of each command
   (3 `intrusive_ptr` copies + a 128 B matrix + `push_back`) and the sorting. `G10`: a better structure must not be sold as a solved performance problem.
3. **Re-measured in Release (2026-11-10, decisive)**: the same recipe in Release gives **2000 drawables 1597 µs** (0.94 µs/command),
   200 drawables **141 µs**, and a steady-state frame (memo hit) **0 µs**. That is: **the 13.8 ms is what Debug amplifies 8.6 times, and the real interaction cost is 1.6 ms/frame**.
   Attribution (Release, 2000): box transform 204 µs + hash round trip 113 µs + command copy / sort / sort keys 226 µs = **543 µs (29%)** explained,
   and **~1339 µs (71%)** of per-command bookkeeping inside the select body - the proportions match Debug, so "the bulk is the per-command bookkeeping" is not a Debug artefact.

## Consequences

- Good: the view no longer does matrix products and folds; two passes that differ only in `layer_mask` also share one bake (inside one frame, shadow and main
  used to walk the whole tree each).
- Good: `viewDependence()` makes "does the position depend on the camera" a **declarable** property, replacing the "call it once and see" protocol (`G4`).
- Cost: an O(nodes) side table (about 0.5 MB for 2000 nodes, rebuilt only when the graph changes; a leaf's state/material/program sit in the same record, and
  a scene with >1e5 nodes would be split into SoA then).
- Open item (the next step of T24): replace the per-view `BoundsCache` hash table - **bake the world box plus a "box stamp"** (with the geometry's
  data revision in the stamp), which is cheaper than the hash table and can still be invalidated after a buffer edit. **That step is not done now that the Release numbers are in**: what it
  saves is 204 + 113 µs (1.6 ms out of the 2 tenths), and it costs one more "buffer edit → scene" announcement; the trigger becomes "in Release, 10000 drawables with a moving camera
  cost more than 4 ms" or "collection costs more than 2 ms of a 60 fps budget", written into backlog T24.

## Evidence

- The whole of `test_graphics`: **496 cases all green** (including 44 `SceneTest` cases: order, culling, opacity, layers, state folding, instancing,
  AutoTransform, memo semantics, two cost witnesses).
- Two repeatable recipes: `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` (moving / still / steady-state frames) and
  `SceneTest.MeasureWhereACollectionSpendsItsTime` (cost attribution by ingredient, including direct timings of three primitives), run in both Debug and Release.
- `Scene::contentBakeCount()`: a new counter pinning the steady state "one bake per graph, one collection per view".
