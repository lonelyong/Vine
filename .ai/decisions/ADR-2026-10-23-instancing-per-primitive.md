# ADR-2026-10-23: instancing hangs off `Primitive`, not off a scene-graph scope

- Status: Accepted
- Date: 2026-10-23
- Related: `../design/graphics-geometry.md`, `../backlog/graphics.md`

## Background

"the same geometry N times" had three possible shapes: the instance matrix as a channel of `Geometry`; an `InstanceScope` node; or a parameter of one draw.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. A `Geometry` channel | Unified with the vertex streams | One geometry gets only one set of instances; the visible count needs a different slice; no engine mounts instances on a mesh asset |
| B. An `InstanceScope` scope | The same shape as vsg's `InstanceNode`; several parts share it | Pick unit, ray recursion, nesting and bounding-box semantics all become complicated |
| C. A `Primitive` parameter | One draw is one instancing batch; the same layer as topology/index/material | Several placements mean several `Geometry` nodes |

## Decision

Chose **C**: `Primitive` gains `instance_transforms` (16 floats per instance, column-major mat4) plus `visible_instances`; collection emits rows per primitive; the backend uses binding 4 and `VN_INSTANCED`; there is no `InstanceScope`.

## Consequences

- Good: the pick identity is uniquely `geometry + primitiveIndex + instanceIndex`; the instances of one draw share material, layers and sort key; the visible prefix is not re-uploaded.
- Cost: per-instance material, colour, opacity, layer, sorting and culling, GPU indirect and automatic batching are all out of scope; several placements need several `Geometry` nodes (which may share buffers).
- Reopening condition: per-instance attributes or GPU indirect gain a real consumer.

## Evidence

- `PrimitiveInstancingTest` (single and multiple primitives, the visible prefix, the bounding box, culling, picking).
- The device-level `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`.
- The earlier `graphics-instancing-channel` / `graphics-instancing-scope` proposals are dropped; their history is in git.
