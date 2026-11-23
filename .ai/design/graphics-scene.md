# graphics scene graph: nodes, state, collection, layers, picking

> **Status**: current (the relation between collection and snapshots is in `graphics-threading.md` §2.2)
> **Last verified**: 2026-11-16
> **Scope**: strict tree, state folding, collection memo, layers and picking.
> **Sources**: `Node.hpp` / `Group.hpp` / `StateNode.hpp` / `RenderState.hpp` / `StateResolution.hpp` /
> `Scene.hpp` / `Camera.hpp` / `RayIntersection.hpp`; the derived half lives in `src/viz/graphics/src/ScenePublisher.hpp`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

This file defines the current design of the graphics scene graph: nodes, state, collection, layers and picking: first the objects and their boundaries, then features, interfaces, constraints and acceptance.

### 1.2 Goals

- Turn §2's design points into §3's features, §4's interfaces and §8's acceptance.
- Keep "what it is now" separate from "why it was decided that way": the reasons are in §2/§7, history is in git.

### 1.3 Non-goals

- This file does not repeat the full signatures from the headers; an interface gives the contract and a pointer only.
- This file keeps no closed history or per-round review records.

### 1.4 One-paragraph conclusion

> A strict tree + folding where the nearest declaration wins; collection is a pure function, the memo is driven by the root subtree epoch;  picking is a mechanism, selection is business.

## 2. Design Points

2.1 **Strict tree**: `Group::addChild` detaches from the old parent automatically and refuses cycles; in one traversal a node has exactly one world matrix.
2.2 **Folding rules fixed**: visibility is a hard gate, opacity multiplies, layer and drawOrder follow nearest declaration wins, state overrides per item.
2.3 **State has one resolution point**: `resolveRenderState(authored, pass, data_topology)` reports the origin of every item.
2.3.1 **A depth band belongs to the pass, not to the node**: `PassDesc::depth_range` in a declaration maps one pass's draws into a segment of the depth buffer;
a node cannot carry it, because two subtrees mapping the same framebuffer would take the unique meaning away from a pixel's depth value (without a per-pixel tag
there is no inverse, and picking has no unique answer). The mapping is order-preserving, but a remapped depth **is no longer a distance**: if the writers of one target use different ranges,
its depth can no longer be read back as a geometric distance.
2.3.2 **Every resolved item has one destination**: a pipeline variant (`topology`) / dynamic state (depth, cull, blend, `polygonMode`, `lineWidth`) / a **data block** (`pointSize` → the draw block's `params.y`, because point size is a vertex-shader output rather than API state). The destination decides "does changing it need a rebuild": none of the three rebuilds (this engine's cull/polygon/line width are all dynamic state), only `topology` enters the variant key — but `pointSize`'s destination is **data**, so it is not even dynamic state. This must be written in `StateResolution.hpp`'s item comment, otherwise the next item is easily slipped into `PipelineKey` by accident.
2.4 **Collection selects only from published values, and there is one rule set**: camera + scene publication (`SceneSnapshot`) + layer mask → a shared immutable command table; the content query and the frame's source both go through `detail::selectSnapshot`, there is no second direct walk. The memo key is **published revision + view-projection + eye + layer_mask (+ viewport when needed)**: the published revision advances exactly when the picture, a box or a value changes, so "what invalidates the list" is answered by the publication's own version rather than a second epoch bookkeeping. A query inside an edit batch gives the picture at batch start (the same account as `publish`).
2.5 **The view context is explicit**: `View` carries the camera values (`CameraSnapshot`), the device-pixel `viewport` and the `layer_mask`; billboarding takes effect only here.
2.6 **Picking is a mechanism**: a ray + a scene walk; selection/outline/box selection are not in the framework.
2.7 **Lights are the scene's ambient quantity**: lights hang off `Scene`, not off nodes (one scene, one lighting; every pass gets the same one, so the main/offscreen/shadow passes always agree), positions and directions are world coordinates. Node lights (a light moving with a subtree) and per-draw light selection are trigger-gated (backlog T7 / S7), not today's model.

