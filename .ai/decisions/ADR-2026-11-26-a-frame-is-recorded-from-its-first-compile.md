# ADR-2026-11-26: a frame is RECORDED from its first compile until its answer, and the pump owns that phase

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/graphics-threading.md` (§2.12 the frame's two halves, C20, P2),
  `detail::FramePump` (`src/viz/graphics/src/FramePump.{hpp,cpp}`: `Build`, `recording`, `phaseGate`),
  `RenderEngine::beginShapeChange` / `frame` / `prepare` (`src/viz/graphics/src/RenderEngine.cpp`),
  `RenderEngine::applyPlan` (`src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp`),
  `vn::graphics::ReportOnce` (`src/viz/graphics/sdk/vine/graphics/ReportOnce.hpp`)

## Background

C20 says the session shape (its passes, resource tables and backend retained state) changes only between frames, and
`RenderEngine::applyPlan` promises `PlanResult::Refused` + `PlanRefusal::Busy` for a call that lands while a frame is
"being recorded". The predicate behind that promise was `detail::FramePump::busy()`, i.e. **"is the frame runner running
a job"**.

A frame is recorded in two halves (`graphics-threading.md` 2.12), and the runner knows about exactly one of them:

| Window | `FrameRunner::busy()` | What runs there |
|---|---|---|
| the content half: `buildContentHalf` (hooks, publication, `SessionShape::compile`), on the thread that called `frame()` | **false** - the job has not been submitted yet | the compile, which walks `slots_`, resolves transients and calls `SessionShape::applyPlan` sibling code |
| the engine half: `expandFrameValue` + `executeFrame` + present, on the frame thread | true | the backend's recording |
| every frame of an **inline** engine (the default: no runner exists at all) | **false, always** | both halves |

So the gate was open for the half that reads the table a shape change edits, and open for the whole of every inline
frame. `prepare()` was outside the predicate entirely - it is not a frame, so nothing was checked - although it walks the
same slots and describes the same passes.

`SessionShape::applyPlanRows` / `SessionShape::removePass` **insert and erase `slots_`**, and they drop the
`intrusive_ptr` that may be a pass' last reference. The compile meanwhile iterates `slots_`, and the `FrameDescription`
it produced names those passes as raw `detail::EngineFrame`-side pointers that the backend dereferences during the engine
half. A change landing in the open window therefore produced, in order of how easy it is to see:

1. a plan **applied** where the contract said `Busy` - the caller retried on the false belief that nothing happened;
2. a pass destroyed while the compile's rows (`plan_rows_[].pass`) and the frame value (`description.passes[].pass`)
   still named it - a use-after-free on a later frame (and inside the current one if the change came from a hook);
3. a pass' transient target released to the device's retirement queue while the backend was recording into it.

Reachable, not hypothetical: the sinks and hooks this module already names as "may drive the same engine"
(`EngineNotReadyReason::Reentrant`, `vk.device.reentry`) run **inside** the content half - `SessionShape::reportPlanProblems`
reports through the engine's route while the compile is still assembling - and this repository's own demo states the
second thread in its source: "a layout step runs on a resize, and the content assembly runs on a pool"
(`src/plugins/demo_plugin/src/DemoScene.cpp`, `addDemoPass`).

The `ReportOnce` behind the episode report was a plain `bool`, so two host threads reaching `sessionChangeAllowed` also
raced on the flag: two reports, or none.

## Evidence

| Fact | Where |
|---|---|
| The rule is about the whole frame, not about the thread that runs the second half | `graphics-threading.md` C20, `RenderEngine::applyPlan`'s `@warning` |
| A shape change mutates the exact vector the compile iterates and drops the last owner of a pass the frame value names | `SessionShape::applyPlanRows` (`slots_.insert`), `SessionShape::removePass` (`slots_.erase`), `SessionShape::compileFrame`, `SessionShape::buildFrameValue` |
| The compile reports through the host's sink **from inside itself** | `SessionShape::compile` -> `reportPlanProblems` -> `Diagnostics::report` (sink invoked with no lock held) |
| An inline engine has no runner at all, so a runner-shaped predicate is structurally blind to it | `FramePump::setThreaded`, `FrameRunner` (constructed only when a host asks for a thread) |
| A plan may be applied from an application's pool while the paint thread draws | `DemoScene.cpp` `addDemoPass` comment + its bounded `waitForFrame` retry |
| Filament serialises "the scene description is being built" the same way: the app builds a frame's commands into the engine's frame, and structural changes go through `Engine::flushAndWait()` before the swapchain or a resource is touched | `filament/filament/src/Engine.cpp`, `Renderer::beginFrame` / `endFrame` |
| Godot states the equivalent rule as a per-frame invariant of `RenderingServer` (`frame_pre_draw` / `frame_post_draw` bracket what a shape change must stay outside of) | `servers/rendering/renderer_scene.h`, `servers/rendering/rendering_server.cpp` |

## Decision

**"A frame is being recorded" is a fact of the pump, over both halves, and a shape change is a critical section against
the moment that fact flips.**

- `detail::FramePump` owns the phase as **two facts with one answer**, each written by the thread that produces it:
  `compiling_` (the content half is running - opened and closed by `FramePump::Build`, an RAII guard returned by
  `beginFrame()` / `beginBuild()`) and `answering_` (a frame was handed to the engine half and nobody has answered it -
  set by `submit()`, cleared by `publishAnswer()`). `FramePump::recording()` is the OR of the two, readable from any
  thread; neither is derived from `runner_`, which the mode-switching thread replaces.
- `FramePump::Build` is a value, not a flag: every exit from the content half ends it (the ordinary return, an early
  one, and a host `describe` that throws), so the phase cannot be left closed by a failed frame.
- `prepare()` gets the same guard (`beginBuild()`): it compiles, so it is a build. It answers nobody, so its interval
  ends with the guard.
- `RenderEngine::beginShapeChange(verb)` replaces `sessionChangeAllowed(verb)` and answers a **`std::unique_lock`** from
  `FramePump::phaseGate()` instead of a bool. The callers (`applyPlan` both forms, `removePassesOwnedBy`) hold that lock
  across **the decision and the change**; `beginFrame()` / `beginBuild()` flip the phase under the same lock. "Nothing
  is recording, so I may change the shape" can therefore no longer be overtaken by "a compile began between the check and
  the change" - the window that two threads could walk into.
- The lock is **never held for a compile**: a shape change is short, a compile is not, and a shape change is still
  refused (Busy) rather than queued - a frame start waits for the former at most.
- The refusal report is emitted **after** the gate is released: a diagnostic sink is host code and may ask for a shape
  change itself, which must be a refusal and not a self-deadlock.
- `vn::graphics::ReportOnce` holds `std::atomic<bool>`; `shouldReport()` is one `exchange`, so exactly one of two
  concurrent threads hears the episode, and no site has to wrap the flag in a lock of its own.

## Rejected options

| Option | Why not |
|---|---|
| Keep the runner predicate and add "and also not in the content half" as a second bool in the engine | Two owners for one phase, and the engine would have to observe the *end* of the engine half, which happens on another thread - it would learn it by polling the runner, i.e. the same blindness in a new place. |
| Have `applyPlan` **wait** for the frame in flight (`waitForFrame`) | Turns a refusal into an unbounded wait on a UI/pool thread - the exact cost the demo works around with a bounded retry, and it would make a deadlocked engine a hung caller (`G7`). |
| A `std::mutex` around `applyPlan` only | Serialises two changers but not "changer vs compile": the compile does not take it, so the race stands. |
| Treat `prepare()` as "not a frame, so no rule" (the status quo) | It compiles the same table; the corruption does not care what the call is named. |
| Report the episode from inside the gate | A sink that answers the report by changing the plan would deadlock on the gate it is being told about. |

## Consequences

- C20 now means what it says in both modes: `applyPlan`, `applyPlan(plan, owner)` and `removePassesOwnedBy` are refused
  from the frame's first compile (hooks, publication, compile) until its answer, inline and threaded alike, and while a
  `prepare()` compiles.
- A host that edits content on a pool thread keeps working: it is refused while the paint thread is inside a build and
  applies between frames - the demo's existing bounded retry is exactly the right loop, and it now actually holds.
- One new lock (`FramePump::phaseGate`) is taken by shape changes and, briefly, by a frame start. A steady frame takes it
  once (the phase flip), which is constant per frame and does not grow with the pass count.
- `FramePump::busy()` is gone; `recording()` is the question the engine asks, and `threaded()` stays the mode question.
- `ReportOnce` is copyable again (the copy carries the episode: a per-(pass, condition) episode is carried from one
  compile to the next), and thread-safe by construction.

## Verification

- `PlanTest.AFrameRefusesAPlanFromInsideItsOwnCompile` - the compile reports through a sink while it runs; the sink
  applies a plan; the answer must be `Busy` and the shape unchanged. **Re-measured with the old predicate**: the case
  goes red (`from_inside.refused()` false, `passCount()` 0), i.e. the plan really was applied to the table being walked.
- `PlanTest.APrepareRefusesAPlanFromInsideItsOwnCompile` - the same for `prepare()`.
- `PlanTest.APlanFromAnotherThreadIsRefusedWhileThisOneCompiles` - a second thread applies a plan while the first is
  stopped inside its compile; refused, and the same plan applies once the frame is answered.
- `FramePumpTest.ThePhaseCoversTheBuildAndTheAnswer`,
  `FramePumpTest.ThePhaseStaysOpenUntilAFrameHandedToItsThreadIsAnswered` - the interval itself, both halves.
- `scripts/tsan_check.sh` over `PlanTest.*:FramePumpTest.*:ReportOnceTest.*:FrameRunnerTest.*`: 0 warnings.
- `scripts/tsan_check.sh` over the whole `test_graphics` and `test_vk` suites (R18(c)).
