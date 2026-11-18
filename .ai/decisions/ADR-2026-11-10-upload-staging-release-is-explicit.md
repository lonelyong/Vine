# ADR-2026-11-10: releasing upload staging is an explicit entry point, triggered by the host (or a user command)

> **Status**: Superseded (the same day's [ADR-2026-11-10-upload-staging-is-not-retained.md](ADR-2026-11-10-upload-staging-is-not-retained.md)
> changed it to "release once it is done, at the frame boundary, automatically": nobody called the explicit entry point on a host that stops producing frames after loading,
> and an expiring park only landed through "ask again")
> **Date**: 2026-11-10
> **Scope**: `src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`, `RenderEngine.hpp`,
> `src/viz/graphics/src/RenderEngine.cpp`, `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp` (and the header of the same name),
> `tests/test_graphics/HeadlessBackendTest.cpp`, `tests/test_vsg/UploadScratchTest.cpp`,
> `.ai/design/graphics-content-lifecycle.md`, `.ai/design/vsg-backend.md`, `docs/guides/graphics.md`

## Background

When vsg uploads a static array, it first copies the whole upload's bytes into a `HOST_VISIBLE|HOST_COHERENT` staging block (`TransferBlock::staging` of the
`TransferTask`), and only then `vkCmdCopyBuffer`s them into device-local memory. That staging block only ever grows, is never given back after the copy, and never goes through any pool,
so `releaseUnusedPools()` cannot reach it: one upload of a 14M-triangle model adds 1.2 GB of working set to the process (the same number is readable from the GPU counter's
`SharedUsage`).

The mechanism had already landed and been measured: replacing that `TransferTask` gives the staging back to the system (`detail::replaceTransferTask`, see
F16.2 of `vsg-backend.md`), at the cost of a 16 MB reallocation on the next upload. What had to be decided this time is **who calls it and when**, since it was then written into
`VsgBackend`'s frame driver, switched by the environment variable `VN_VSG_KEEP_UPLOAD_SCRATCH`. Three problems:

1. **The timing lives inside one backend.** Neither `RenderEngine` nor the SDK knows it exists; a second backend (D3D12/WebGPU have their own upload heaps / staging too)
   would have to reinvent it, and forgetting it means holding the memory forever while the picture looks perfectly fine (`G2`: the layering is broken; `G5`: the same thing has to be rewritten per backend).
2. **The host has no entry point.** An environment variable cannot be decided in production code, and it has global semantics.
3. **The original trigger condition was itself wrong (measured).** What was hung on it was a "wave": after a frame with an upload, reclaim on **the first frame with no new upload**. But a host redraws on demand
   — a viewer goes still once the model is loaded, and that frame **does not exist**. Measured: after `Vine.exe` loads a model, the process sits at 1% of one core / 0% GPU with the 1.2 GB of staging resident for minutes,
   and `device_waits` never moved: the feature looked implemented and never ran once.

## Decision

1. **The mechanism enters the SDK, with one verb**: `RenderDevice::releaseUploadStaging()` (returning false by default, meaning "this backend has no staging to give back").
   Contract: call it between frames, it returns whether this call swapped the staging out, and it **does not block** (see 5).
2. **The engine does not forward it**: there is no `RenderEngine::releaseUploadStaging()`. The host can already reach the device (`RenderEngine::device()`),
   and a one-hop entry point would only give one thing two names (`G4`).
3. **The timing belongs to the host / user**: the SDK provides **no** automatic timing and no switch. The host calls it where it belongs: at the end of a load, on closing a document,
   when the window is minimised, from a user command, on a memory-pressure signal - only the host / product knows those.
