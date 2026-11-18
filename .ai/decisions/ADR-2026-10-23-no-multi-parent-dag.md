# ADR-2026-10-23: no multiple parents (no DAG)

- Status: Accepted
- Date: 2026-10-23
- Related: `../design/graphics-scene.md`, `../design/graphics-geometry.md`, `../backlog/graphics.md`

## Background

The DAG half of R7: `Group::addChild` currently detaches a child from its old parent, so the scene graph is a strict tree. The question was whether a `Node` should be allowed several parents, sharing a whole subtree rather than a geometry buffer.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. A full DAG (the OSG/vsg shape) | Anything can be shared; editing once affects every placement | `worldMatrix()`, `boundingBox()` and the pick identity lose a unique answer; a `NodePath` is needed; caches, invalidation and lifetimes all change |
| B. Explicit reference nodes (the USD shape) | Ordinary nodes keep single-parent semantics; placement identity is explicit | A new type plus the expansion of four walks; a holder invalidation chain |
| C. A strict tree plus instancing | No risk; "the same geometry N times" is already covered | A repeated subtree has to duplicate nodes; an edit does not propagate |

## Decision

Chose **C**: `Group::addChild` keeps "detach from the old parent and refuse a cycle". "The same geometry N times" uses `Primitive` instancing; "the same subtree N times" duplicates nodes (the `Buffer` is still shared). If a real need for "the same subtree N times, with one edit reaching every placement" appears, redesign along B instead of loosening `Group`.

## Consequences

- Good: `parent()`, `worldMatrix()`, `boundingBox()` and the pick result stay unique; the bounding-box cache and the content epoch keep holding along a single-parent chain.
- Cost: a repeated subtree has no "edit once, applies everywhere"; nodes are duplicated.
- Reopening condition: a real need to edit a shared subtree (not "the same geometry placed several times").

## Evidence

- The DAG of both OSG and vsg comes with a `NodePath` (the pick and selection identity); the vendored vsg 1.1.16 `Node` does not even store a parent pointer, and the path lives only inside a traversal.
- The single-parent assumptions in the code: `Node::worldMatrix()`, `invalidateBounds()`, `BoundsCache`, `RayIntersectionResult`.
- Recorded in `../backlog/graphics.md` T1-T2; R7 was closed on 2026-10-23.
