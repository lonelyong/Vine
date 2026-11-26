# ADR-2026-11-26: the frame report is the frame's ANSWER, published with it

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/graphics-threading.md` (§2.12 the frame's two halves, C21),
  `vn::graphics::FrameReport` / `RenderFrameExecutor::frameReport` (`src/viz/graphics/sdk/vine/graphics/FrameReport.hpp`),
  `Device::FrameDriver::frameReport` / `closeFrameBooks` / `answered_books`
  (`src/plugins/gfx_backend_vk/src/internal/Device.cpp`), `RenderEngine::frameReport`
  (`src/viz/graphics/src/RenderEngine.cpp`)

## Background

`FrameReport` is the host's one answer to "how did the frame go": what it ran (`FrameCounters`), what it uploaded
(`FrameUploads`), where the frame clock is (`FrameProgress`), what it cost on the device (`FrameTiming`) and what it spent
on the content lock (`FrameLocking`). The vk backend keeps those numbers in `Device::FrameDriver::books` - plain fields
the **frame body** increments as it goes (`++books.progress.submitted`, `books.timing = ...`) and fills at the end
(`closeFrameBooks`).

`frameReport()` read that live struct. With a frame thread, that is a read of a value another thread is writing, at the
one moment a host is most likely to ask: right after `frame()`, while the engine half runs. Nothing in the interface said
so. The only statement of the rule was in a consumer's source, written after it hit the problem:

> "with a frame thread the numbers would otherwise be read while the engine half writes them (the report is not published
> as thread-safe, unlike PresentRecord)" - `src/plugins/demo_plugin/src/DemoScene.cpp`, `reportFrameBenchmark`

Two failures follow from that shape:

1. **A data race** (undefined behaviour, TSan-detectable) whenever a host reads the report while a frame is being
   recorded - which is the natural place to read it, and what any HUD does.
2. **A transferred obligation** (`G7`): the framework owns the completion signal (`waitForFrame`) and the published
   answer (`presentRecord`, `frameResult`, the per-pass costs), and still made every host know and remember a rule the
   API never stated (`G4`).

The same review round had just fixed this pattern one layer up - a pass' record cost became a fact of the frame,
published with its answer (`ADR-2026-11-26-a-pass-cost-belongs-to-the-frame`) - so the frame report was the remaining
host-readable per-frame value with two writers and no publication.

## Evidence

| Fact | Where |
|---|---|
| The numbers describe the frame that **ended** ("the frame that just ended", "disengaged when none are kept") | `FrameReport.hpp`, `FrameBooks` (`Device.cpp`) |
| The frame body writes them, on whichever thread runs the frame (`executeFrame`, and the `BooksCloser` that closes them on every exit) | `Device::FrameDriver::executeFrame`, `closeFrameBooks` |
| A host may read the report from another thread: `frameReport()` is `const noexcept` and takes no gate | `RenderEngine::frameReport`, `RenderFrameExecutor::frameReport`, `Device::frameReport` |
| The engine already publishes a frame's answer as one value under one lock | `FramePump::publishAnswer` (present word + `FrameResult` + per-pass costs + completion) |
| A consumer had to write the missing rule into its own comment | `DemoScene::reportFrameBenchmark` |
| Filament publishes per-frame statistics from the driver **after** the frame is finished (`Engine::flushAndWait` before a reader sees the finished frame's numbers), and Godot's profiler reads the *previous* frame's profile through `get_frame_profile_frame()` | peer sources (as in the pass-cost ADR) |

## Decision

**The books are the frame's answer. `closeFrameBooks` publishes them, and `frameReport()` reads the published copy.**

- `Device::FrameDriver` keeps two values: `books` (what the frame being recorded is writing - the frame body's alone) and
  `answered_books` (the last CLOSED frame's - guarded by `books_mutex`, written only by `closeFrameBooks`, read by
  `frameReport()`).
- `closeFrameBooks` runs on every exit of a frame (`BooksCloser`'s destructor) and ends with one copy under the lock, so
  the value a host reads is always a whole frame: this frame's counters, this frame's uploads, this frame's timing, this
  frame's lock cost.
- `frameReport()` copies `answered_books` under the same lock and composes the device-level parts (retention, residency,
  the pool's byte account) exactly as before - those are each already guarded or atomic and describe the device, not one
  frame.
- `destroy()` resets `answered_books` under the lock: a report read after a session ends must not describe a frame whose
  objects are gone.
- The device-level `Device::frameReport()` and `RenderEngine::frameReport()` are unchanged in shape: they forward to the
  driver, which now answers a value.

## Rejected options

| Option | Why not |
|---|---|
| Document "read the report only after `waitForFrame`" (the status quo) | Leaves UB in the natural usage, and puts a framework rule in every host's head - the demo's comment is the evidence that this is what happens. |
| Make each field of `FrameBooks` atomic | Twenty fields becoming atomics, and still not one value: a reader could see this frame's counters beside last frame's timing, which is a report about no frame at all (`G1`: the report is one fact). |
| Take the driver's `FrameGate` exclusivley to read the report | `frameReport()` is a *read a HUD calls per frame*; making it wait for the frame in flight would stall the reader on the very frame it is describing. |
| Have the reader build the report from cumulative device counters | Those numbers are the device's, not one frame's, and the parts this backend does not keep must stay **disengaged** rather than be reconstructed (`FrameReport`'s "absent is not zero"). |

## Consequences

- `frameReport()` is safe to call at any time, from any thread, and answers the last finished frame - the same contract
  `presentRecord()` and `frameResult()` already had. A host no longer needs to wait before reading it; waiting still
  matters for "which frame".
- One mutex copy per frame on the frame thread, and one per read. That is the cost of the answer; it does not grow with
  the draw count.
- The rule is now in the type's contract (`answered_books`'s comment, `FrameReport`'s "the frame that just ended") rather
  than in a consumer's comment.

## Verification

- `VkConcurrentFrameTest.TheFrameReportIsTheFrameThatEndedWhileTheNextOneRuns` - one thread records frames while another
  reads `frameReport()`; the parts only a closed frame can carry arrive together, and the last answer is the frame that
  ended.
- **Re-measured with the old read** (reading `books` directly instead of `answered_books`): the same case reports
  **15 ThreadSanitizer warnings**, the first at `closeFrameBooks` - the race this ADR removes.
- `scripts/tsan_check.sh` with `VN_TSAN_TARGET=test_vk VN_TSAN_FILTER='VkConcurrentFrameTest.*'`: 0 warnings.
