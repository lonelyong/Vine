# ADR-2026-11-24: completion belongs to the submission, not to the window

- Status: **accepted** (2026-11-24).
- Date: 2026-11-24
- Scope: `DeviceClock` / `FrameTimeline` / `RetirementQueue` (the seam's completion evidence), and every backend place
  that dates a park: `Impl::releaseLater`, the two submit sites (`executeFrame`, `prepareFrame`).
- Related: `vk-backend.md` §2.15 / §2.18; `graphics-sessions.md` 2.6; `ADR-2026-11-24-residency-borrowing` (the two
  windows a release waits for); `ADR-2026-11-15-vk-keeps-one-frame-in-flight`. Owner-directed on 2026-11-24
  ("设计、概念问题要重做").

## Background

`DeviceClock` answered "has the work that named this object finished?" as a **minimum over the registered windows**:
every frame driver reported `windowCompleted(frame)` when one of its swapchain-style slots was recycled, and the device
watermark was the minimum of those reports.

That is wrong whenever a driver stops framing. The fold keeps a silent window's last report forever, so with two
engines where one idles, the watermark never rises again and **no parked release ever runs** — released VRAM is held
until that driver is released. Measured 2026-11-24 while writing
`VkMultiSessionTest.AFrameKeepsTheSourceItReadsUntilTheFrameEnds`: `submitted` reached 7 while `completed` stayed at 2.

The defect is in the CONCEPT, not in the arithmetic. A frame driver is not a witness: it knows about the work **it**
submitted, and it knows nothing about another driver's submission — which is still named by the same shared objects. So
a minimum over drivers can only ever be a conservative guess that one silent participant freezes.

## Decision

**One submission sequence per device; the watermark is the oldest unproven submission minus one.**

- A backend takes a number where it hands a command buffer to the queue (`DeviceClock::noteSubmission`, called inside
  the queue's own external-synchronization lock so the numbers follow the order the queue sees) and reports that
  number's proof when the fence says its work is done (`noteCompletion`).
- `completedSubmission()` is the highest number such that EVERY submission up to it is complete: the front of the
  unproven list minus one, or every number handed out when nothing is unproven. Exact regardless of the order
  completions arrive in, and monotonic.
- A park is dated `submissions() + slots + 1` (`slots` = the frames that may be in flight), the same conservative window
  as before, now expressed in a sequence that only grows.
- Driver registration stays, but it is **identity only**: it mints `FrameDriverId` for a backend's own tables
  (`ResidencyClaims`, a target lease). The verbs are `addDriver` / `removeDriver` / `drivers()`, matching the type
  (`G6`; the mismatch was already recorded as naming debt).
- `FrameTimeline` keeps the frame numbers (minted at `begin`, spent at `submitted`/`abandoned`) and loses the
  submitted/completed watermarks and `retirePoint`: a frame number is minted when a body STARTS, a submission is
  numbered when a command buffer is HANDED OVER, and conflating the two is what produced the per-window arithmetic.
- `RetirementQueue::retire` / `advance` / `retirePoint` take the clock (`ReleaseFn` receives a `const DeviceClock*`),
  because both facts a park needs — "may this go" and "what number is a park made now" — live there.

## Rejected options

- **Implement the rule the clock's note already stated** ("a window proves nothing about work it never did") by giving
  each window an in-flight count: `windowSubmitted` + `windowCompleted`, a window with nothing outstanding contributes
  nothing. Small, and it fixes the frozen watermark — but the evidence would still be PUSHED by the caller, so a
  forgotten `windowSubmitted` makes a live window look idle and destroys in-flight objects. The safe direction of a
  missed report must be retention, which is what pulling the number from the queue gives us.
- **A device-wide timeline semaphore** (`VkSemaphore` with `VK_SEMAPHORE_TYPE_TIMELINE`, queried with
  `vkGetSemaphoreCounterValue`): the Vulkan spelling of D3D12's `GetCompletedValue`, and correct — but it adds a device
  object and a queue query to answer a question the fence waits already answer, because every submission in this
  backend is waited before its call returns (`ADR-2026-11-15`). The sequence model keeps the seam device-agnostic and
  costs nothing per frame.
- **Keep the minimum and ignore idle windows** (a window with no frames since the last report stops constraining):
  that is the first rejected option spelled differently, and it still lets a stale report stand for work the window
  never did.

## Consequences

- An idle engine holds nothing back: the watermark follows the queue, so a release announced beside a live engine is
  destroyed on schedule.
- Two namespaces with two meanings: `FrameToken` (a frame body) and a submission sequence (a queue hand-off). A
  warm-up's transfer batch is a submission like a frame's, which is the honest reading — work was handed to the queue
  and may name released objects.
- A submission whose `vkQueueSubmit2` was refused is proven at once (nothing it named will execute); one that was taken
  and whose fence never signalled can only be a lost device, whose teardown releases everything (`releaseAll`).
- Costs: one `std::vector<uint64_t>` walk on completion (the unproven list holds at most the submissions in flight —
  one per driver here) and one extra lock acquisition per submit (the sequence is taken inside the queue lock).
- **Evidence**: `CoreDeviceClockTest.TheWatermarkIsTheOldestUnprovenSubmission` (out-of-order proofs claim nothing),
  `CoreDeviceClockTest.AnIdleDriverHoldsNothingBack` (two registered drivers, one never submits; mutation: an extra
  driver freezing the watermark turns it red), `VkMultiSessionTest.AFrameKeepsTheSourceItReadsUntilTheFrameEnds` (the
  park drains while the idle driver is still registered; the same mutation turns it red).
