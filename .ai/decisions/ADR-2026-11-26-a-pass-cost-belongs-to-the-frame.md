# ADR-2026-11-26: a pass' record cost is a fact of the FRAME and travels with its answer

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/graphics-threading.md` (§2.12 the frame's two halves), `FrameTable::PassRow::record_micros`
  (`src/viz/graphics/sdk/vine/graphics/FrameTable.hpp`), `detail::PassCost` / `detail::EngineFrame::pass_costs`
  (`src/viz/graphics/src/EngineFrame.hpp`), `FramePump::publishAnswer` / `answeredPassCosts`
  (`src/viz/graphics/src/FramePump.hpp`), `SessionShape::frameTable`
  (`src/viz/graphics/src/SessionShape.cpp`), `RenderEngine::publishFrameAnswer` (`src/viz/graphics/src/RenderEngine.cpp`)

## Background

`FrameTable::PassRow::record_micros` answers "how long did this pass' record take" - the number a host reads to decide
which pass to look at. The frame is measured by **both halves on their own threads**: the content half times the
description and the binding of the view a pass states (`SessionShape::buildFrameValue`), the engine half times the
expansion of the frame's draw lists (`RenderEngine::expandFrameValue`).

The number was stored on `SessionShape::Slot` - the session's pass table - and written in two phases by two threads: the
compile added the content half's share, and the engine half's share reached the table only through
`RenderEngine::publishEngineMicros`, called from the **next** `frame()` / `prepare()` after the runner went idle. The
engine half itself could not write it: the table belongs to the content half, and 2.12 says the halves share no mutable
state.

What that produced, in `FrameThreading::EngineThread`:

| Symptom | Cause |
|---|---|
| A host reading `frameTable()` right after `waitForFrame(N)` saw N's **content share only** - the expansion it waited for was missing | the engine share is published at the start of the *next* frame, not with the answer |
| The last frame an engine ever ran never got its number, so a benchmark that stops at frame N reads N-1 | same |
| A `prepare()` published the pending share and then zeroed the slot again in its own compile, so a rehearsal erased a measurement | the slot is a session cell that every compile resets |
| `record_micros` meant one thing inline and another threaded (the review-6 P2-1 finding) | the two modes published at two different moments |

The modelling error is one sentence: **a measurement of one frame was stored in a cell that belongs to the session.**
Every engine that reports per-frame costs attaches the number to the frame instead, and lets the reader look it up.

## Evidence

| Fact | Where |
|---|---|
| The public contract is "the last frame's record cost", read after `waitForFrame` - so the answer must be whole at that moment | `FrameTable::PassRow::record_micros`, `RenderEngine::frameTable` |
| A frame value already exists, is reused across frames, and already carries per-frame engine state (`engine_order`) | `detail::EngineFrame` |
| The answer to a host already has one publisher, one lock and one moment: `publishFrameAnswer` | `RenderEngine::publishFrameAnswer`, `FramePump::publishResult` |
| A shape change may retire or re-order passes between the answer and the read, so a slot INDEX cannot name the pass a cost was measured for | `SessionShape::applyPlanRows`, `removePass` |
| Filament hands the app a **history of `FrameInfo`**, each carrying `frameId`, and the app matches by that id (`getFrameInfoHistory`) | `filament/include/filament/Renderer.h`, `samples/async_stream_bench.cpp` |
| Godot publishes the profile **with the frame it belongs to** (`get_frame_profile()` + `get_frame_profile_frame()`, the debugger stores `frame.frame_number`) | `servers/rendering/rendering_server.h`, `servers/debugger/servers_debugger.cpp` |
| Timestamp readback elsewhere (Diligent, Vulkan samples) keys results by frame index in a ring: a measurement is read from the slot of the frame that produced it, never merged into a live per-pass cell | peer documentation |

## Decision

**A pass cost is measured on the frame value and published with that frame's answer. The reader joins the answered
frame's costs to the current rows by pass identity.**

- `detail::PassCost` (`const void* pass`, `std::uint64_t micros`) lives on `detail::EngineFrame::pass_costs`, one entry
  per registered slot. The content half adds its describe share while it compiles; the engine half adds its expansion
  share while it answers - each still writing only the frame value it owns.
- `FramePump::publishAnswer(FrameResult, span<const PassCost>)` publishes the costs and the rest of the answer **under
  one lock**, so `FrameResult::frame` and the numbers always describe the same frame. Reading is
  `answeredPassCosts()`, a copy under that lock.
- `RenderEngine::publishFrameAnswer` is the only publisher (an answered frame and a failed one), so inline and threaded
  publish at the same moment - the mode no longer changes what the number means.
- `SessionShape::frameTable(span<const PassCost>)` fills each row from the cost whose TOKEN names that row's pass, and
  leaves it at 0 when none does. The token is compared, never dereferenced (`SessionShape::Slot::owner`'s precedent), so
  a pass retired after the measured frame cannot misattribute a number.
- Deleted with the old shape: `SessionShape::Slot::record_micros`, `SessionShape::addEngineMicros`,
  `RenderEngine::publishEngineMicros`, `FramePump::notePendingMicros` / `takePendingMicros` / `pending_micros_`,
  `EngineFrame::engine_micros`, and the `if (!pump_->threaded())` branch in `runEngineHalf`. The compile no longer walks
  the slots a second time to zero them: `pass_costs.assign` is the reset, and it happens where the table is built.
- A `prepare()` compiles the value the next `frame()` would build, so it measures a rehearsal into that value - and the
  table does not read values, it reads the answered frame's record. A rehearsal therefore cannot rewrite what a frame
  that ran actually cost.

## Rejected options

| Option | Why not |
|---|---|
| Publish the pending share inside `waitForFrame()` | Turns a read into a write of the session's table, from any thread, for any caller - and a host that never waits still reads half a number. |
| Let the engine half write `Slot::record_micros` directly under a lock | Puts a mutex on the content half's per-pass path and makes a value type a concurrent object (`G10`); it also keeps the two-writer cell the defect is made of. |
| Keep publishing at the start of the next frame (the status quo) | The defect itself: a half number between the answer and the next frame, no number for the last frame, and two meanings for one field. |
| Add `FrameTable::frame` (or a Filament-style N-frame history) so the reader can tell which frame the numbers are | The answered frame is already named by `presentRecord().frame` / `frameResult().frame`, and a history has no consumer today (`G8`). The one fact this ADR adds - "the numbers are the answered frame's" - is in the field's contract instead. |
| Fold the answered costs POSITIONALLY (row index = slot index) | A plan change between the answer and the read hands a pass its predecessor's number: a silently wrong number under the right name. |

## Consequences

- `record_micros` now means "the last **answered** frame's cost" in both modes. With a frame thread, reading the table
  before `waitForFrame` reports the frame before the one just requested - stated in the field's documentation, the same
  rule `frameDescription()` already states for the engine half's command lists.
- One small copy per answered frame (`slots_ × 16 bytes`, `<PassCount>` entries) under the pump's existing mutex, and a
  linear token scan per table read. Neither is on a per-frame hot path: the table is a diagnostic read.
- A failed frame publishes the shares it measured before the throw. That is the frame's real cost, and it keeps the
  field's contract ("what this frame's record took") instead of inventing a zero.
- The token's address may be reused by a later allocation of a freed pass. The comparison then matches a pass that is not
  the measured one - the same class of staleness `SessionShape::Slot::owner` accepts, and cheaper than holding a
  reference to a pass the shape may retire.
- Witnesses: `RenderEngineTest.AFrameThreadedEnginesPassCostIsWholeOnceTheFrameIsAnswered` (gated backend: nothing
  published while a frame is in flight; the number complete once it is answered),
  `RenderEngineTest.PreparingAFrameDoesNotRewriteTheLastFramesNumbers`,
  `RenderEngineTest.APassAnswersItsOwnCostNotTheOneThatLeftItsSlot` (identity, not position).

## Verification

- Build 0 errors; `test_graphics` 653, `test_core` 169, `test_vk` 222 pass.
- Mutation, measured: folding the answered costs POSITIONALLY (row index = slot index, no token comparison) turns exactly
  one case red - `APassAnswersItsOwnCostNotTheOneThatLeftItsSlot`; every other case in the suite stays green, which is why
  that one exists.
- Mutation, measured: publishing an empty cost span with the answer instead of the frame's own turns four cases red -
  `TheFrameTableCarriesWhatEachPassLastCost`, `AFrameThreadedEnginesPassCostIsWholeOnceTheFrameIsAnswered`,
  `PreparingAFrameDoesNotRewriteTheLastFramesNumbers` and `APassAnswersItsOwnCostNotTheOneThatLeftItsSlot`.
