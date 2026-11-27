# ADR-2026-11-27: a teardown is an interval entry

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-threading.md` 2.17 / 2.20 / C25 / T45,
  `../research/graphics-2026-11-27-review-19.md` `Z21`,
  `src/viz/graphics/src/FrameRunner.{hpp,cpp}`,
  `src/viz/graphics/src/FramePump.{hpp,cpp}`,
  `src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp`

## Background

The interval (C20) says which verbs may run while a frame is being built or answered. Two of its answers were missing at
the teardown, and they failed the same way - each mechanism was written for the caller it was born with.

**The engine's teardown took no interval at all.** `RenderEngine::shutdown()` stopped the pump, applied the deferred
take-backs, released the transients and detached the surface, whoever the caller was. From another host thread that is
a teardown beside a compile; from a frame body it is a teardown beside the frame itself - and a diagnostic sink runs ON
THE FRAME THREAD, which is exactly where the vk backend reports a lost device, so "device lost -> shut down" is host
code the tree invites and then breaks.

**The runner had three answers to one question.** "Will this runner run a job handed to it?" was answered by `live_`
(is the thread joined? no), `stopping_` (was a stop asked for? yes) and the pump's `runner_ != nullptr` (is an object
stored? yes). A `stop()` asked from the frame thread cannot join itself, so it returns early - and it cleared neither
fact in the reader's favour: the object stayed stored, `live_` stayed true although the loop had exited. The pump then
answered `threaded() == true`, `frame()` handed its job over, `run submit` took the threaded branch, the wake found no
waiter, `has_request_` stayed set - and the next frame blocked for ever in `beginFrame`'s `waitIdle` (review 19 `Z21`).

The review's own prescription was two statements where the pump returns plus "shutdown() should also take the
interval". The two statements are the patch that fixes the symptom on the reported path: `live_.store(false)` in the
self-stop path makes `wait`/`waitIdle` answer "idle" while the last job is still inside the device (breaking the
one-in-flight rule `beginFrame` depends on), and clearing `runner_` there leaves a joinable thread nobody owns to join
(`std::terminate` at the runner's destructor).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. The review's minimum: clear the "live" flag in the self-stop path and clear `runner_` in the pump | Two statements | `live_` also gates `wait`/`waitIdle`, so clearing it early reports "nothing in flight" while the last job runs - two jobs in the device at once; and dropping `runner_` loses the only handle that can join the thread |
| B. Make `shutdown()` wait like a device verb | Nothing is ever refused | The frame thread waits for itself (the sink case), and a second host thread would have to wait for another thread's hook - there is no "not recording" notification to wait on, and a sink freeze stalls the engine thread |
| C. Defer the shutdown ("stop after this frame") | A sink gets its wish | A new lifecycle state machine for a caller that can simply report and retry from the host; `G8` - no consumer beyond the defect |
| D. Refuse the entry with the rest of the tree's answers, and give the runner one life fact the pump publishes | One rule per question (`G1` / `G4`), no waiting, no half state | One more public diagnostic pair, one more `State` enum, and a shutdown that a sink must retry from the host (which is what a sink should do anyway) |
| E. Assert / terminate on a shutdown from a frame body | Impossible to misuse | The tree reports; an abort inside the engine thread takes the host down with a diagnostic it never sees |

## Decision

Option D.

**The engine's teardown is an interval entry.** `shutdown()` takes the lease `prepare()` takes
(`detail::FramePump::beginBuild`) and holds it across the whole body: the pump stops, the deferred take-backs land, the
transients are released, the surface is detached. The lease refuses exactly like `beginFrame`:
- `Reentrant` - the caller is the frame thread, or is inside a build on this thread - refuses with
  `engine.shutdown.refused_from_frame`;
- `Concurrent` - another host thread holds the interval - refuses with `engine.shutdown.refused_in_flight`.

Either way NOTHING is torn down, and the sentence is said once per episode (`ReportOnce`), as a refused mode change
already is. A frame ANSWERED on the engine thread is not a refusal: `pump_->stop()` waits for it and joins, which is
the asymmetry 2.17 already states for a mode change.

**The runner has one life fact.** `detail::FrameRunner::State` is `Inline` / `Running` / `Stopping` / `Stopped`, and
`accepting()` - true only in `Running` - is the answer to "will a job handed over now run on the engine thread". `stop()`
publishes `Stopping` under the same lock `submit` decides under, BEFORE it returns; the loop drains the request it was
woken for and exits, and a later `stop()` from another thread performs the join. So:
- a job submitted after the stop runs on the caller's thread (never accepted by a loop that will not run it);
- a job submitted just before the stop is still run (accepted is run);
- `wait` / `waitIdle` answer from the JOB IN FLIGHT, not from "is there a thread", so a stopping runner is never
  reported idle under its last job.

**The pump publishes that answer.** `threaded()` is `runner != nullptr && runner->accepting()`, so the fact `frame()`
reads stops being true when the stop is asked for, not when somebody joins. The object stays stored until a join, which
is the only thing it is still needed for.

## Consequences

- Good: "device lost -> shut down" from a sink is a refusal the host can act on (the frame returns, the host shuts the
  engine down), and the engine keeps driving in the meantime; a teardown can no longer release tables under a compile;
  the runner's contract ("after `stop`, an inline runner") holds on every path, including the one where the join is
  somebody else's; one question has one fact, read under one lock.
- Bad / cost: one more diagnostic pair and one `State` enum; a host that shuts down from a sink must do so after the
  frame returns (which is what the tree's other interval verbs already ask).
- Caveat, unchanged and re-stated: a stop asked from the frame thread still cannot join itself - the loop leaves after
  its job and the destructor / `shutdown()` of the next thread completes the join. Destroying the runner (or the engine)
  from inside its own frame remains the caller's lifetime error, as `FrameRunner::stop` has always said.
- Trigger (when to reopen): a host that genuinely needs a teardown initiated from inside a frame (that is "stop after
  this frame", option C, and it needs its own design and consumer).

## Evidence

- `src/viz/graphics/src/FrameRunner.{hpp,cpp}` - the one `State`, `accepting()`, `stop()` publishing before it returns,
  `wait` / `waitIdle` answering from the job in flight.
- `src/viz/graphics/src/FramePump.{hpp,cpp}` - `threaded()` publishing the runner's answer; `beginBuild()`'s doc naming
  the teardown as a second lease holder.
- `src/viz/graphics/src/RenderEngine.cpp` (`shutdown`) and `sdk/vine/graphics/RenderEngine.hpp` - the entry, the two
  refusals, the episode.
- `scripts/diagnostic_codes.txt` - `engine.shutdown.refused_from_frame` / `refused_in_flight`.
- Cases: `RenderEngineTest.AShutdownFromTheFrameThreadIsRefusedAndTheEngineKeepsRunning` (a sink reporting from the
  frame body; the engine keeps driving and the next frame is answered - the wedge this records),
  `RenderEngineTest.ASecondThreadCannotShutTheEngineDownWhileAFrameIsBeingBuilt`,
  `FrameRunnerReentryTest.ARunnerAStopHasLeftRunsTheNextJobOnTheCaller`,
  `FramePumpTest.AStopAskedFromTheFrameThreadLeavesAnInlinePump`.
- `scripts/tsan_check.sh` - 2026-11-27: `test_graphics` 703/703, **0 ThreadSanitizer warning(s)**;
  `scripts/asan_check.sh` - `RESULT: PASS`.
