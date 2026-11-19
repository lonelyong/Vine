# progress primitives (`vn::Progress`)

> **Status**: living
> **Last verified**: 2026-11-19
> **Scope**: the dependency-free progress arithmetic - one [0, 1] scale, the move-only reporting handle and the staged scope. Not the ambient registry, the foreground stack, labels, cancellation ownership or any presentation; those are appfw's `ProgressHost` (`appfw-progress.md`).
> **Sources**: `src/base/progress/sdk/vine/progress/` (the contract), `tests/test_progress/ProgressIndicatorTest.cpp` (the behaviour), `src/fw/appfw/sdk/vine/appfw/ProgressHost.hpp` (the one consumer of the whole surface).
> **Backlog**: `../backlog/base-progress.md`

## 1. Overview

### 1.1 Problem and Background

A long-running operation has to answer two questions while it runs: "how far along am I" and "did the user ask me to stop". The first is arithmetic on a normalized scale, and it is needed by layers that must not depend on a UI toolkit - a mesh loader, a curve sampler or a B-rep kernel reports progress even when the caller is headless.

### 1.2 Goals

- One scale, `[0, 1]`, with an atomic position readable from any thread.
- A reporting spelling that cannot lose or double-count a step: allocation of a portion and the obligation to report it are the same object.
- A stage vocabulary (`scope`) so a multi-step operation maps one local `[0, max]` scale onto its share of the global one, and nests.
- Notification cheap enough for the reporting loop: one callback per percent step, not one per item.
- Zero dependencies beyond the platform (`vn::Global`); no Qt, no Core, no allocation on the reporting path.

### 1.3 Non-goals

- No presentation: no widget, no text line, no rate or ETA estimate. The module never decides what 42 % looks like.
- No ambient registry, no foreground stack, no label, no cancel-button ownership: a process-level view is application state and lives in appfw (`appfw-progress.md`).
- No time estimate: a percentage is known, the remaining duration is not.
- No `setPos(double)`: a loop knows its step, not its absolute position (`D5`, `G8`).

### 1.4 One-paragraph Conclusion

`vn::Progress` is two nouns and one handle: a `ProgressIndicator` owns the `[0, 1]` position and observes an externally-owned `std::stop_token`; a `ProgressScope` maps a local `[0, max]` scale onto a portion of that position; and a `ProgressRange` is the move-only, RAII reporting obligation for one portion - it reports when it is completed or destroyed, so the common `scope.next(1);` statement is the whole update. Notification is coalesced at the single writer to one callback per percent step.

## 2. Design Points

### 2.1 Three objects, one direction (`G1`, `G3`)

- What it is: `ProgressIndicator` (state) → `ProgressScope` (accounting) → `ProgressRange` (one portion). Data flows one way: a scope is built from a range, a range is only minted by a scope or by `start()`.
- Why: the caller reads "make a stage, take a step, the step reports" in one pass, and a simple job (`indicator.start()` plus one scope) needs no other concept.
- Boundary: a range never reaches back into a scope to change accounting; it only calls `ProgressIndicator::increment` through the scope it borrows.

### 2.2 The position is atomic, the accounting is not (`G10`, `G4`)

- What it is: `position_` and `announced_` are `std::atomic<double>` with relaxed ordering; every scope/range field is a plain value.
- Why: the operation reports from a worker while a presenter reads `position()` from the UI thread; that is the one cross-thread read, and it costs one relaxed load.
- Boundary: the scope/range graph is single-threaded by contract, so no lock exists anywhere in the module. The supported concurrent shape is one pre-allocated range or stage per thread, reporting into the same indicator; two threads never share a scope, because `next()` mutates its accounting.

### 2.3 The range is the obligation, so it is move-only (`G4`, `G10`)

