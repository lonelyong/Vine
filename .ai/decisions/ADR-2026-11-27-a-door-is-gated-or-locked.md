# ADR-2026-11-27: a door is gated or locked, never neither

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-threading.md` 2.16 / 2.18 / C20 / C23 / T42 / T43,
  `../research/graphics-2026-11-27-review-19.md` `Z1` / `Z2`,
  `src/viz/graphics/src/FrameResources.hpp`,
  `src/viz/graphics/sdk/vine/graphics/PassRegistry.hpp`

## Background

The engine has one interval per frame and states it twice: `detail::FramePump` owns "a frame is being BUILT and ANSWERED"
(`recording()`, claimed under `phaseGate()`), and `RenderEngine::beginShapeChange` holds that same lock across the
DECISION and the CHANGE of a shape change (C20). That covers what was written after the interval existed:
`applyPlan`, `removePassesOwnedBy`, `setFrameThreading`.

Two doors were missed, and both were missed the same way - each mechanism had been written for one caller and kept its
public doors when the engine grew a frame thread.

- **`Z1` - the image tables.** `RenderEngine::publish` / `unpublish` / `resolve` / `transientTarget` / `transientTargetCount`
  reach `detail::FrameResources`, which states about itself: "All methods are called from the engine's single frame
  thread, so no synchronisation is needed". The compile walks those tables every frame (`beginFrame`,
  `seedFromHostBindings`, `recordPublication`, `resolveTransient`, `retireUndeclared`), and `publish`'s own doc describes
  the host as re-publishing "when it swaps what it offers", i.e. at a time of the host's choosing. A host doing the
  documented thing from the thread that is not driving `frame()` mutated a `std::map` while a compile copied it: UB, not
  a refusal. The class one over, `detail::PublicationStore`, already solves this shape with one mutex over its table.
- **`Z2` - the second door onto a pass' declaration.** `RenderEngine::bindPassContent` rewrites `PassDesc::content` -
  "the same one storage a plan writes and the compile reads" - and took no gate, while `applyPlan` refused with
  `PlanRefusal::Busy`. A host re-binding a pass' scene at the moment a frame was compiled got a silent race; the same
  host re-stating the plan got a value back.

The review offered the cheap answer for `Z1` ("give `FrameResources` the store's own mutex") and a `bool`-shaped
refusal for `Z2`. Both are patches: a `bool` could not tell "no such pass" from "a frame holds it", and a mutex on four
named methods leaves the fifth door - and the accessor that hands out a reference to the locked table - free to
reintroduce the defect.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Lock the four host verbs of `FrameResources`, and route `bindPassContent` through `beginShapeChange` answering `bool` | Smallest diff; the review's own prescription | The rule stays a list of methods rather than a property of the table: `hostBindings()` still hands out a reference to a locked container, and a `false` still conflates "not registered" with "retry between frames" (`G4`). The next verb added to either class can miss the rule again |
| B. State the rule in the five public docs and refuse the verbs outside the interval | No new synchronisation | `publish` is a STANDING binding: "a host has no per-frame hook to re-publish from" (the class' own argument), so refusing it outside the interval takes away the documented use. And `resolve` / `transientTarget` are queries - making a query wait for a frame is the opposite of hiding complexity (`G7`) |
| C. Make the compile's tables private behind a lock and snapshot every host verb's answer | No lock on the compile path at all | Copies a target map per pass (`resolve` walks it per declared input), and a snapshot of a borrowed target is not ownership, so the lifetime question stays open anyway |
| D. One lock over every table a host can reach, in the type that owns them; the frame head takes the host's bindings as a VALUE; `bindPassContent` takes the shape gate and answers a three-state value | The rule becomes a property of the two owners (`G12`): no door can be forgotten, and one frame sees one answer | A describe-shaped cost on the compile path (`recordPublication`, `findPublication`, `resolve` take a plain mutex), and a new public answer type |
| E. One lock per table (host bindings, publications, transients) | Less contention in theory | Three locks whose order must be stated for a `resolve` that walks two of them; the tables are read a handful of times per frame, so the granularity buys nothing (`G8` / `G10`) |

## Decision

Option D.

**The image tables** (`detail::FrameResources`). `tables_mutex_` guards every table a host verb can reach: the standing
bindings and their episode set, the frame's publications, the transients. Every verb that touches them takes it for a
lookup, an insert or an eviction, and never across a device call (`releaseRenderTarget` is told about a replaced or
retired transient AFTER the lock is dropped, so the table's lock never nests under the backend's).
`beginFrame` copies the host's bindings into the frame's own table, and the compile then seeds, checks and resolves
against that copy (`frameBindings`, `resolveForFrame`), while the host's `resolve` keeps answering the live table -
`G1`: one frame, one answer, and a host that just published asks what it just published.
`hostBindings()` (a reference into a locked container) is gone; the two questions it served have their own verbs
(`frameBindings` for the compile, `hostBindingNames` for a plan validated between frames, which must see a binding
published a moment ago).
`findPublication`'s mutable row pointer is gone too: `recordPublication` answers the target the name served before it,
so the collision report - the compile's wording, its episode rule - reads its other half from a value.

**The shape's doors.** `bindPassContent` takes `beginShapeChange("bindPassContent")` and answers
`ContentBindOutcome` - `Bound` / `NotRegistered` / `Refused` - with the same one-per-episode
`engine.pass.changed_during_frame` report `applyPlan` gives. `PassRegistry::bindContent` forwards it (a dead handle answers
`NotRegistered`, which is exactly what its `find` / `contentOf` answer). The type has no `operator bool` on purpose: a
caller that wants to know whether to retry asks `.refused()`, and the compiler makes every existing call site decide
(`G4`).

**The one place a host arrives by takes the remedy.** `SceneView::setScene` re-binds its chain's presenting pass, so on
`Refused` it waits the frame out once (bounded, the way a resize waits before applying a plan) and binds again; if it is
still refused it reports `engine.view.content_not_bound` instead of letting the chain's content check accuse the wrong
cause.

The lock is the plain kind for the same reason C22's cache is: an uncontended `std::mutex` lock/unlock measured **2.8 ns**
(2026-11-27, clang-23 -O2, 20 M rounds), and these verbs run a handful of times per frame (`recordPublication` per pass,
`resolve` per declared input), so the cap is a few microseconds against a 16.6 ms frame.

## Consequences

- Good: the concurrency model is visible in the two types that own the state (`G12`) instead of being a list in a
  design doc; a new verb cannot miss the rule without a test failing, because the tables are private and every public
  verb of `FrameResources` locks; a host may publish, withdraw, resolve and read a transient during a frame; one frame
  cannot see two sets of host bindings; and a host that gets a refused content bind can tell it from a stale handle.
- Bad / cost: a few mutex acquisitions per frame on the compile path (microseconds), a new public type
  (`ContentBindOutcome`), and a behavioural edge a caller may notice: a host `resolve` answers the live table while a
  frame being built answers its head copy, so "what does name X serve" can differ between the two for the length of one
  compile - which is the point, and is stated in both docs.
- Trigger (when to reopen): a measured frame cost attributable to these locks (a per-draw verb would qualify; a per-pass
  one does not), or a third world reaching the tables - a reader that must see the PREVIOUS frame's answer would need a
  published value, not a second lock.

## Evidence

- `src/viz/graphics/src/FrameResources.{hpp,cpp}` - the two kinds of table, the lock, the head copy, `resolveForFrame`,
  `hostBindingNames`, and `recordPublication` answering the previous target.
- `src/viz/graphics/src/EngineCompile.cpp` / `SessionShape.cpp` - the compile reads `frameBindings()` / `resolveForFrame`;
  the collision report reads the returned value.
- `src/viz/graphics/sdk/vine/graphics/PassRegistry.hpp` (`ContentBindOutcome`), `RenderEngine.{hpp,cpp}`
  (`bindPassContent`), `SceneView.{hpp,cpp}` (the remedy).
- `tests/test_graphics/FrameResourcesTest.cpp` - the head's value and the two-thread churn;
  `PlanTest.AFrameRefusesAContentBindFromInsideItsOwnCompile`,
  `PlanTest.TwoPassesPublishingOneNameAreReportedAndTheLastOneServesIt`.
- `scripts/tsan_check.sh` - 2026-11-27: `test_graphics` 695/695, **0 ThreadSanitizer warning(s)**;
  `scripts/asan_check.sh` - `RESULT: PASS`.
