# ADR-2026-11-09: `Buffer`'s write entry points become one scope

- Status: Accepted
- Date: 2026-11-09
- Related: `../design/graphics-geometry.md` (F18 / F25), `../../src/base/core/sdk/vine/Buffer.hpp`, `../../docs/guides/graphics.md`

## Background

`core::Buffer<T>` is the only "shared mutable bytes" type in the engine: the model side reads it as `T`, the render side uploads it as bytes. The whole GPU-side cache chain
(`StreamKey`'s `revision`, the backend fact table's `described_revision`, `Scene`'s content epoch) **knows only its `revision()`**.
And `revision()` used to have only a manual path: `setRevision()` / `bumpRevision()`, which the writer **had to remember** to call after writing.

The long comment in `Buffer.hpp` argued that choice through, and concluded "**no** write announces itself, so the rule is absolute":

> It cannot see every write (a non-const `data()`, an element reference, a write from another thread), and it does not know where an edit begins or ends,
> so self-bumping can only be half true: one write path announces, the next stays quiet.

That argument is correct - **against "each write bumps itself"**. But it misses a third option: **one edit, one scope**. The fact the object lacks is exactly
"where the edit ends", and handing out pointers, references and spans only from a **scope that ends** supplies that fact.

The cost was countable: `setRevision(` at 18 sites and `bumpRevision(` at 13 are a separate second step; `docs/guides/graphics.md` used to teach
this two-step dance directly ("`buffer->setRevision(buffer->revision() + 1)` + `geometry->setRevision(...)`"); a missing announcement reports no error,
it just shows up as "the picture froze on the bytes of the last upload", the same family of bug fixed earlier in that section, only on the producing side.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Status quo: four silent write entry points plus a manual announcement | No change; writing code is easiest | Correctness depends on memory; a missed announcement is undetectable; three documents each explain the same thing |
| B. Bump automatically on every write (keep `data()`/`operator[]`/`view()`) | The simple way of writing does announce itself | Once a pointer is handed out it can never be seen again - **exactly the hole the original argument found**: half the writes announce and half do not, which is as unreasoning as A |
| C. **One scoped write entry point**: `auto edit = buffer->edit();`, announcing once at destruction | The rule is absolute and checkable (there is no silent path); one edit, one announcement - the granularity the consumers want; the documents no longer teach a two-step dance | A write site changes from `b->x()` to `edit.x()` inside the scope; a mechanical migration is needed |

## Decision

Chose **C**. `Buffer::Edit` is the only handle that can change elements (`data()` / `view()` / `operator[]` / `push_back` / `append` /
`clear` / `reserve`), and `Buffer` keeps only the **const** `data()` / `view()` / `operator[]` / `bytes()` / `size()` /
`revision()`. When the scope ends, `revision` advances once; a second live writer of the same buffer `assert`s (two "edit ended" signals would leave the consumer unable to hear either).
`setRevision()` / `bumpRevision()` are kept, their meaning narrowed to **writes made outside the object** (mapped files, device writes, producers that carry their own version numbers).

## Consequences

- Good: `G10` - correctness no longer rests on the caller's memory; `G11` - there is no path of "written but not announced", so this class of failure cannot recur;
  `G4` - `data()` no longer both says read and allows write. Not one line changed on the consumer side (they only read).
- Good: measurement showed **no production code needed changing** - only 15 write sites in tests had to enter a scope. That in turn shows production code was already written
  "by edit block" (build a `Buffer(std::vector)` and hand it to the geometry); what was missing was the API shape.
- Cost: a batched build has to wrap itself in `{}`; calling `reserve()` inside one scope also announces once (conservative - the reader reads once more, with no
  correctness impact).
- Cost: `Buffer` grows by 4 bytes (`editing_`), negligible for an object shared at vertex level.
- Reopening condition: a legitimate case appears where the bytes of a `Buffer` must be written without holding it (mapped memory, an external DLL writing directly) - such cases
  already go through `setRevision()`, which is the explicit path, so no reopening is needed.

## Evidence

- `BufferTest.AnEditAnnouncesItselfExactlyOnce` (one edit, one announcement, nothing announced inside the scope),
  `BufferTest.ASecondWriterCannotOpenWhileOneIsLive` (the second writer asserts),
  `BufferTest.SetRevisionStaysForBytesWrittenOutsideTheBuffer` (the escape hatch).
- Every migration site is in tests: `ChannelSliceTest`, `GraphicsTest` (instance stream / BVH invalidation / channel growth), `AdaptersTest`
  (`BufferSliceSource`'s "handed to somebody else and then changed" case), `ContentPassTest`, `StreamRevisionTest`.
- The whole of `test_core` / `test_graphics` / `test_vsg`.