- What it is: `ProgressRange` deletes copy construction and copy assignment, has a `noexcept` move constructor, and its move assignment completes the pending portion before taking over (`ADR-2026-11-19-progress-range-move-only.md`).
- Why: a second live handle would report the same portion twice, and the previous spelling - a copy constructor that disarmed its source through a `mutable` flag - made a copy of a `const` object change that object. One owner, transfer by move, is the `std::unique_lock` contract and what the repository's `Signal::Connection` already does.
- Boundary: "empty" is a single invariant (`parent_scope_ == nullptr`), not an extra flag; a moved-from, default and completed handle are the same state and every operation on it is a no-op.

### 2.4 Notification is coalesced at the single writer (`G7`, `G10`)

- What it is: `increment()` is the only writer of the position, and it announces a whole percent step, the end of the scale and a backwards step (a restart) - everything in between is silent. The constant is `kAnnounceStep = 0.01` in `ProgressIndicator.cpp`.
- Why: the reporting path runs once per item, so the per-item cost must stay a compare-exchange; the callback is one indirect call per percent. A presenter gets about 100 wakes for 10 000 items (`appfw-progress.md` 2.3 holds the measurement).
- Boundary: the coalescing mark is a hint, not a lock - two reporting threads may both announce, or one may overwrite the other's mark and leave one update for the next percent step. The position itself is already published, so nothing is lost.

### 2.5 Cancellation is observed, never owned (`G4`, `G5`)

- What it is: the constructor takes a `std::stop_token`; `isCancelled()` forwards to `stop_requested()` and `token()` hands the same token back.
- Why: the operation knows when it is worth stopping, and appfw shares one `std::stop_source` between the indicator, the host and the cancel button. An indicator that owned a source would create a second truth.
- Boundary: the indicator never calls `request_stop()`; a fresh `start()` does not clear a cancellation, because the token belongs to the operation.

### 2.6 A stage maps a local scale onto its portion (`G3`, `G9`)

- What it is: `ProgressScope(range, name, max)` becomes active by consuming the range; `next(step)` advances the local position and returns the range covering the mapped portion; `complete()` (also run by the destructor) reports the remainder and deactivates the scope.
- Why: a loop counts items locally and never converts to a global fraction; nesting is the same call pattern with a child scope built from `next()`'s range.
- Boundary: the local length is `max` clamped at `kMinLength` (`1e-6`), so a zero length cannot divide by zero; a step that does not advance the mapping returns an empty range.

### 2.7 The base layer stays dependency-free (`G2`, `G5`)

- What it is: the library links `vn::Global` only, and the header does not include Core, Qt or appfw.
- Why: a loader or a kernel reports progress without dragging in a framework; appfw's `ProgressHost` supplies the registry, labels and signal on top (`appfw-progress.md` 2.1).
- Boundary: the only seam toward presentation is `setPositionCallback(std::function<void()>)`; appfw turns that one call into its change signal.

### 2.8 The hidden root scope exists to mint the first range (`G1`, `G8`)

- What it is: every indicator owns a private root `ProgressScope` covering the whole scale; `start()` resets it and returns its `next()`.
- Why: a range is constructed from the scope it belongs to (that is how it finds the indicator), and the first range has no caller-made scope to come from. The root scope is that owner.
- Boundary: it is the one asymmetry in the model, it is not exposed, and it is never completed by a caller - `start()` resets its local position each run. A second range-carrier concept would remove it at the cost of a second way to reach the indicator, which is why it stays.

## 3. Features

### 3.1 Feature Index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Position on one scale | atomic `[0, 1]` position, clamped, readable from any thread | landed |
| F2 | Coalesced notification | one callback per percent step, the end, or a backwards step | landed |
| F3 | The reporting handle | move-only RAII portion; reports on completion or destruction | landed |
| F4 | The stage | local `[0, max]` mapped onto a portion; completes on destruction | landed |
| F5 | Nested stages | a child scope is built from `next()`'s range | landed |
| F6 | Cancellation | an externally-owned `std::stop_token`, observed only | landed |

### 3.1.1 F1 Position on one scale

