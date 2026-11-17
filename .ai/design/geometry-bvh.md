# geometry bounding volume hierarchy (BVH)

> **Status**: current (the structure and the builder are landed, and `Bvh<T, Aabb<T>>` is exported in both precisions; the tree has a "mutable / sealed" pair of states, see 2.16; an `Obb` volume was rejected by measurement, see `D2` / §9.2)
> **Last verified**: 2026-11-10
> **Scope**: `Bvh.hpp`, `BvhBuilder.hpp`, `BvhTopology.hpp` and `TriangleBvh.hpp` of `src/base/geom/sdk/vine/geom/spatial`,
> and the build kernel under `spatial/detail/`; it records the ownership of a static bounding volume hierarchy, the structure / build boundary, the query contract, the staleness rule, the topology storage
> and the numbering rule from "a set of triangles" to "a tree".
> Not scene traversal and picking identity (see `graphics-scene.md`), not ray–primitive intersection itself (see `geometry-ray.md`).
> **Sources**: `Bvh.hpp`, `BvhBuilder.hpp`, `BvhTopology.hpp`, `TriangleBvh.hpp`, `detail/VolumeTraits.hpp`,
> `detail/BvhBuildJob.hpp`, `detail/BvhBuilders.hpp`, `tests/test_geom_spatial/Bvh*Test.cpp`, `TriangleBvhTest.cpp`.
> **Backlog**: `../backlog/geometry.md` (open items such as dynamic insertion and removal, and parallel building)

## 1. Overview

### 1.1 Problem and background

Ray–primitive intersection landed in `geometry-ray.md` already, but one query intersects every primitive one by one: past a few thousand primitives, each frame's picking and
LiDAR simulation degrade into a full scan. An acceleration structure is needed, and it has to be reusable by **any** consumer that "has a bounding volume and can intersect",
rather than growing inside the rendering side's scene graph.

The problem is not only "write a BVH" but four questions:

- **Where the structure lives**: both `brep` (`base`) and `graphics` (`viz`) need it; putting it on either side is a back edge (`G2`).
- **How to add build strategies**: median split, SAH, and possibly thousands of heuristics later; written into the tree, each new one would change the core type (`G9`).
- **How to support different volumes**: `Aabb` and `Obb` merge, intersect and ray-test differently, but that must not make the tree know the concrete type.
- **How to reuse buffers**: one build costs several MB, and a rebuild (after a model change) must not allocate from scratch every time (`G10`).

### 1.2 Goals

- One **structure**: it holds the primitives' bounding volumes, one flat node tree and one query traversal; it does not know what a primitive is, nor how the tree was built.
- One **builder**: it builds the structure with composable algorithms, configurable in quality and leaf size, and **touches** neither the queries nor the staleness rule.
- The volume type is replaceable: the tree obtains the nine operations it needs only through `detail::VolumeTraits<V>`.
- Zero temporary allocation growth during a build: the node array and the primitive-order array reuse their capacity, so a rebuild writes without allocating.

### 1.3 Non-goals

- Not doing dynamic insertion / removal: the tree is **static**, and once built it only allows `update()` to change a bounding volume plus `refit()` to tighten, or an explicit rebuild (`D3`).
- Not doing scene-graph traversal, instancing, world matrices or picking identity: that is `graphics-*`'s business.
- Not doing a GPU-side structure (LBVH, ray packets).
- Not offering "pick the algorithm automatically": there are only two quality levels, and extra heuristics are injected by the caller through `buildWith()` (`G8`).

### 1.4 One-paragraph conclusion

**`Bvh` is a static bounding volume hierarchy: it holds the primitives' bounding volumes, maintains three invariants, and offers two traversals, overlap and ray;
building is the separate type `BvhBuilder`, which writes the tree through the single interface `detail::BuildJob`; the volume type is adapted by `VolumeTraits`;
once built the tree can `seal()` - release the build-time primitive boxes and keep only the nodes, the node boxes and the primitive order.**

## 2. Design Points

### 2.1 Two types, one directed edge

`Bvh<T, Volume>` (`spatial/Bvh.hpp`) is the **structure**: the primitive bounding-volume array, the primitive order, the node array, the quality statistics, the query traversals, `refit`
and `validate`. Its only exposed build entry is the private `friend` interface for `BvhBuilder`.

`BvhBuilder` (`spatial/BvhBuilder.hpp`) is the **builder**: three chainable settings, `leafSize` / `maxDepth` / `quality`,
`build(bvh)` dispatching by quality level, and `buildWith(bvh, algorithm)` accepting any external algorithm.

There is one edge only: `BvhBuilder.hpp` → `Bvh.hpp`. The reverse dependency is rejected by rule 6 of `scripts/check_geom_layering.py`
(`spatial/Bvh.hpp` must not include `spatial/BvhBuilder.hpp`). The reasons (`G1`, `G2`, `G12`):

- the questions the tree can answer are only "which primitives are there, where are their boxes, how do I traverse them", and they have nothing to do with "who arranged them this way";
- conversely, once the tree knows the builder, adding an algorithm changes the tree, and the tree's two users are forced to recompile together;
- the criterion is **executable**: delete `BvhBuilders.hpp` and `BvhBuilder.hpp`, and `Bvh.hpp` must still compile on its own.

### 2.2 The only interface a build algorithm sees: `BuildJob`

`detail::BuildJob<T, Volume>` is the **only** contract between the two headers:

| Member | Meaning |
|---|---|
| `prim_bounds` | the read-only view of the primitive bounding volumes |
| `order` | the writable primitive order (an algorithm only does `swap` / partition / `nth_element`) |
| `nodes` | a reference to the node **topology** array (`BvhNodeTopology`); an algorithm appends with `appendNode()` |
| `node_bounds` | the node volume array **parallel** to `nodes`, grown together by `appendNode()` |
| `leaf_size` / `max_depth` / `depth` | the size parameters of this build |
| `rangeBounds` / `writeLeaf` / `appendNode` | three primitives: the merged bounding box of a group, write a leaf, append an internal node |

The two node arrays are appended together by `appendNode()`, so "the node count equals the node box count" is guaranteed by the interface's shape rather than by a convention (`C12`).

An algorithm gets only "read boxes, reorder, append nodes", so it can break no invariant and touch no query state.
`nodes` is a `std::vector<Node>&` rather than a pre-allocated window: the leaf count depends on the split, and pre-computing the capacity as `2·⌈n/leaf⌉+1` would overflow when the leaf size is 5 and the split comes out 2+3; `prepareForBuild()` empties the arrays but keeps their capacity, and appending grows them, so a rebuild allocates nothing.

