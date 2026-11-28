# graphics geometry: channels, primitives, instances, revision

> **Status**: current
> **Last verified**: 2026-11-28
> **Scope**: the `Geometry` channel set, one `Primitive` draw, instance streams, revision and storage sharing.
> **Sources**: `Geometry.hpp` / `Primitive.hpp` / `Buffer.hpp` / `ShaderAbi.hpp` / `Mesh.hpp`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

This file defines the current design of graphics geometry: channels, primitives, instances, revision: first the objects and their boundaries, then features, interfaces, constraints and acceptance.

### 1.2 Goals

- Turn §2's design points into §3's features, §4's interfaces and §8's acceptance.
- Keep "what it is now" separate from "why it was decided that way": the reasons are in §2/§7, history is in git.

### 1.3 Non-goals

- This file does not repeat the full signatures from the headers; an interface gives the contract and a pointer only.
- This file keeps no closed history or per-round review records.

### 1.4 One-paragraph conclusion

> `Geometry` is a channel set, `Primitive` is one draw; the instance stream is this draw's parameter; shape belongs to geometry, bytes belong to buffers.

## 2. Design Points

2.1 **One object, one responsibility**: `Geometry` holds channels only; `Primitive` describes one draw only; material/program are geometry-level defaults.
2.2 **Data is the draw**: adding a position channel automatically provides one default triangle primitive, so there is never "data but no idea how to draw it".
2.3 **Channel = segment + stride**: `BufferSlice<float>` + components; the slicing rule exists in one place (`BufferSlice`).
2.4 **Instances are a draw parameter**: attached to `Primitive`, not a scene-graph scope; there is no `InstanceScope`.
2.5 **Two-level revision**: `Geometry::revision()` describes the shape, `Buffer::revision()` describes the bytes.
2.6 **The bounding box is cached from the data**: local box key = positions buffer + its revision + segment + geometry revision.
2.7 **Geometry data is uniformly float**: meshes and parametric primitives have one float implementation, with no precision template and no aliases such as `Meshf` / `Meshd`;
    indices are `uint32_t`. Geometry data is shape description in a local frame, and float covers every existing consumer (robot IO, rendering, collision),
    while double has no downstream demand and a template would push the precision choice onto every call site. `Shape` is the runtime polymorphism boundary, and one `Mesh` is the base class of all meshes.
    Satisfies `G1` / `G4` / `G8`, and pays the `G10` cost as caller-side conversion in the double scenario (there is no such consumer today).
