# ADR-2026-11-27: a description is built once per revision, behind one lock

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-threading.md` 2.4 / 2.16 / C22 / T41,
  `../research/graphics-2026-11-27-review-19.md` `Z7`,
  `src/viz/graphics/sdk/vine/graphics/DescriptionCache.hpp`

## Background

Three content types turn their current revision into an immutable value the recording side reads instead of the live
object: `Geometry::description()` (`GeometryDescription`), `MaterialValues::description()` (`MaterialDescription`) and
`ShaderProgram::description()` (`ShaderProgramDescription`). The rule is one - "the value for this revision, built once
and shared" - and it was written three times by hand.

Only the geometry copy was correct. `Geometry` held a `std::shared_mutex`, took it shared for the cached answer and
unique for the one builder, and its comment recorded why: the TSan gate found the unguarded cache on 2026-11-16.
`ShaderProgram` and `MaterialValues` were still the original shape - a raw compare, a raw write, and `mutable` members -
so two threads could tear the cache member, and two threads that missed the same revision could each build a value,
handing one `(id, revision)` two identities in the tables that key on the description's address.

The design says that shape must be safe. `graphics-sessions.md` states the requirement ("geometry / shader / texture
resources are shared ... sharing must stay thread-safe; two engines may frame at the same time"), and
`graphics-threading.md` 2.16 claimed it held, citing the geometry lock as the proof. Two engines publishing one scene
is the documented editor + offscreen-bake shape, and a publication calls `description()` per row; `releaseContent`
calls it too, from whichever host thread released the object. TSan had already named
`ShaderProgram::description()` once (2026-11-24) and the reading at the time was "a race in the test".

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Copy `Geometry`'s lock into the other two (`shared_mutex` + unique builder) | Smallest diff; each type keeps its own code | The rule stays written three times, which is what produced the defect: the next content type copies it a fourth time, and one of the copies will be the wrong one. Two of the three already proved that (`G1` / `G6`) |
| B. Make the revision `std::atomic` and leave the cache unguarded | One line per type | Fixes the key, not the cache: two buckets still write `description_` concurrently, and two readers still build two values for one revision - the identity the backend keys its tables on |
| C. One cache type, with `get` taking the revision and `build` taking the lock only on a miss | Keeps a shared fast path | The key is measured OUTSIDE the lock, and a material instance's `revision()` is not a pure read: it resolves its parent's values on first read, so two engines describing one instance race inside `resolved()` before the cache is ever consulted |
| D. One cache type, one plain `std::mutex`, measure + compare + build in one critical section | One spelling of the rule; correct for a resolving key; no reader can see a half-updated key | Parallel describes serialize for the duration of a compare (a builder still runs once, under the same lock); the type is a new header in the SDK |
| E. Lock-free: `std::atomic<const Description*>` read outside any lock | No lock on the fast path | Publishing a new raw pointer drops the last reference while another thread is about to increment it - that needs hazard pointers or RCU, i.e. a mechanism far larger than the defect it removes (`G8`) |

## Decision

Option D. `vn::graphics::detail::DescriptionCache<Description>` holds the cached `intrusive_ptr<const Description>` and
one `std::mutex`; `get(measure, build)` measures the source's revision, compares it with the cached value's own
`revision()` and builds on a miss - all inside one critical section - and `build` receives the measured revision, so
the value's revision and the cache key are the same number by construction. `peek(measure)` is the read-only face: it
answers the cached value only while it still belongs to the current revision, and never builds.

All three types use it; `Geometry`'s hand-written `shared_mutex` pair and `MaterialValues`' `described_` /
`described_revision_` trio are deleted rather than kept beside it.

The lock is the plain kind on purpose. Because the measurement belongs to the same critical section as the compare, a
read-only side cannot be offered - and it would not pay: on the development machine an uncontended `std::mutex`
lock/unlock costs **2.8 ns** against **13.2 ns** for a shared lock (measured 2026-11-27, 20 M rounds, clang-23 -O2),
so the change makes the geometry cache's steady path cheaper, not dearer.

The rule the cache does NOT take over is stated where it now lives: an EDIT is still the owning thread's act. The cache
makes readers safe; it does not let a host edit content while another thread describes it
(`graphics-threading.md` C22, and `R13`'s "an in-flight frame reads old values" is delivered by the value channel, not
by reading the object).

## Consequences

- Good: one promise, one implementation, one place to test - `DescriptionCacheTest` pins the three halves of the rule
  (one build per revision, a new revision is a new value, `peek` never builds) plus the three real users; TSan now has
  a witness for the claim 2.16 makes. `MaterialInstance`'s resolving `revision()` is measured inside the lock, so two
  engines describing one instance no longer race in the resolve either. `Geometry`'s steady path drops from a shared
  lock (13.2 ns) to a plain one (2.8 ns).
- Bad / cost: parallel describes of one object serialize for a compare plus, on a miss, one build. A describe is not on
  the recording path (`DrawListBake::refreshValues` walks rows at publication, not per frame draw), so the measured cap
  is `rows × 3 ns` per publication - 30 µs at 10 000 rows, against a 16.6 ms frame. The type is published in the SDK
  because it is a private member of three public classes; it sits in `detail` and is not part of the contract.
- Trigger (when to reopen): a host that describes one object from threads whose rate makes the compare meaningful
  (measured, not argued), or a content type whose revision cannot be measured under a lock because the measurement
  itself publishes - the second would be a new mechanism, not a second copy of this one.

## Evidence

- `src/viz/graphics/sdk/vine/graphics/DescriptionCache.hpp` - the type (the rule, the rejected shapes in prose).
- `src/viz/graphics/src/Geometry.cpp` / `ShaderProgram.cpp` / `Material.cpp` - the three `description()` bodies; `Material.cpp`'s
  `cachedDescription()` is `peek`.
- `tests/test_graphics/DescriptionCacheTest.cpp` - four cases; `ThreadsDescribingOneProgramOneMaterialAndOneGeometryShareOneValue`
  is the 2.16 promise written down, and the identity assertion fails on a cache that builds twice even without a sanitizer.
- `scripts/tsan_check.sh` → `test_graphics` 691/691, **0 ThreadSanitizer warnings**; before the change the same suite
  had named `ShaderProgram::description()` (2026-11-24, recorded in `../memory/graphics.md`).
- `VN_ASAN_TARGET=test_graphics VN_ASAN_FILTER='*' scripts/asan_check.sh` → `RESULT: PASS`.
- The lock measurements: `std::mutex` 2.8 ns, `std::shared_mutex` shared 13.2 ns per lock/unlock pair, same machine,
  same compiler, 2026-11-27.
