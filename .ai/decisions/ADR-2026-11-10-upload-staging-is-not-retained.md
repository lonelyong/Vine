# ADR-2026-11-10: upload staging is not retained - it goes back as soon as the copy's fence completes, decided at the frame boundary

> **Status**: Superseded (2026-11-13, see [ADR-2026-11-13-staging-keeps-the-reusable-block.md](ADR-2026-11-13-staging-keeps-the-reusable-block.md):
> the release-once-finished evidence rule is unchanged, but **a block at or below the reusable floor is kept**, and only a one-off block larger than it goes back)
> **Date**: 2026-11-10
> **Supersedes**: [ADR-2026-11-10-upload-staging-release-is-explicit.md](ADR-2026-11-10-upload-staging-release-is-explicit.md)
> **Scope**: `src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp` (the verb is deleted),
> `src/plugins/gfx_backend_vsg/{include,src}/internal/{TransferStaging,VsgBackend,SessionContent,OffscreenSession}`,
> `src/plugins/model_viewer/src/{ModelRenderView.hpp,ModelViewerPlugin.cpp,commands/ReleaseMemoryCommand.*}` (deleted),
> `tests/test_vsg/{UploadScratchTest.cpp,OffscreenTargetTest.cpp}`, `tests/test_graphics/HeadlessBackendTest.cpp`,
> `.ai/design/{graphics-content-lifecycle,vsg-backend,vsg-pitfalls}.md`

## Background

vsg copies the bytes of one upload into a `HOST_VISIBLE|HOST_COHERENT` staging block (`TransferTask::transferData()` does
`if (!staging || staging->size < totalSize) staging = createBufferAndMemory(...)`, which only ever grows), and **never gives it back** afterwards.
Measured: the 704 MB body model left `SharedUsage = 1,228 MB` in the process - exactly `2 × 614 MB`, because uploads rotate by
`dataToCopy.frameIndex`, so two uploads land on two blocks and both grow to 614 MB and stay
(`TransferTask` has `2 × numBuffers = 6` blocks, each with its own `staging` + fence + command buffer + command pool).

The earlier ADR of the same day made it an **explicit entry point** (`RenderDevice::releaseUploadStaging()`, triggered by the host or by a user command),
on the reasoning that "only the host knows when this memory is no longer needed". After it landed, two problems showed:

1. **The most common host in the product does not call it**: a viewer that goes still after loading never reaches "the next frame", and the automatic "wave" rule (wait one frame with no upload)
   does exactly the wrong thing in the one place it mattered most; so the 1.2 GB stayed, and all the user saw was "the command did nothing" (that time the park had not expired yet).
2. **One verb carried two things**: the park's retirement point is `slots + 1` frames after submission, and the completion watermark lags by another `slots`, so either you wait
   `2 × slots + 1` frames, or "ask again" pays one `deviceWaitIdle()` to drain it.

## Decision

1. **Staging is not retained.** A staging block is the source of **one** `vkCmdCopyBuffer` and has no second reader; once the copy is done it is dead weight.
2. **The evidence is that block's own fence.** `block->fence->status() == VK_SUCCESS` means "the copy that read it has completed", which is stronger than a timeline window,
   and it **does not wait**: a timeline needs frames, park needs a window, device idle stops the device - a fence is one `vkGetFenceStatus`.
3. **The trigger is the frame boundary, not the host.** `VsgBackend::executeFrame()` runs `releaseFinishedUploadStaging()` once at the very start:
   any block whose fence has completed and whose `DataToCopy` has no data left to copy has its `staging` and `buffer_data` cleared
   — exactly the state vsg itself produces when it rebuilds a block that is too small. The host does nothing and needs to know nothing.
4. **The SDK drops `RenderDevice::releaseUploadStaging()`**, and the model viewer's `release_render_memory` command is deleted with it.
   With no "when does it go back" question, no entry point is needed for the host to answer it (`G8`: no extension point without a consumer).
5. **Derive a vsg task to read and clear that state.** `detail::StagingTask` (`vsg::Inherit<vsg::TransferTask, StagingTask>`) can see and clear each block's `staging`, `buffer_data` and `fence`
   inside `_earlyDataToCopy` / `_lateDataToCopy`; when a session starts, `installStagingTask()` replaces the task vsg built (nothing is in flight then, so the swap costs nothing).
   `gfx_backend_vsg` has always been a vsg adapter, and its internal conventions - `numBuffers = 3`, the manual `transferQueue` hand-over and the `device->transferTask`
   observer - have always been written into this layer.
6. **Do not clear a block that is still in flight.** A block whose fence has not completed is left to the next frame (the frame boundary must not block); a block whose `DataToCopy`
   still has data to copy is skipped too (the next `transferData()` would allocate again, so clearing early only loses an allocation for nothing).