### 2.3 The `Volume` contract: adapted by traits, without changing existing types

`detail::VolumeTraits<V>` declares the nine operations the tree needs: `merged`, `center`, `halfArea`, `overlaps`, `contains`,
`corners`, `isFinite`, `hitByRay`, plus the type aliases `Scalar` / `VolumeType`. The `Aabb<T>` adaptation is exact:
`merged` goes through `expandBy`, `overlaps` is six comparisons, and `hitByRay` reuses `preparedRayBoxInterval` from `detail/RaySlab.hpp`
(the box queries share that one implementation, see `geometry-ray.md`).

`merged` may be **conservative** (containing both is enough; it need not equal the union) but must not **fail to contain**: containment is the tree's only correctness premise, and `validate()` checks it.
`halfArea` rather than volume: SAH charges by surface area, and a box that degenerates into a rectangle has zero volume but a meaningful surface area.

### 2.4 Node layout and traversal order

A node's **topology** is the two fields of `BvhNodeTopology`, 8 bytes; its bounding volume lives in the **parallel** `node_bounds_` array (48 bytes under `Aabb<double>` - six `double`s; `Aabb<float>` is 24):

- `offset`: for an internal node, the **index of the right child**; for a leaf, the **starting slot in the primitive order array**;
- `count`: for a leaf, the number of primitives in the group; for an internal node, 0;
- `node_bounds_[index]`: the merged bounding volume of that subtree (for a leaf, of that group of primitives) - the other array at the same position in the same tree.

The two arrays are separate rather than one AoS struct, for **storage** reasons: the topology array is at the same time `BvhTopology::nodes`, so
`topologyView()` needs no copy and `restoreTopology()` can move the whole array in (2.13, 2.15). The cost is one more load per node
during traversal (`node_bounds_[i]` and `nodes[i]`); and `refit()` touches the volume array only, so it is more concentrated than AoS.

**The left child is always `parent + 1`**, and the right child is given by `offset`. That rule saves one index per internal node (4 bytes per node), at the
cost of writing `nodes_[index + 1]` rather than `nodes_[node.offset - 1]` during traversal - the latter is a bug that was really hit (`P1`).

### 2.5 The three invariants

- **I1 containment**: any node's `node_bounds_[i]` contains all corner points of its children (for a leaf, the eight corners of every primitive bounding volume in the group).
- **I2 permutation**: `shape_.order` is a permutation of `[0, primCount)`, and the leaves' slot ranges are disjoint and cover the whole range.
- **I3 node upper bound**: `shape_.nodes.size() <= 2 · primCount`.

All three are checked exhaustively by `validate()` (`O(n)`, one `std::vector<bool>` allocation, called only from tests and assertions, never on the query path).
When an invariant is broken `validate()` returns `false` rather than throwing: queries are part of the `noexcept` logic, and validation is an optional development-time tool.

### 2.6 Building: two quality levels

- `BvhBuildQuality::kSah` (the default): a 12-bucket binned SAH. A two-way sweep over three axes × 12 buckets computes the cost, takes the best split and runs `std::partition`.
  When the split degenerates (everything on one side, or a non-finite cost) it falls back to a median split, guaranteeing that **any input can be built**.
- `BvhBuildQuality::kFast`: split at the centre median along the longest axis (`nth_element`).

Both are driven by the single recursive template `detail::buildTopDown<T, Volume, Split>`, differing only in "which split point is chosen";
the recursion writes a leaf when `count <= leaf_size` or `depth + 1 >= max_depth`.

The recursion depth is bounded by `max_depth` (`kBvhMaxDepth = 64`), so building needs no explicit stack; a recursion stack of `83 KB` is safe under that bound,
while an explicit `83 KB` stack would need a heap allocation, which would introduce exactly the cost `G10` is about.

### 2.7 `refit` belongs to the structure; the caller judges the quality

When a primitive moves, `update(id, bounds)` only writes `prim_bounds_[id]` and marks the tree as "needs refit"; `refit()` makes one reverse scan
(a child is always after its parent, so a single `for (index = size; index-- > 0;)` is a legal post-order) and recomputes every node box.
It does not change the topology, so repeated update + refit loosens the leaves; `qualityReport()` gives the sum of the leaf surface areas and the depth, and the caller decides when to rebuild.

### 2.8 Ray query: one preprocessing pass, nearest first

`forEachRay(ray, visit, max_t)` first builds a `detail::PreparedRay` (reciprocals and sign bits, so node tests need no division),
then goes depth first, **pushing the near child first**, and prunes branches whose entry distance already exceeds the current `limit`.

`visit(id, bounds, max_t&)` may write back a smaller `max_t` - that is how a nearest-hit query prunes: after a primitive hit the distance is written back, and
later nodes are pruned automatically. The callback returning `false` terminates the traversal immediately; the return value takes no part in the semantics.

### 2.9 The choice of query interface: a callback, not a returned container

`forEachOverlap` / `forEachRay` are both a callback + `const` + a `std::array<PrimId, kMaxDepth + 2>` stack on the call stack,
with **zero allocation**. Returning a `std::vector<PrimId>` would make a single-result query such as "nearest hit" allocate an array too,
while "collect all candidates" is ten lines in a callback (`G7`: the complexity is absorbed in the traversal, not pushed onto the caller to manage a container).

### 2.10 Buffer reuse

`prepareForBuild()` empties the node array and the order array but **keeps their capacity**; `reserve(prim_count)` lets the caller reserve by primitive count.
A rebuild after a build (a model change of the same size) produces no new allocation; `clear()` likewise keeps the capacity.

### 2.11 Threads and re-entrancy

The structure is not thread-safe: `add` / `update` / `build` all write state. **Concurrent queries after the build are safe** (queries are all `const` and allocate nothing),
and that is a deliberate design point: building is occasional and single-threaded; querying is frequent and may be multi-threaded.

### 2.12 Failure contract

- `add()` returns `kInvalidPrim` when the array is full (the `PrimId` limit) and does not throw;
- querying an empty structure returns immediately (`shape_.nodes.empty()` returns early) instead of crashing;
- illegal parameters (`leaf_size == 0`, `max_depth == 0`) are rejected by `prepareForBuild()` with `false`, so `BvhBuilder::build` does nothing;
- a build does not crash on degenerate input (all coincident, NaN): a degeneracy falls back to a median split, and NaN stays observable through `isFinite`.

