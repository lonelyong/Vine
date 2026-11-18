# ADR-2026-11-09: a cache keyed by address must hold the object its key names

- Status: Partly superseded (the "identity" half is superseded by `ADR-2026-11-11-content-identity-is-a-handle.md`: a content key became a
  `Handle`/`ContentRef`. This document's rule, "holding is identity, so hold it as long as the key stays queryable", still governs **the tables not yet moved to handles**
  - stream uploads, compiled halves, pipeline variants)
- Date: 2026-11-09
- Related: `../design/graphics-content-lifecycle.md` (2.5 / 2.6), `../bugs/vsg-content-address-recycled-before-rows.md`,
  backlog `graphics.md` T19 (closed when this decision landed and the item deleted; the evidence stays below)

## Background

Every cache table in the backend keys on the **address** of a host object: the fact table (`ContentStore`'s geometry / program / material rows), the compiled halves
(`ContentHalves::Data::Key`), stream uploads (`StreamKey`'s `buffer`, `DataKey`'s `buffer`) and pipeline variants
(`PipelineKey`'s `program`). The reason was that only pointers exist in a plan (`CompiledCommand` is a `const void*`), so converting to an address costs nothing.

But an entry outlives its object: a fact row parks until its in-flight frame ends, a stream entry lives `slots + 1` frames longer, and a pipeline variant lives until it is evicted or the session ends.
A switch-over is exactly "create a new program and drop an old one", so a new object of the same size easily gets the just-freed address, and a built-in preset has the
same revision every time it is created - so an old entry impersonates a new identity. Three consequences were measured:

1. `ContentStore` dropped its object reference once the park succeeded → a row a new program appended at that same address was erased by the old identity's park → `LiveProgram.described`
   still said → the row was never rebuilt → the whole pass (clear included) was refused for good.
2. `ContentHalves`' key was only (address, revision, …) → after the address was reused, the new program was fed the pipeline compiled for the previous one (the picture froze on the previous one).
3. A `StreamUploads` entry held only the key and not the buffer → a new buffer (revision 0) was fed bytes written by a different geometry
   (historically the strip/fan pixel cases of `PrimitiveDrawTest` passing or failing at random, see backlog T19).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. **Holding is identity**: the entry (or its park) holds the object its key names, so the address lives as long as the entry | Local, no new concept, moves with the existing park mechanism; the key is still a pointer, so the conversion is free | It is a **convention**: the author of a new table must remember to hold, and forgetting it means a silently wrong picture |
| B. **Stable identity**: hand every cacheable object an id that never repeats (one counter each on `Object` / `RefCounted`) and key everything by it | Dangling is impossible; patches such as `generation` can go | The public key vocabulary and the plans of `StreamKey`/`DataKey`/`PipelineKey`/`ProgramRef`/`CompiledCommand` all change, 30+ files; `Buffer` is not in the `Object` family, so another identity source is needed as well |
| C. **Add a generation to every key**: whoever reuses an address bumps a counter | A small change | One concept written four times (fact rows, half keys, stream keys, pipeline keys), which turns A's convention into four patches; `G6` is explicitly against two spellings of one fact |

## Decision

Chose **A**, and turned the "convention" into a checkable rule rather than a memory exercise:

- **The data is read, so the reader holds it**: the instance stream a plan holds changes from raw pointers to `intrusive_ptr<const Buffer<float>>`
  (`CollectedCommand` / `CompiledCommand`). That also pins the address - a stream entry's key is exactly that address.
- **Hold it as long as the key stays queryable**: the fact table's park callback holds the object, and a stream entry holds its buffer (`StreamUploads::VertexEntry::owner` /
  `IndexEntry::owner`); when a park cannot be scheduled, the entry is **kept** and retried next frame, and the address is never handed back to the allocator early.
- **The same address is not the same identity**: a fact row carries `ProgramFacts::generation`, and so does a half key - with A in force it should never differ, so it is
  insurance against "betting correctness on another module's discipline" (`G10`), not a second identity.
- The rule is written into 2.6 of `.ai/design/graphics-content-lifecycle.md`, and every table keyed by address has a matching test (see the evidence).

**B is not done**: no consumer today needs an id that is global across objects; A's three violations are all fixed and pinned by tests; replacing the whole key vocabulary with ids is
a 30+ file refactor that buys the insurance of "forgetting to hold is still correct", and "forgetting" is now caught by that same set of tests.
**C is not done**: it is exactly the fragmented version of B, turning "one identity concept" into four counters.

## Consequences

- Good: the key vocabulary is unchanged (a pointer is still the carrier of identity), and the plan / fact / stream layers have one lifetime rule; the instance stream no longer needs
  the "a plan borrows but does not hold" exception.
- Cost: any new table keyed by address must hold the object - that is a discipline, kept by design page 2.6 plus one test per table.
- Cost: `CompiledCommand::instance_revision` is removed (the revision is read from the held buffer, the same spelling as `ChannelFacts::streamKey()`),
  so a plan no longer snapshots a number that is cheap to read on demand.
- Reopening conditions (any one of them means doing B): a **third** hole that violates the convention appears; a table appears that **cannot reach the handle** (the instance stream here was
  almost exactly that - it reached it only by giving the plan a holder); or one piece of content has to be recognised across sessions or processes.

## Evidence

- `FrameCompilerTest.ThePlanHoldsTheInstanceStreamItReads`: the plan holds the instance stream, and the bytes stay readable after the host drops its own handle.
- `ContentPassTest` (the instanced-draw case): after recording, `instance_storage->useCount() > 1`, so the entry really does hold the buffer.
- `StreamUploadsTest.AnEntryHoldsTheBufferItsKeyNames`,
  `ContentStoreTest.AParkedRemovalKeepsTheObjectsAddressReservedUntilItsRowsLeave`,
  `ContentStoreTest.AReleaseThatCannotBeParkedYetKeepsItsEntryAndIsRetried`,
  `ContentHalvesTest.AnotherDescriptionOfOneIdentityIsAnotherHalf`.
- `PrimitiveDrawTest.AStripAndAFanCoverTheSameQuadAListDoes` (the former backlog T19, closed) runs green 10 times in a row now that a stream entry holds its buffer.
- The whole of `test_vsg`, and the whole of `test_gui` / `test_graphics`.