- **Behaviour**: `position()` returns the stored value; `increment(step)` adds a step with a compare-exchange loop and clamps at `1.0`; `start()` stores `0.0` and returns the root range.
- **Constraints** (C1, C4): relaxed ordering; a step that would pass `1.0` lands exactly on `1.0`.
- **Requirements** (R1): a reader on another thread never sees a value outside `[0, 1]`.
- **Acceptance** (T1, T10, T12): `StartsAtZeroAndReportsThroughScope`, `PositionIsReadableFromOtherThread`, `ConcurrentRangesReportWithoutLosingAStep`.
- **Caveats** (N3): `start()` alone, with its range dropped, reports the whole run.

### 3.1.2 F2 Coalesced notification

- **Behaviour**: the installed callback runs from `increment()` when the new position is at least `kAnnounceStep` past the announced one, when it reaches `1.0`, or when it moves backwards; `start()` announces a restart when the mark was past zero.
- **Constraints** (C5): the callback runs on the reporting thread with no lock held; it must be safe to run concurrently with itself.
- **Requirements** (R2): installing a callback after reporting starts is a data race, not a supported sequence.
- **Acceptance** (T7, T8, T9, T14): `PositionCallbackIsCoalescedToPercentSteps`, `ReachingTheEndIsAlwaysAnnounced`, `RestartingAnnouncesAgain`, `PositionCallbackIsSafeFromConcurrentReporters`.
- **Caveats** (N6, N7): a throwing callback terminates when it fires from a destructor.

### 3.1.3 F3 The reporting handle

- **Behaviour**: `complete()` reports the portion and empties the handle; the destructor calls it; move construction transfers and empties the source; move assignment completes the target's pending portion first.
- **Constraints** (C3): copy construction and copy assignment are deleted; an empty handle is inert.
- **Requirements** (R3, R4): no path drops a portion silently, and `complete()` is idempotent.
- **Acceptance** (T3, T4, T5, T11, T12): `RangeIsActiveOnceOnly`, `MoveTransfersTheReportingObligation`, `MoveAssignmentCompletesThePendingPortionFirst`, `ReportingWithoutACallbackIsHarmless`, `ConcurrentRangesReportWithoutLosingAStep`.
- **Caveats** (N1, N2): the handle borrows its scope, so the destruction order is part of the contract.

### 3.1.4 F4 The stage

- **Behaviour**: `ProgressScope(ProgressRange&&, name, max)` consumes the range and maps `[0, max]` onto it; `next(step)` returns the covering range; `complete()` reports what is left of the stage and deactivates it.
- **Constraints** (C2): a scope is neither copyable nor movable; an empty range yields an inactive scope that reports nothing.
- **Requirements** (R5): the mapping is linear and the local length is clamped at `kMinLength`.
- **Acceptance** (T1, T6, T13): `StartsAtZeroAndReportsThroughScope`, `NestedScopesMapOntoTheParentRange`, `ConcurrentScopesReportTheirOwnPortions`.
- **Caveats** (N4, N5): the step is reported when its range dies, and a zero length completes instantly.

### 3.1.5 F5 Nested stages

- **Behaviour**: a child `ProgressScope` is constructed from the range that `next()` returned, so the child's `[0, max]` covers exactly that step of the parent.
- **Constraints** (C2): the child borrows the parent scope; it must be destroyed first.
- **Requirements** (R5): a child step at local `k` maps to `parent_start + parent_length * k / parent_max`.
- **Acceptance** (T6): two child steps of four report `0.2` and `0.4` of the global scale.
- **Caveats** (N1, N2): a child alive across a parent completion or an indicator restart reports into the wrong run.

### 3.1.6 F6 Cancellation

- **Behaviour**: `isCancelled()` and `token()` on the indicator, `isCancelled()` forwarded by the scope and the range.
- **Constraints** (C6): the token is stored by value and never requested by the module.
- **Requirements** (R6): a cancellation is a request; the operation decides whether the step loop stops.
- **Acceptance** (T2): `CancelTracksTheExternalToken`.
- **Caveats** (N8): `start()` does not clear a cancellation.

## 4. Public Interface

### 4.1 `ProgressIndicator`

