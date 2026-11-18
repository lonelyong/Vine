# ADR-2026-10-23: L2a is reverted, layered targets wait for L2b and land as one piece

- Status: Accepted
- Date: 2026-10-23
- Related: `../design/graphics-rendering.md`, `../backlog/graphics.md`

## Background

L2a (array targets, per-layer framebuffers, per-layer readback) landed and was reverted the same day. The question was whether, with no consumer, the shape "one pass per layer" was worth keeping.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep L2a | Cube shadows, CSM and stereo could write per layer | Public surface plus combination refusals; no consumer |
| B. Revert L2a and keep only the L2b trigger design | The code returns to a simple shape | No layered capability for now |
| C. Do L2b straight away | Several layers in one pass, less collection and submission | Needs array layers plus a shader writing the layer index; the prerequisite is not done |

## Decision

Chose **B**: L2a is reverted; N 2D targets plus N passes is the current shape. When VR or single-pass multi-view actually lands, L2a and L2b are redesigned **as one piece**.

## Consequences

- Good: about 1150 lines across 40 files and four combination refusals are deleted; the L1 content mask, instancing and the existing 2D target and readback paths stay.
- Cost: no array layers and no per-layer readback; it has to be redone when needed.
- Reopening condition: VR or single-pass multi-view, or a measured N-pass cost becomes the bottleneck.

## Evidence

- After the revert `test_graphics` 448, `test_vsg` 503.
- No production code calls `setLayerCount > 1`; only new tests and documentation did.
- Recorded in `../backlog/graphics.md` T4.