### 2.13 The topology can be stored and restored: boxes are not

Only half of the tree is expensive to recompute after a build: the **grouping and the order**. Every node's bounding box is a function of the primitive positions, so the storage carries one value only,
`BvhTopology{nodes, order, depth}` (one `BvhNodeTopology{offset, count}` per node), and restoring runs one reverse scan of `detail::refitNodes()`
that recomputes every box from the current primitives. That decides four things (`G13`, `G10`, `G1`):

- there is no floating point in the file, and no inconsistency of the "boxes in the file and vertices in the file evolve separately" kind that only a tolerance can hide;
- a stale topology, or one from another model, can only be a **worse tree**, never a wrong answer, so no content fingerprint is needed;
- restoring is not `Bvh::load()` but **two entries, one check**: when all you hold is a view, go through another algorithm of `BvhBuilder::buildWith()`
  (`RestoreTopology`), which shares the `finishBuild` tail with median / SAH; when you own that value, go through
  `Bvh::restoreTopology(BvhTopology)`, which moves the arrays in. Both read `detail::installableHeight()`,
  so "what shape fits in this tree" has exactly one definition;
- **the shape is one value**: a node's `offset` points into `order`, and `depth` states the actual height of this set of nodes; apart from each other the three mean nothing,
  so they go into one type rather than three parallel arrays (`G1`: one concept, one definition; `G4`: the caller cannot pair one tree's nodes with another tree's order).

`detail::refitNodes()` is therefore the only implementation of "recompute the father from its children, a leaf from its primitives", and both `refit()` and the two restore paths call it.

**The shape is the tree's storage, not a transcription of it.** The tree's node topology is stored directly in the shape of `BvhTopology` (`shape_.nodes` /
`shape_.order` / `shape_.depth`), and the volumes live in the parallel `node_bounds_`. Three things follow (`G10`, `G1`):

- `topologyView()` is zero-copy; writing `.vbtp` encodes the tree's own arrays directly;
- restoring can **move** rather than copy node by node: the decoded `BvhTopology` is simply handed to `restoreTopology()`;
- in the older implementation `BvhNode` and `BvhNodeTopology` were two transcriptions of one convention, and one `topology()` over 38M triangles copied
  about `1 GB` (`≤2n` nodes × 8 B + 113M `order` × 4 B); that copy no longer exists.

Cost and boundary: the convention exists once, as `BvhNodeTopology`, and both the tree and the file store it; the node volumes do not go into the file (the first reason in 2.13),
so what the tree holds and what the file holds differ by one parallel array, not by one struct.

### 2.14 A triangle tree has one numbering rule: `TriangleSource` + `registerTriangles()`

`Bvh` does not know triangles: it receives bounding volumes. So the rule "one primitive per triangle, primitive id == triangle index" has to live
outside `Bvh`, on the **common layer** below `Mesh` and `Geometry`, and the inputs that rule needs are only three things - "which vertices are there, how many triangles are there,
who are the three corners of triangle `t`". The three form one concept (`G1`):

- `TriangleSource` (`spatial/TriangleBvh.hpp`) is the C++20 `concept` of that concept: `positions()`, `triangleCount()`,
  `triangleCorners()`. It is written with Qt-style accessors, like the library's other value types.
- `registerTriangles(tree, source)` is the **only** numbering and box rule: one primitive per triangle, order equals index, and the box is the `double` bounding box of the triangle's
  corners (the widening is exact, so leaves stay accurate when they are small and far from the origin). It returns `false` when a corner cannot be resolved or a coordinate is not finite -
  "no tree" is a return value, not an exception flying out of the picking stack. A caller may still decline to register (a mesh whose `isValid()` fails, for example),
  but that loop exists once.
- `buildTriangleBvh()` / `restoreTriangleBvh()` combine "register, then build" and "register, then install the stored shape" into one call,
  so the model frame and the draw frame each write one line instead of each writing a loop.
- Both sides pin it down with `static_assert(TriangleSource<T>)`: `Mesh` and `TriangleStream` must both be triangle sources, or the numbering of those two frames
  could quietly diverge (`G6`, `G11`).

### 2.15 The borrowed form of the shape: `BvhTopologyView`

`BvhTopology` owns a set of arrays, while a reader of a shape (codec, restore, writer) only needs to look at it. So the shape has two forms (`G4`):

- `BvhTopology`: the owned value - the result of one decode, the copy the tree itself holds, a value that can be compared, the return of `Bvh::topology()`
  (that one is a copy, so use `topologyView()` when you only need to read or encode);