| Interface | Contract | Notes |
|---|---|---|
| `ProgressIndicator` | constructs on an externally-owned token; empty token means "never cancelled" | allocates the private root scope |
| `start` | resets the position and announced mark, returns the root range covering `[0, 1]` | the range is one-shot and move-only |
| `position` | overall progress in `[0, 1]` | atomic relaxed load, any thread |
| `isCancelled` / `token` | the observed cancellation state and the token itself | never requests a stop |
| `setPositionCallback` | installs the coalesced notification | install before reporting |

### 4.2 `ProgressRange`

| Interface | Contract | Notes |
|---|---|---|
| `ProgressRange` | empty, move-only handle; no copy | the obligation has exactly one owner |
| `isActive` | true while the portion is still owed | empty handle answers false |
| `isCancelled` | forwards the indicator's cancellation | false when empty |
| `complete` | reports the portion once, then empties the handle | idempotent, may run the callback |

### 4.3 `ProgressScope`

| Interface | Contract | Notes |
|---|---|---|
| `ProgressScope` | inert default; the range-taking constructor consumes the range | neither copyable nor movable |
| `next` | allocates a step and returns the range covering it | the temporary reports when it dies |
| `complete` | reports the stage remainder and deactivates the scope | also run by the destructor |
| `isActive` / `isCancelled` | whether the scope reports / the operation should stop | an inert scope answers false / false |
| `localPos` / `localLength` / `globalLength` | the stage's position and lengths | `localPos` of an inert scope reports its length |
| `name` / `parent` / `indicator` | the stage label and the borrowed links | no consumer today (`G8`, `../backlog/base-progress.md`) |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints

- **C1**: only the position and the announced mark are atomic; a scope/range graph belongs to one thread.
- **C2**: a scope and a range borrow what they point at; neither extends a lifetime.
- **C3**: a range is move-only and one-shot; a scope is neither copyable nor movable.
- **C4**: the position is clamped to `[0, 1]`; a portion that would pass the end lands on it.
- **C5**: notification is coalesced to `kAnnounceStep`, the end, and a backwards step; it runs on the reporting thread with no lock held.
- **C6**: cancellation is an externally-owned `std::stop_token`; the module only observes it.

### 5.2 Requirements

- **R1**: a position read from another thread is always inside `[0, 1]`.
- **R2**: the position callback is installed before reporting starts.
- **R3**: no code path drops a pending portion silently - destruction, `complete()` and move assignment all report it.
- **R4**: `complete()` is idempotent on a range and on a scope.
- **R5**: a scope maps its local `[0, max]` linearly onto its portion, with `max` clamped at `kMinLength`.
- **R6**: a cancellation is a request; the module never decides to stop for the caller.
- **R7**: several threads may report through disjoint, pre-allocated ranges or stages; allocation (`start()`, `next()`) stays on one thread, and no two threads share a scope.

### 5.3 Invariants

- **N1**: `0 <= position() <= 1` after every operation.
- **N2**: an inserted step never moves the position more than the local step's mapped share.
- **N3**: a range that is empty (`parent_scope_ == nullptr`) touches nothing.

## 6. Caveats and Traps

### 6.1 Destruction order

- How it fails: a range or scope that outlives what it borrows dereferences a destroyed object - `isActive()` and `complete()` read `parent_scope_->indicator_`.
- How to detect it: a use-after-free on a stack address, usually reported as a corrupted pointer or a crash inside `increment`; ASan only sees it when the reading code is instrumented too.
- How to prevent it: declare the members in the order indicator → scope → `unique_ptr<ProgressRange>`, so reverse destruction is range → scope → indicator. `StartupProgress.cpp` does exactly that.

### 6.2 A stage that outlives a restart

- How it fails: `start()` resets the position, but a still-live old scope completes later and adds its remainder to the new run - measured as the new run jumping to `0.8` before any work.
- How to detect it: a progress bar that starts a new stage already near its end, with no item reported.
- How to prevent it: destroy every scope and range of the previous run before calling `start()`; `StartupProgress::stage()` resets its stage scope first. A debug assertion in `start()` is in the backlog.