2.8 **Face topology is expressed by the mesh type, and triangle identity has one definition**: `Mesh` holds only vertex attributes and index storage, and promises nothing about whether a face is a triangle, a quad or a
    polygon (that is the subclass's); but "which triangles make up this mesh" is answered only by `Mesh::triangleCount()` / `Mesh::triangleCorners()`,
    and each concrete mesh implements it by the same rule as its own `toTriangleMesh()`. A triangle index is therefore stable across conversion: it is both the tree's primitive id
    and the identity a pick result reports. `MixedMesh` describes each face's corner range with `face_offsets`. Satisfies `G1` / `G4` / `G6`, and pays the `G10` cost as
    Quad/Mixed computing it by rule on every query (building no triangle array).

2.9 **The triangle tree is a cache, not content**: `Mesh` caches a `Bvh<double>` (`bvh()` / `hasBvh()` / `setBvh()` / `buildBvh()` /
    `clearBvh()`), under the same convention as `aabb_` — it describes the triangles already stored, holds no identity, and is dropped by any setter that changes triangles.
    The tree is `double` while the vertices are `float`: the tree is not geometry data but a guide structure for queries (`G13`), and query rays are `Rayd` in every consumer;
    narrowing a ray to float for box tests would lose the precision a small leaf box needs far from the origin, whereas widening float vertices into a box is exact.
    Satisfies `G1` / `G13` / `G4`, and pays the `G10` cost as one more empty `Bvh` per mesh.
    The draw frame is another instance of the same rule: each draw derives a keyed tree on demand (F33), and mesh source geometry shares the mesh's own tree directly at
    `geometryFromMesh()`.

2.10 **One triangle stream, one tree, two frames**: the unit a tree belongs to is neither geometry nor mesh but **one triangle stream**.
    - Mesh frame: one stream is the whole mesh (indices stay stable across `toTriangleMesh()`); draw frame: one stream is one `Primitive`'s index segment +
      the geometry's position segment + topology (the index is "the nth triangle of this draw", strip / fan included).
    - What the two frames share is the whole implementation (`Bvh<double>`, `BvhBuilder`, `forEachRay` / `forEachOverlap`, the key discipline); what differs is
      only the owner and the meaning of the index — because they really are two streams.
    - The index has one source: `graphics`' internal `TriangleStream` (`triangleCount()` / `triangleCorners()`), and linear scanning, building and exact intersection all ask it,
      so the tree's primitive id and the `triangleIndex` a pick reports cannot get out of step.
    - Without a tree, picking is a linear scan, so "having a tree" is pure gain: the same results one by one, at a different cost. When to prepare it belongs to the consumer (the threshold is set by the UI).
    - Satisfies `G1` (one concept, one definition), `G5` (a change stays in its own frame), `G13` (the tree only describes a stream and holds no identity).

2.11 **Three geometry facts are a CLOSED boundary, not an oversight** (`G8` `G9`; review 17 `W7`): the vertex input is
fixed at three points that are cheap to widen and expensive to guess at, and each is stated here with the trigger that
would reopen it.

- **Indices are `std::uint32_t` only** (`Primitive::IndexSpan` is `BufferSlice<std::uint32_t>`, and the backend binds
  `VK_INDEX_TYPE_UINT32` unconditionally). Trigger: the first asset whose index buffer (not its vertices) is the memory
  peak - a 16-bit spelling is a second overload of the index policy and a second backend binding, not a flag.
- **No primitive restart.** A strip or fan that needs a break is several primitives, which is what the geometry model
  already says (one `Primitive` is one draw). Trigger: the first multi-strip asset whose draw count (not its bytes) is
  the cost.
- **No per-instance attribute beyond the `mat4`** (`GeometryChannel` has a location and a per-vertex segment; the
  instance rate exists for the four instance-matrix columns and nothing else). Trigger: the first per-instance highlight
  (a selection colour or an ID per instance), which today costs one draw per distinct value.

All three are data-shape decisions (`G9` says a new format or device must not edit the core model, not that every
format feature must exist), and none of them changes the picture today: `F8` / `F10` / `F11` above state what a
primitive carries, and these sentences say what it deliberately does not.

## 3. Features

### 3.1 Channels

| # | Feature | Notes |
|---|---|---|
| F1 | Canonical channels | positions / normals / texcoords / colour; locations are defined by `ShaderAbi`. |
| F2 | Custom channels | From location 3 up, `addBuffer(location, AttributeChannel)`. |
| F3 | Shared buffers | `setPositions` / `setNormals` / `setTexcoords2/3` / `setColors` take an `intrusive_ptr<const Buffer<float>>`. |
| F4 | Arena slices | `first_vertex` / `vertex_count`; one buffer holds the vertices of several geometries. |
| F5 | View access | `scalars()` / `xyz()` / `vec3View()` / `vertexCount()` / `stride()`. |

### 3.2 Primitives

| # | Feature | Notes |
|---|---|---|
| F6 | One draw | `Primitive{topology, indices, material, program}`. |
| F7 | Multiple parts | One `Geometry` with several `Primitive`s sharing the vertex channels. |
| F8 | Index segment | `BufferSlice<uint32_t>`; empty = non-indexed; one arena can share the upload. |
| F9 | Material/program defaults | The primitive's own value > geometry default > nearest `StateNode` > pass. |
| F10 | Topology | `Triangles` / `TriangleStrip` / `TriangleFan` / `Lines` / `LineStrip` / `Points`; it is pipeline identity, not dynamic state (one pipeline per value, although Vulkan allows dynamic switching within a topology class). `isTriangleTopology()` is the single criterion for "is this a surface" (used by picking). |

### 3.3 Instances

| # | Feature | Notes |
|---|---|---|
| F11 | Instance stream | `BufferSlice<float> instance_transforms`, 16 floats per instance, a column-major mat4. |
| F12 | Instance count | `std::optional<size_t> instance_count`; changes the draw parameter only, and retransmits nothing. |
| F13 | Collection | One command per primitive; an instance count of 0 only skips that primitive. |
| F14 | Backend | Binding 4's per-instance stream + `VN_INSTANCED` (bit 8) + `Draw.instances` / `firstInstance`. |
| F15 | Bounding box | The union of the local box transformed by each primitive's drawn instance matrices. |
| F16 | Picking | Per primitive, per drawn instance; the result is `geometry + primitiveIndex + instanceIndex`. |

### 3.4 Revision and bounding boxes

| # | Feature | Notes |
|---|---|---|
| F17 | Geometry revision | `setRevision()` / `bumpRevision()`; it means a shape change only. |
| F18 | Buffer revision | Announced when the `Buffer::edit()` scope ends (the only write entry point); `setRevision()` is only for "writes from outside the object". The upload cache only looks at the revision; the revision is a `std::atomic` (writers release, readers acquire), so polling across threads has a defined order. |
| F19 | Fact-row rebuild | A geometry revision change ⇒ rebuild the layout / channel set / counts. |
| F20 | Local box cache | `localBounds()` + `localBoundsComputationCount()` as the witness. |
| F21 | World box | The local box times the parent chain; `subtreeBounds` is in the node's own frame. |

### 3.5 Storage and sharing (geometry module)

| # | Feature | Notes |
|---|---|---|
| F22 | `core::Buffer<T>` | `RefCounted`, internally holding a `std::vector<T>`; not an `Object`, no vptr. |
| F23 | Uniform float | `Mesh` attributes are stored in `Buffer<float>`, indices in `Buffer<std::uint32_t>`; no precision template and no `Meshf` / `Meshd` aliases. |
| F24 | The rendering boundary | `geometryFromShape()` hands only `TriangleMesh` to the converter and shares the attribute buffers as they are; `QuadMesh` / `MixedMesh` are converted to `TriangleMesh` at this boundary first, an analytic `Primitive` is tessellated (`vn::geom::spatial::tessellateSolid()`, see `geometry-tessellation.md` 9.1) and assembled with `meshFromTriangulation()`, and an invalid mesh yields empty geometry rather than throwing. |
| F25 | Manual announcement | Nobody bumps `Shape` / `Geometry` automatically: a writer advances `Shape::revision()` itself after finishing an edit; `Buffer` writes happen inside the `edit()` scope (announced once automatically); only bytes changed from outside the object use `Buffer::setRevision()`; the draw layout is `Geometry::setRevision()`'s job. |
| F26 | Backend aliasing | The backend reads Vine buffer `Data` in place (no copy); a vertex input's stride is the channel's element size, not the buffer's allocation size. |
| F27 | The pointer-identity criterion | Rendering evidence is indifferent to "copy vs share"; only a `dataPointer()` identity assertion can tell them apart. |

### 3.6 Mesh face topology

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F28 | Unified triangle mesh | `TriangleMesh` distinguishes indexed faces from sequential triangles by `hasIndices()`. | Landed |
| F29 | Quad mesh | `QuadMesh` is explicitly converted to `TriangleMesh` along a fixed diagonal. | Landed |
| F30 | Mixed polygon mesh | `MixedMesh` splits faces by offsets, gives indices and offsets together as a whole, and produces a `TriangleMesh` through an explicit conversion. | Landed |
| F31 | Point cloud | `PointCloud`: `setPositions()` replaces the point set as a whole, `normals()` / `colors()` are optional, `computeAabb()` caches the box; constraint C8, requirement R7, acceptance T10. | Landed |
| F32 | Triangle tree cache | `Mesh` caches a `Bvh<double>`: `buildBvh()` adds one primitive per triangle (the id is the triangle index, the rule lives in `vn::geom::spatial::buildTriangleBvh()`), and `bvh()` serves ray queries; any setter that changes triangles or face topology drops the tree; `setBvh()` only accepts a tree that is already built and covers this mesh's triangle count. | Landed |
| F33 | The draw frame's triangle tree | One `Bvh<double>` per draw, derived on demand and invalidated by the stream key: `prepareBvh()` builds or shares one (a built tree is sealed, default leaf capacity 64), `bvh()` is read-only query (`hasBvh()` is its boolean form), `adoptBvh()` accepts an existing tree, `clearBvh()` drops them all; picking uses the tree when there is one and falls back to a linear scan when there is not. | Landed |
| F34 | Coordinate-frame marker | `makeAxesMarker(style)` builds ONE geometry and ONE primitive holding the style's arrows (a cylinder shaft closed by a cone tip each, from `tessellateSolid()`), told apart by VERTEX COLOUR; a process-wide, bounded prototype cache shares the vertex/index/colour streams between every marker of the same style, so N markers read one upload. | Landed |

#### 3.6.1 F28 Unified triangle mesh

- **Behaviour**: with indices, one face per three; without, one face per three sequential vertices; `triangleCount()` / `triangleCorners()` are declared by `Mesh` and fix the index (see 2.8), and this type implements it as one face per three.
- **Constraint** (C5): the index count and the vertex count must both form whole triangles, and every index must fall within the vertex range.
- **Requirement** (R4): robot IO accepts `TriangleMesh` only; primitives are float too.
- **Acceptance** (T6): `MeshShapeTest` covers both storage forms and the whole-triangle constraint.
- **Caveat** (N8): indexed/soup is a storage state of the same type and is not distinguished by `ShapeType`.

#### 3.6.2 F29 Quad mesh

- **Behaviour**: `QuadMesh` inherits `Mesh` directly; indexed or sequential corners make one face per four; `toTriangleMesh()` returns a triangle mesh with converted indices and copies of the vertex attributes.
- **Constraint** (C6): the corner count must be a multiple of four; indices must not go out of range.
- **Requirement** (R5): graphics and meshio validate the mesh before consuming it and reuse that conversion interface.
- **Acceptance** (T7): `MeshShapeTest.QuadMeshUsesFourCornersAndTriangulates` verifies the conversion result.
- **Caveat** (N9): the conversion result is an independent `TriangleMesh`; changing the result does not write back to the source QuadMesh.

#### 3.6.3 F30 Mixed polygon mesh

- **Behaviour**: `face_offsets` start at 0 and end at the corner count, giving each face's boundary; indices and offsets are given together as a whole by `setIndices()` / `setFaceOffsets()`; `toTriangleMesh()` generates an independent triangle mesh as a triangle fan and copies the vertex attributes and the name.
- **Constraint** (C7): offsets are monotonically non-decreasing with at least three corners per face; indexed corners must fall within the vertex range.
- **Requirement** (R6): graphics, meshio and the model viewer's statistics reuse the explicit conversion; when the offsets are invalid the conversion must not access their face data.
- **Acceptance** (T8): `MeshShapeTest.MixedMeshValidatesOffsetsAndConvertsToTriangleMesh` and `MeshShapeTest.MixedMeshWithSeveralFacesConvertsEveryFace`.

**Contract**:
- A mesh has one fill entry point: the whole group `setPositions()` / `setNormals()` / `setTexcoords()` / `setColors()` / `setIndices()` (plus `setFaceOffsets()` for Mixed), with no incremental append.
- `Mesh`'s `span` is borrowed; any growth invalidates existing views.
- A channel holds a buffer, not a snapshot: after the source buffer grows the channel does not dangle, and the length and address are taken fresh.
- A channel's bytes and count come from ONE observation (`BufferSlice` resolves the run against the size the writer announced) and each uploaded stream records the BUFFER and the REVISION the bytes came from; a later frame that sees that revision move re-uploads (`detachStaleStreams`), and the segment (offset / count) is part of the geometry's `GeometryDescription`, so changing it is a new description rather than a second cache layer.
- NOTHING IS INVENTED for a channel the geometry does not author: every non-empty channel is uploaded as authored, and an absent one is simply missing from the vertex layout, so a stage that reads that location reads UNDEFINED values. The retired backend's zero-filled texcoords, white opacity carrier and derived normals have no successor here - opacity is `VineDrawBlock::params.x` (`graphics-material.md` 2.7), a normal is the authored channel or the flat program's screen-space face normal (`Geometry.hpp` `setNormals`, review 11 `V4`).

#### 3.6.4 F32 Triangle tree cache

- **Behaviour**: `buildBvh(builder)` adds one primitive per triangle, whose box is the bounding box of the three corners in `double`; once built,
  `bvh().forEachRay(ray, visit)` yields candidate triangle indices and `bvh().forEachOverlap(box, visit)` yields the triangles whose boxes overlap.
  Registration and numbering are not a loop `Mesh` writes itself but `vn::geom::spatial::buildTriangleBvh()` (`geometry-bvh.md` 2.14 / F9):
  the same rule also builds the draw frame's tree.
  `setBvh()` accepts a tree the caller built with its own builder, and verifies that it is built and covers this mesh's triangle count (otherwise it refuses, the same admission rule as
  `Geometry::adoptBvh()`). `hasBvh()` reports whether the tree is built.
  A tree is published by shared pointer as soon as it is built (`sharedBvh()`) and is never modified afterwards: a rebuild **publishes a new object**, so old holders keep reading a tree that is still valid,
  and another frame can share the same one at zero cost (see §2.10 and F33).
