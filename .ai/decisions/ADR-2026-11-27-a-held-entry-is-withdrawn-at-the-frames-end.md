# ADR-2026-11-27: a held cache entry is withdrawn at the frame end where its share is the last

- Status: accepted
- Date: 2026-11-27
- Related: `../design/vk-backend.md` 2.5 / N11 / D9 / D10 / T11,
  `../design/graphics-content-lifecycle.md` 2.3 / 2.5 / 2.6 / 2.9,
  `ADR-2026-11-09-address-keys-hold-the-object.md` (the rule this one finishes),
  `../research/graphics-2026-11-27-review-19.md` `Z12`,
  `src/plugins/gfx_backend_vk/{include/vine/backend_vk/internal/ContentCaches.hpp,src/internal/ContentCaches.cpp}`

## Background

The shipped backend keeps one table of instance streams, keyed by the instance `Buffer`'s address. Review 19 `Z12` found
it to be the one content entry that held **nothing it was keyed by**: the entry stored the uploaded `VkBuffer` and the
`Buffer`'s revision, no reference to the buffer, and it compared `revision()` against the caller's object - while every
fresh `Buffer` starts at revision 0.

Two facts followed.

1. **A recycled address inherited bytes.** A `Buffer<float>` allocated at an address a withdrawn entry was keyed by, at
   the same revision, was served the previous object's GPU bytes. The revision check passed, so nothing was reported -
   the only symptom was a wrong picture.
2. **Nothing ended an entry.** `instance_streams` lost one only on an explicit `releaseContent(Buffer)`, so every buffer
   a host stopped using left a `VkBuffer` and a pool slice behind for the session's life.

`ADR-2026-11-09-address-keys-hold-the-object` had already decided the identity half ("a cache keyed by address must hold
the object its key names"), and the geometry and program entries follow it; the instance stream was the hole. But holding
turns the cache into an owner of the host's bytes, so the second fact cannot be left alone: without an end, the fix would
trade a stale `VkBuffer` for a stale `VkBuffer` **plus** the host's memory - and the shape this table serves is a host
that rebuilds its instance buffer every frame.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Hold the buffer **and** withdraw the entry at the frame end where the cache's share is the last one | One identity rule and one end, both observable; the address cannot be recycled under a live entry, and the bytes stop outliving the host's use | The cache becomes a strong owner of host bytes until the sweep; one table walk per frame; a second withdrawal source beside the announcement |
| B. Hold the buffer, withdraw on the announcement only (the geometry path's shape) | Nothing new to state | Leaves fact 2 standing, and makes it larger: the entry now pins the host's bytes too, while the host pattern this table exists for has no announcement to make |
| C. A weak handle in `base/core`, so a dead key can be noticed without holding it | The exact answer, and an entry would never own the bytes | `RefCounted`'s counter lives IN the object, so a weak handle needs a control block beside it - a redesign of the primitive every holder uses, for one table - and it does not answer the multi-session case (two caches holding weak handles never see expiry) |
| D. Give `Buffer` a unique id and key by it | Dangling is impossible without holding | A public `base` API and a counter on every `Buffer`, and it still does not end the entry (`ADR-2026-11-09` option B, rejected there) |
| E. End unheld entries by a TTL, an LRU or a byte cap | No bookkeeping at all | The SDK states the opposite rule (`graphics-content-lifecycle.md` 2.3: no cap, no LRU, no pressure eviction), and "the host stopped holding it" is already observable exactly |
| F. Sweep when the table would grow, instead of at a frame end | No per-frame walk | Quadratic in a frame that names many buffers for the first time, and it leaves a table that stops growing holding its dead entries; the frame end is already where the cache settles what the host let go |

## Decision

Option A.

**The entry holds its key.** `InstanceStream::source` is an `intrusive_ptr<const vn::Buffer<float>>`, and
`ensureInstanceStream` takes the command's `BufferSlice` - so the share is GIVEN by the caller that owns it, not invented
from a borrowed reference. While the entry exists the allocator cannot hand that address to a later object, and
`revision` is a version of the one object the bytes came from.

**Two triggers, one body.** `withdrawInstanceStream` erases the entry, takes its source share down with it and parks the
detached handle behind the entry's own share (so a frame that borrowed the entry still finishes recording, and the park's
`use_count` probe counts frames rather than the act's own capture). `releaseStreams` (the host's announcement) and
`sweepUnheldStreams` both run it: the table's states are "held" and "absent", never a placeholder.

**The sweep reads holdings, once per frame.** `endFrame` walks the table and withdraws every entry whose
`source->useCount()` is 1 - "this entry and nobody else", which is exactly "the host let go". A frame that resolved the
entry holds the entry (`FrameBuilds::borrow`) and the draw command holds the buffer, so a stream a frame is about to
record cannot be swept; the premise that a count of 1 means "nobody else" is the ownership vocabulary itself (a raw
pointer that outlived its owner is already outside it).

**Where the rule does not reach, and who carries that.** A geometry entry is still withdrawn by the announcement only:
residency for scene content is the host's decision (`graphics-content-lifecycle.md` 2.3) and its rows are what leave the
tree, while a `Buffer` is content DATA with no row that could say it is gone. `N11` states that residual.

## Consequences

- The cache is a strong owner of host content for a bounded time: until the frame end at which nothing else holds it, and
  the parked `VkBuffer` follows the device's watermark (`§2.15`).
- A buffer that two sessions' caches both hold is never swept by either - each sees a count of 2 - so the announcement
  remains the way out for shared content. Written as a residual rather than hidden.
- A host that never ends a frame keeps its entries: there is no frame end to ask at, which is the same reason a host that
  never draws pays nothing.
- The table's invariant is now stated and simple - an entry IS the share of its key - which is what lets `releaseStreams`
  erase instead of replacing with a placeholder.
- `ContentCaches.hpp`/`.cpp` only: no SDK header, no public API and no ABI change. A host sees at most that
  `streams_live` drops where it previously stayed.

## Evidence

- `VkBuiltinProgramTest.AnInstanceStreamHoldsItsBufferAndEndsWhenNobodyElseDoes`: after a frame that drew with the buffer
  and with the command list gone, `useCount()` is 2 (this test's and the cache's); after the host lets go, a frame whose
  pass draws no instance stream leaves `streams_live` one lower with no announcement.
- `VkBuiltinProgramTest.ARecycledAddressDoesNotInheritThePreviousInstanceBytes`: the withdrawal gives the address back,
  a fresh same-shaped `Buffer` lands on it (revision 0 again), and the picture is the NEW buffer's matrix at +x with
  nothing at -x.
- **Mutation** (the old rule restored: the entry takes no share, `endFrame` does not sweep): the recycled-address case
  draws the PREVIOUS object's matrix at -x and fails on both pixels, and the first case fails on the live count - so both
  cases are load-bearing.
- `test_vk` 231/231, TSan 231/231 (0 warnings), ASan `RESULT: PASS`, `ctest` 31/31, the eleven gates and
  `vn_shader_check.sh` green.