### 6.3 Dropping the range `start()` returns

- How it fails: `indicator.start();` as a statement destroys the returned range immediately, which completes it - the position becomes `1.0` before the operation runs.
- How to detect it: a bar that reaches 100 % at the start of an operation.
- How to prevent it: keep the range (`auto root = indicator.start();`) or hand it to a scope; `ProgressHost::scope()` exists so the common case never touches it.

### 6.4 The bar leads the loop

- How it fails: `scope.next(1);` reports the step on the same line, so the bar shows the item as done before the body runs it - one item ahead at every step.
- How to detect it: the bar reaching 100 % while the last item is still processing.
- How to prevent it: call `next()` at the end of the loop body when the bar must not lead, or accept the lead - it is a deliberate consequence of "the step reports when its range dies".

### 6.5 A zero or negative local length

- How it fails: `max <= 1e-6` is clamped to `1e-6`, so an operation that computes a total of zero gets a stage that completes instantly and reports 100 %.
- How to detect it: a stage that reaches the end before reporting anything.
- How to prevent it: treat "no items" as "no stage" at the call site (as `StartupProgress::stage(name, total)` does with `total > 0.0`).

### 6.6 A throwing position callback

- How it fails: `complete()` runs inside `~ProgressRange` and `~ProgressScope`; a callback that throws escapes a `noexcept` destructor and calls `std::terminate`.
- How to detect it: the process dies during a scope's destruction with no exception handler running.
- How to prevent it: the callback must not throw. `Signal::trigger` does not catch either, so a subscriber that throws has the same effect one layer up.

### 6.7 Installing the callback after reporting starts

- How it fails: `on_position_` is a plain `std::function`; writing it while another thread reads it in `announce()` is a data race.
- How to detect it: a torn read under TSan, or a callback that fires once and then stops.
- How to prevent it: install the callback in the owner's constructor, before the operation starts (`ProgressHost` does).

## 7. Rejected Options

| # | Option | Why not | ADR |
|---|---|---|---|
| D1 | A copyable range whose copy disarms the source (the previous spelling) | A copy constructor that mutates its source changes a `const` object's observable state (`G4`), and the `mutable` flag it needed existed only to hide that | `../decisions/ADR-2026-11-19-progress-range-move-only.md` |
| D2 | A reference-counted handle that reports on the last copy's destruction | The reporting moment becomes unknowable; an accidental long-lived copy holds the bar back (`G4`, `G10`) | - |
| D3 | Notifying on every item | One indirect call per item on the reporting path; measured at about 100 wake-ups for 10 000 items with coalescing (`appfw-progress.md` 2.3, `G10`) | - |
| D4 | A virtual presenter interface inside the module | Puts a presentation-shaped extension point in the dependency-free layer, and every consumer would have to implement it (`G2`, `G7`) | - |
| D5 | `setPos(double)` instead of `next(step)` | An absolute setter lets two reporting paths disagree and needs a fraction the loop usually does not have; there is no consumer today (`G8`) | - |
| D6 | The indicator owning its own `std::stop_source` | Cancellation belongs to the operation that knows when to stop; a second source would disagree with appfw's cancel button (`G4`) | - |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | half the steps put the bar at half, the scope's end puts it at the end | `ProgressIndicatorTest.StartsAtZeroAndReportsThroughScope` |
| T2 | the external source drives `isCancelled()` and `token()` | `ProgressIndicatorTest.CancelTracksTheExternalToken` |
| T3 | a root feeds exactly one scope; the consumed range is empty | `ProgressIndicatorTest.RangeIsActiveOnceOnly`, `ProgressHostTest.RangeIsActiveOnceOnly` |
| T4 | moving transfers the obligation and empties the source | `ProgressIndicatorTest.MoveTransfersTheReportingObligation` |
| T5 | move assignment reports the pending portion before taking over | `ProgressIndicatorTest.MoveAssignmentCompletesThePendingPortionFirst` |
| T6 | a child stage maps onto its parent's portion | `ProgressIndicatorTest.NestedScopesMapOntoTheParentRange` |
| T7 | 10 000 steps fire about 100 notifications | `ProgressIndicatorTest.PositionCallbackIsCoalescedToPercentSteps` |
| T8 | a sub-percent last step still announces the end | `ProgressIndicatorTest.ReachingTheEndIsAlwaysAnnounced` |
| T9 | a restart announces the reset | `ProgressIndicatorTest.RestartingAnnouncesAgain` |
| T10 | `position()` is readable while another thread reports | `ProgressIndicatorTest.PositionIsReadableFromOtherThread` |
| T11 | reporting without a callback is harmless | `ProgressIndicatorTest.ReportingWithoutACallbackIsHarmless` |
| T12 | eight threads completing pre-allocated ranges lose no step and never expose a position outside `[0, 1]` | `ProgressIndicatorTest.ConcurrentRangesReportWithoutLosingAStep` |
| T13 | eight stages, one per thread, together cover the whole scale | `ProgressIndicatorTest.ConcurrentScopesReportTheirOwnPortions` |
| T14 | the position callback is callable from several reporting threads at once | `ProgressIndicatorTest.PositionCallbackIsSafeFromConcurrentReporters` |

