# ADR-2026-11-27: a take-back is accepted, never refused

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-threading.md` 2.18 / 2.19 / C20 / C24 / T44,
  `../research/graphics-2026-11-27-review-19.md` `Z22`,
  `src/viz/graphics/src/SessionShape.{hpp,cpp}`,
  `src/viz/graphics/sdk/vine/graphics/PassRegistry.hpp`,
  `src/viz/graphics/src/SceneView.cpp`

## Background

The shape gate (C20) has one answer for every mutator: "a frame is recording, so REFUSE". That answer is right for a
verb a host calls - the host waits for the frame and calls again - and it is wrong for the one ask whose caller is a
destructor.

`SceneView::~SceneView` ends with `engine->passes().clearOwnedBy(this)`, and `~PassChain` reaches the same verb when the
chain owns its own scope. `RenderEngine::removePassesOwnedBy` took `beginShapeChange("removePass")` and, while a frame
was being built or answered, **refused and returned** - with a diagnostic telling the caller to wait for the frame or to
change passes between frames. A destructor can do neither: it has no later call, and waiting is a deadlock when the
frame being destroyed is the one destroying it.

So the rows stayed in the engine's table - the slots hold their `RenderPass` objects by `intrusive_ptr`, so they stayed
alive and stayed in the frame with a dead `owner` and a chain already dropped - and the per-pass backend release
(`removePass`: `releasePass` and the pass' own target) never ran. This is reachable from the shipped host: in threaded
mode (the `SurfaceWindow` default) `frame()` returns as soon as the job is submitted, and `~Impl` destroys the view
immediately before the engine whose destructor stops the frame thread. Closing a window during a frame is enough
(review 19 `Z22`).

The review's own words are the requirement: *"The fix has to be structural rather than a retry"*, and it named two
shapes - a deferred removal (what `RetirementQueue` already does for Vulkan objects) or ordering the view's death behind
the frame. It also warned that "ask, be refused, and drop it" is the one answer the tree has no idiom for.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Defer the take-back, keyed by the owner address, applied at the next frame start | Smallest structural change; the review's first suggestion | The owner is a RAW ADDRESS, and `operator new` hands the next view the memory the last one freed. A record that outlives its address by one frame deletes the rows of the object that registered second: the defect is fixed by introducing another one |
| B. Order the view's death behind the frame (make `~SceneView` wait, or hand the view to the engine) | Nothing is ever stale; one owner | A destructor cannot wait - when the destroying thread is the frame thread it waits for itself, and when it is the host thread it blocks the host on a frame it cannot influence. Handing a host object to the engine inverts ownership (`G10`) and delays the host's own teardown by a frame |
| C. Refuse as today and retry from the host | No engine change | Needs a LATER call site, i.e. a callback every host must implement (`G7`), and `~PassChain` has no hook to put it in. This is the shape that was here, and what it produced is a dead owner's rows |
| D. Defer the take-back, keyed by the REGISTRATION TICKET current at the ask; drain at the compile head and by `shutdown()` | Stale-proof by construction (a row registered later is a different object's, whatever the address); no waiting, no report, no host hook | One small queue and its leaf lock, one ticket per slot, and a rule the corpus has to state: a take-back looks at the table as it was when it was asked |
| E. Capture the doomed passes at the ask and remove them later by handle | Nothing to interpret at drain time | Reading the slot table at the ask is the very race the gate prevents - the compile on another thread is walking it |

## Decision

Option D.

**The rule.** A take-back is never refused. Between frames it is applied at once (same walk, same backend release).
While a frame records it is RECORDED - `detail::SessionShape::deferTakeBack` - and applied at the head of the next
compile, before `compileFrame()` reads the table, so the frame being built no longer announces the dead owner's rows.
`RenderEngine::shutdown` applies what is still waiting after the pump stops, so a take-back never outlives the engine
that held it. The engine does NOT report `engine.pass.changed_during_frame` here: that sentence asks the caller to wait
or to change between frames, and a destructor can do neither - the accepted-and-deferred answer is the whole answer.

**Why the ticket and not the address.** `Slot::registration` is a monotonic number stamped by `insertSlot`, i.e. the
only place a row joins the table, and `deferTakeBack` records `next_registration_` - the ticket not yet handed out -
under the same `phaseGate` every registration takes. The drain removes the rows of that owner whose ticket is smaller, so
it takes back exactly the rows that existed when it asked. A host that destroys a view and builds the next one at the
same address keeps its own rows; four lines of bookkeeping replace a class of defect the address-keyed version would
reintroduce. The ticket is read under the gate for one reason: without it, a registration landing between the refusal
and the record would be inside the bound, which is the same defect one step later.

**What the deferral costs nobody.** The frame already built or being answered keeps the shape it compiled, and the rows
are self-sufficient: `PassDesc` holds the camera, the content, the program and the target by `intrusive_ptr`, so a dead
owner's pass is drawn once more at most (invisible in practice - the view's surface was dismissed first) and the next
frame no longer announces it. The drain runs after `beginFrame`'s `waitIdle`, so no compiled frame still holds a
`raw_ptr` to a row it removes.

**The queue's lock.** `deferred_take_backs_` is appended from a destructor on any thread and swapped by a drain that
runs inside a frame, so it has its own leaf `std::mutex`; the phase gate cannot be taken in the drain (the drain is the
frame). Order: `phaseGate` then `deferred_mutex_` on the append path, never the reverse, and the drain holds it only
across the `swap` and releases backend state after it - the same "release outside the lock" rule `FrameResources` uses
for a retired transient.

## Consequences

- Good: a view (or a one-off chain) may be destroyed at any moment, including while a frame runs, and its rows and the
  backend state keyed by them leave by the next frame head; the ask cannot be silently dropped, and the host needs no
  new callback; `G3` reads in one line - `~SceneView` asks, the engine answers "now" or "at the head", never "no".
- Bad / cost: one vector append (a 2.8 ns plain-mutex lock, the same measurement C22 / C23 use) per dead owner and one
  `swap` per frame, empty in the steady state; one `std::uint64_t` per slot; and a deferred state the docs must state -
  between the ask and the next frame head, `passCount()` / `pass()` still see the dead owner's rows.
- Caveat, stated rather than served: a row the SAME address updates IN PLACE after asking is still older than the ask
  and goes with it. A live owner that asks to be taken back and then re-declares is a contradiction, not a case.
- Boundary: `applyPlan` and `bindPassContent` stay refusals (C20). They are retryable, and the refusal is the value that
  tells the host to wait. Nothing else becomes deferred - the take-back is the only ask whose caller is a destructor.

## Evidence

- `src/viz/graphics/src/SessionShape.{hpp,cpp}` - `Slot::registration`, `next_registration_`, `deferTakeBack`,
  `applyDeferredTakeBacks`, `removeOwnedRows` (the one walk both forms use).
- `src/viz/graphics/src/RenderEngine.cpp` - `removePassesOwnedBy` (gate, then apply-or-defer), `shutdown` applying what
  waited; `EngineCompile.cpp` - the drain at the compile head.
- `src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp` / `PassRegistry.hpp` - the contract as a host reads it.
- `tests/test_graphics/PlanTest.cpp` -
  `PlanTest.ATakeBackAskedInsideAFrameLandsOnTheNextFrame` (deferred, no refusal report, released at the next head),
  `PlanTest.ATakeBackThatWaitedLeavesRowsRegisteredAfterTheAskAlone` (the recycled address),
  `PlanTest.ATakeBackStillWaitingWhenTheEngineStopsIsApplied` (shutdown drains), and
  `PlanTest.ATakeBackWhileAFrameIsAnsweredWaitsForTheNextHead` (threaded: the take-back asked while the engine half
  answers - the shipped host's own order).
- `scripts/tsan_check.sh` - 2026-11-27: `test_graphics` 699/699, **0 ThreadSanitizer warning(s)**;
  `scripts/asan_check.sh` - `RESULT: PASS`.
