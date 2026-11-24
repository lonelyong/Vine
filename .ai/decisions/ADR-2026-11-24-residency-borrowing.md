# ADR-2026-11-24: residency borrowing — a release ends residency, not the objects

- Status: **accepted** (2026-11-24).
- Date: 2026-11-24
- Scope: when a vk GPU object may be destroyed (`Impl::releaseLater`, `TargetTable::release`, `RetirementQueue`),
  the values that keep one alive (`PassTarget`, `FrameRecording::read_records`), and `RenderDevice::releaseContent` /
  `releaseRenderTarget`'s timing contract.
- Related: `vk-backend.md` §2.18 / §2.20; `graphics-sessions.md` 2.6; `ADR-2026-11-15-vk-keeps-one-frame-in-flight`;
  `ADR-2026-11-10-gpu-residency-pinned-by-content`. Owner-directed on 2026-11-24 ("概念、设计、框架问题要重来").

## Background

Two host verbs end residency without waiting for the frames in flight (`R6`, `releaseRenderTarget` /
`releaseContent`): they erase the entry and hand the record to the park. The park then dates the destruction against
the device's completion watermark — "every submitted frame that could name it has finished".

That answers ONE of the two windows. The other is **recording**: a frame body has resolved a record and its command
buffer names the images, but nothing is submitted yet, so the watermark says nothing about it. The code survived that
by three coinciding invariants — one frame in flight, frame bodies serialised device-wide, every body waiting its
fence — and by accident of two of them being false: **off-screen frame bodies DO overlap** (`executeFrame` takes the
body gate SHARED; only a presenting frame takes it exclusively). So with two engines on two threads:

1. engine B records a pass whose handle names target T's images;
2. a host thread releases T (no wait, `R6`): the entry leaves the table, the record enters the park;
3. engine A's frame completes — its frame end runs `retirement.advance()` — and the park destroys T's images;
4. B submits a command buffer that names destroyed images.

Step 1 also covered records a frame only READS (a full-screen draw's source, a pass' shadow map): those were resolved
by `TargetTable::lookup` and kept nowhere, so even the record could be freed under the frame.

## Decision

**Residency borrowing.** A target's or a content entry's GPU objects have exactly one owner: the device's residency
(its table). Residency LENDS them to a frame for that frame's life, and to a host verb for that call's life. A
`release` ends *residency*, not the objects. The objects are destroyed when BOTH hold:

