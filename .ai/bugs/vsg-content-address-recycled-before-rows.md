# A content object is released while its rows can still be read, so one shading-mode switch stops drawing the whole pass

- **Status**: Fixed (2026-11-09).
- **Found**: 2026-11-09
- **Module**: `src/plugins/gfx_backend_vsg`
- **Files**: `ContentStore::releaseAbandoned()`, `ContentStore::releaseGeometry/releaseProgram/releaseMaterial`,
  `ContentHalves::Data::Key` / `find` / `answerable`, `ContentFacts::ProgramFacts`, `ContentPass::record()`

## Symptom and impact

Switching the model viewer to one shading mode (the measured one was "normals") **froze the picture at what it was before
the switch**: every later frame, mouse drag included, reported only

```
[graphics] the pass' sampled inputs is not drawn: no compiled content half was built for this pass
```

The command itself succeeded (`CommandManager` logged `Command succeeded: set_shading_normals`) and the frame of the
switch was fine - from the **next** frame on, the whole pass (clear included) was refused, so the picture never updated
again. Switching to a different mode "fixed" it. It looked like "sometimes a switch does nothing".

## Root cause

The three fact tables in `ContentStore` are keyed by **object address** (`ProgramFacts::program` /
`GeometryFacts::geometry` / `MaterialFacts::material`), and rows are deleted later than the object: the deletion is parked
on the `RetirementQueue` (in-flight frames may still name those rows). But `releaseAbandoned()` **dropped the store's
reference to the object immediately after parking succeeded**:

```cpp
const bool parked = retirement.retire(timeline, [weak, identity]() { ... eraseProgramOf(identity); });
if (!parked) { ++d->retained; }
it = d->programs.erase(it);      // <- the object is gone here; its address is free to reuse
```

The address could therefore be reused by the allocator for the whole parked window in which the row was still in the
table. A shading switch is exactly "create one new program object and drop one old one", so the new object easily lands on
the address that was just freed (same size class, just freed). Then:

1. the new program `track()`s on the same address -> a new `LiveProgram` (`described = false`) -> `ensureProgram()`
   **appends** rows for the **new text** under the same key `(address, revision, variant, kind)`;
2. the old object's park expires -> `eraseProgramOf(address)` erases **by identity** - and takes the rows the new object
   has just described with it;
3. the new object's `LiveProgram` still records `described && described_revision == revision && variant built`, so
   `ensureProgram()` returns early forever and **the rows are never rebuilt**;
4. `halvesFor()`'s `findProgram()` misses forever -> the pass has no content half -> `ContentPass::record()` refuses the
   whole pass.

The same class of problem had a second home: a `ContentHalves` key held only
`(address, revision, layout, variant, topology, attachment count)`. A built-in preset program is created with the **same
revision** every time (two stages), so when an address was reused the old half's key matched the new program exactly - even
without the accidental row erase, the new program would be fed the pipeline compiled for the **previous** mode (the
picture stays on the previous mode).

## Fix

1. **The park owns the object** (three tables plus three explicit release paths): the store's reference moves into the
   park callback, so the same action decides when the row goes and when the object dies; when the park cannot be
   scheduled (no in-flight window yet) the **entry is kept** and retried next frame - the address is never released early.
2. **Description generation**: `ContentStore` increments `ProgramFacts::generation` on every appended row, and both the
   `ContentHalves` key and `answerable()` compare it. Two different programs with the same address and the same revision
   can therefore never share one compiled result.
3. **Report the real cause**: a pass-level refusal now reports one extra line with the conclusion of
   `describeMissingContentHalf()` (a geometry row, a material row, a program row, or "the half build was refused") - the old
   message pointed at the sampled inputs, while the real cause was one of four.
4. **The third instance of the same shape (`StreamUploads`)**: upload entries are keyed by
   `(buffer address, revision)`, a new buffer always has revision 0, and an entry outlives the frame by `slots + 1` frames
   - a reused address meant an old entry impersonating a new stream. An entry now **holds** the `vn::Buffer` its key names
   (`VertexEntry::owner` / `IndexEntry::owner`), so the address lives as long as the entry.
5. **The instance stream had no handle**: `CompiledCommand::instance_buffer` used to be a **borrowed raw pointer**, so
   `ContentPass` could not reach a handle and could not make the upload entry hold it. The plan now **holds** the instance
   stream (`intrusive_ptr<const Buffer<float>>`, passed through collection, compilation and recording), and
   `instance_revision` is deleted with it - the revision is read from the held buffer, the same spelling as
   `ChannelFacts::streamKey()`. One rule therefore holds everywhere: **a reader holds the data it reads, and a key table
   holds the object its key names** (`ADR-2026-11-09-address-keys-hold-the-object`).

## Verification

- `ContentStoreTest.AParkedRemovalKeepsTheObjectsAddressReservedUntilItsRowsLeave`: after parking the object must **not** be
  destroyed and a new allocation cannot get the same address; when the park expires the object and its rows disappear
  together.
- `ContentStoreTest.AReleaseThatCannotBeParkedYetKeepsItsEntryAndIsRetried`: when the park cannot be scheduled the entry
  stays together with its object and the rows still answer; after `setSlots()` the same entry is retried and parked, and
  only then does the park expire and destroy it.
- `ContentHalvesTest.AnotherDescriptionOfOneIdentityIsAnotherHalf`: the same address, the same revision and a different
  `generation` must compile **another** half, the old half is parked, and afterwards it is stable and not rebuilt.
- `StreamUploadsTest.AnEntryHoldsTheBufferItsKeyNames`: the entry holds the buffer (`useCount == 2`), a new buffer cannot get
  the same address, and the reference count falls back to 1 once the entry is evicted.
- `FrameCompilerTest.ThePlanHoldsTheInstanceStreamItReads`: the plan holds the instance stream, and the bytes stay readable
  after the host drops its own handle.
- `ContentPassTest` (the instanced-draw case): after recording, `instance_storage->useCount() > 1`, so the upload entry
  really holds the instance stream.
- `ContentSweepTest.EverythingTheHostDroppedIsLetGoInOneCallAndItsRowsLeaveAtThePark`: a material object outlives the drop
  by one window and its textures are released by the **next** sweep (the lifetime changed, so the assertion follows).
- The full `test_vsg` and the full `test_gui`.

## Related risks still open (not fixed here)

- **A failed program-row build is retried every frame**: a failed geometry or material build records the revision it tried
  (`described_revision = revision`) and is not retried, while a program only records `described`, so a variant that fails to
  build (for example its text sits in an `#error` branch, or the ABI cannot be scanned) **copies both shader texts and runs
  `scanProgramAbi` every frame** until the revision changes. Any change to a `ShaderProgram` bumps the revision, so the retry
  buys nothing but cost - a cost problem, not a correctness one, and not observable from outside (`builds()` counts only
  successful appends), so no code was changed for it and it is recorded here.

## Design constraints

- **G10**: the address is this cache's key, so "how long the object lives" is part of correctness and not a memory
  optimisation; releasing the address inside the parked window bets correctness on the allocator.
- **G11**: the failure surface said only "no content half", which points at the wrong place; a failure now says which table
  it is missing from.
- **G4**: `releaseAbandoned`'s `retained` used to count without retaining, so the name did not match the behaviour.