Run with `ctest -R test_progress` (or `ctest -R test_gui` for the host-level case).

## 9. Status and Evolution

### 9.1 Current Status

All six features are landed, the fourteen cases above pass, and the module is clean under AddressSanitizer, LeakSanitizer and ThreadSanitizer (the three concurrent cases were repeated thirty times under TSan with no report). The public surface has no owed documentation (`scripts/public_docs_baseline.txt` names none of its headers).

### 9.2 Known Gaps and Backlog

- The introspection accessors (`name`, `parent`, `indicator`, `localPos`, `localLength`, `globalLength`) and the inert default constructor have no consumer; `localPos()` reports the local length for an inert scope. See `../backlog/base-progress.md` - removal waits for the next deliberate API pass.
- The restart trap (6.2) is documented, not detected. A debug assertion is the proposed cheap fix.
- `max <= 0` silently becomes `1e-6` (6.5); whether that should be an assertion is undecided.

### 9.3 Compatibility and Migration

There is no ABI promise for this module yet. appfw embeds `ProgressIndicator` by value inside `ProgressHost`, so a layout change here silently changes appfw's plugin ABI - `VN_APPFW_PLUGIN_ABI_VERSION` compares a declared number, not a layout. The next batch that changes this module's layout is the one that justifies an appfw ABI bump.

Source migration from the previous spelling: replace `ProgressScope s(range, ...)` with `ProgressScope s(std::move(range), ...)`; a copy of a `ProgressRange` no longer compiles, and `complete()` on an empty handle stays a no-op.

## 10. References

### 10.1 Code

- `src/base/progress/sdk/vine/progress/ProgressIndicator.hpp`, `src/base/progress/src/ProgressIndicator.cpp` - the scale, the position and the coalesced notification.
- `src/base/progress/sdk/vine/progress/ProgressRange.hpp`, `src/base/progress/src/ProgressRange.cpp` - the move-only reporting handle.
- `src/base/progress/sdk/vine/progress/ProgressScope.hpp`, `src/base/progress/src/ProgressScope.cpp` - the stage and its mapping.
- `src/fw/appfw/sdk/vine/appfw/ProgressHost.hpp` - the ambient consumer and its shares.
- `tests/test_progress/ProgressIndicatorTest.cpp`, `tests/test_gui/ProgressHostTest.cpp` - the acceptance cases.

### 10.2 Documents and ADRs

- `appfw-progress.md` - the process-level registry, foreground stack, labels and the two presenters; 2.3 holds the coalescing measurement.
- `../decisions/ADR-2026-11-19-progress-range-move-only.md` - why the handle is move-only.

### 10.3 External Sources

- `std::unique_lock` (C++17) - the one-owner, move-assigned, complete-on-replacement shape this handle follows.
- `std::stop_token` / `std::stop_source` (C++20) - the cancellation vocabulary.