1. **no submitted work names them** (the park's timeline), and
2. **no value this device lent still names them** (the recorder's window).

Neither condition ever makes a release wait.

- The park's act asks condition 2 itself (`Impl::parkRelease`, a `use_count` probe on a `weak_ptr` the release hands
  over) and asks again at a later frame end when the answer is "still named". There is no spin: the only owners are
  frame bodies and host verbs, and every frame end is another chance to ask.
- `RetirementQueue::ReleaseFn` now receives the timeline it was reached at (`nullptr` at teardown), which is what
  makes "a release callback may park something new" — a promise the queue's own note already made — actually
  possible without taking the clock lock the caller holds.
- A value that names a record keeps it alive: the off-screen pass handle (`PassTarget`, `ADR-2026-11-24` predecessor)
  for what a frame DRAWS INTO, `FrameRecording::read_records` for what a frame READS. Both are dropped when the frame
  ends (`FrameHoldCloser`), so the frame's hold ends with the frame and not at the next frame's `beginFrame`.
- `TargetTable::ensure` answers the record it built (one call, one answer), which is what both of those values are
  built from.

## Rejected options

- **Make `release` wait for the frame bodies** (take the body gate exclusively). Safe, and what the retired reference
  backend effectively did — but it makes a UI thread free VRAM at the price of stalling an engine, which is exactly
  what `R6` was decided against. Kept only as the no-evidence fallback (no armed queue), where a counted idle was
  already the honest answer.
- **A device-wide "how many bodies are recording" counter.** One atomic and it covers content and targets alike — but
  it is coarse: with two engines that never idle, a released object waits for a moment when *no* body runs, which may
  never come. Retention would grow without bound, and residency is a budget in this framework (`ADR-2026-11-10`).
- **Let the record's destructor destroy the Vulkan objects (deleter on the last `shared_ptr`).** The cleanest-looking
  shape, but the last reference can be dropped by the recording thread, which would put an allocator lock and a
  `vkDestroyImage` on a path that is not allowed to lock (`C3`/`C5`). The delete must happen at a frame end.
- **Null-check the second `lookup`.** Trades undefined behaviour for a silently skipped pass and still asks the table
  twice for one fact.

## Consequences

- A release is safe from any thread at any moment, and the *statement* ("this target is not resident") and the
  *object lifetime* ("the images live until nobody names them") are separate facts with separate owners.
- Same shape as the engines this backend is measured against: Filament's `Driver::destroyDeferred`, Unreal's
  `FRHIResource` deferred delete, WebGPU's internal destroy-behind-a-submission-index, Metal's
  `addCompletedHandler` capture. Every one of them is "refcount + submission watermark".
- Costs: one `std::weak_ptr` + one probe per release; `std::shared_ptr` copies per pass handle and per unique read
  (atomics, no allocation on the recording path beyond the vector's first growth).
- **Known follow-ups, both recorded in `../backlog/graphics.md`**:
  - **Content entries are not shared yet.** `DrawPlan` holds raw `VkBuffer`/`VkImage` handles, so a content release
    has the same recording window and no refcount to ask about. The rule is the same; the change is to let the frame
    keep the *entries* it resolved (the plan/call payload) instead of raw handles.
  - **An idle frame window freezes the park.** `DeviceClock::updateWatermark` folds a MINIMUM over the live windows,
    and a window that stops framing keeps contributing its last proven frame — so with two engines where one idles,
    no parked release ever runs. The clock's own note says "a window proves nothing about work it never did, which is
    vacuously done"; the fold does not implement that. Until it does, a parked release waits for the idle driver to be
    released (`releaseFrameDriver`), which is what `VkMultiSessionTest.AFrameKeepsTheSourceItReadsUntilTheFrameEnds`
    states.
- **Evidence**: `VkMultiSessionTest.APassHandleKeepsItsTargetAliveAfterARelease` (the park's turn defers while a
  handle lives, and destroys once it is dropped; mutation: disabling the probe turns `pending` to 0 while the handle
  is alive), `VkMultiSessionTest.AFrameKeepsTheSourceItReadsUntilTheFrameEnds` (a release announced while another
  engine records), `CoreRetirementQueueTest.AReleaseCallbackMayParkSomethingNew` (the callback is handed the
  timeline), `VkMultiSessionTest.AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses` (R6 still holds).

## Correction (2026-11-24, appended)

Both follow-ups this ADR recorded are closed, in the same shape it predicted:

- **Content entries are shared now.** The caches hold `shared_ptr` entries, `FrameBuilds::borrow` keeps every entry a
  frame head resolved until the frame ends, and every release path erases or REPLACES the table's share before it parks
  (replacing, not mutating, because a table's own share inside a parked entry would make the `use_count` probe count it
  and never destroy anything). Witnesses:
  `VkMultiSessionTest.AFrameKeepsTheContentItResolvedUntilTheFrameEnds` (three frame ends run the park while another
  engine records, and the frame still draws; with `borrow` a no-op the pool loses the streams and the picture is black)
  and `VkMultiSessionTest.AReleaseRacingAFramesContentNeverLeavesACommandBufferNamingAFreedBuffer`.
- **The idle window no longer freezes the park**: the watermark is the device's own submission sequence, not a minimum
  over drivers — `ADR-2026-11-24-completion-belongs-to-the-submission`. The note in the consequences above ("a parked
  release waits for the idle driver to be released") no longer holds, so
  `VkMultiSessionTest.AFrameKeepsTheSourceItReadsUntilTheFrameEnds` drains with that driver still registered.