- `BvhTopologyView`: three `std::span`s plus a height, an **explicit** borrow ("I borrow it" is a type, not a comment);
  the owned form converts implicitly to the borrowed one (a stronger promise to a weaker one; only ownership is dropped, as with Qt's `QByteArray` → `QByteArrayView`).

`RestoreTopology` therefore holds only one `BvhTopologyView` member: it no longer needs three parallel spans, and it no longer needs a `@warning` telling the caller
"do not move the topology away" - the parameter type already says it. Neither direction goes through an intermediate copy: writing `.vbtp` encodes
`Bvh::topologyView()` (the tree's own arrays); reading decodes an owned `BvhTopology`, which `Mesh::restoreBvh(BvhTopology)`
**moves** into the tree. There is borrowing and moving, no copying.

### 2.16 Sealing: keep only the result once built

`prim_bounds_` is **build-time data**, not the result. Queries read `node_bounds_` only (leaf-box culling); the primitive boxes are read in exactly three places:
incremental modification (`update` / `refit`), topology restore (`restoreTopology` → `refitNodes`), and the query callback's second argument.
So `seal()` **releases** the primitive boxes (`vector().swap()`, not `clear()`), and the tree is left with the result only:

| Kept | Size (P per primitive, N per node) | Released |
|---|---|---|
| `shape_.nodes` + `shape_.order` | `8 B/node` + `4 B/primitive` | `prim_bounds_` |
| `node_bounds_` | `48 B/node` (`Aabb<double>`) | `48 B/primitive` |

"One box holding hundreds of triangles" only holds once this is closed: leaf granularity (`BvhBuilder::leafSize`) changes the **node count** only, while the primitive boxes are
`48 B × primitive count`, independent of granularity. Take a 14.09M-triangle STL (leaf capacity 4 → 64 → coarse leaves, then sealed again):
`1.36 GB` (primitive boxes 676 MB + node layer 632 MB + order 56 MB) → `764 MB` (granularity only cuts the node layer) → `88 MB` (after sealing only
node layer 32 MB + order 56 MB remain).

Why one state rather than another kind of tree: the two states differ only in "whether the primitive boxes are kept". Two types would mean duplicating the only nearest-first pruning
traversal or turning it into a template (`D20`, `G7`). The state is derived from facts (built **and** primitive boxes empty) rather than kept in a flag,
so no object can have a flag that contradicts its data; `primCount()` answers from `shape_.order.size()`, and `validate()` only ever
needed a primitive **count**.

Why the default is **not** to seal: the consumers of `Mesh::buildBvh()` (a `.vbtp` restore may keep calling `update`/`refit`, and a tree is built before export) need
the mutable path. **Whoever wants mutability pays those 48 B**.

No production code calls `update()` / `refit()` today (only the two cases in `BvhBuildTest`, `F2`), and sealing decouples "the read-only consumer" from
"the possibly-mutable consumer" out of the same cost - the way a capability with no consumer is **neither deleted nor allowed to drag others down** (`G8`).

## 3. Features

| # | Feature | Section |
|---|---|---|
| F1 | Primitive registration and batch registration | 3.1 |
| F2 | Bounding-volume update and `refit` | 3.2 |
| F3 | Building (two quality levels + external algorithms) | 3.3 |
| F4 | Overlap query | 3.4 |
| F5 | Ray query and nearest-hit pruning | 3.5 |
| F6 | Invariant validation and quality report | 3.6 |
| F7 | Buffer reuse and release | 3.7 |
| F8 | Topology view and restore (`BvhTopologyView` / `RestoreTopology`) | 3.8 |
| F9 | Triangle set → triangle tree (`TriangleSource`) | 3.9 |
| F10 | Sealing: keep only the result once built | 3.10 |

### 3.1 F1 Primitive registration

- **Behaviour**: `add(bounds)` appends one primitive, returns a `PrimId` and marks the structure as not built; `addRange(count, bounds_of)` appends in bulk,
  reserving the capacity once.
- **Constraints** (C1): registration does not build the tree; after registration a `build` is required before querying (a query on an unbuilt tree returns empty).
- **Requirements** (R1): bulk registration reserves once; adding one at a time must not be used for a loop over ten thousand primitives.
- **Acceptance** (T1): `BvhBuildTest` compares one-at-a-time `add` against `addRange` for identical `primCount` and query results.

### 3.2 F2 Update and `refit`

- **Behaviour**: `update(id, bounds)` rewrites one primitive's bounding volume, and `refit()` recomputes every node box without touching the topology.
- **Constraints** (C5): after `refit` the leaf boxes are an **exact** merge (when no primitive was deformed) and the internal boxes are a conservative merge.
- **Note** (N2): repeated update loosens the leaves; the tree's quality degrades but the answers stay correct.
- **Note** (N3): a sealed tree cannot `update` / `refit`; to change it, `clear()` and register again (3.10).
- **Status**: this repository has **no production caller** today (only the two cases in `BvhBuildTest`). Neither real "the box changed" scenario goes through it:
  a rigid transform is the node matrix' business (the tree is in local space and the transform is folded in at query time), and a vertex change goes through `Mesh`'s
  drop-the-tree-and-rebuild and through `Geometry::prepareBvh()`'s new key and new tree. It is kept because "the primitive set stays, the boxes change one by one" really is a
  consumer class, and sealing stops that class and the read-only class from paying for each other (2.16).
- **Acceptance** (T2): in `BvhBuildTest`, after translating every primitive, `refit` + `validate` pass and the queries match a rebuilt tree.

### 3.3 F3 Building

- **Behaviour**: `BvhBuilder{}.leafSize(8).quality(kFast).build(bvh)`; `buildWith(bvh, algorithm)` accepts an external algorithm satisfying the
  `BvhBuildAlgorithm<T, Volume>` concept (a C++20 `concept`: callable, taking `BuildJob&`, returning the node count), and the caller then guarantees that it maintains
  the three invariants too.
- **Constraints** (C2): building is idempotent: the same input and configuration produce the **same** tree (the order of equivalent elements under `nth_element` is not guaranteed,
  but the partition result is; the tests therefore assert structure and quality, not concrete indices).
- **Requirements** (R2): any configuration and any input (including all-coincident, degenerate and NaN boxes) must produce a tree that passes `validate()` or build nothing at all.
- **Acceptance** (T3): `BvhBuildTest` covers the two levels SAH / median, leaf sizes 1 / 4 / 64, `max_depth` truncation, and a `validate` plus quality comparison over
  1000 random boxes.

### 3.4 F4 Overlap query

- **Behaviour**: `forEachOverlap(query, visit)` visits every primitive whose **leaf box intersects `query`**.
- **Constraints** (C3): the result is a **superset**: leaf boxes are conservative merges, so a visited primitive may still not intersect under the exact test (`N1`).
- **Acceptance** (T4): `BvhQueryTest` compares against a brute-force full scan as a **subset** (`expectCoversExactAnswer`).

### 3.5 F5 Ray query

- **Behaviour**: `forEachRay(ray, visit, max_t)` visits every primitive whose leaf box the ray enters within `[0, max_t]`;
  `visit` may write back a tighter `max_t`, and `return false` terminates immediately.
- **Constraints** (C4): a zero-direction ray (`direction_length2 <= eps²`) visits no primitive and produces no NaN (following `geometry-ray.md` C3).
- **Note** (N3): near-child-first is only a **visit order** and does not guarantee that "the first callback is the nearest hit" - the primitives themselves still have to be compared by `t`.
- **Acceptance** (T5): `BvhQueryTest` compares ray query results against a brute-force full scan, exercises `max_t` pruning, and checks that a nearest-hit query calls back once (the write-back prunes the rest).

### 3.6 F6 Validation and quality

- **Behaviour**: `validate()` checks I1–I3 exhaustively; `qualityReport()` returns `{ prim_count, leaf_count, leaf_half_area_sum, depth}`.
- **Constraints** (C6): `validate()` allocates (a `std::vector<bool>` marking used order slots) and is not on the query path.
- **Acceptance** (T6): `BvhBuilderContractTest` asserts that `validate()` is true after a build, and that a broken invariant is detected (the test breaks a tree's node boxes by hand).

### 3.7 F7 Buffer reuse

- **Behaviour**: `reserve(prim_count)` reserves the primitive capacity; `prepareForBuild` empties but keeps the node and order capacity; `clear()` empties everything but keeps the capacity.
- **Constraints** (C7): a rebuild allocates nothing new (when the capacity is sufficient).
- **Acceptance** (T7): `BvhBuilderContractTest` records the `capacity` after the first build and asserts it is unchanged after a rebuild.

### 3.8 F8 Topology view and restore

- **Behaviour**: the tree's shape is its own three arrays, so there are two ways to read it and two ways to write it:
  - `topologyView()` **borrows** the tree's own arrays (zero copy), and the `.vbtp` writer uses it directly; `topology()` returns the owned copy (one copy,
    for a caller that needs a value or wants to modify it);
  - when you own a shape, `Bvh::restoreTopology(BvhTopology)` **moves** the arrays in and recomputes every box; when all you hold is a view,
    `RestoreTopology{shape}` is handed to `BvhBuilder::buildWith()` as a build algorithm, which rebuilds the nodes from the stored grouping, writes the order,
    and then recomputes the boxes from the current primitives with `detail::refitNodes()`.
  - the node order is depth first, and the meaning of `offset` is in 2.4; `order` is the very permutation of `primOrder()`, `depth` is the tree's own height,
    and `primitiveCount()` is the length of `order`.
- **Constraints** (C8): restoring has a single acceptance check (`detail::installableHeight()`, read by both entries): the primitive count and the shape are non-empty →
  the length of `order` equals the primitive count → `order` is a permutation of the primitives → no node's `offset + count` runs past the primitive count → every node is
  reachable from the root and its measured height equals the declared `depth` → the height does not exceed `max_depth`. If any of them fails, no tree is built; there is no partial restore.
- **Requirements** (R5): the trees restored by both paths answer the same query item by item as a rebuilt tree; the boxes have one definition only - `detail::refitNodes()`
  writes the node volume array only and cannot reach the topology.
- **Acceptance** (T10): `BvhBuildTest.ARestoredTopologyAnswersTheSameQueries`,
  `ARestoredTopologyThatDoesNotDescribeTheTreeIsRefused`, `AnOwnedTopologyIsAdoptedByTheTreeItself`,
  `ATopologyViewBorrowsTheTreesOwnArrays`; `TriangleBvhTest.AMovedShapeIsAdoptedInsteadOfCopied`.

### 3.9 F9 Triangle set → triangle tree

- **Behaviour**: `TriangleSource` is a `concept` (`positions()` + `triangleCount()` + `triangleCorners()`);
  `registerTriangles(tree, source)` registers one primitive per triangle index; `buildTriangleBvh(tree, source, builder)` and
  `restoreTriangleBvh(tree, source, shape)` each combine "register + build" and "register + restore" into one call.
- **Constraints** (C10): a triangle tree's primitive numbering has one definition only (`registerTriangles()`): primitive id == triangle index, on both sides.
- **Constraints** (C11): when a corner cannot be resolved, a coordinate is not finite, the tree is not empty, or the source has no triangles, it returns `false` and
  **does not throw** (`Bvh::add()`'s `invalid_argument` is an unacceptable failure shape on the picking path); on refusal a prefix may already be registered, and the caller simply discards that tree.
- **Requirements** (R6): `Mesh` and the draw frame's `TriangleStream` must both satisfy `TriangleSource` statically (one `static_assert` on each side).
- **Acceptance** (T11): `TriangleBvhTest`: primitive id == index, building in one call, bad corners and non-finite coordinates rejected, restore and rebuild
  item-by-item identical, a foreign shape refused, a non-empty tree not renumbered.

### 3.10 F10 Sealing

- **Behaviour**: `seal()` releases the per-primitive bounding volumes and keeps `shape_` (nodes + order + height) and `node_bounds_`; `isSealed()` answers the state.
  After sealing, `add` / `addRange` / `update` / `refit` / `reserve` / `restoreTopology` and building all throw `std::logic_error`,
  while `primCount()` / `primOrder()` / `topology()` / `validate()` / `qualityReport()` / both queries work as before.
- **Constraints** (C13): sealing is a **final state**: the only way back is `clear()` (it clears the tree along with everything else, so "sealed" disappears with the tree it described).
- **Requirements** (R7): a query callback on a sealed tree receives the **leaf box** (`N3`), which contains every primitive in the leaf - the answer is still a superset (`C3`).
- **Note** (N4): calling `seal()` on an unbuilt tree is a no-op (there is no result to keep, and the primitives must stay mutable).
- **Acceptance** (T12): the five `*Sealed*` cases in `BvhQueryTest` - overlap and ray report **the same primitives** before and after sealing, `primCount`
  is unchanged, `validate` still holds, all six write interfaces throw, `clear()` restores mutability, and sealing an unbuilt tree is a no-op.

## 4. Public Interface

| Type / file | Contents |
|---|---|
| `spatial/Bvh.hpp` | `Bvh<T, Volume = Aabb>`: `add` / `addRange` / `update` / `reserve` / `clear`, `primCount` / `empty` / `isBuilt`, `seal` / `isSealed`, `forEachOverlap` / `forEachRay`, `refit`, `validate`, `qualityReport`, `primOrder`, `topology` / `topologyView` / `restoreTopology`; `kBvhMaxDepth = 64` |
| `spatial/BvhTopology.hpp` | pure vocabulary, **standard library only**: `BvhNodeTopology{offset, count}` (one node's topology, a value type with `==` and a `static_assert` of trivial copyability); `BvhTopology{nodes, order, depth}` (a whole tree's shape, `primitiveCount()` / `view()` / `==`); `BvhTopologyView{nodes, order, depth}` (the borrowed form of the same shape, `primitiveCount()` / `empty()`, 2.15) |
| `spatial/TriangleBvh.hpp` | `TriangleSource` (the `concept` of a triangle source); `registerTriangles` / `buildTriangleBvh` / `restoreTriangleBvh` (a view overload and a move overload, 2.14) |
| `spatial/BvhBuilder.hpp` | `BvhBuildQuality{kFast, kSah}`; `BvhBuildAlgorithm` (the `concept` of a build algorithm); `BvhBuilder`: `leafSize` / `maxDepth` / `quality`, `build(bvh)`, `buildWith(bvh, algorithm)`; `RestoreTopology{shape}` (installs a stored grouping as a build algorithm) |
| `spatial/detail/BvhBuildJob.hpp` | `BuildJob<T, Volume>` (`prim_bounds` / `order` / `nodes` / `node_bounds` + the three primitives) - an **internal contract**, not public API |
| `spatial/detail/BvhBuilders.hpp` | `MedianSplit`, `SahSplit`, `buildTopDown`, `SahBuild`, `MedianBuild` - internal |
| `spatial/detail/BvhRefit.hpp` | `refitNodes(nodes, node_bounds, order, prim_bounds)`: recompute a leaf from its primitives and a father from its children - internal |
| `spatial/detail/BvhTopologyCheck.hpp` | `orderIsPermutation`, `nodesFitPrimitives`, `measuredHeight`, `installableHeight` (the single acceptance check for restoring) - internal |
| `spatial/detail/VolumeTraits.hpp` | `VolumeTraits<V>` and `VolumeTraits<Aabb<T>>` - internal |

The full signatures and comments are in the headers; this page records only ownership, boundaries and reasons.

## 5. Constraints, Requirements and Invariants

- **C1**: registration does not build the tree; a query on an unbuilt tree returns empty.
- **C2**: building produces an isomorphic tree for the same input and configuration; the relative order of equivalent elements is not guaranteed.
- **C3**: an overlap query's result is a superset; the exact test is the caller's own business on the primitive.
- **C4**: a zero-direction ray produces no visits and no NaN.
- **C5**: `refit` does not change the topology and only recomputes boxes; the leaves may therefore loosen.
- **C6**: `validate()` may allocate and is for development time only; the query path allocates nothing.
- **C7**: a rebuild allocates nothing new when the capacity is sufficient.
- **C8**: restoring accepts only a self-consistent topology (see F8); the view entry and the owned-value entry read the same check (`detail::installableHeight()`); inconsistent means no tree, not a partial restore.
- **C9**: `refit()` and the two restore paths share `detail::refitNodes()` - the box has one definition, and it cannot write the topology.
- **C10**: a triangle tree's primitive numbering has one definition only: `registerTriangles()` (primitive id == triangle index), which both `Mesh` and `Geometry` go through.
- **C11**: `registerTriangles()` / `buildTriangleBvh()` / `restoreTriangleBvh()` do not throw: unusable input is `false` (a prefix may already be registered, and the caller discards that tree).
- **C12**: `shape_.nodes` and `node_bounds_` are always the same length: only `appendNode()` can grow both, and `refitNodes()` writes only the latter.
- **C13**: sealing is final: after `seal()` every write interface throws `logic_error`, and the only way back is `clear()` (3.10).
- **I1 / I2 / I3**: see 2.5.
- **R1**: bulk registration reserves once; one-at-a-time `add` is for few primitives.
- **R2**: any input must produce a tree that passes `validate()`, or an explicit refusal to build; it must not crash.
- **R3**: queries are `const` and allocate nothing, so they may be called concurrently; the write interfaces may not.
- **R4**: `Bvh.hpp` does not depend on `BvhBuilder.hpp`; rule 6 of `check_geom_layering.py` enforces it.
- **R5**: the trees restored by both paths answer the same query item by item as a rebuilt tree; restoring introduces no second build kernel (the view goes through `buildWith`, the owned value through `restoreTopology`, and both flesh out the boxes with `refitNodes()`).
- **R6**: `Mesh` and `TriangleStream` each carry a `static_assert(TriangleSource<...>)`: the two sides' triangle contract may not diverge.
- **R7**: a query callback on a sealed tree receives the leaf box rather than the primitive box; it still **contains** the primitive, so the superset semantics (`C3`) are unchanged.
- **N1**: the box handed to a query callback is conservative: on a mutable tree it is the primitive's own box, on a sealed tree it is the leaf box (`R7`); neither is a statement of exact intersection.
- **N2**: repeated `update` + `refit` loosens the leaves (`C5`): quality degrades and the answers stay correct.
- **N3**: a sealed tree refuses `add` / `addRange` / `update` / `refit` / `reserve` / `restoreTopology` and building (`C13`).
## 6. Caveats and Traps

- **P1**: leaf boxes are conservative merges, so the query result is a **superset**. How it goes wrong: taking the hits in the callback as the final answer and selecting a primitive that does not intersect.
  How to detect: compare against a brute-force full scan and assert "full scan ⊆ BVH result". How to prevent: run the exact intersection on the primitive inside the callback.
- **P2**: the left child index is `parent + 1`, the right child is `node.offset`. How it goes wrong: copying a linear BVH's `node.offset - 1` as the left child and
  traversing into your own parent or out of bounds. How to detect: work out the node indices by hand on a small case, or `validate()` reporting an I1 failure. How to prevent: use only the two spellings `index + 1` and
  `node.offset`.
- **P3**: `PrimId` is 32-bit. How it goes wrong: it overflows past 4 billion primitives (practically impossible, but `add` returns `kInvalidPrim`, and the caller has to check).
- **P4**: an SAH split that degenerates into "everything on one side" recurses forever if unhandled. How it goes wrong: stack overflow. How to detect: `SahSplit` returns 0. How to prevent: `buildTopDown`
  switches to a median split as soon as it sees a degenerate partition, and a median split always cuts the range into two.
- **P5**: `leaf_size` and `max_depth` interact. How it goes wrong: too small a `max_depth` leaves the leaves far above `leaf_size` (the tree is truncated and the leaves become large blocks).
  How to detect: `qualityReport().depth` and `leaf_count`. How to prevent: `kBvhMaxDepth = 64` is far above a normal tree's depth and is reached only on extreme input.
- **P6**: querying after `update` without `refit`. How it goes wrong: the leaf boxes are still the old ones and a moved primitive is missed. How to detect: `isBuilt()` reflects "has been built",
  not "is dirty". How to prevent: `refit()` after `update`, or rebuild.
- **P7**: a floating-point NaN box. How it goes wrong: `merged` propagates the NaN and the whole ancestor chain becomes unusable. How to detect: `VolumeTraits::isFinite`. How to prevent: check before registering,
  or accept that "this tree's queries will miss that primitive".
- **P8**: storing or reading `BvhNodeTopology::offset` as "two child indices". How it goes wrong: an internal node's `offset` is the **right child** index (the left child is always
  `parent + 1`), so writing the file the way a linear BVH does and restoring makes the traversal jump to the parent's sibling. How to detect: `validate()`'s containment check fails,
  or restoring returns 0 outright (the right child must come strictly after the left one). How to prevent: encode and decode with the one field meaning of 2.4, and let only that one convention appear in the file.
- **P9**: forgetting `primOrder()`. How it goes wrong: a leaf range points into the order array while the order array is the identity permutation - every leaf's primitive set is wrong, and
  the query results are **wrong but look normal**. How to detect: `validate()`'s permutation check. How to prevent: topology, order and height are one value (`BvhTopology`),
  and encoding, decoding and restoring only touch that one value; there is no "only half of it" call shape.
- **P10**: calling `update` / `refit` on a sealed tree, or taking the leaf box in a callback for the primitive box. How it goes wrong: the former throws `logic_error` (loud, easy);
  the latter **reports nothing** - the leaf box is larger than the primitive box, so using it as "where this primitive is" gives a region containing the neighbours' boxes, in a highlight box or a bounding-volume report, for example.
  How to detect: compute the primitive box in the callback when an exact box is needed, or look at `isSealed()`. How to prevent: the reason to seal must be "this tree answers only which primitives are there" - a consumer that needs per-primitive geometry must not seal.

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | put the BVH in `graphics` or `brep` | both sides need it; either side is a back edge (`G2`). It lives in `geometry`, on the same layer as `Aabb` / `Ray` (`G1`). |
| D2 | ship `Obb` as a second `Volume` | rejected by measurement: pairwise merging has no exact solution, and PCA fitting of the sixteen corners gives leaf surface areas about **twice** those of an `Aabb` tree in elongated-part scenes, while the fitted box does not even contain the corners it was fitted to (off by one rounding step), so containment would need a tolerance. See `ADR-2026-11-07-bvh-as-structure-plus-builder.md`. |
| D3 | support dynamic insertion / removal | the consumers (picking, LiDAR) are all "rebuild after the model changes", and a dynamically balanced tree's complexity buys nothing corresponding (`G8`). |
| D4 | build the algorithm into `Bvh` (one `build(kSah)` enum switch) | adding an algorithm changes the core type; an external algorithm cannot be injected; the tree's translation unit is forced to carry every build kernel (`G9`, `G5`). |
| D5 | builders inherit from `BvhBuilder` for polymorphism | building is an occasional one-off action, and virtual functions plus heap allocation buy "swappable at run time" flexibility nobody needs (`G8`). |
| D6 | `buildWith` takes a `std::function<void(BuildJob&)>` | one type erasure per build; a template parameter is free, and building already lives in the header (`G10`). |
| D7 | pre-allocate a node window of `2·⌈n/leaf⌉+1` | the leaf count depends on the split (`5` primitives split into `2+3`), so that bound overflows; instead `appendNode()` grows naturally and the capacity is kept. |
| D8 | internal nodes store two child indices | 4 more bytes per node; the `parent + 1` rule is just as clear, and the cost is one place where the index is written (`G10`). |
| D9 | queries return `std::vector<PrimId>` | a single-result query such as the nearest hit would allocate too; a callback with early termination covers every use (`G7`). |
| D10 | build with an explicit stack | `max_depth` already gives a bound on the recursion depth, and an explicit stack merely trades that bound for a heap allocation (`G10`). |
| D11 | leaves store copies of the primitives | a primitive (mesh, triangle) can be large; the tree stores only bounding volumes and ids, and the geometry stays where it is (`G13`). |
| D12 | put the BVH into `math` | a structure with a topology and a build flow is not numeric vocabulary (`geometry-primitives.md` 2.1, `G1`). |
| D13 | store / restore the bounding boxes | a box is a function of the position, so storing it creates the inconsistency of "boxes in the file and vertices in the file evolving separately" (`28 B/triangle`, plus byte order and tolerances); storing the topology costs about `8 B/triangle`, and the boxes are recomputed from the current primitives on restore (2.13, `G10`, `G13`). |
| D14 | give `Bvh` a `save()` / `load()` | file formats belong to `robotics/io` (`G2`); and `load()` would read bytes into a tree, bypassing the single check on the restore path. Restoring has exactly two entries: a view goes through `RestoreTopology` (a build algorithm), an owned value through `restoreTopology()` (a move), and both read the same `installableHeight()`. |
| D15 | compute a content fingerprint for the cache | a stale cache only makes the tree worse (2.13), so a fingerprint can raise quality but not correctness, while it would have to scan all the data on both save and load (`G8`). |
| D16 | a `TriangleBvh` class wrapping `Bvh` and a triangle source | it would block sharing that already exists: `Mesh::sharedBvh()`, `Geometry::adoptBvh()` and picking all share by `Bvh<double>` (one reference count). What was missing was never "yet another tree type" but the "triangles → bounding volumes" registration rule; the rule becomes free functions and the tree stays `Bvh` (`G8`, `G7`). |
| D17 | make the registration rule a `BvhBuildAlgorithm` (handed to `buildWith`) | the algorithm interface faces `BuildJob`'s internal contract, while registration is "the first step over the data" rather than "deciding the shape"; `RestoreTopology` in turn presupposes "the primitives are already registered", so the two are not on the same layer and mixing them would make each guess about the other (`G1`, `G4`). |
| D18 | make `BvhTopology` a nested type of `Bvh` | the shape has nothing to do with precision, while `Bvh<float>` / `Bvh<double>` are both explicitly instantiated, so nesting would create two mutually unassignable types for one layout, and a `.vbtp` codec would have to pull the whole tree's header in for one POD; and the layering would be backwards - `Bvh.hpp` depends on the shape header, not the other way round (`G2`, `G10`). The shape is a value that **can exist without a tree**. |
| D19 | the shape as a **transcription** of the tree (the previous version: `detail::BvhNode` plus `BvhNodeTopology`, two copies) | one convention in two structs, so every `topology()` paid for a full copy (38M triangles ≈ `1 GB`), and restoring appended node by node twice. With "the shape is the tree's storage", the view is zero-copy and restoring is a move; the only cost is one more load per node during traversal, while `refit()` touches the volume array only and is more concentrated (`G1`, `G10`). |
| D20 | sealing as another type (`SealedBvh` / `BvhView`) | the two states differ only in "whether the primitive boxes are kept", and making that a type would mean duplicating the single nearest-first pruning traversal, or writing the traversal as a template over the two storages - the former gives two sources of truth (`G7`), the latter merely trades a state for a compile-time parameter while the outer signature stays. The cost is paid on "no state field at all": the state is derived from "built, and the primitive boxes are empty" (`isSealed()`), so no flag can contradict the data. See `ADR-2026-11-10-bvh-seal-and-coarse-picking-tree.md`. |

## 8. Tests and Acceptance

| # | Criterion | Case |
|---|---|---|
| T1 | one-at-a-time `add` and `addRange` agree | `tests/test_geom_spatial/BvhBuildTest.cpp` |
| T2 | after `update` + `refit`, `validate` holds and the queries match a rebuild | `BvhBuildTest.cpp` |
| T3 | two quality levels × three leaf sizes × depth truncation × 1000 random boxes | `BvhBuildTest.cpp` |
| T4 | an overlap query covers a brute-force full scan (superset semantics) | `BvhQueryTest.cpp` |
| T5 | ray query: hit, miss, `max_t` pruning, nearest hit calls back once | `BvhQueryTest.cpp` |
| T6 | `validate()` is true after a build; breaking I1 is detected | `BvhBuilderContractTest.cpp` |
| T7 | `capacity` does not grow after a rebuild | `BvhBuilderContractTest.cpp` |
| T8 | querying an empty tree does not crash; querying an unbuilt tree returns empty | `BvhBuilderContractTest.cpp` |
| T9 | an SAH tree's overlap query visits no more nodes than a median tree (random scenes) | `BvhBuildTest.cpp` (node visits asserted indirectly through `qualityReport`'s leaf surface area) |
| T10 | topology → restore → identical queries; an inconsistent topology is refused; both restore paths answer alike | `BvhBuildTest.ARestoredTopologyAnswersTheSameQueries`, `ARestoredTopologyThatDoesNotDescribeTheTreeIsRefused`, `AnOwnedTopologyIsAdoptedByTheTreeItself`, `ATopologyViewBorrowsTheTreesOwnArrays`, `TriangleBvhTest.AMovedShapeIsAdoptedInsteadOfCopied` |
| T11 | primitive id == triangle index; one call builds; bad corners and NaN are refused; restore and rebuild answer alike; a foreign shape is refused; a non-empty tree is not renumbered | `tests/test_geom_spatial/TriangleBvhTest.cpp` |
| T12 | overlap and ray queries report the same primitives before and after sealing; `primCount` is unchanged; `validate` still holds; all six write interfaces throw `logic_error`; `clear()` restores mutability; sealing an unbuilt tree is a no-op | `tests/test_geom_spatial/BvhQueryTest.cpp` (`*Sealed*`, `ClearingASealedTreeMakesItMutableAgain`) |

The 28 existing BVH cases run in the same batch as the existing geometry cases: `build/bin/test_geom_spatial` (256 green, 2026-11-09).

## 9. Status and Evolution

### 9.1 Current status

The code is in `spatial/Bvh.hpp`, `spatial/BvhBuilder.hpp`, `spatial/BvhTopology.hpp`, `spatial/TriangleBvh.hpp` and
`spatial/detail/{RaySlab,VolumeTraits,BvhBuildJob,BvhBuilders,BvhRefit,BvhTopologyCheck}.hpp`;
`Aabb::intersectWith()` and the box solids' ray queries call `preparedRayBoxInterval()` directly (the single slab implementation of `geometry-ray.md`);
`src/base/geom/src/Instantiations.cpp` exports `Bvh<float>` / `Bvh<double>`.
Topology storage and restore are F8; the loading side that consumes them is in `robotics-io-design.md` (`.vbtp`).
Sealing is F10: `Geometry::prepareBvh()` seals as soon as it is built (the draw frame's tree is always a read-only result), and the other consumers are unaffected.

### 9.2 Known gaps

- The `Obb` volume (`D2`): it needs a merge that "keeps both children's orientation" rather than fitting a new box on every merge; it is not offered until then.
- Parallel building, SIMD traversal, ray packet traversal: no consumer (`G8`).
- Consumers: `Mesh::buildBvh()` / `Mesh::restoreBvh()` (`graphics-geometry.md` 2.9 / F32),
  `Geometry::prepareBvh()` (F33), and `robotics/io`'s `.vbtp` cache; both go through `buildTriangleBvh()` /
  `restoreTriangleBvh()`, so the numbering rule exists once (F9).
- There is no direct counter for "visited nodes"; quality is compared today indirectly through `qualityReport()`'s leaf surface area (T9).
- **The two-array traversal layout has no bench baseline yet**: the repository has no BVH benchmark target, so the cost of "one more load per node" has not yet been compared
  against an AoS version with `scripts/callgrind_diff.py`. Until a baseline exists, the zero-copy of `topologyView()` / `restoreTopology()`
  is **structurally provable** (the pointer-equality cases pin it), while traversal throughput is only a qualitative judgement.

### 9.3 Compatibility and migration

`Bvh<T>`'s layout is two AoS arrays (node topology and node volumes apart), and it is an explicitly instantiated DLL class, so it is
**ABI-incompatible** with a one-AoS-array version: consumers must recompile, and the source-level interface stays (`add` / queries / `refit` / `validate` / `primOrder` unchanged).
`topology()` returns an owned value; `topologyView()`, `restoreTopology()`, `Mesh::restoreBvh(BvhTopology)`
and `restoreTriangleBvh(..., BvhTopology)` are the borrow/move entries. `RestoreTopology` lives in `BvhBuilder.hpp` -
it was always used by "handing it to `buildWith`", so a caller that includes `BvhBuilder.hpp` needs no change.

Sealing is a **pure addition**: the class layout is unchanged (no new member) and everything is a header-inline function, so the ABI is not broken; the only semantic change
is that the six write interfaces throw `logic_error` on a sealed tree - and only tests call them today. `primCount()` / `empty()` answer from
`shape_.order.size()` on a sealed tree, identical bit for bit on a mutable one.

## 10. References

- Code: `src/base/geom/sdk/vine/geom/spatial/Bvh.hpp`, `BvhBuilder.hpp`, `BvhTopology.hpp`, `TriangleBvh.hpp`,
  `src/base/geom/sdk/vine/geom/spatial/detail/VolumeTraits.hpp`, `BvhBuildJob.hpp`, `BvhBuilders.hpp`, `RaySlab.hpp`.
- Related design: `geometry-ray.md` (rays and the slab implementation), `geometry-primitives.md` (where the value types belong).
- Decision: `../decisions/ADR-2026-11-07-bvh-as-structure-plus-builder.md`.
- External: Wald, *On fast Construction of SAH-based Bounding Volume Hierarchies* (binned SAH); Aila & Laine,
  *Understanding the Efficiency of Ray Traversal on GPUs* (the `parent + 1` left-child layout).
