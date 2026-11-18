# ADR-2026-11-13: upload staging keeps a reusable block and still gives a one-off large one back to the device

> **Status**: Accepted
> **Date**: 2026-11-13
> **Supersedes**: [ADR-2026-11-10-upload-staging-is-not-retained.md](ADR-2026-11-10-upload-staging-is-not-retained.md)
> (changing one clause only: no longer "retain nothing")
> **Scope**: `src/plugins/gfx_backend_vsg/{include,src}/internal/TransferStaging.*`,
> `src/viz/graphics/sdk/vine/graphics/FrameStats.hpp` (the `FrameUploads` of the same batch),
> `tests/test_vsg/UploadScratchTest.cpp`, `.ai/design/{graphics-content-lifecycle,vsg-backend}.md`

## Background

The conclusion of ADR-2026-11-10 was "release everything as soon as staging is done": a staging block is the source of one `vkCmdCopyBuffer`, and it is
dead weight the moment the copy's fence completes. That brought the 704 MB body model's 1,228 MB residency down to zero, with zero waiting and zero idle.

The **cost** of that rule was written down at the time: a host that uploads every frame reallocates a staging block of at least 16 MB every frame. The repository never had such a host
(an in-frame upload happens only when a revision changes), so the cost was never incurred. T26 decided to pay it in advance: **keep the ones below the limit, release only the large ones**.

## Decision

1. `StagingTask::releaseStaging()` releases only blocks with **`staging->size > minimumStagingBufferSize`**; blocks equal to or below that
   limit stay. The limit is read from the task's own `minimumStagingBufferSize` (vsg's `TransferTask`, 16 MiB by default), not
   hard-coded as a constant.
2. The rest of the evidence rule is unchanged: the block's own `fence->status()` is still the only condition; an unfinished copy is left to the next frame; zero waiting, zero idle, zero park.
3. The result: a host that uploads every frame does **zero allocation** in steady state (vsg's `transferData()` reuses the kept block directly); a one-off large upload (loading a model)
   still gives the large block back - the 1,228 MB class of problem does not come back.

## Rationale

- `G10`: the cost of "one 16 MB allocation per frame" is not the same order as "resident memory", but it is the only one a one-line rule can remove. Keeping
  a reusable block makes both curves look good: zero allocation in steady state, no residency at the peak.
- `G4`: the condition is **the block's size**, not "is there still data to copy" - the latter looks cheaper, but `_transferData()` returns early when there is nothing to copy
  without clearing the map, so the condition would be true forever and the sweep would freeze (the trap from ADR-2026-11-10, still present).
- `G8`: no entry point is opened for "how much the host wants to keep"; this is part of the reclaim rule, not a policy.

## Rejected options

| Option | Rejection reason |
|---|---|
| Keep "release everything" | A host that uploads every frame pays one 16 MB allocation per frame; a one-line rule removes it without affecting the release of large blocks. |
| Keep every block, no size test | The blocks of a large upload (loading a model) would stay resident - exactly the problem ADR-2026-11-10 solved. |
| Use "does this `DataToCopy` still have data to copy" as the skip condition | `_transferData()` returns early without clearing the map, so the condition is always true ⇒ the sweep freezes and the whole load's staging stays resident. |
| Let the host configure how much is kept | No consumer (`G8`); the reclaim rule should not become another knob. |

## Consequences

- **Memory**: after small uploads `uploadStagingBytes()` stops at a multiple of the limit (one block per frame slot that has been used, at most `slots × 16 MiB`),
  instead of dropping to zero; after a large upload it falls back to that limit.
- **Performance**: a host that uploads every frame does zero allocation in steady state (previously one malloc + map per frame). The behaviour of a one-off load is unchanged.
- **Evidence**: `UploadScratchTest.TheUploadScratchIsNotRetainedAndTheFramesGiveItBack` now asserts "the live blocks are all multiples of a reusable
  size, and a one-off block larger than the limit is given back"; the T26 case is in the same commit.
- **API**: none (`StagingTask` is still an internal type; `minimumStagingBufferSize` is still vsg's).

## Trigger conditions (when to change it again)

A real host appears where "the blocks kept in steady state still hold an unacceptable amount of video memory / host memory": then a new ADR decides whether a
configurable ceiling is needed, rather than going back to "release everything".
