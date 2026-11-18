# A data race between the vk target record's `fresh` and a refusal check that does not wait

- **Status**: Fixed (2026-11-17, `gfx_backend_vk`; `test_vk` 169 green, TSan clean)
- **Found**: 2026-11-17
- **Module**: `src/plugins/gfx_backend_vk` (inside the plugin; no public API change)
- **Files**: `include/vine/backend_vk/internal/TargetObjects.hpp` (`fresh`),
  `include/vine/backend_vk/internal/TargetTable.hpp` and `src/internal/TargetTable.cpp` (`peek`),
  `src/internal/Device.cpp` (readback phase one and two, the frame body's reads and writes),
  `src/internal/TargetObjects.cpp` (build / rebuild / destroy)

## Symptom and impact

`VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` reported a real race: a write at `Device.cpp:1733` (the frame body's
`images->fresh = false`) against a read at `Device.cpp:3484` (`peek->fresh` in `readColorBuffer`'s first phase), on two
different threads with no synchronisation. The suite itself was 167/167 green - a plain assertion never sees this.

The consequence is undefined behaviour in the textbook sense (a plain `bool` shared by two threads without a lock). The
visible consequence is absorbed by the semantics of the answer: a stale read still lands on the `NotReady` or Ok the SDK
allows, so "the result was right" cannot be used as evidence; the only evidence is TSan (or one day a compiler
optimisation).

The same phase one also read `built()` / `colorCount()` / `colorFormat()` through `lookup`'s `shared_ptr`. `ensure` rebuilds
**in place** (a resize rewrites `desc`, a rebuild calls `destroyTarget` and then builds), while `release` / `drain` hand the
record to the park and destroy it **outside the table** - so this is not one missing field but the whole shape "read fields
through a borrowed record" failing in phase one.

## Root cause

Three kinds of writer, two kinds of synchronisation:

1. `ensure` (holds the map lock): build / resize / destroy rewrite the record in place;
2. `release` / `drain` (erase under the map lock, with the park deciding outside the table when it is destroyed);
3. the **frame body** (takes no lock - the recording path must not lock, `C3` / `C5`): writes `fresh = false` when each pass
   ends.

Phase one read fields through `lookup`'s pointer outside the lock: it held neither the lock of 1 and 2 nor an atomic for 3.
Phase two (inside the body gate's EXCLUSIVE section) was always safe, because the gate drains the frame body - so "look
again a little later" preserved the **result** without preserving the **read** itself. That is exactly why the defect is
easy to miss: it only lives in the stretch between the two checks.

## Fix

1. `TargetObjects::fresh` became `std::atomic<bool>` (relaxed). Reason: it is the only field the frame body writes on the
   recording path, and that side cannot take a lock; relaxed is enough because phase one's conclusion is re-checked inside
   the gate, and the two real happens-before edges come from the existing hand-offs - the claim lock (this frame's write to
   the next holder's read) and the body gate (the frame body's write to the host verb's read).
2. New `TargetTable::peek()`: reads `built` / `fresh` and the **shape** into one value (`TargetTable::Peek`) under the map
   lock, and phase one reads only that value. The lock is the one every writer takes, so the snapshot is always a complete
   state (either before or after a rebuild) and no record is borrowed across the table.
3. Phase one's depth format uses the format of the **built shape** rather than the host object's current declaration - the
   hint has to describe the image that will really be copied; otherwise, when the host changes the declaration before the
   frame arrives, phase one can answer a permanent `Unsupported`.
4. Every read in phase two and in the frame body is an explicit `load(std::memory_order_relaxed)`: one object must not be
   half atomic and half not.

## Verification

- `VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` -> **169/169, 0 ThreadSanitizer warnings** (the same command reported one
  real race before the fix).
- `test_vk` **169** (167 plus 2 new cases), `test_graphics` **617**, `test_vsg` **572**, `BackendParityTest` **13**.
- New case `VkMultiSessionTest.AReadbackRacingARebuiltTargetAnswersAValueItCanStandBehind`: a readback thread races a driver
  thread that resizes once per frame; the answer may only be `Ok` or `NotReady`, and an `Ok` must be a whole frame (its size
  one of the two that were drawn, its pixels the clear colour). New case
  `TargetTableTest.APeekAnswersAnUnbuiltTargetAndInsertsNothing`: `peek` answers "not built" for an unknown target and
  **inserts no entry**.
- All nine gates green (`check_ai_docs --strict` / `check_design_docs` / `check_doc_symbols` / `check_include_hygiene` /
  `check_api_style` / `check_diagnostic_formats` / `check_public_docs` / `check_export_annotations` / `check_vk_exposure`).

## Design constraints

- **G10**: shared mutable state has to be costed with ownership and synchronisation; the price of one `bool` is one
  undefined behaviour.
- **G11**: a failure has to be locatable - phase one now has one entry and one lock, and which state it read can be said at
  a glance.
- **G4**: `lookup` (borrows a record, has a lifetime) and `peek` (takes a value, has none) are two spellings for two things,
  and the caller does not have to guess.

## Neighbouring problem (not fixed here, recorded in the backlog)

`executeFrame` / `prepareFrame` immediately call `*target_table.lookup(target)` after `ensure` (`Device.cpp:1343`,
`Device.cpp:2056`). `releaseRenderTarget` **does not wait for in-flight frames** (R6), so it can erase the entry between
those two steps and the second `lookup` returns null, which is a null dereference. The fix is to let `ensure` hand out the
record itself, or to null-check the second `lookup` and skip that pass as a claim conflict.
