# ADR-2026-11-19: the progress reporting handle is move-only

> **Status**: Accepted
> **Date**: 2026-11-19
> **Scope**: `src/base/progress/sdk/vine/progress/ProgressRange.hpp`, `src/base/progress/src/ProgressRange.cpp`, `src/base/progress/sdk/vine/progress/ProgressScope.hpp`, `src/base/progress/src/ProgressScope.cpp`, `tests/test_progress/ProgressIndicatorTest.cpp`, `tests/test_gui/ProgressHostTest.cpp`

## Background

`ProgressRange` is the RAII handle that reports one portion of the progress scale: it advances the indicator when it is completed or destroyed, which is what makes `scope.next(1);` the whole per-item update.

Its original spelling was "move-on-copy": a copy constructor that copied the fields and then set `other.used_ = true`, with `used_` declared `mutable` so a copy of a `const` range could disarm the source. Two consequences were visible in review:

1. A copy constructor that mutates its source means `const ProgressRange r = ...; auto s = r;` changes what `r` reports - a `const` object's observable state. The type claimed value semantics and implemented transfer.
2. Copy assignment never completed the target's pending portion. Measured with a scope of 10: after `a = b` the `0.4` that `a` still owed was gone, and `a.complete()` reported only `b`'s `0.2` instead of `0.6`.

The question that had to be settled - "is it reasonable to forbid copying and allow only moving?" - is likely to be asked again whenever a caller wants to keep a range around, so it is recorded here.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the copyable handle that disarms its source (the previous spelling) | No `std::move` at any call site; value syntax | A copy mutates its source, so copying a `const` object changes it (`G4`); the `mutable` flag existed only to hide that; copy assignment silently drops the target's pending portion (`G10`) |
| B. Move-only handle; move assignment completes the target first (**adopted**) | One owner, so a portion cannot be reported twice; "empty" is one invariant (`parent_scope_ == nullptr`) instead of a flag; replacement reports instead of dropping; identical to the `std::unique_lock` contract and to `Signal::Connection` in this tree | Two test call sites must spell `std::move`; the layout loses the `used_` flag (a module-internal ABI change); move assignment cannot be `noexcept`, because completing runs the position callback |
| C. Copyable handle where a copy reports and empties the source immediately | Still one report per portion | The reporting moment becomes a side effect of copying, which is worse than assignment; a copy inside an expression would move the bar |
| D. No handle at all - only `scope.advance(step)` | Smallest public surface | A step could only be reported when it starts, so "report when the work is done" would need a second concept or an explicit `begin`/`end` pair (`G3`, `G7`) |
| E. Reference-counted shared handle | Copyable, no `std::move` | The bar would advance when the last copy dies, an unknowable moment, and one forgotten copy holds it back (`G4`, `G10`) |

## Decision

Adopt **B**:

- `ProgressRange` deletes copy construction and copy assignment.
- `ProgressRange(ProgressRange&&)` is `noexcept` and transfers the portion, leaving the source empty.
- `ProgressRange& operator=(ProgressRange&&)` completes the target's pending portion first, then takes the new one over. It is deliberately not `noexcept`.
- `ProgressScope` takes the range as `ProgressRange&&` and consumes it (`range.clear()`), so a named range cannot be handed over twice by accident; `ProgressScope` itself becomes explicitly neither copyable nor movable.
- "Empty" is expressed by `parent_scope_ == nullptr`; the `mutable bool used_` flag is deleted.

## Consequences

- Good: there is exactly one owner of a reporting obligation at any time, and every path that gets rid of a live handle - destruction, `complete()`, move assignment - reports what it owed.
- Good: the class follows a standard-library shape (`std::unique_lock`) and the repository's existing handle (`Signal::Connection`), so a reader does not have to learn a private convention.
- Good: the internal flag is gone; the state is the pointer.
- Bad / cost: a caller that wants to pass a range on must write `std::move`; two tests were updated and their comments no longer describe a "move-on-copy" range.
- Bad / cost: `operator=(ProgressRange&&)` can throw through the position callback, so it is not `noexcept` and cannot be used in a context that requires one.
- Bad / cost: the layout changed (`used_` removed), which is free today because the module has no external consumer, but changes appfw's plugin ABI whenever `ProgressHost` embeds an indicator.
- Trigger to reopen: a consumer that genuinely needs two independent owners of the same portion (none exists today), or a standard that makes a non-`noexcept` move assignment unacceptable at a call site.

## Evidence

- `ProgressIndicatorTest.MoveTransfersTheReportingObligation` - the source is empty after the move and the moved-to handle reports `0.4`.
- `ProgressIndicatorTest.MoveAssignmentCompletesThePendingPortionFirst` - the position is `0.4` right after the assignment and `0.6` after the completing `complete()` (the defect measured `0.1`).
- `ProgressIndicatorTest.RangeIsActiveOnceOnly` and `ProgressHostTest.RangeIsActiveOnceOnly` - a root feeds exactly one scope, and a consumed range is empty.
- A negative compile check: `ProgressRange b(a);` and `c = a;` fail with "use of deleted function".
- `ctest -R test_progress` (14 cases) and `ctest -R test_gui` pass; the module is clean under AddressSanitizer and LeakSanitizer, and the three concurrent cases repeat thirty times under ThreadSanitizer without a report.
