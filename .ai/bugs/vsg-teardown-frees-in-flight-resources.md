# vsg backend: teardown released resources a submission was still reading

> **Criterion**: the Release `test_vsg` suite crashed about once in two runs with `malloc(): unaligned tcache chunk detected`
> or SIGSEGV, at a drifting location (`FrameUploadsTest` and `PrepareUploadTest` once each); the Debug suite rarely triggered
> it; an older commit triggered it as well.
> **Conclusion**: the vsg backend's **resource lifetimes were out of step with its submissions** - three releases happened
> after the GPU/driver threads were still reading - and all are fixed (see section 4); after the fix the memory checker
> reports 0 `Invalid read` on the same suites.
> **Authoritative sources**: `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp` (teardown order),
> `OffscreenSession.cpp` and `Session.cpp` (frame graph turnover; now `WindowSession.cpp`) <!-- drift-ok -->,
> `include/vine/backend_vsg/internal/GraphLifetime.hpp` (keep-alive); the memory checker's raw stacks are in section 2.

## 1. Symptom

- The full Release `test_vsg`: about one run in two crashed in libc's heap check
  (`malloc(): unaligned tcache chunk detected`) or with SIGSEGV, at a different place each time (`FrameUploadsTest`,
  `PrepareUploadTest.ASecondPrepareCopiesNothing`, ...).
- Rerunning a single case was almost always green; the full Debug suite was mostly green.
- `git switch` to a commit before the change and rebuilding the binaries reproduced it **as well** => no single change
  introduced it.

## 2. Evidence (valgrind, Release binaries)

`valgrind --error-exitcode=9 --num-callers=16 ./build-release/bin/test_vsg` (with the window suite filtered out; valgrind
itself segfaulted after 416 cases) reported five `Invalid read`s, **all** of them a worker thread of `libvulkan_lvp.so`
reading memory that had already been `free`d, with the free stacks inside this process:

1. **The previous frame graph was dropped** (`FrameUploadsTest`): the reader is a lavapipe worker thread; the freer is
   `vsg::Buffer::~Buffer <- ... <- vsg::RenderGraph::~RenderGraph`, triggered by `task->commandGraphs = graphs;` inside
   `OffscreenSession::assignFrameGraphs()` - the previous frame's graph lost its last reference at that moment.
2. **Teardown released the material images** (`PrepareUploadTest.ASecondPrepareCopiesNothing`): the freer is
   `vsg::ImageView::~ImageView <- MaterialImages::Data::~Data <- VsgBackend::releaseContentWorld() <- shutdown()`.
3. The other three are other read points of the same story (same thread stacks, same batch of blocks).

## 3. Root cause

**This backend advances frames as a pipeline** (`core::FrameTimeline` + slot ring + `FrameProgress`), so "the previous
`executeFrame` returned" does not mean "the queue is empty"; and Vulkan's lifetime rule is that **memory a submission still
references must not be destroyed** (the VUID family about destroying device memory / buffers / images still in use). The
backend itself **never** calls `waitForFences` (confirmed by a repository-wide grep), and resource release relies entirely on
the slot-reclaim "evidence" - but the **frame graphs and the content world were not released by that evidence**:

- `assignFrameGraphs()` replaced `commandGraphs` outright and dropped the previous frame's graph (whose node destructors
  free driver memory);
- `shutdown()` called `releaseContentWorld()` outright while the session could still have frames in flight.

## 4. What was fixed / what is left

**Fixed (two places, both by making the evidence the session's):**

1. `VsgBackend::shutdown()` makes every session `waitDeviceIdle()` **before any destruction** (the offscreen public method
   plus the window session's `SessionContentAccess::waitDeviceIdle`), and counts that wait into
   `RetirementQueue::noteDeviceWait()`.
2. New `detail::GraphLifetime` (`include/vine/backend_vsg/internal/GraphLifetime.hpp`): a session moves the `commandGraphs`
   it is about to replace into a keep-alive list and drops them frame by frame according to
   **`core::FrameTimeline::completedFrame()`** (the completion evidence slot reclaim provides, the same set content
   retirement uses); both sessions (window and offscreen) now build `assignFrameGraphs` this way.

**Measured effect**: the full Release suite ran **4 out of 4 green** (about 50% crashed before).

**The third place (fixed, later on 2026-11-15)**: the **prepare-only** path of `PrepareUploadTest` - it calls `shutdown()`
right after `prepareFrame()`, while `OffscreenSession::waitDeviceIdle()` returned early on
`timeline().submittedFrame() == 0`, so "no frame was submitted" counted as "nothing is running". The truth is that prepare
creates pipelines and submits its transfer batch (the ADR allows it), and the work it drives can outlive that call. Fix:
**replace the early-exit criterion with "has this session created any work"** - `beginFrame()` sets it, and when
`prepareFrame` prepares through that session the backend calls `notePreparedWork()`; `waitDeviceIdle()` only waits when that
fact is true (the wait is counted into `deviceWaits()`). This fixes prepare-only teardown and keeps the honest count that a
session which never created work does not pretend to have waited (`OffscreenSessionTest.TheDestructiveWaitIsCountedOnce`
pins exactly that). Verification: `valgrind` on `PrepareUploadTest.*` went from four `Invalid read`s to **0**;
`*FrameUploads*:*PrepareUploadTest*:*BlockStorageTest*:*ContentPassTest*` (44 cases) showed 0 errors under valgrind.

## 5. Why the validation layer cannot see it

Validation checks **references between Vulkan objects**; here **host memory** (the bookkeeping blocks the driver allocates
in `vkAllocateMemory`/`vkMapMemory`) was `free`d by the host while the driver thread kept reading it - the object handle
relations are perfectly legal. Only a memory checker (or luck) can see that. It also explains "zero VUIDs but it crashes".

## 6. Lessons

- **"A frame returned" is not "the queue is empty"**: every release path in a pipelining backend must be able to say who
  proves the resource is done. The evidence this project already has is slot reclaim (`FrameTimeline::completedFrame()`),
  and a new resource class must join that same evidence instead of inventing its own.
- **The shutdown path must use the same evidence as the steady state**: idling before `shutdown()` is cheap; the steady state
  relies on keep-alive and teardown on idle, and neither requires sacrificing the pipeline.
- **"The counter is zero" is not "nothing is running"**: `submittedFrame() == 0` was taken as evidence of idleness, while
  prepare submits transfers and builds pipelines. A wait saved by an early return bought a heap corruption that only shows
  on the prepare-only path - **the condition for waiting must be "can anything still be read", not "do I remember a frame
  happening"**.
- **Run the memory checker on the tree that crashes**: ASan (Debug) ran three times green while valgrind on Release caught
  five places in one run - for timing-related defects, rule out overruns with ASan first, then run valgrind on the build that
  really crashes.