## Rationale

- `G1` / `G7`: one rule - the evidence has arrived, so it goes back. The explicit-entry version bundled "should it go back" and "can it go back now" into one verb,
  and then needed "ask again" to express "now".
- `G10`: the cost difference is real. When the host is not producing frames, the explicit version could only `deviceWaitIdle` (stopping the whole device); now there is zero waiting and zero idle,
  and `RetentionStats::device_waits` never moves on this path (the tests assert it directly).
- `G8`: with no consumer wanting the "now" semantics, the verb has no reason to exist; nor does the automatic path need a switch - it is equivalent to "release when done",
  not an optional policy.
- `G11`: failure is visible. Staging occupancy has a readable number (`VsgBackend::uploadStagingBytes()` /
  `OffscreenSession::stagedBytes()`), and the tests assert it reaching zero instead of inferring it from a falling working set.

## Rejected options

| Option | Rejection reason |
|---|---|
| Keep the explicit verb plus park/drain (the earlier ADR) | The most common host (load, then do nothing) does not call it; park needs `2 × slots + 1` frames and still needs "ask again = now" on top. One feature, two sets of semantics. |
| Clear every frame without waiting for the fence | That destroys memory a copy is still reading. |
| Clear only blocks larger than `minimumStagingBufferSize` (keep a 16 MB reusable block) | It saves one allocation for a host that uploads every frame, but it turns "release when done" into "release when big enough", one more test and one more line to explain; the repository has no host that uploads every frame (see the backlog trigger). Add it when one appears - one line of test. |
| Delete elements from `TransferBlock` / replace the whole task | Elements cannot be deleted (`*(frames[frameIndex])` dereferences directly), and replacing the task throws away the command pool / command buffer / fence together and makes the three hand-overs correct again. |
| Do not derive; "replace the task once the upload completes" instead | That is the previous mechanism: it can give the block back, but knowing "the copy has finished reading" needs park (frames) or `deviceWaitIdle` (stops the device). |
| Hang the reclaim on "the first frame after the upload has no upload" | Equivalent to the frame-boundary rule but weaker (one frame later, plus a counter comparison); the frame boundary already is that place. |
| Start a thread / timer polling the fence | Destroying a Vulkan object has to happen on the frame thread; a timer only trades the race for a lock. |

## Consequences

- **Memory**: the staging of one upload lives until "the first frame boundary after its fence completes". The 704 MB body's 1,228 MB → **0**
  (the other two parts of the process working set - the model's own host copy at ~700 MB and the process baseline at ~760 MB - are outside this path).
- **Performance**: at most `≤ 6` more `vkGetFenceStatus` calls per frame (nanoseconds); zero idle, zero park, zero waiting.
  **Cost**: a host that uploads every frame reallocates a staging block (16 MB at minimum) every frame - see the backlog trigger.
- **Depends on vsg internals**: reading and writing `_earlyDataToCopy` / `_lateDataToCopy`, `TransferBlock::staging`, `buffer_data` and `fence`.
  An upstream rename is a compile error (loud); an upstream semantic change is caught by `OffscreenSessionTest`'s byte-count assertions (`vsg-pitfalls.md` P42.2).
- **ABI**: `RenderDevice` has one virtual function fewer (the slot added in this round disappears with it), and `RenderEngine` is unchanged.
- **Behaviour**: the host no longer needs to (and cannot) start a reclaim; `RenderDevice`'s interface goes back to "declare resources and run frames", with no "clean up one upload" verb.
- **Tests**: `UploadScratchTest.TheUploadScratchIsNotRetainedAndTheFramesGiveItBack` (frames do not idle; the frame after the upload drives the number to zero;
  redrawing the same content uploads nothing; a new stream uploads again and later frames drive it to zero), `OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands`
  (the 256 MB working set falls back + a 4 KB byte-for-byte read-back + the byte-count assertion), `OffscreenSessionTest.AReplacedTransferTaskLeavesTheSessionSubmitting`
  (a queryable task is installed from the start, and producing frames raises no idle).
- **Documents**: `graphics-content-lifecycle.md` §2.8 / F16–F22 / C4 / R2 / N7–N11 / 6.3 / D6–D13 / T5–T8;
  `vsg-backend.md` F16.2 / T4; `vsg-pitfalls.md` P42.2.

## Follow-up

- There is no call site on the host side any more. A host that wants to observe reads the GPU counter's `SharedUsage`, or the backend's own `uploadStagingBytes()`.
- If a "uploads every frame" host appears (dynamic geometry, a streaming point cloud) and allocation shows up in a profile: add back the one test "clear only blocks larger than the minimum".
