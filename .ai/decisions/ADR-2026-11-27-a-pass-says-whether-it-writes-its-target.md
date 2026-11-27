# ADR-2026-11-27: a pass says whether it writes its target

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-rendering.md` 2.16 / C9 / N15 / F31 / T10,
  `../design/graphics-architecture.md` 2.5,
  `../research/graphics-2026-11-27-review-19.md` `Z11`,
  `src/viz/graphics/sdk/vine/graphics/RenderPass.hpp`,
  `src/viz/graphics/src/{RenderPass,FullscreenPass,EngineCompile}.cpp`

## Background

The producer half of a frame - "does this pass change the target it draws into" - was answered in three places, by two
different rules, and each rule was wrong in the direction its author did not look (`Z11`).

**What the pass announces and what the device does** (the physical fact): `RenderPass::describe` fills target, state and
attachment policy for a camera-less pass and issues no packet - `"the pass announces what it is and where, and issues no
draw"` - and the backend records that entry's own load ops, so the target is CLEARED. The recording path names it in its
own words: `"a clear is the pass' own load-op, so a clear-only pass is a real pass with no draws"`.

**What the scheduler believed**: `scheduleRows`' `writes` lambda answered
`camera != nullptr && (content != nullptr || isClearEnabled())` and its comment stated the opposite rule (`"a clear is
part of a draw, not a substitute for one"`). A camera-less pass therefore added no producer edge.

**What the fill checks believed**: `FrameResources::recordProduced` and the wiring's `noteFilled` used
`renderTarget() != nullptr`, i.e. "a pass with a target fills it".

The consequences are orders, not sentences. A consumer of a camera-less clearing pass' target could be scheduled BEFORE
the clear - and nothing was reported, because the fill checks had already declared the target filled. In the other
direction, a pass that announces nothing at all (a Scene pass with a target and no scene) counted as a filler, so a
reader of its target was never told that nothing wrote it. Only the first is a wrong picture, and it arrives silently.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Make the announcement follow the scheduler's rule: a pass with no camera announces nothing (not even a target) | No new predicate | Deletes a capability the backend, the frame value and its tests already implement - the clear-only pass - and leaves no way to clear a target without drawing; also contradicts `"a clear-only pass is a real pass with no draws"` |
| B. Keep the three answers and document the difference | Nothing changes | The question has no owner, and the wrong answer is a picture, not a sentence (`G1`) |
| C. Describe every pass first and schedule from the frame value's entries | The order reads the announcement literally | Describing needs resolved inputs, and resolving them needs to know who wrote what - the cycle the prediction exists to break; it also costs a second `CameraSnapshot` per pass per frame (`G10`) |
| D. The pass owns the answer: `RenderPass::writesTarget()`, the value-side rule stays `passHasEffect`, and all three readers call it | One question, one answer, one owner (`G1` / `G4` / `G12`); a clear counts as a write on both sides | One more virtual beside `describe()`, `attachmentPolicy()` widens to `protected`, and the full-screen half stays a PREDICTION (`N15`) |
| E. Make the merged rule the fill checks' rule: a pass with a target writes it | One line changes | It is the second wrong answer: a target a pass merely NAMES is not a target it writes, so a reader of it reads whatever the last frame left, unreported |

## Decision

Option D.

**The predicate lives on the pass.** `RenderPass::writesTarget()` is true when the entry this pass announces changes its
target: it issues at least one draw packet, or it announces an attachment policy. A clear is a WRITE - the backend opens
the pass' own load ops over the target even when no packet draws - so a camera-less Scene pass with content and a target
is a clear-only pass and a producer, not a pass that does nothing. A pass that announces nothing at all (a Scene pass
with no scene, a full-screen pass with no resolvable source) writes nothing.

**The value side is the same rule.** `describe()` fills the `PassDraw`; `passHasEffect(const PassDraw&)` already asked
the announcement the identical question. The declaration-time form exists because the ORDER is compiled before any pass
is described (the order is what makes a description's inputs resolvable), and the pair is pinned shape by shape by
`PassScheduleTest.TheWriterPredicateIsTheShapeOfTheAnnouncement`: if `writesTarget()` and
`passHasEffect(described entry)` ever disagree, the test fails.

**Three readers, one answer.** `scheduleRows`' producer edges, `FrameResources::recordProduced` (what a later pass may
resolve) and the wiring's filler check (`engine.pass.input_target_not_produced` /
`engine.pass.input_image_not_produced`) all call `writesTarget()`. The `obj_cast<FullscreenPass>` branch and the
`renderTarget() != nullptr` shortcut are deleted, so a pass cannot be a producer in one reader and a non-producer in
another.

**A subclass states both halves.** `FullscreenPass::writesTarget()` sits beside `FullscreenPass::describe()` and applies
the same gate one step earlier: a declaration whose inputs are all imported textures has nothing that can ever resolve to
be its source, so it writes nothing; past that gate a program AND a camera make its packet, and a stated policy (the
depth clear a HUD owns) is a write on its own.

## Consequences

- `attachmentPolicy()` moves from `private` to `protected`: the announcement is the subclass' too, and a kind that
  states its own description must answer "does this pass clear, and how" from the same rule (`G5`).
- The declaration-time answer is a PREDICTION for a full-screen pass whose declared input a producer does not fill this
  frame: the pass is a writer on paper and announces nothing in fact. The graph then carries one superfluous edge
  (harmless for the picture - `writesTarget()` may over-count writes, never under-count them) while the reader of its
  target goes unreported. `N15` states that residual cost; it is the compile order, not the rule.
- Every test that modeled "a writer" as `camera + target` now names content (`vn::test::emptySceneForTest`): that shape
  announces nothing at all, which is exactly what the corrected rule says. `TheFrameValueIsEveryPassAndEveryDrawTheFrameRan`
  re-derives its run order, because the camera-less pass it already had in it now clears the target its composite
  samples and is therefore ordered first.
- `engine.pass.*` diagnostics gain an answer rather than a code: a target only a nothing-announcing pass declares is now
  reported to its reader, which is the report the old filler rule silenced.

## Evidence

- `PassScheduleTest.TheWriterPredicateIsTheShapeOfTheAnnouncement` - seven shapes, the declaration equal to the
  announcement; fails if either side drifts.
- `PassScheduleTest.AClearOnlyPassRunsBeforeTheConsumerThatSamplesItsTarget` - fails on the old lambda (the composite
  ran first).
- `PlanTest.APassThatAnnouncesNothingDoesNotFillItsTarget` - fails on the old fill rule (no report, and the input
  resolved to a produced target).
- `RenderEngineTest.TheFrameValueIsEveryPassAndEveryDrawTheFrameRan` - the frame value's own order now puts the
  camera-less pass before the pass that samples the target it clears.