**The scene has no light-count limit**: `addLight` / `setLighting` take as many as they are given, because capacity is a property of the lighting program, not content semantics — the same Scene is drawn today by the forward and the deferred path, and may later be drawn by host programs with different capacities. The limit is `kMaxLights` (`ShaderAbi.hpp`); excess lights are not represented when packing and one `ChannelIgnored` warning per episode is reported (see `graphics-shader-params.md` §2.2). What should limit the light count is the light-selection policy (backlog S7), not a `Scene` cap.
2.8 **The root must be a root**: a scene is entered from a parentless node. `setRoot` first detaches a node that is already under another Group (the same rule as `Group::addChild`'s automatic detach from the old parent); if the root is adopted by another tree afterwards, it is no longer this scene's root — collection and the bounding box are empty, and the check runs before the memo query, so a list from before the move is never returned. Two scenes may share the same still-root subtree (each keeps its own scene-level state and memo), the seam is only "is it still a root".

2.9 **Transparent order is an object-level approximation**: within one `drawOrder` opaque goes front→back and transparent back→front; the order key is a **documented total-order value**
(`detail::SortKey`: drawOrder → layer (smallest layer index) → opaque/transparent → quantised camera-space depth → row/primitive index), the representative point is
**the world box centre** (without a box it falls back to the command's own matrix origin), and depth is a quantised **camera-space depth** rather than a Euclidean distance. It guarantees the order of "two objects that look separated", it does not guarantee self-intersection, non-convex shapes or several layers at one centre. Content that needs a stronger order goes through `Node::setDrawOrder` (already supported), split geometry,
or a separate pass with an independently sorted transparent pass (trigger in the backlog). The account and the fix record are in `../bugs/graphics-transparent-sort-uses-origin.md`.

2.10 **Node type dispatch goes through the metatype**: the scene walk decides `Node` derived types with `obj_cast<T>` (`vine/Object.hpp`'s metatype parent chain + `static_cast`),
not `dynamic_cast`. Why: there are several checks per node, and measured `obj_cast` is about 2–2.6× `dynamic_cast` on both hit and miss (Debug / MSVC,
20k nodes); it also unifies the spelling the project already uses (`PassChain`). Boundary: this is a **contract** — `T` must carry a correct `VN_OBJECT_META_DECL/IMPL`,
and the meta parent chain must match C++ inheritance; a wrong chain makes `static_cast` land silently at the wrong address. Types not derived from `Object` (`CameraManipulator` and other non-`Object` types)
keep using `dynamic_cast`. It satisfies `G6` / `G10`, the price is that the metatype chain must be kept correct.

2.11 **Derived state gets its own object**: `Scene` holds only the author tree, lights and queries; the baked rows (`SceneBake`), the per-view memo, the edit batch
depth and the published values live in `detail::ScenePublisher` (`src/viz/graphics/src/ScenePublisher.hpp`), and `Scene`'s `collectDrawItems` /
`publish` / `beginEdit` / `endEdit` / `setContentFrame` / `invalidateContent` and three counters all delegate. Why: with the three in one class,
changing the cache or the publication meant touching the author's class (`G1` / `G5`), and the author API had to carry lifetime rules it did not own; split apart, the dependency is only Scene → publisher
(the publisher reads the author's tree, the reverse does not hold). Boundary: `Scene` is **not** the frame's source (W9a deleted its `DrawListSource` base) —
a frame draws a publication (`SnapshotSource`), `Scene::collectDrawItems` answers "what does the current publication draw for this view" (the same
`selectSnapshot` rule as the frame's source, only read on its own thread); the public API and test semantics are unchanged;
`detail::SceneRows::snapshot` is still the module's one-line gate, forwarding to the publisher.

2.12 **Three derivation verbs, one meaning each** (naming, `G12`): the state a draw finally uses is reached
through three named steps, and a helper is spelled by **which inputs it takes**, never by what it returns —
`collect*` folds the tree into one authored value (`collectRenderState(node)` answers the still-optional block,
whose unset items mean "nobody on that path said anything", not "the default"); `resolve*` applies **one context**
to an authored value (`resolveRenderState(authored, pass, data_topology)` answers the final state *and* the
`origin` of every item — one implementation of the priority table); `effective*` is the **one-step composition**
a caller usually wants (`effectiveRenderState(node)` equals `applyStateDefaults(collectRenderState(node))`, and
`effectiveProgram` / `effectiveMaterial` are the same shape for a single-valued property, whose search ends at
the first override because a program or a material replaces the shading wholesale rather than one item of it).
The fourth spelling that used to exist here, a one-argument `resolveRenderState(state)`, is now
`applyStateDefaults(state)` (**BC-24**): it fills what nobody stated and takes no context, so under this rule it
was never a `resolve*` — and a reader could not tell the two apart without opening the header.
Why: while the three words look interchangeable, a reader cannot tell whether a derived value has
already walked the tree, so every call site is read twice (`G1`, `G4`). Boundary: a new derivation picks its verb
by its **inputs** — node and context together is `effective*`, a node alone is `collect*`, a value plus a context
is `resolve*` — and a function that composes two steps says so in its comment.

## 3. Features

### 3.1 Nodes and tree

| # | Feature | Notes |
|---|---|---|
| F1 | `Node` | Name, visibility, opacity, layer, drawOrder, parent chain, world matrix, bounding box. |
| F2 | `Group` | Child container; `addChild` / `removeChild` / `children()`. |
| F3 | Transform nodes | `MatrixTransform`, `PositionAttitudeTransform` (TRS + pivot), `AutoTransform` (billboard). |
| F4 | `StateNode` | Subtree render state (depth / cull / blend / polygon / polygonOffset / pointSize / lineWidth); nestable, deeper overrides. |
| F4a | `DepthRange` | Pass-level depth band: maps one pass's draws into `[min,max]` (default the whole range = no remapping), for semantic layering; order-preserving but no longer a distance. |
| F5 | `Geometry` | Leaf: vertex channels + a `Primitive` list (see `graphics-geometry.md`). |

### 3.2 Folding rules

| # | Fact | Rule |
|---|---|---|
| F6 | Visibility | `isVisible` is a hard gate: an invisible subtree leaves collection, picking and the scene box. |
| F7 | Opacity | The MAGNITUDE: scene × every ancestor × itself, clamped to [0,1] at the leaf. Material alpha (its factor and its base map's) is a second magnitude and the material's `alphaMode` says how the product is used, so classification reads both - see `graphics-material.md` §2.7 and `DrawItem::isTransparent`. |
| F8 | Layers | Nearest declaration wins; `clearLayers()` restores inheritance; default `kDefaultLayer`. |
| F9 | drawOrder | Nearest declaration wins; the outer sort key within a pass (higher = drawn later). |
| F10 | material / program | Primitive > geometry-level default > nearest `StateNode` > pass. |
| F11 | State | Every item optional; `StateOrigin::Content / Pass / Data / Default` records the origin. The ASSEMBLY (topology) is a data fact and reports its own origin beside the state (`ResolvedCommandState::topology_origin`: `Data` or `Pass`). |

### 3.3 Collection

| # | Feature | Notes |
|---|---|---|
| F12 | Command table | `Scene::collectDrawItems(camera, layer_mask, viewport?)` / `(View)`. |
| F13 | Command snapshot | geometry, primitive, model matrix, opacity, draw_order, state, material, program, instance. |
| F14 | memo | Key = published revision + view-projection + eye + layer_mask (+ viewport when needed); invalidated automatically when a new publication is created because the picture/box/value changed. |
| F15 | Frustum culling | The subtree box `subtreeBounds` is cached persistently in the node's own frame; moving the camera does not recompute. |
| F16 | Sorting | `detail::SortKey` total order: drawOrder → layer → opaque/transparent segment → quantised camera depth → row/primitive index; the representative point is the world box centre. Classification is evaluated when a publication is built, so the order is a property of that publication: a material edited through its live object is re-taken into the next one (`refreshValues`), which moves the publication's revision - the memo key - and the table is re-sorted, as `SceneContentCacheTest.AMaterialAlphaModeEditRebatchesAndReorders` requires. The content query and the frame's source use the same `selectSnapshot`. |
| F17 | Scene box | `Scene::boundingBox(mask)` counts only visible content that hits the layer mask. |

### 3.4 Layers and view context

| # | Feature | Notes |
|---|---|---|
| F18 | L1 content mask | `Node::setLayers` + `PassDesc::layer_mask`; decided on drawable nodes. |
| F19 | Subtree layer union | `subtreeLayerUnion` lazily cached; `(union & mask) == 0` skips the whole subtree (the cache is invalidated by a process-level layer epoch, see N11). |
| F20 | `View` | Camera values / device-pixel viewport / layer mask; collection and description share the same value. |
| F21 | ViewDependence | `None` / `View` / `ViewAndViewport`; only the last puts the viewport into the memo key. |
| F22 | AutoTransform | `RotateToScreen` / `RotateToCamera` / `RotateToAxis`; `setAutoScaleToScreen` is constant pixels. |
| F23 | Conservative bounds | AutoTransform uses a sphere box centred on position; the scene box does not follow the last camera. |

### 3.5 Picking

| # | Feature | Notes |
|---|---|---|
| F24 | Ray | `Camera::screenToWorldRay` takes normalised screen coordinates. |
| F25 | Scene query | `RayIntersection::intersectScene` / `intersectSceneAll`. |
| F26 | Hit identity | `geometry + primitiveIndex + instanceIndex`; triangle topologies only (list / strip / fan, `isTriangleTopology`). |
| F27 | Mask | The same layer rule as collection; containers are not culled by the mask. |

## 4. Public Interface

### 4.1 `Node` / `Group`

| Interface | Purpose |
|---|---|
| `name()` / `setName()` | Debugging and lookup |
| `isVisible()` / `setVisible()` | Subtree hard gate |
| `opacity()` / `setOpacity()` | Subtree multiplication |
| `layers()` / `setLayers()` / `clearLayers()` | L1 content mask |
| `drawOrder()` / `setDrawOrder()` / `clearDrawOrder()` | Order within a pass |
| `parent()` / `worldMatrix()` / `boundingBox()` | Parent-chain queries |
| `cacheStats()` | Read-only witness of the two caches (`bounds_recomputes` / `layer_union_recomputes`), returned by value; an observation port, it takes part in no decision |
| `contentEpoch()` | The subtree content announcement clock; the invalidation key of `Scene`'s memo, and the invalidation key a host-built subtree cache should use (a contract, not an observation port) |
| `Group::addChild()` / `removeChild()` / `children()` | Structure |

### 4.2 Transforms and state

| Interface | Purpose |
|---|---|
| `MatrixTransform::setMatrix()` / `matrix()` | Local matrix |
| `PositionAttitudeTransform::setPosition()` / `setAttitude()` / `setScale()` / `setPivot()` | TRS |
| `AutoTransform::setAutoRotateMode()` / `setAutoScaleToScreen()` / `setAxis()` / `setNormal()` | billboard |
| `StateNode::setDepth()` / `setCullMode()` / `setBlend()` / `setPolygonMode()` / `setPolygonOffset()` / `setPointSize()` / `setLineWidth()` / `clearRenderState()` / `renderState()` | Subtree state |
| `StateNode::setMaterial()` / `setProgram()` | Subtree material / program |
| `PassDesc::depth_range` / `RenderPass::depthRange()` | The whole pass's depth band (`DepthRange`; unset = the whole range) |
| `resolveRenderState(authored, pass, topology)` | The only resolution point (`StateResolution.hpp`) |

### 4.3 Scene and collection

| Interface | Purpose |
|---|---|
| `Scene::setRoot()` / `root()` / `clear()` | Content root (a node that is already elsewhere is detached first) |
| `Scene::setVisible()` / `setOpacity()` | Scene-level folding |
| `Scene::addLight()` / `lights()` / `setLighting()` | Lights |
| `Scene::lightSet()` | One **immutable shared value** of the lights (`LightSet`): cached by each light's `revision()`, zero copies when describing a pass (B11) |
| `Scene::collectDrawItems()` | Collection |
| `Scene::boundingBox(mask)` | Visible content box |
| `Scene::setContentFrame()` | memo frame token |
| `Scene::findNode(name)` | Lookup by name |

### 4.4 Picking

| Interface | Purpose |
|---|---|
| `Camera::screenToWorldRay()` | Screen → world ray |
| `RayIntersection::intersect()` | One geometry |
| `RayIntersection::intersectScene()` / `intersectSceneAll()` | Scene |
| `RayIntersectionResult` | hit / point / normal / distance / geometry / primitiveIndex / instanceIndex |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | The scene graph is a strict tree; `addChild` detaches from the old parent automatically and refuses cycles. |
| C2 | State folding is nearest declaration wins; containers do no layer-mask pruning. |
| C3 | Collection is a pure function; the memo is driven by the root subtree epoch. |
| C4 | Lights live on the scene: `Scene::lights()` is the only list, there are no node lights and no per-draw light subset; light values are world coordinates. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | Selection/highlighting/box selection are business, not the framework. |
| R2 | Picking is a mechanism: a `Camera` ray + `RayIntersection`, no selected state introduced. |
| R3 | A bounding box / world matrix has one answer, on the single parent chain. |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | A node has one parent; when "one subtree placed several times" is needed, copy the nodes, do not loosen the tree (see D1). |
| N2 | `Node::worldMatrix()` walks the parent chain; inside a traversal accumulate top-down instead of calling it per leaf. |
| N3 | The memo is invalidated by announcement: edits must go through a setter or `invalidateBounds()`; changing bytes directly only announces a buffer revision. |
| N4 | Editing one scene must not invalidate another scene's memo (the content epoch is root-subtree level). |
| N5 | The layer decision is on drawable nodes; containers are not culled by the mask, otherwise a descendant's redeclaration would be unreachable. |
| N6 | AutoTransform's auto-rotation is computed in the node's own frame; parent scaling enters the final size. |
| N7 | Picking hits triangle primitives only; lines/points are not surfaces. |
| N8 | `Camera` carries its own aspect; a pass rectangle inconsistent with the camera shape is reported at compile time. |
| N9 | The root must still be a root: `setRoot` detaches a node that is already under another Group; once the root is adopted by `Group::addChild`, that scene's collection and bounding box are empty, and the memo from before the adoption is not reused. |
| N10 | Transparent order is an object-level approximation: the order key takes the quantised camera-space depth of the **world box centre** (without a box it falls back to the matrix origin), and does not handle per-fragment / self-intersection / several layers at one centre; before relying on it confirm the content does not overlap, otherwise use `drawOrder` or split geometry. |
| N11 | The layer-union cache uses a **process-level** epoch (`detail::g_layer_epoch`): a layer / structure edit in any scene makes every scene recompute its own layer union once at its next walk. Layer edits are authoring-time operations, the recompute happens only at the next walk, and a stale cache is never used to cull wrongly; if the walk cost becomes visible while several documents are edited at once, switch to a per-root-subtree epoch (record the measurement first, `G10`). |

## 6. Caveats and Traps

### 6.1 One fact, one home

- How it fails: the same state is kept in two places, change one and the other goes stale.
- How to detect it: two writable values / two writable registries appear.
- How to prevent it: go back to §5's N#; keep one owner, everything else read-only.

### 6.2 Backend/platform boundary

- How it fails: platform differences are written into the engine/application layer, and every platform change forks everywhere.
- How to detect it: an "if Windows/X11 then …" branch appears.
- How to prevent it: platform facts stay in the backend, the engine consumes only compiled frame values.

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Multi-parent / DAG scene graph | `worldMatrix()` / `boundingBox()` / picking identity lose their unique answer; duplicating a subtree means copying nodes. See [ADR](../decisions/ADR-2026-10-23-no-multi-parent-dag.md). |
| D2 | Selection state (selection set / outline / box selection / multi-select) in the framework | Picking is a mechanism, selection is business; the industry's pickers are libraries too. |
| D3 | Built-in LOD / spatial partitioning in the framework | Wait for a real load where "collection itself per frame" is the bottleneck; see the backlog. |
| D4 | Layer-mask pruning on containers | Nearest declaration wins would make a descendant's redeclaration unreachable. |
| D5 | State folding as a per-leaf upward query | O(depth) and allocating; top-down `InheritedState` is the only implementation. |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `SceneTest` / `NodeTest` / `GroupTest` / `StateNodeTest` / `RayIntersectionTest`. |
| T2 | `SceneContentCacheTest`: editing one scene does not lose another scene's memo. |
| T3 | `ContentLayersTest.VisibilityAndLayersAreTwoQuestions` / `APassThatSelectsNoLayerDrawsNothingAndSaysSoOnce` / `AutoTransformTest` / `PositionAttitudeTransformTest`. |
| T4 | Moving a subtree into the frustum is not hidden by a stale box; a layer conflict is reported once. |
| T5 | `SceneTest.SettingAParentedNodeAsRootDetachesIt` / `SceneTest.ARootAttachedElsewhereTakesTheSceneOutOfThePicture`. |
| T6 | Transparent order: two transparent objects whose local coordinates are far from the origin have a command order consistent with their bounding boxes' distance (the current gap is `../bugs/graphics-transparent-sort-uses-origin.md`). |

## 9. Status and Evolution

### 9.1 Current status

Current design; the feature points and interfaces are the tables above.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items stay by trigger condition.

### 9.3 Compatibility / migration

Old names/paths appear only in §7's rejected options or in git history; new code follows §4's interfaces.

## 10. References

`graphics-architecture.md` · `graphics-geometry.md` · `graphics-rendering.md` · `../backlog/graphics.md`
