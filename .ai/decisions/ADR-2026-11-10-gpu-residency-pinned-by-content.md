# ADR-2026-11-10: GPU residency is pinned by content objects and reclaimed by "who is alive"

- Status: Accepted (steps one to five all landed; step five on 2026-11-12)
- Date: 2026-11-10
- Related: `../design/graphics-content-lifecycle.md`, `../design/vsg-backend.md` §2.10, `../backlog/graphics.md`
  `src/viz/graphics/src/Scene.cpp`, `src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp`,
  `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/Streams.hpp`, `src/viz/graphics/sdk/vine/graphics/Residency.hpp`

## Background

The same question - "when may this piece of GPU residency be released" - has five answers today:

| Residency | Criterion | Mechanism |
|---|---|---|
| Vertex/index/instance streams | Recently named + a window (`grace = slots + 1`) | `SharedStreams::releaseUnseen` |
| Texture images | Reference count `owner->useCount() <= 1` | `MaterialImages::releaseAbandoned` |
| Material block slots | Reference count + a row sweep | `ContentStore::releaseAbandoned` → `BlockStorage::releaseMaterial` |
| Compiled halves (pipelines) | Counterfactual query (does the table still answer program@rev ∧ does any geometry still ask for this layout) | `ContentHalves::answerable` |
| Descriptor sets | As above + a per-frame `ensure` | `ContentSets::sweep` |