- **Constraint** (C9): primitive id == the triangle index of `triangleCount()`; `triangleCount()` / `triangleCorners()` are in the same order as
  `toTriangleMesh()`; an invalid mesh builds no tree.
- **Requirement** (R8): only setters that change triangles or face topology drop the tree (`setPositions()` / `setIndices()` /
  `MixedMesh::setFaceOffsets()`); normals, texture coordinates and colours do not.
- **Acceptance** (T11): `tests/test_geom_spatial/MeshBvhTest.cpp`, seven items: triangle index, same order as the conversion, tree building and ray pruning,
  invalidation rules, no tree for an invalid mesh, accepting a foreign tree, foreign/unbuilt trees refused by `setBvh()`.
- **Caveat** (N15): a candidate is a superset — the leaf box is the three points' bounding box, and the exact intersection is still the caller's `intersectTriangle()` (the same triangle-fan
  restriction as N10).

#### 3.6.5 F33 The draw frame's triangle tree

- **Behaviour**: a draw's triangle stream (position segment + index slice + topology) can derive its own tree on demand: `prepareBvh(draw)` builds a tree or
  takes over another one under the same key, `bvh(draw)` returns the tree under the current key read-only (`nullptr` when there is none, `hasBvh(draw)` is its boolean form),
  `adoptBvh(draw, tree)` accepts a tree
  that is already built (`geometryFromMesh()` adopts the mesh's), and `clearBvh()` drops every slot. Picking takes the tree per draw: with a tree it uses
  `forEachRay` to get candidates and then intersects exactly, without one it runs the original linear loop — both paths share the same triangle-index implementation.
- **Behaviour**: building goes through `vn::geom::spatial::buildTriangleBvh()` — `TriangleStream` satisfies `TriangleSource` (there is a `static_assert` in `TriangleStream.hpp`
  pinning it), so a draw's tree and a mesh's tree take the same "primitive id == triangle index" rule, and the mesh's tree can be adopted by a draw with no conversion at all (`geometry-bvh.md` F9).
- **Constraint** (C10): the slots are in the same order and of the same length as `primitives_`; the key = position buffer + its revision + segment + index buffer + its revision +
  slice + topology + geometry revision, and any mismatch means "no tree" (a rebuild, rather than a wrong answer).
- **Constraint** (C11): only a triangle-topology draw can have a tree; `prepareBvh()` on a line/point draw returns false and takes no tree.
- **Constraint** (C12): a tree the engine builds is **sealed** (`Bvh::seal()`) with a default leaf capacity of `kPickTreeLeafSize = 64`: the former because it is a
  cache (a new key is a new tree, and it can never be `update`/`refit`ed), the latter because a tree stores boxes per **node** — with a leaf capacity of 4 each triangle amortises
  half a node, with 64 a thirty-second, and the price is only a few dozen more triangle tests per click. A caller may pass its own `BvhBuilder` to change the granularity
  (`prepareBvh(draw, builder)`), but sealing is not optional.
- **Requirement** (R9): a second draw under the same key shares the same immutable tree and does not rebuild; adoption only happens at `geometryFromMesh()`.
- **Acceptance** (T12): `GeometryTest.AHandBuiltDrawPreparesItsOwnTree` / `MovingTheStreamDropsThePreparedTree` /
  `TwoDrawsOverOneIndexStreamShareOneTree` / `MeshSourceGeometryAdoptsTheMeshesTree` /
  `ATriangulatedQuadMeshAdoptsNothing` / `ThePreparedTreeIsSealedAndKeepsTheEnginesCoarseLeaf` /
  `TheCallerChoosesTheLeafSizeOfItsPickTree`, `RayIntersectionTest.APreparedTreeChangesTheCostAndNotTheAnswer` /
  `APreparedTreeStillReportsEveryHit`.
- **Caveat** (N17): the presence of a tree changes no semantics, only the cost; the witness is `triangleTestCount()`.
- **Caveat** (N18): the sealed callback's second parameter is the **leaf box** rather than the primitive box (`geometry-bvh.md` `R7`). Picking ignores that parameter, so it is unaffected;
  a new consumer that relies on it for "where is this triangle" must either build the primitive box itself or not seal.

#### 3.6.6 F34 Coordinate-frame marker

- **Behaviour**: `makeAxesMarker(AxesStyle)` returns the geometry of one coordinate-frame marker: the arrows `axis_mask`
  names, each a cylinder shaft closed by a cone tip (`vn::geom::spatial::tessellateSolid()`, rotated from +Z onto its
  axis), `length` long with proportions stated as fractions of it. All arrows are ONE primitive over ONE vertex stream;
  their colours are the per-vertex colour channel, which the shading multiplies into the material's base colour.
- **Behaviour**: every call returns a NEW geometry node reading the streams of a prototype built once per style (a
  process-wide, mutex-guarded cache). A node has one parent (`Group::addChild` reparents), so two placements are two
  geometries - and they share every vertex, index and colour byte, which is one device upload for any number of markers.
- **Constraint** (C13): a style with no axis (`axis_mask == 0`) or a non-positive `length` builds a geometry with no
  channels and no primitive: "draw nothing" is the absence of a draw, not a hidden node.
- **Constraint** (C14): the prototype cache keys on the style's exact fields (`AxesStyle::operator==`), keeps at most
  32 styles, and past that bound builds without keeping the result; a published prototype is immutable.
- **Requirement** (R10): the marker states its own material (white, diffuse), so its vertex colours reach the frame
  unchanged; a caller that wants the marker shaded like its content sets its own material on the returned geometry.
- **Acceptance** (T13): `AxesMarkerTest.TheThreeArrowsAreOneDraw` / `EveryTipReachesTheStyledLength` /
  `EachArrowCarriesItsColour` / `AMaskLeavesTheOtherArrowsOut` / `AnEmptyStyleBuildsNothingToDraw` /
  `MarkersOfOneStyleShareTheirStreams`, and `AxisGizmoTest.BuildsOneAxesMarker`.
- **Caveat** (N19): `axis_mask` is a BUILD parameter: hiding an arrow is a different marker, not a runtime switch. A
  caller that needs both states holds two markers, which is cheap exactly because the streams are shared.
- **Caveat** (N20): a marker's transform is its NODE's and its instance stream is unused: hundreds of frames that must
  each be shown, hidden or restyled independently are that many draws. A field that only moves together should be one
  geometry with an instance stream instead (`R1`, `R2`).

## 4. Public Interface

### 4.1 `Geometry`

| Interface | Purpose |
|---|---|
| `addBuffer()` / `removeBuffer()` / `buffer()` / `bufferCount()` / `bufferLocations()` | Channels |
| `setPositions()` / `setNormals()` / `setTexcoords2/3()` / `setColors()` / `has*()` / `*Count()` | Canonical channels |
| `addPrimitive()` / `setPrimitive()` / `clearPrimitives()` / `primitive()` / `primitives()` | Primitives |
| `material()` / `setMaterial()` / `program()` / `setProgram()` | Geometry-level defaults |
| `revision()` / `setRevision()` / `bumpRevision()` | Shape announcement |
| `vertexCount()` / `localBounds()` / `boundingBox()` / `subtreeBounds()` | Data and boxes |
| `prepareBvh()` / `bvh()` / `hasBvh()` / `adoptBvh()` / `clearBvh()` / `triangleTestCount()` | The per-draw triangle tree cache and its read-only witness (F33); `prepareBvh(draw, builder)`'s builder chooses the granularity, defaulting to `pickTreeBuilder()` (`kPickTreeLeafSize = 64`), and a built tree is always sealed |
| `triangleCount(draw)` | One draw's triangle count; a count only, building and caching nothing, for a consumer's threshold policy |
| `setInstanceTransforms()` / `setInstanceCount()` / `clear*()` | Instance convenience for default primitive 0 |
| `geometryFromShape()` / `packAttribute()` / `packIndices()` | The bridge and packing |
| `makeAxesMarker()` / `AxesStyle` | A coordinate-frame marker's geometry: the arrows of one style as one draw, reading streams shared with every marker of that style (F34) |

### 4.2 `Primitive`

| Interface | Purpose |
|---|---|
| `topology` / `indices` | Topology and index segment |
| `material` / `program` | This draw's overrides |
| `hasInstances()` / `instanceCapacity()` / `firstInstance()` / `instanceCount()` | Instances |
| `setInstanceTransforms()` / `setInstanceCount()` | Instance editing |

### 4.3 `AttributeChannel`

| Interface | Purpose |
|---|---|
| `packed()` / `shared()` / `slice()` / `fromSlice()` | Construction |
| `scalars()` / `scalarSlice()` / `floatCount()` / `vertexCount()` / `stride()` | Reading |
| `xyz()` / `vec3View()` | Vertex-level views |

### 4.4 Storage and sharing

| Interface | Purpose |
|---|---|
| `Buffer<T>::view()` / `size()` / `setRevision()` / `revision()` | Byte storage and announcement |
| `Shape::revision()` / `setRevision()` / `bumpRevision()` | Shape content generation: the holder advances it after building or editing |
| `Mesh::vertexCount()` / `hasIndices()` | Vertex count and index state; does not define face topology |
| `Mesh::setPositions()` / `setIndices()` / `indices()` | Shared vertex and corner index storage (set as a whole) |
| `Mesh::positions()` / `normals()` / `texcoords()` / `positionsBuffer()` etc. | float views and storage handles; the array alias is in `Array.hpp` |
| `Mesh::triangleCount()` / `triangleCorners()` | Triangle identity: the count and the three vertex indices; each concrete mesh implements it (2.8) |
| `Mesh::bvh()` / `hasBvh()` / `setBvh()` / `buildBvh()` / `clearBvh()` | The triangle tree cache: build, accept, query, drop (F32) |
| `Mesh::restoreBvh()` | Restores the tree from a stored shape (`BvhTopology`: nodes + order + height), recomputing the boxes from the current vertices; the `.vbtp` cache goes this way |
| `Mesh::sharedBvh()` | Hands the published immutable tree to another frame to share (no copy) |
| `TriangleMesh::isValid()` | Completeness of a sequential or indexed triangle mesh |
| `QuadMesh::quadCount()` / `toTriangleMesh()` | Quad faces and the explicit triangle-mesh conversion |
| `MixedMesh::faceOffsets()` / `faceOffsetsBuffer()` / `setFaceOffsets()` / `toTriangleMesh()` | Shareable polygon boundaries and the explicit triangle-mesh conversion |
| `Model::Entry::mesh` | The `Mesh` handle; model import writes a `TriangleMesh`, which is triangulated before drawing at `geometryFromShape()`; one mesh can be referenced by several nodes |
| `geometryFromShape()` | `Shape` → `Geometry`, sharing buffers |
| `packAttribute()` / `packIndices()` | Typed input packing |

### 4.5 Shape types and module boundaries

| Interface | Purpose |
|---|---|
| `Box` / `Sphere` / `Cone` / `Cylinder` / `Ellipsoid` | Parametric primitives, all dimensions `float`, described in a local frame; robot IO reads and writes them directly |
| `Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` | The mesh type hierarchy; graphics and meshio accept this float mesh set only |
| `PointCloud` | Point cloud: positions + optional normals/colours, no face topology; shares the `Buffer<float>` channels with meshes |
| `Primitive` / `BrepShape` / `Shape` | The common base of primitives and B-rep; the `revision` trio lives in `Shape` |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | Shape belongs to `Geometry`, bytes belong to `Buffer`; one channel, one layout. |
| C2 | The instance stream is a `Primitive` parameter, not a `Geometry` channel. |
| C3 | Revision only advances when the content really changes; the visible instance count is a prefix. |
| C4 | Geometry values use `float` only: mesh vertex attributes, primitive dimensions and indices (`std::uint32_t`) have no precision variants. |
| C5 | An empty triangle-mesh index means sequential vertices; a non-empty index must be grouped in threes and be entirely valid. |
| C6 | A Quad face's corner count must be a multiple of 4, and indices must not go out of range. |
| C7 | Mixed offsets start at 0, are monotonically non-decreasing, end at the corner count, and give every face at least 3 corners. |
| C8 | Point cloud channel layout: 3 floats per point for positions and normals, 4 floats per point for colours, all in `Buffer<float>`. |
| C9 | The triangle tree only describes the triangles already stored: primitive id == triangle index, any setter that changes triangles or face topology drops the tree, and an unbuilt tree answers no candidates. |
| C10 | The draw frame's tree is cached per draw and invalidated by the key: any mismatch in the position/index buffers, their revisions, the segment, the slice, the topology or the geometry revision = no tree. |
| C11 | Only a triangle-topology draw can have a tree; `prepareBvh()` on a line or point draw returns false and takes no tree. |
| C12 | A tree the engine builds is sealed, with leaf capacity `kPickTreeLeafSize` (64) unless the caller passes its own builder; sealing itself is not optional. |
| C13 | A marker whose style names no axis, or whose length is not positive, is a geometry with no channels and no primitive. |
| C14 | The marker prototype cache is exact-keyed, bounded (32 styles), immutable once published, and a style past the bound is built without being kept. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | Instances share material/layer/order; per-instance attributes wait for a real need. |
| R2 | Several placements = several `Geometry` nodes (which may share buffers). |
| R3 | Bounding-box / pick identity = `geometry + primitiveIndex + instanceIndex`. |
| R4 | Robot IO accepts `TriangleMesh` and float primitives only, with no double variant left behind. |
| R5 | graphics and meshio validate the mesh before consuming it and reuse the `toTriangleMesh()` conversion interface. |
| R6 | The model viewer's statistics and export reuse the explicit conversion; when the offsets are invalid the conversion must not access their face data. |
| R7 | A point cloud's normal/colour channels are consumed only when their length equals `pointCount()`, and are otherwise dropped rather than re-indexed (the same rule as N11). |
| R8 | When bulk rays are needed, build the tree once with `Mesh::buildBvh()` and then query, rather than scanning triangles linearly inside the picking loop. |
| R9 | The draw frame's tree is explicitly `prepareBvh()`ed before interaction starts; several draws over one index segment share one tree; `geometryFromMesh()` adopts the mesh's tree, and a triangulated Quad/Mixed adopts nothing. The first consumer is the model viewer: it calls it once per draw with ≥512 triangles when building the scene (the threshold is its policy, `kPickTreeTriangleCount`). |
| R10 | A coordinate-frame marker states its own white diffuse material, so its vertex colours are what reaches the frame; shading it like its content is the caller's explicit choice. |

### 5.3 Invariants (N#)

| # | Caveat |
|---|---|
| N1 | Changing bytes requires `Buffer::setRevision()`; bumping only the geometry revision keeps drawing the old bytes. |
| N2 | A group of edits announces the geometry revision once; a setter does not advance it itself. |
| N3 | The instance stream's upload key is the whole buffer (offset 0 / count 0); the visible prefix does not change the buffer. |
| N4 | Instance matrices multiply in **the draw's geometry local space**; all of one Geometry's primitives share the node's world matrix. |
| N5 | A Geometry can have only one parent; several placements use several Geometry nodes (which may share buffers). |
| N6 | A custom channel must not take a canonical location. |
| N7 | `POINT_LIST` requires the vertex stage to write `gl_PointSize` statically. |
| N8 | `TriangleMesh`'s `ShapeType` does not express indexed/soup; use `hasIndices()` to decide. |
| N9 | The triangulation result of Quad and Mixed is for consumers only and does not rewrite the original mesh data. |
| N10 | A triangle fan is unsuitable for concave polygons; when a concave face needs correct triangulation, take an input that preserves a correct triangulation in the first place. |
| N11 | A channel is consumed only when it has one item per vertex: when a normal/UV/colour length disagrees with the positions the consumer drops that channel, and `isValid()` does not become false because of it. |
| N12 | `Mesh::aabb()` is a cache: a position change (`setPositions()`) clears it, and before it is computed it is the empty box. |
| N13 | geometry advances no revision on its own: `set*` swaps the buffer object (a new address is a new cache key), a writer changing bytes in place uses `Buffer::setRevision()`, and a whole-shape change is reported by the holder through `Shape::bumpRevision()`; the three are not interchangeable (N2). |
| N14 | `PointCloud::isValid()` only asks "are there points", and `hasVolume()` is always false; `geometryFromShape()` does not take point clouds yet, so do not pass off a `Mesh` with no indices as one. |
| N15 | `Mesh::bvh()` is a cache: `setPositions()` / `setIndices()` / `MixedMesh::setFaceOffsets()` clear it, normals/UV/colour do not; an unbuilt tree returns no candidates and does not degrade into a full scan. |
| N16 | A published tree is immutable: `buildBvh()` publishes a new object when it rebuilds, and `clearBvh()` only stops publishing without touching the objects already handed out, so whoever holds `sharedBvh()` can never read a half-built tree. |
| N17 | `Geometry::bvh(draw)` only answers a tree whose "key still holds"; when the data moved without a new `prepareBvh()` it returns `nullptr` (picking falls back to the linear scan) rather than the old tree. |
| N18 | A tree changes no semantics, only the cost; the witness is `triangleTestCount()`, and the sealed callback's second parameter is the leaf box (`geometry-bvh.md` `R7`). |
| N19 | `AxesStyle::axis_mask` is a build parameter: hiding an arrow means a different marker, not a runtime toggle. |
| N20 | Markers of one style share streams, not draws or nodes: N independently controlled frames are N draws. A field that only moves together belongs in one geometry with an instance stream. |

## 6. Caveats and Traps

### 6.1 Polygon conversion

- How it fails: reading faces straight from the offsets or indices, where invalid input may access out of range.
- How to detect it: `isValid()` is false, or the last offset disagrees with the corner count.
- How to prevent it: the conversion entry point checks validity first; a concave polygon cannot be assumed to keep its shape under a triangle fan.
### 6.2 The triangle tree

- How it fails: querying a tree that was never built yields "no hit", which looks as if the ray missed the mesh.
  How to detect it: `hasBvh()` is false.
  How to prevent it: build once before querying, and treat `hasBvh()` as a precondition.
- How it fails: after the tree is built, changing the positions buffer's bytes in place around `setPositions()`, so the tree still describes the old positions.
  How to detect it: the hit position and the displayed position disagree.
  How to prevent it: call `clearBvh()` yourself after writing bytes in place (the same kind of manual announcement as `Buffer::setRevision()`, N13).
- How it fails: taking a candidate as the final hit.
  How to detect it: a triangle that did not actually intersect gets selected.
  How to prevent it: candidates are only used to skip; the decision still uses `intersectTriangle()` (N15).
- How it fails: pruning a `Mode::AllHits` query by nearest hit as well.
  How to detect it: after building the tree, `intersectSceneAll(..., AllHits)` on the same geometry returns fewer results than without a tree, while the nearest-hit query is still correct.
  How to prevent it: pruning belongs to the "nearest only" path alone; an all-hits path keeps the distance limit at infinity (F33).
- How it fails: the geometry's stream changed (buffer swapped, bytes changed in place with `setRevision()`, `bumpRevision()`) without a new `prepareBvh()`.
  How to detect it: `bvh(draw)` returns `nullptr` and `triangleTestCount()` is back to the order of the triangle count.
  How to prevent it: call `prepareBvh()` after editing the data and before interacting; a key mismatch only drops the tree and does not affect the answer (C10).
## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | An `InstanceScope` scope | Choosing the unit, recursive rays, nesting and bounding-box semantics are complex; it became a `Primitive` parameter. See [ADR](../decisions/ADR-2026-10-23-instancing-per-primitive.md). |
| D2 | Instance matrices as a reserved `Geometry` channel | One geometry would get one instance set; the visible count would have to change the slice; no engine attaches instances to a mesh asset. See [ADR](../decisions/ADR-2026-10-23-instancing-per-primitive.md). |
| D3 | Multiple parents / a DAG | The rendering path could be a DAG, but the public world query and the pick identity do not hold; copying the node suffices. See [ADR](../decisions/ADR-2026-10-23-no-multi-parent-dag.md). |
| D4 | Per-instance material / colour / layer / order / culling | One draw's instances share these; wait for a real need. |
| D5 | GPU indirect / automatic batching | No consumer; instancing only solves "N copies of one geometry". |
| D6 | Making the point cloud a `Mesh` subclass | `geometryFromShape()` and the exporters would see a mesh with no faces, and neither the name nor the behaviour would match (`G1`, `G4`); a point cloud is an independent `Shape` (F31). |
| D7 | Building a tree on `TriangleMesh` only | `QuadMesh` / `MixedMesh` are independent consumers (the model viewer, meshio) and could only query after `toTriangleMesh()` copied one (`G1`, `G10`); triangle identity therefore moved up to `Mesh` (2.8). |
| D8 | A tree in `float`, the same precision as the vertices | Query rays are `Rayd` in every consumer, and narrowing to float for box tests loses the precision a small leaf box needs far from the origin and misses hits (`G4`, `G10`); widening float vertices into a box is exact, so only the tree is double. |
| D9 | Materialising each triangle into a numeric type before building | Building has to sweep anyway, and materialising only adds a copy that must be invalidated in step with the mesh; `triangleCorners()` can compute it by rule (`G10`, `G13`). |
| D10 | Adding a `Role` / `GeometryRole` enum to `Geometry` to declare "this draw is a surface" | That concept is already called `Topology` (`isTriangleTopology()` is the single criterion, `G1`); a declaration disagreeing with the derivation is unobservable, whereas `prepareBvh()`'s return value and `bvh()`'s `nullptr` do not lie (`G11`); and the use changes at run time (a tree is only needed once selection mode is on). |
| D11 | `Geometry` holding a `Shape` back, borrowing the mesh's tree when picking | It would need four "the numbering is isomorphic" conditions re-checked on every pick, and a hand-built geometry would never have a tree; instead the geometry derives one per stream on demand (F33). |
| D12 | Copying the mesh's tree into the geometry / making the tree mutable and having the geometry hold a reference to the mesh | The same triangles would get two equally large trees (`G10`); a mutable tree would let a rebuild change an object another holder is reading. Instead they share an immutable object and a rebuild publishes a new one (N16). |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `PrimitiveInstancingTest` / `GeometryTest`. |
| T2 | A revision is part of the residency key: `VkResidenceWireTest.AVertexBufferThatAnnouncesANewRevisionIsUploadedAgain` / `GeometryTest.BumpRevisionMovesForwardFromWhereverItIs` / `BufferTest.SetRevisionStaysForBytesWrittenOutsideTheBuffer`; an unchanged revision uploads nothing (`VkResidencyTest.TheSecondFrameOfTheSameContentBuildsNothing`). |
| T3 | `MeshShapeTest` / `PrimitiveInstancingTest` / `VkVertexInputTest`. |
| T4 | The device-level `PrimitiveInstancingTest.AnInstancedPrimitiveDrawsEveryPlacement` (the stream is declared once per primitive: `EachPrimitiveOwnsItsInstanceStream`) + `VkBuiltinProgramTest.EveryInstanceIsDrawnWhereItsMatrixPlacesIt`. |
| T5 | `MeshShapeTest` verifies mesh storage and runtime types, including one positions buffer shared by two `Geometry`s. |
| T6 | Both indexed and sequential triangle-mesh vertices verify whole triangles; `MeshShapeTest`. |
| T7 | A Quad is split into two triangles along a fixed diagonal; `MeshShapeTest.QuadMeshUsesFourCornersAndTriangulates`. |
| T8 | Mixed offset validation and the triangle-fan output; `MeshShapeTest.MixedMeshValidatesOffsetsAndConvertsToTriangleMesh`, `MeshShapeTest.MixedMeshWithSeveralFacesConvertsEveryFace`. |
| T9 | The box cache and the revision contract; `MeshShapeTest.TheCachedBoxFollowsTheVertices`, `MeshShapeTest.TheRevisionIsReportedByTheHolderNotByASetter`, `PkgIOTest.EditedMeshIsRefusedWhenThePackageIsFinallyWritten`. |
| T10 | Point cloud: an empty cloud is unusable, three-scalar packing and buffer identity, independent channels, a position change clears the box cache; `PointCloudTest`. |
| T11 | Triangle indices in the same order as `toTriangleMesh()`, one primitive per triangle, ray-picking pruning, invalidation rules, no tree for an invalid mesh, accepting a foreign tree; `MeshBvhTest.cpp` (in `tests/test_geom_spatial`). |
| T12 | The draw frame's tree: a hand-built geometry can build one and use it for picking, hits and `triangleIndex` are the same with and without a tree, the triangle test count drops, the same key shares, mesh source geometry adopts, a triangulated Quad adopts nothing; `tests/test_graphics/GraphicsTest.cpp`'s `GeometryTest.*Tree*` / `RayIntersectionTest.APreparedTree*`. The model viewer prepares the trees of large draws when it builds the scene: `ModelViewerTest.ALargeMeshGetsItsPickTreePreparedWhenTheSceneIsBuilt`. |
| T13 | Markers: three arrows are one primitive and one draw, each tip reaches the styled length, each arrow carries its own vertex colour, a mask leaves the other arrows out, an empty style draws nothing, and two markers of one style share their vertex and index buffers while hiding one leaves the other visible; `AxesMarkerTest.*` and `AxisGizmoTest.BuildsOneAxesMarker`. |

## 9. Status and Evolution

### 9.1 Current status

Current design; features and interfaces are as the tables above.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items stay by trigger condition. The three closed boundary facts (32-bit indices
only, no primitive restart, no per-instance attribute beside the instance matrix) are stated with their triggers in 2.11
and are not backlog entries until a trigger fires.

### 9.3 Compatibility / migration

Indexed and sequential triangles are unified as `TriangleMesh`, and an `IndexedTriangleMesh` type is no longer offered;
meshes and primitives no longer offer a precision template, aliases such as `Meshf` / `Boxd` are deleted,
and new code is written against §4's interfaces. Old names appear only in git history.

`Mesh` declares the two virtual functions of triangle identity (`triangleCount()` / `triangleCorners()`, base-class defaults 0 / false) and the triangle tree
cache; `TriangleMesh::triangleCount()` is its override, with an unchanged signature and meaning (only that "defined by this type alone" became a base-class declaration).

## 10. References

`Mesh.hpp` · `TriangleMesh.hpp` · `QuadMesh.hpp` · `MixedMesh.hpp` · `PointCloud.hpp` · `MeshShapeTest.cpp` · `PointCloudTest.cpp` ·
`MeshBvhTest.cpp` · `geometry-bvh.md` · `graphics-scene.md` · `graphics-rendering.md` · `graphics-architecture.md` · `../backlog/graphics.md`
