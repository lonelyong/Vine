# ADR-2026-11-26: byte content is caller-managed, and `Buffer` holds no lock

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/graphics-threading.md` (§2.4 byte content, F7), `../design/graphics-geometry.md` (F18, N1, N13),
  `Buffer<T>` / `Buffer<T>::Edit` / `BufferSlice<T>` (`src/base/core/sdk/vine/Buffer.hpp`),
  `ContentCaches::ensureGeometry` / `fillStreams` / `detachStaleStreams`
  (`src/plugins/gfx_backend_vk/src/internal/ContentCaches.cpp`), `GeometryDescription`'s note in
  `src/viz/graphics/sdk/vine/graphics/Geometry.hpp`

## Background

A frame uploads a geometry's vertex and index streams on the **engine thread**, reading the bytes a `Buffer` owns. A host
may edit those bytes on its own thread: `graphics-threading.md` 2.4 calls that channel "byte content", announces it with
`Buffer::revision()` (an atomic a reader acquires), and lists "a loading worker, a streaming write" as legitimate
writers. The application's own shape makes the overlap real in principle - the GUI thread edits frame N+1 while the engine
thread answers frame N - so "who guarantees the storage does not move between the read of `revision()` and the copy"
became a question.

The first answer implemented was an internal `std::shared_mutex` in `Buffer`: `Edit` held it exclusively, a scoped read
lease (`BufferRead<T>`) held it shared for the duration of the upload's copy, and the renderer read through that lease.
It removed the hazard, but it charged every producer of every `Buffer` - including the modelling path that appends one
vertex at a time (`Edit`'s own note) - for a policy only a streaming host needs, and it put a threading policy inside a
value type. The owner rejected it on both counts, and the repository's original contract agreed: `Edit`'s warning already
said "not thread-safe: another thread writing at the same time is a data race, with or without this scope".

## Evidence

| Fact | Where |
|---|---|
| `Buffer` exists so that ONE allocation serves the model (typed view) and the renderer (bytes); the renderer copies those bytes into GPU storage anyway | `Buffer.hpp` class note, `ContentCaches::upload` |
| The original write contract is "the writer owns the timing": one scoped writer, an atomic revision as the publish, no internal lock | `Buffer<T>::Edit` (`@warning`), `revision()`'s note |
| A byte-only edit must NOT move the geometry's shape revision; the stream is re-uploaded when its buffer announces | `Geometry.hpp` `revision()` / `setRevision()`, `GeometryDescription`'s note, `graphics-geometry.md` F18/N1 |
| The upload runs on the engine thread at the frame head, and `ContentCaches` already owns the lifetime of everything it builds (entry replacement + parking) | `ContentCaches::fillStreams`, `detachStaleStreams`, `releaseStreams` |
| Peer engines do not lock the CPU-side vertex array: Diligent documents "do not update a resource a pending frame uses"; UE's `Lock`/`Unlock` is on the RHI resource and its contract is "the RHI does not touch this block until Unlock"; three.js is single-threaded; Dawn copies in `write_buffer` because the caller's storage may die at once | peer documentation |

No measurement was taken, and none is needed for the direction: the lock's cost is paid per edit scope by every
producer, while the benefit is a promise only a host that grows a buffer under a live frame would use - and `G8` says an
extension point without a consumer today does not get built.

## Decision

`Buffer<T>` stays a single-writer value type with **no lock**. The cross-thread rule is the caller's, stated once in
`Edit`'s warning and here:

- **Rewriting the same elements and announcing them** (`Edit` scope, or `Buffer::setRevision`) is the ordinary streaming
  path. The revision pair orders the bytes (`release` on the writer, `acquire` on the reader), and the renderer re-uploads
  every stream whose buffer revision moved.
- **Growing or replacing the storage** moves the elements, so it must not happen while a frame that reads that buffer is
  in flight: grow between frames, or hand over a **new buffer object** (a new identity, which is a new cache key).
- The renderer makes no copy of its own and takes no lock: `fillStreams` reads the same spans a single-threaded consumer
  would, and records the buffer revision it read *before* the bytes so a writer landing in between costs one extra upload
  rather than stale bytes labelled current.

## Rejected options

| Option | Why not |
|---|---|
| Internal `std::shared_mutex` + scoped read lease | A threading policy inside a value type (`G1`/`G5`), paid by every producer (`G10`: one unique lock per append scope), for a case no in-tree host creates today (`G8`). Also makes the type non-copyable in spirit and its size grow for every user of `core`. |
| Copy the bytes on read (`Buffer::copyElements`) | The duplication `Buffer` exists to avoid, on every stream upload (`Buffer`'s own class note). |
| Copy-on-write inside `Edit` (swap an immutable `shared_ptr<const vector>`) | O(n) per edit scope: worse than the lock for the append-heavy modelling path. |
| Let the renderer read only on the content thread | Uploads are engine-thread work by design (`graphics-threading.md` 2.12); the seam would have to move back. |
| Have the renderer wait for "no editor" at the frame head | The frame head is not the place a host's edit can be awaited without making frame timing depend on host timing (`G3`). |

## Consequences

- A host that streams growth into one buffer across frames is outside the contract; the sanctioned shapes are "rewrite in
  place + announce" and "publish a new buffer object". The rule lives in `Edit`'s warning, so it is read where the edit is
  written.
- No test can prove the absence of a race in the application's timing; the rule is a contract, not a mechanism, and the
  TSan gate still covers the mechanism that *does* exist (the revision pair, the entry replacement, the parking).
- What the investigation did fix stays: a stream is re-uploaded when **its buffer** announces (the contract
  `Geometry.hpp` and `Buffer.hpp` always stated), and the draw count is derived from the uploaded bytes rather than a
  live channel length.