Three failure shapes, all measured and all present: **culling drops uploads** (the first one's temporality: leaving the view for `slots + 1` frames gives up the upload, and coming back re-uploads);
**the `useCount` threshold fails silently when the holders change** (the second and third: any new holder turns 1 into 2);
**a per-pass O(#halves × #geometries) counterfactual scan** (the fourth and fifth, growing with the number of resource kinds).

## How other libraries do it (what to borrow, what not to)

- **wgpu-core**: resources carry two kinds of reference, **user refs** and **submission refs**, and destruction goes through triage (`ResourceTracker`) -
  it really destroys only when no in-flight submission refers to the resource. **Borrow**: two kinds of reference are two things (this repository's "reference count" and "retirement queue").
  **Do not borrow**: it hangs both kinds of reference off the resource; this repository's retirement queue is already a separate object, and a resource does not need to know the in-flight state.
- **bgfx**: handles plus a deferred-destruction queue, delayed by frames (`BGFX_CONFIG_MAX_FRAME_LATENCY`). **Borrow**: the delayed-destruction window is
  `RetirementQueue`; **do not borrow**: a delay window cannot replace the judgement "who is still using it".
- **Diligent Engine**: an explicit `ReleaseStaleResources()` plus a fence. **Borrow**: release is explicit and driven by the host (the same direction as
  `ADR-2026-10-22-cache-unbounded-explicit-release`).
- **Godot RenderingDevice**: RID + `free_rid(rid, dependencies)`, where the dependencies decide when the real release happens. **Borrow**:
  "naming the dependencies when releasing" is this ADR's pin.
- **QRhi (Qt)**: a `QRhiResource` base class + `create()` / `destroy()`. **Do not borrow**: it works because QRhi **itself**
  creates the Vulkan objects; this repository hands creation to vsg, so a base class would become a forwarding layer (`G7`).
- **Dawn (WebGPU)**: internal references inside objects plus command buffers' "internal use" references. **Borrow**: internal references kept apart from user references;
  **do not borrow**: it needs a change to `RefCounted`'s layout (this repository's `intrusive_ptr` is deliberately allocation-free).
- **VMA**: suballocation (material blocks and per-frame blocks are exactly its shape). **Borrow**: a block is not an independent resource, so accounting and release are kept apart.
- **VulkanSceneGraph**: intrusive references plus new command nodes recorded/compiled every frame, with `CompileTraversal` caching the implementation.
  **Fact**: an object can only be released by "dropping references", so "when to drop" has to be answered by this repository itself.

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A Keep the status quo | No change | Five criteria; culling drops uploads; counterfactual scans (`G1` / `G6` / `G10`) |
| B Enlarge the window (`grace = N`) | Less thrashing | Still a guess about time; "how long is long" is policy, not fact (`G8`) |
| C Two-tier eviction (release the residence but keep the key) | Gives memory back earlier | Coming back still re-uploads; one more state (`G1`) |
| D A `GpuResource` base class plus virtual release | One verb | Cannot cover streams / descriptor sets / pipelines (no content object to inherit from); needs a `RenderDevice&` parameter (a `G2` back edge); pushes GPU concepts onto content types (the same problem as `G13`) |
| E A lifetime state machine hung on the object | Intuitive | The state is a function of three clocks to begin with; in-flight facts would enter the object, which `RetirementQueue` already expresses exactly (`G1`) |
| F **pin = (content identity, revision), reclaimed by "who is alive"** | One rule, one verb, zero time windows, the counterfactual scans disappear | Video memory is no longer given up automatically (see "Cost") |

## Decision

Chose **F**:

1. Every residency entry records a **pin = (the content object's identity, revision)**; `revision` is the content object's own "this is the content now" announcement
   (`Geometry::revision` / `Material::revision` / `ProgramFacts::generation`), so "one version replaced another" and "one object replaced another"
   are expressed by the same key.
2. Reclaiming does not ask "did it die" but only "**who is alive now**": every frame (or on a content change) the fact tables enumerate the live pin set, and residency
   outside that set is retired together (`RetirementQueue` parks it, it does not free it).
3. All time windows and capacity caps are deleted: `Streams.hpp`'s `last_frame` / `grace` / `releaseUnseen` / `evictions` retire;
   `ContentHalves::answerable` and `ContentSets::sweep` are replaced by pin liveness (a half's pin = program@rev, see "Open items").
4. Explicit release stays a host verb (`releaseContent(Object)`, gaining `Buffer`), equivalent to "declare these pins dead right now".
5. Rebuilding is natural: an `acquire` miss calls the builder; the same key may have **several generations coexisting** (`generation`), and the old one disappears
   through the parking window.

**Why this is better than a base class**: a pin is a **value** that does not touch the object model, so content types carry no GPU concepts (`G13`),
no `RenderDevice&` parameter is needed (`G2`), and it covers the three kinds that have **no content object to inherit from** (streams, descriptor sets, pipelines).

## Cost

- **Video memory**: residency = everything live content needs. A 600 MB model removed from the scene but still held by the host stays resident until `releaseContent()`.
  That is the inevitable result of explicit management (`ADR-2026-10-22`) and the only real cost of this ADR, and it must be written into
  `graphics-content-lifecycle.md`'s standing points.
- **Half accumulation**: with a pin that only looks at program@rev, layout/variant turnover makes halves accumulate (each a pipeline of a few tens of KB).
  That has to be measured once under `G10` and then decided (see open item 1). **It was already removed by the layout declaration index when step five landed** (2026-11-12):
  `declaresLayout` is an O(1) "is there still a live geometry row feeding this layout", so there is no choice between accumulating and scanning.

## Migration

Each step is independently verifiable and revertible:

1. **Vocabulary + ledger** (landed): `vn::graphics::ResidencyLedger` / `ResidencyRow` / `ResidencyKind` (the `graphics/` half,
   Vulkan-free), exposed as `RetentionStats::residency`; `gfx_backend_vsg` hooks up the three tables for streams / images / material blocks.
   No behaviour change.
2. **Streams** move into the pin model: the `StreamKey` table generalises into `backend/ResourceTable<K, V>`, the pin = what a live geometry row declares
   (the buffer address, and that buffer's revision at this moment), and the window is deleted. Acceptance: culled for 100 frames with zero re-uploads; leaves within
   `slots + 1` frames of leaving the tree.
3. **Textures** move in: delete `MaterialImages::releaseAbandoned`, pin = texture@rev.
4. **Material blocks** move in: the builder writes the arena and the destroyer frees the slot; delete `ContentStore`'s three release loops → one `retireOwner`.
   (Implementation: the pin = the material's **identity**, and the other half is answered by the material table's own per-identity row count (`declaresMaterial`) -
   that "identity → slot" table in the arena already is this table, and stacking a `ResourceTable` on top would record slot+revision twice,
   so freeing the slot is written into `MaterialArena::retireUnpinned`; and it does **not park**: the arena's copy rotation is the window, and putting it on the parking queue would
   give the slot back inside the window, after an entry was rebuilt.)
5. **Halves + descriptor sets** move in (landed 2026-11-12): delete `answerable` and `ContentSets`' sweep, and both tables go through
   `ResourceTable::retireUnpinned`. A half's pin = program@rev **+ `ContentDeclarations::declaresLayout`** (the "layout → live row count" the geometry table
   maintains, O(1)); a descriptor set adds 5 texture pins (`declaresTexture` plus the key's own revision).
   What is deleted is the two counterfactual scans, O(halves × geometries) / O(sets × geometries) (a hand-built `ContentFacts` without the published index falls back to scanning
   under the optional-index rule; the production `ContentStore` always publishes); a refused build does not become resident (there is no GPU object), it only remembers the key,
   forgotten along with the program revision. Halves no longer accumulate per layout: when the last geometry row using that layout leaves the tree, the declaration disappears and the half parks
   with it.

### Design of migration 2: streams → pins (`backend/ResourceTable<K, V>`)

#### Why a table first, rather than changing `StreamUploads` in place

Streams, textures, material blocks, halves and descriptor sets want **the same rule** (a value built on demand, pinned by live content, parked when nothing pins it),
with only the key and the value differing (`G1`, `G6`). Written as one table, `StreamUploads` keeps only "what counts as a stream, and which objects one upload has to build";
the four later steps no longer rewrite "table + parking + generation" each, nor grow a lifetime criterion each (`G5`: the change stays inside the module).

#### Interface (`src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/ResourceTable.hpp`, Vulkan-free) <!-- drift-ok -->

```cpp
template <class Key, class Value, class Hash = std::hash<Key>>
class ResourceTable {
  public:
    struct Acquired {
        Value*        value{ nullptr }; // the value hit or just built; readable while it stays in the table (unordered_map references are stable)
        std::uint64_t generation{ 0 };  // this "value"'s generation (see below)
        bool          built{ false };   // true = the value was built by this call
    };

    template <class Build>              // Build() -> std::optional<Value>; empty = refused, the table is unchanged
    [[nodiscard]] Acquired acquire(const Key& key, Build&& build);

    template <class Live>               // Live(const Key&) -> bool
    std::size_t retireUnpinned(Live&& is_live, FrameTimeline&, RetirementQueue&);

    [[nodiscard]] std::size_t   live() const noexcept;
    [[nodiscard]] std::uint64_t builds() const noexcept;
    [[nodiscard]] std::size_t   retired() const noexcept;  // cumulative departures (this generation is parked)
    [[nodiscard]] std::size_t   retained() const noexcept; // times it could not park and was kept
    void clear();
};
```

- **No capacity, no timestamp, no `last_frame`**: the lifetime is decided by `is_live` alone. An `acquire` hit does not touch the builder
  (zero builds per frame in steady state); a missed builder runs outside the table and the value is moved in only on success, while a refusal (derived channels, non-canonical locations, …)
  leaves the table unchanged.
- **`retireUnpinned`**: for every entry with `is_live(key) == false`, the value is moved into the parking lambda, removed from the table and `retired++`;
  if `RetirementQueue::retire` refuses (the in-flight slot count is not known yet) the entry is **kept** and `retained` is incremented, retried next frame. It never deletes early
  (the same discipline as `ContentStore`'s `retained`).
- **`Value` must be copyable**: the `ReleaseFn` used for parking is a `std::function`, which requires a copyable callable; `vsg::ref_ptr`
  and `intrusive_ptr` satisfy that, and a "value" is exactly their composition.

#### `generation`: several generations per key

`generation` is the in-table number of **a built value** (monotonic, never recycled), so both the caller of `acquire` and the table's own log can identify a value.
The same key can be rebuilt while the old value is **still inside the parking window**: the old generation parks in `RetirementQueue` with the value it captured, and the new generation starts
serving lookups from the next number. So "several generations of one key" is not two entries in the table but **one entry in the table plus N in the queue**; the number lets any slow
accounting (tests, logs, an old handle a lower layer may hold) recognise which generation it holds, instead of mistaking a new value for an old one - the same idea as
`ProgramFacts::generation`, one layer lower (that field was deleted with step 2 of ADR-2026-11-11: the program table is keyed by handle and
no longer needs the insurance of "another generation at the same address").

In step 2 a stream's key already contains the buffer revision, so the normal path never rebuilds a key; this exists for step 4 (material blocks keyed on identity, with the revision counted separately)
and step 5 (halves on program@rev + the layout declaration) - the same drawer (`G9`), pinned today by `ResourceTableTest`'s counting `Value`.

#### Where live pins are enumerated: the content layer publishes, the entry answers itself

**Cost first**: the residency account may only walk O(#residency entries) per frame, never O(live content). The pin model keeps culled content resident, so a "culled
geometry row" stays in the table forever - and scanning those rows every frame would make a frame that draws nothing the most expensive one. That was exactly the first version's defect
(estimated from the code structure: 100k geometries × 4 declarations ≈ several hundred thousand hashes/dereferences per frame, unrelated to what is on screen). So v2 splits the pin into two
halves, each answered by one O(1) fact:

- **The address half is published by the content layer**: the geometry table wrapper in `ContentStore` (`GeometryTable`) maintains an `address → declaration count` index
  **inside the same method** that does a row's `append` / `eraseIf` / `clear` (a shared arena declared by several channels / rows is reference counted, and the address leaves
  only when the last declaration goes), and `ContentFacts::declarations` (`ContentDeclarations`) only borrows it. Culling deletes no row ⇒ the count does not change ⇒
  one `declaresBuffer()` answers it.
- **The revision half is answered by the entry itself**: the entry holds the buffer handle it uploaded with, and `key.revision == owner->revision()`
  is one comparison. A buffer `edit()` changing generation touches no row, and the old entry knows by itself that it should go.
- So `StreamUploads::beginFrame(const ContentFacts&, timeline, retirement)` only walks its own two tables every frame;
  the extra cost on a frame where content changes is the row change itself (already O(delta)) plus one index borrow, not a rescan every frame.
- `PrimitiveFacts::instances` stays: the instance buffer is the bytes a primitive declares, and the index counts it when the row is built - so a culled instanced
  draw does not lose its instance upload either.
- **owner is required**: a "borrowed identity" with no handle has nothing that could keep the address alive (the old implementation fell back to a snapshot, which left a silent wrong picture for a
  new buffer landing at the same address). Now `acquire*` answers `Refused` outright; the production record path always carries a handle anyway, and the test fixtures build a real
  `vn::Buffer` (`tests/test_vsg/TestStreamUploads.hpp`), the same shape as production.

#### The builder / destroyer shape of `StreamUploads`

- **builder**: the miss branches of `acquireVertex` / `acquireIndex` move as they are into `ResourceTable::acquire`'s builder lambda
  (a zero-copy alias or a snapshot + one `Bind*Buffers`); the return value changes from "Action + ref_ptr" to "Action + the Value* the table gives",
  and the caller copies the `ref_ptr` once. The entry still holds the buffer handle (the address is not recycled while the key is queryable,
  see 2.6 of `graphics-content-lifecycle.md`).
- **destroyer**: there is no separate function. `retireUnpinned` moves the whole `Value` (bind + owner handle) into the parking lambda, and when the park
  expires the lambda's destruction releases the bind and the handle. So "the entry departs" (now) and "the object is destroyed" (when the window expires) stay apart,
  the same measure as `ContentStore`'s parking.
- **Deleted**: `beginFrame(frame, grace)`, `releaseUnseen()`, `evictions()`, `unused()` and the capacity constructor parameter;
  the whole `core::SharedStreams` class (`backend/Streams.hpp` keeps only `StreamKey` / `StreamKeyHash`).
  The names freed up, under the new reading:
  - `beginFrame(facts, timeline, retirement)` returns this frame's departure count (reading `facts.declarations`);
  - `retired()` replaces `unused() + evictions()` feeding `ResidencyLedger::Streams.retired`;
  - `uploads()` / `aliases()` / `refusals()` are unchanged (`aliases` is counted by the adapter layer, because the table only counts builds).

#### How acceptance lands as cases

| Criterion | Case |
|---|---|
| Culled for 100 frames, `uploads()` does not rise | `ContentStoreTest.ACulledGeometryKeepsItsUploadsForAHundredFrames` (the same facts for 100 frames, zero builds; the entry count unchanged) |
| `live()` falls within `slots + 1` frames of leaving the tree | `ContentStoreTest.AStreamLeavesWithinTheParkingWindowAfterItsGeometryIsDropped` (`releaseAbandoned` parks → the row is deleted → the declaration disappears → the entry departs) |
| Two geometries of one buffer share one upload, and one leaving does not disturb it | `StreamUploadsTest.TwoGeometriesOfOneBufferShareOneUploadUntilBothLeave` (declaration counts merge) |
| A revision advance retires the old entry | `ContentStoreTest.AStreamWhoseBufferMovedLeavesWithoutTheGeometryChanging` + `StreamUploadsTest.ABufferRevisionThatMovedRetiresTheEntryTheContentStillDeclares` (the entry reads its own handle) |
| The declaration index follows the rows, the count for a shared buffer, and the parking window | `ContentStoreTest.TheDeclarationsFollowTheRowsAndOutliveThemForTheParkingWindow` |
| The table's own contract (same-position hit / a refusal leaves the table unchanged / parking / retention / generation) | `ResourceTableTest` (a counting `Value`: the old value is still alive inside the parking window and the new one starts a new generation) |

After every step, run: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_doc_symbols.py`,
`check_include_hygiene.py`, `check_export_annotations.py`, `check_public_docs.py`,
`check_vsg_content_seam.cmake`, and `test_vsg` (including the edit-cost recipes of `StreamUploadsTest` / `BlockStorageTest` / `ContentPassTest` /
`VsgBackendTest`).

## Open items

1. Whether a half's pin should look only at program@rev (which accumulates) or also add "the layout is still needed by live geometry" (back to a scan). **Closed
   (2026-11-12)**: the pin that landed in step five is `program@rev ∧ declaresLayout`, and the layout half is answered by the geometry table's
   `layout → live row count` index, which is O(1), so there is neither a scan nor accumulation.
2. Whether to add weak support to `RefCounted`: **not doing it**. The pin set is enumerated by the fact tables, and no "the object died" notification is needed.
3. `Buffer` is still not in the fact tables, but it does not need to "become an identity": a stream's pin is read from **the geometry row's buffer handle**
   (the address plus that moment's `revision()`), so the geometry declaring bytes and the buffer announcing a new generation take effect separately; an entry for a shared buffer
   is naturally pinned by several geometry rows together.

## Consequences

- `G1` / `G4` / `G6`: one definition, one verb and one criterion for residency lifetime.
- `G7`: the host's interface is unchanged (`releaseContent` plus the existing reports), and the complexity is drawn into the backend.
- `G10`: the per-pass counterfactual scans are deleted; the cost is video memory, written down and measured.
- `G13`: content types gain no GPU concepts; the backend derives the pin from identity.
- Superseded: `Streams.hpp`'s reader counts and time windows all retire (the whole `SharedStreams` class is deleted); `StreamKey` as an identity is unchanged.
- **Correction (2026-11-18, audit-4 B13)**: the class this ADR calls `ResourceTable` is `PinnedResources` today
  (`vn::backend_vsg::core`), renamed so the two residency tables stop sharing a near-name (`ResidenceTable`
  became `ResidencyClaims`). No rule in this ADR changed.
- **Correction (2026-11-18, the T30 re-argument)**: step one's vocabulary is a VALUE on the public surface and stays one;
  the resource TYPES were never public and are settled as never public - a host never names a GPU object, so the missing
  precondition was a host's need, not a backend count (`graphics-architecture.md` 2.15 states the admission test, and
  option D above is the same conclusion from the other side). What a census of the kinds shows: `Streams` / `Images` are
  filled by both backends, `Halves` by vk only, `MaterialBlocks` by vsg only, and `Sets` by nobody (one reference in the
  tree, its own name table). The taxonomy's neutrality is `../backlog/graphics.md` T43; the placement of the two tables
  above is T30, and each still has one consumer, which is `ADR-2026-11-16`'s own test for staying plugin-private.