4. **The backend only answers "can it"**: `VsgBackend` deletes its own timer and wave state and the environment-variable read, and becomes an implementation of this verb
   (ask **the session's own upload task** whether it holds staging → swap the task if needed; no longer comparing a device-level upload counter, see 7).
5. **No blocking: the swap is immediate, destruction waits for evidence.** Swapping the `TransferTask` is one pointer exchange, and the old task is handed back; when its staging
   may be destroyed is decided by evidence - a timeline proving no frame is in flight (`submittedFrame() == completedFrame()`) destroys it right away, otherwise it is **parked into the
   retirement queue** and released by the frame that completes that slot (the same path as `releaseContent()`). Only when the queue cannot park (the session never learned its in-flight
   frame count) does it degrade to one counted device idle.
6. **Asking again means "now".** A park's retirement point is `slots + 1` frames after the submission point, and the completion watermark lags the submission by another `slots`, so it only expires after
   `2 × slots + 1` frames (vsg's 3 slots = 7 frames) - a host that produces no more frames after loading never gets there. So a later call does: one counted
   `deviceWaitIdle()` (that is the evidence) → `RetirementQueue::drain()` runs every remaining park → returns true. A host that keeps producing frames pays nothing.
7. **"Is there anything left to give back" is answered by each session's own task, not by a device-level counter.** When a session starts it installs `detail::StagingTask`
   (derived from `vsg::TransferTask`, reading the protected `TransferBlock::staging` to report the byte count). The old approach compared
   `ContentAssembly::uploads().uploads()` with `MaterialImages::count()`: that is the **device's** accounting, not the session being asked -
   with several sessions, the one whose swap succeeded advances the count, and the other ("the copy is still queued, this call did not swap") is never told again; its staging waits for the
   next upload to be reclaimed (a narrow, self-healing trigger, but what is missed is one whole model's staging). Deriving is the only way to read it without patching vsg, at the cost of depending
   on three protected names: an upstream rename is a compile error, and a semantic change is caught by the tests' direct byte-count assertions (`vsg-pitfalls.md` P42.2).

## Rationale

- `G1` / `G6`: staging reclaim and `releaseContent()` follow **one rule** - an explicit entry point, where whoever owns the object decides its lifetime. Inventing a separate automatic policy for it
  would set a second precedent for what "release" means.
- `G2` / `G5`: the backend is the only party that knows whether there is anything left to give back at the moment of the call, and there is no third place that can drift; the engine does not take part,
  so the resource question has one name.
- `G8`: no consumer has ever **asked for** an automatic policy - a host can write one line itself for the same effect, while defaulting it on would make every uploading frame pay a device idle for nothing.
- `G4`: the verb returns a bool meaning "did this call give it back" (the same reading as `releaseContent()`); there is no switch to understand.
- `G10`: it does not block the caller. One `deviceWaitIdle` on the UI thread freezes the interface, while correctness only needs "evidence before destruction" - evidence that a timeline /
  park provides for free, paying one counted idle only when the in-flight frame count is unavailable.

## Rejected options

| Option | Rejection reason |
|---|---|
| Leave the policy in the backend's frame driver | Neither the engine nor the SDK can see it; a second backend would rewrite it (`G2`/`G5`). An environment-variable switch also cannot be decided from production code. |
| The "wave": reclaim once a frame has no new upload | Measured: that frame does not exist (the host redraws on demand, 1% of one core / 0% GPU); it never runs at the moment reclaim matters most. |
| The engine calls it once per frame automatically (on by default, can be turned off) | A host writing one line beside its frame loop gets the same effect; on by default makes every uploading frame pay a device idle that the host never asked to pay (`G8`). |
| Add a forwarding entry point on the engine | The host already has `engine->device()`; a one-hop entry point is two names for one thing (`G4`). |
| Let the engine read the per-frame upload count and decide | It turns "should we call it" from one counter comparison into one counter comparison, and adds an SDK field nobody reads (`G8`). The per-frame upload count goes into backlog T25 as an **observation** metric. |
| The backend reclaims inside `executeFrame()` itself | A backend's frame path should carry no resource policy; and it cannot see whether the host still needs that memory. |
| Reclaim asynchronously from another thread / a timer | Destroying staging touches Vulkan objects and command buffers, and doing it off the frame thread needs either a lock or accepts a race; one idle is already enough. |
| Request-style: only record "to be cleaned", leave the real cleaning to the next frame | "The next frame" may never happen (a host redraws on demand): recording is not releasing, and that was this feature's earliest failure mode. What was actually adopted is **swap immediately + park for evidence**: it neither blocks nor hides "not released yet" in state. |
| The environment variable as the entry point or switch | It cannot be decided in the product from code or a user action, and it has global semantics. The engine / backend no longer read it. |
| Use the device-level upload count to decide "is there anything left to give back" | The count answers about the device, not about the session being asked (see decision 7): one session's successful swap overwrites another's answer to "nothing", and what is missed is that whole model's staging. |
| Keep a count snapshot per session instead of asking the task | It does not miss anything either, but it answers "did the device's uploads move" rather than "do I hold a block": it swaps for nothing (creating a task nobody needs) and cannot obtain the byte count. Asking the task is a true statement, a snapshot is an approximation. |

## Consequences

- **Memory**: one host call brings it back down (measured on 14M triangles: that 1.2 GB no longer stays resident). **Not calling leaves it resident** - that is the cost of an explicit entry point,
  and the boundary of this decision (F18 / 6.3 of `graphics-content-lifecycle.md`).
- **Performance**: the request itself does not block (one pointer exchange plus one read of "does the task hold a block"); destruction happens either immediately (no frame in flight) or in the next frame's
  retirement queue. Only a session that cannot learn its in-flight frame count pays one counted idle, plus the counted idle "ask again" uses to drain the parks.
- **ABI**: `RenderDevice` gains one **non-pure** function (appended at the **end** of the interface's virtual function list, so existing slot numbers do not move); `RenderEngine` is unchanged.
  Plugin and SDK must build together (as this repository always does); the old `gfx_backend_vsg.dll` in an install tree must be rebuilt, or the vtables do not line up.
- **Behaviour**: a backend that does not implement the verb returns false from `releaseUploadStaging()`; the host does not need to tell "the backend has no staging" from "nothing to give back this time".
- **Tests**: `UploadScratchTest` (frames do not idle; an explicit call → true with no rise (park); another call → true and exactly +1 (`drain()`); another → false with no rise;
  after a new upload → true; 10 frames in a row → the park drains for free),
  `HeadlessBackendTest.TheUploadStagingIsReleasedThroughTheDeviceTheEngineBorrows` (the engine does not forward it; the call goes through `engine->device()`),
  and at the mechanism layer `OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands` (including the byte-count assertion on `stagedBytes()`)
  and `AReplacedTransferTaskLeavesTheSessionSubmitting` (a queryable task is installed from the start; asking with no upload → false and no idle).
- **Documents**: `graphics-content-lifecycle.md` 2.8 / F16–F22 / C4 / R4 / N7–N11 / 6.3 / D6–D15 / T5–T8;
  `vsg-backend.md` F16.2 becomes "the backend implements the verb, the timing is the host's, the criterion asks the task"; `vsg-pitfalls.md` P42.2;
  `docs/guides/graphics.md` loses the environment-variable line.

## Follow-up

- The call sites on the host side (these are product decisions, not fixed here): model load complete, document close, window minimise, and one user command
  ("release memory").
- The per-frame upload count as an observation metric (`FrameCounters::uploads`): do it when the HUD or a gate really reads it, see
  `../backlog/graphics.md` T25.
