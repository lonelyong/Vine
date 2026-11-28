# Graphics derived-state census (2026-11-28): every cached answer, and the three questions that decide whether it can go stale

> **Status**: census + two landings. It opened no new theme; it walked every value the graphics module keeps BETWEEN
> calls and asked the same three questions of each. One row failed (`Z32`, the light set) and was **fixed the same day**
> with one invalidation body, two cases and three mutation arms (§4); the four shapes the census derived are now the
> normative rule `graphics-architecture.md` 2.20, and the one test-only stand-in it found (`S26`) was retired the same
> day with its acceptance rows repointed at the shipped mechanism's cases.
>
> **Scope**: the cross-call derived state of `src/viz/graphics/src`, its SDK, and the vk backend's content-side tables.
> Excluded by construction, and why: per-call scratch (a plan's maps, a bake's local maps) cannot outlive the call; the
> per-frame half of `FrameResources` and `FrameScratch` cannot outlive the frame or the driver; registries allocate
> identity rather than derive a value (`PassRegistry`, `RenderBackendRegistry`, `OutputRegistry`); gates, schedules and
> latches answer "may I" / "when" / "have I said this" rather than "what" (`FrameGate`, `RetirementQueue`,
> `ReportOnce`); and authored helpers that write a mirror every frame (`CameraMirror`, `FpsOverlay`) hold no
> invalidation key at all.
>
> **Method**: every row was read at the line it cites. The three questions are §1. A row is `ok` only when all three
> have an answer that is a mechanism, not a convention - a convention is what `ADR-2026-11-09` refused to accept.
>
> **In one line**: the module's derived state falls into four shapes - a per-object value keyed by its source's own
> revision measured under one lock; a content-side table keyed by identity plus a clock; a key of
> `(handle, revision)` across the thread seam; and an address key that holds what it names - and every row belongs to
> one of them except one: `Scene::lightSet`, whose key was a `(Light* address, revision)` pair that neither held the
> address nor noticed the list changing (fixed §4).

## 1. The three questions, and what a failing answer looks like

1. **What is the key, and is it read from the same place the value is built?** A key measured in one step and a value
   built in another two can disagree with each other (this is why `DescriptionCache` takes a callable and measures
   inside its lock, `DescriptionCache.hpp:67-77`).
2. **Who ends the entry, and can that ever fail to run?** A release verb only the host can call is a duty; a sweep, an
   epoch compare or an identity compare is a mechanism. The distinction is the one review 21 settled for the backend
   (`Z25`) and review 22 settled for the root's epoch (`Z30`).
3. **Can the key name a dead thing?** Three acceptable answers exist - the entry holds what it names (a share), the key
   is a handle whose generation retires (ABA-safe), or the key is only compared while its owner provably holds the
   named object. A bare address with none of the three is the failure this census exists to find: a new object of the
   same size lands on the freed address and is served the old answer (`ADR-2026-11-09`, whose three measured cases are
   exactly this).

A row that cannot answer (2) or (3) is a gap even when every test is green: the defect is silent - the picture is wrong
and every counter reports health.

## 2. The census

### 2.1 Values keyed by their own source's revision (per-object caches)

| # | Held value | Key, measured where the value is built | Who ends it | Pinned by | Verdict |
|---|---|---|---|---|---|
| S1 | `DescriptionCache<GeometryDescription>` (`DescriptionCache.hpp:67-97`; used `Geometry.cpp:424-441`) | the geometry's own `revision()`, read inside the cache's lock; one value per revision | replaced when the measure moves; `peek` serves readers that must not build | `DescriptionCacheTest.cpp:170` (two threads, one value) | ok |
| S2 | `DescriptionCache<MaterialDescription>` (`Material.cpp:39-54`) | same rule, source = `MaterialValues::revision()` | same | `DescriptionCacheTest.cpp:84-170` | ok |
| S3 | `DescriptionCache<ShaderProgramDescription>` (`ShaderProgram.cpp:45-56`) | same rule, source = `ShaderProgram::revision()` | same; `setName` deliberately does not move it, so a cached description keeps the old name (`ShaderProgram.hpp:56`) | `DescriptionCacheTest.cpp:170` | ok (trap stated in the header) |
| S4 | `Geometry::local_bounds_` (`Geometry.hpp:1055-1063`, `Geometry.cpp:513-523`) | positions buffer address + buffer revision + primitive slice + geometry revision. **The geometry holds the buffer** (`AttributeChannel::values`), so the address in the key cannot be recycled | recomputed on key mismatch | `GraphicsTest.cpp:11596` | ok |
| S5 | `Geometry::triangle_trees_` (`Geometry.cpp:596-632`) | positions/indices addresses + their revisions + slice + topology + geometry revision; same holding property | key mismatch, `clearBvh`, or a non-triangle stream | `GraphicsTest.cpp:1889`, `1925` | ok |
| S6 | `MaterialInstance::resolved_` + composite revision (`MaterialInstance.cpp:124-144`) | parent revision + own revision; a recompute bumps the cache's own generation, which is what `revision()` reports | recomputed when either number moves | `MaterialInstanceTest.cpp:55`, `77`, `157` | ok - with the trap that the composite number is per INSTANCE: it is never compared across objects, and the outgoing ref always pairs it with a `MaterialId` (`DrawListBake.hpp:617-620`) |
| S7 | `Node::cached_bounds_` (`Node.cpp:44-72`) | the node's own + its ancestors' bounds stamp | stamp mismatch | `GraphicsTest.cpp:11493`, `11519` | ok |
| S8 | `Node::cached_subtree_layers_` (`Node.cpp:86-104`) | process-wide `g_layer_epoch` + inherited mask | epoch or mask mismatch; a stale union is only recomputed, never used for a prune decision | `GraphicsTest.cpp:431`, `458`, `493` | ok |
| S9 | `detail::g_layer_epoch` (`LayerEpoch.hpp:22-38`) | a monotonic, never-reset process-wide clock; only ever compared for equality | n/a (a clock, not a value) | the three layer cases above | ok by construction |
| S10 | `Texture::revision_` (`Texture.hpp:159-208`) | an atomic monotonic counter owned by the texture; the backend keys on address + revision and its entry holds the texture (§2.4 S22) | n/a | `VkResidenceWireTest.cpp:482` | ok |

### 2.2 The content side: scene → baked rows → publication

| # | Held value | Key | Who ends it | Pinned by | Verdict |
|---|---|---|---|---|---|
| S11 | `SceneBake` rows + the raw `nodes` record (`DrawListBake.hpp:281`; built by `ensureRows`, `ScenePublisher.cpp:222-275`) | the table's ROOT IDENTITY (`baked_root_`) first, then the root's `contentEpoch`; per-row refresh compares `node_epochs` | rebuild on identity or epoch mismatch; `resetToEmpty` when the scene is hidden or rootless | `SceneTest.AReplacedRootIsNeverServedFromThePreviousTablesEpoch`, `AClearedSceneDoesNotInheritItsOldTablesAnswer` (review 22 §3.5) | ok - the raw pointers are content-side only and never cross the thread seam (the snapshot copies values) |
| S12 | `ScenePublisher::content_memo_` (`DrawListMemo`) | view (view_proj, eye, viewport, layer mask) + the PUBLICATION's revision (`ScenePublisher.cpp:98-127`); an entry nothing asked for in the previous frame is dropped | `beginFrame` ageing | `SelectTest.cpp:431-432` | ok |
| S13 | `ScenePublisher::publication_cache_` + `published_version_` (`ScenePublisher.hpp:212`, `238`; `ScenePublisher.cpp:198-206`, `272-273`) | one `RowsVersion{table, epoch, boxes, values}` (`SceneSnapshot.hpp:33-37`); `table` moves on every rebuild and reset | identity change, epoch move, box or value refresh | the four review-22 cases + four mutations (review 22 §2.5 / §3.5 / §5) | ok |
| S14 | `SceneSnapshot`'s shared 32-row chunks (`SceneSnapshot.cpp:44-72`) | version (table/boxes/values) + row count + chunk count, then per row: epoch, the fold group, the resolved serial | rebuilt, never shared, when any of the three row facts moved | `RenderWorldTest.cpp:327-341` | ok |
| S15 | `Scene::light_set_` / `light_key_` (`Scene.hpp:537-538`; `Scene.cpp:278-295`) | per slot: `(Light* address, Light::revision())`, compared ONLY while the list is unchanged - every list mutator drops the cache through `invalidateLightSet` (`Scene.cpp:219-227`, called at `230-237`, `239-249`, `251-255`) | the three mutators, so `setLighting` (`Scene.cpp:262-266`) inherits the rule | `SceneTest.AReplacedLightListDoesNotReuseTheOldAnswer`, `SceneTest.ReplacingTheLightingBuildsTheNewSet` | **fixed 2026-11-28** - was `Z32` (§4) |

### 2.3 The thread seam: content thread → engine thread

| # | Held value | Key | Who ends it | Pinned by | Verdict |
|---|---|---|---|---|---|
| S16 | `PublicationStore::publications_` / `bindings_` (`PublicationStore.hpp:167-176`) | `(SceneId, publication revision)` for a source; `SceneId` alone for an override | `beginFrame` erases a source no frame named for one frame (`PublicationStore.cpp:73`) and an override whose scene answers gone (`PublicationStore.cpp:85`) | `PublicationStoreTest.cpp:97-133`, `167-220` | ok |
| S17 | `SnapshotSource`'s snapshot + memo (`SnapshotSource.hpp:40-90`) | the publication revision; the value is immutable, the memo engine-thread-only | the store's eviction above | `SelectTest`; `PublicationStoreTest` | ok |
| S18 | `SceneRows::snapshot` (`SceneRows.hpp:26-48`) | a bridge onto S11-S13, no state of its own | n/a | the review-22 cases | ok |
| S19 | `ContentRegistry` slot generations (`ContentId.hpp:28`, `73`; `ContentRegistry.cpp:38-72`) | a 24-bit generation inside the handle; retired to 0 at `kGenerationMax` (ABA) | release moves the generation | `ContentRegistryTest.cpp:22`, `45`, `59` | ok |
| S20 | `ContentRef {id, revision}` (`ContentId.hpp:131-175`) | the stable key every table from S21 down uses | the table's own rule | `ContentRegistryTest.cpp:99` | ok |

### 2.4 The backend's content tables (the half review 21 settled)

| # | Held value | Key | Who ends it | Pinned by | Verdict |
|---|---|---|---|---|---|
| S21 | `ContentCaches` programs / geometries / buffers / pipelines (`ContentCaches.cpp:192`, `755-837`) | the description address (the entry HOLDS the description) or `(description, variant)` for pipelines | `releaseContent`, or the frame-end sweep when the key's share is the last (`ContentCaches.cpp:526`, `625`, `2498-2501`) | `VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced`, `ContentTheHostNoLongerHoldsLeavesResidencyBehind`; three mutations | ok (review 21 §2.3) |
| S22 | `ContentImages::textures` (`ContentImages.cpp:149`) | the `Texture*`, with the entry holding an `intrusive_ptr` of it | release, or `sweepUnheldTextures` (`ContentImages.cpp:693`) | same cases | ok |
| S23 | `ContentImages::samplers` (`ContentImages.cpp:192`, `883-920`) | the `Sampler` VALUE | nothing - no cap, no sweep | none | **recorded `Z26`** (review 21 §4), trigger-gated: a host that animates sampler state |
| S24 | `TargetTable::targets_` (`TargetTable.hpp:25-54`; `Device.cpp:3864`) | the host `RenderTarget` address; the entry does NOT hold the target | the host's `releaseRenderTarget`, whose contract is "no frame still names it" (`RenderDevice.hpp:117`) | none for the unannounced-drop path | **host duty** (stated): an unannounced drop leaves an entry keyed by a recyclable address; the engine's own path releases through `FrameResources::releaseAll` (`graphics-frame-pipeline.md` §4.2) |
| S25 | `ResidencyClaims` (`ResidencyClaims.hpp:14-46`) | the description address, with an `Absent / Uploading / Resident` state machine and a monotonic token per claim | `publish` / `withdraw`; a stale token cannot resurrect an entry | the multi-driver cases | ok |
| S26 | `detail::ResidencyTable` (deleted 2026-11-28) | it was keyed by `(id, revision)` and aged by snapshot epoch | `reclaim(oldest_live)` - a rule the shipped backend no longer uses | its three cases (deleted with it) | **retired 2026-11-28**: no production consumer AND not the shipped mechanism (the backend keys by identity and sweeps by who holds the key, `graphics-content-lifecycle.md` 2.10). The acceptance rows that cited it (`graphics-frame-pipeline.md` T6 / `graphics-content-lifecycle.md` T1 / T10 / `graphics-geometry.md` T2) now name the `VkResidencyTest` / `VkResidenceWireTest` cases that pin the shipped rule |

### 2.5 Frame-scoped and driver-scoped state (cannot outlive its key)

| # | Held value | Why it cannot go stale | Verdict |
|---|---|---|---|
| S27 | `FrameResources`' frame half + the standing bindings (`FrameResources.hpp:25-60`) | the frame half is cleared per frame; the host's bindings are COPIED at the frame head, so a publish mid-compile answers next frame | ok |
| S28 | `SurfaceLedger`'s announced vs applied size (`SurfaceLedger.hpp:21-39`) | the announced size is taken at the frame head; an announcement during a frame belongs to the next one | ok |
| S29 | `FramePump`'s last answer (`FramePump.hpp:349-366`) | one slot, overwritten with its frame number; readers compare numbers under one lock | ok |
| S30 | `FrameScratch` | one per frame driver; dies with the driver | ok |
| S31 | `RetirementQueue` | a destruction SCHEDULE against the device's completion watermark, not a value keyed by content | ok |
| S32 | `Scene::publishedSnapshot_` (`ScenePublisher.hpp:242`) | a handle to an immutable publication; the publisher's version decides whether a new one is built (S13) | ok |

## 3. The four shapes, and the one row that fails

Every row above is one of four shapes, and the shape decides which question (3) needs:

1. **Source-revision value** (S1-S10). The key is a number the source reports about itself, measured under the same
   lock as the build. It can collide across objects, and that is fine: the value never leaves the object without the
   object's own identity beside it. Safe.
2. **Content-side table keyed by identity plus a clock** (S11-S14). The clock alone has already been proved insufficient
   once (`Z30`: two trees built by one builder reach the same epoch), so the identity is part of the key and moves
   whenever the table is rebuilt. Safe - and this is the shape a new content-side table must copy.
3. **`(handle, revision)` across the seam** (S16-S25). The handle retires a slot's generation, so a recycled address is
   a different key; the revision is the other half because one object can be many values. Safe.
4. **Address key that holds what it names** (S21, S22). The entry keeps a share of the object its key names, so the
   address cannot be recycled while the entry is queryable (`ADR-2026-11-09`). Safe, and review 21 added the sweep
   that ends such an entry when the last outside share goes.

Three rows sit outside the shapes and each is accounted for: S23 keys on a VALUE and has no holder to ask (recorded
`Z26`); S24 keys on an address and deliberately does not hold it, which is the host's documented duty; S26 has no
production consumer.

S15 was the failure: it had shape 4's key (an address) without shape 4's hold, and shape 2's problem (the input is a
LIST, which changed) without shape 2's identity. Review 22 §4 read this cache and pronounced it clean - it asked
whether an EDIT reaches the picture ("every `Light` mutator bumps the revision") and the answer is yes; it did not ask
whether a REPLACEMENT can reuse the number and the address, and the answer is that it can. That is the same question
`Z30` asked of the root's epoch and `ADR-2026-11-09` asked of four backend tables. The fix (§4.1) gives it shape 2's
identity: the list.

## 4. Z32 - the light set outlives the list it describes

**What it is.** `Scene::lightSet()` caches one `LightSet` and the pairs it was built for
(`Scene.hpp:528-531`; `Scene.cpp:264-289`). The comparison is "same count, same address per slot, same revision per
slot"; `addLight` / `removeLight` / `clearLights` (`Scene.cpp:219-241`) leave the cache alone, and `setLighting` is
those two of them (`Scene.cpp:248-252`).

**The collision.** A list change drops the scene's last share of a removed light (`removeLight` erases the
`intrusive_ptr`), so the object is freed. The next light is allocated at the same address with high probability - same
size class, freed immediately before - and `Light::revision_` starts at `1` for EVERY light (`Light.hpp:362`) and is an
EDIT COUNT, not a value identity: two rigs built through the same sequence of setters reach the same number whatever
their values are. So a replaced list can match the cached key exactly and be served the old `LightSet`.

**The trigger.** `setLighting` is the product spelling of exactly this: `clearLights()` frees every light, then
`rig.addTo(*this)` builds fresh ones (`LightRig.cpp:70-83`; the test at `GraphicsTest.cpp:5502` pins "fresh lights every
time"). A host that relights a scene (a document editor, a time-of-day control) and renders between the two calls can
keep the first lighting. No in-tree host calls it twice today, so this is trigger-gated - but it is the one operation
the verb exists for.

**Why the tests cannot see it.** The only light-set case (`ValueChannelTest.cpp:223`) moves ONE light's values, which
is the revision path, not the replacement path. There is no case that replaces the list.

**The rule the fix must move.** A content-side derived value is keyed by the IDENTITY of its input, and this cache's
input is the LIST. The list's identity changes when the list changes - not when an address happens to differ. So the
three mutators must end the cached answer, and `setLighting` inherits the rule through them. Holding the lights in the
key (`ADR-2026-11-09`'s shape 4) is the BACKEND's spelling and is wrong here: content-side tables never keep authored
objects alive - `SceneBake` keeps no node alive either (`S11`), which is why it uses identity plus clock instead.

### 4.1 Fixed 2026-11-28 - the list ends the answer

`Scene::invalidateLightSet` (`Scene.cpp:219-227`) is the one body (`light_set_.reset(); light_key_.clear();`), and the
three list mutators call it (`addLight` `:236`, `removeLight` `:247`, `clearLights` `:254`). `setLighting` is
`clearLights` + `rig.addTo`, so it inherits both calls. `Scene::lightSet`'s own comment (`Scene.cpp:278-284`) and the
header contract (`Scene.hpp` `lightSet`, and the members' note at `:533-538`) now state the invariant: an address is
only ever compared while the list still holds the light it names.

**Witnesses.**

- `SceneTest.AReplacedLightListDoesNotReuseTheOldAnswer`: `lightSet()`, then `removeLight` + `addLight` of the SAME
  pointer - addresses and revision match by construction, so only "a list edit ends the cache" can make the answer a
  new value. It does not depend on the allocator.
- `SceneTest.ReplacingTheLightingBuildsTheNewSet`: the product shape - two rigs that differ only in direction; the
  second set must be a new value AND carry the second direction.

**Mutations, each run separately** (suppress the call, rebuild, run both cases, restore):

| arm | suppressed | result |
|---|---|---|
| A | all three calls | **both cases red** - and the second failed on the DIRECTION (`y` 0 vs 1), so on this allocator the freed light's address was reused inside the same run: the collision is not hypothetical |
| B | `removeLight`'s call | green - the following `addLight` already ends the cache, and a removal with no add changes the list's size, which the comparison catches. **Depth, not load-bearing** |
| C | `clearLights`'s call | green - `setLighting` goes on to add lights, and a bare clear changes the size. **Depth, not load-bearing** |

The load-bearing arm is `addLight`'s - the one a size compare would NOT catch, because remove-then-add restores the
count while an address can be recycled. That corrects this page's pre-fix guess (it expected `addLight`'s to be the
redundant one); the mutation, not the reading, decided it.

## 5. What this census changes about "how many problems are left"

The count of findings was never the useful measurement; the useful one is whether the NEXT table can be wrong in a way
no rule catches. Before this census the answer was unknown for every table not yet read. Now every table is on one of
four shapes, three of them are safe BY RULE, and the fourth's requirement is written down where a new table is designed
(`graphics-architecture.md` 2.20) - which turns the next round's question from "read another seam and see" into three
finite checks:

- a new content-side table must name its input's identity (shape 2) - the question `Z30` asked, now written down;
- a new key across the thread seam must be a `(handle, revision)` pair (shape 3) - the rule `ADR-2026-11-11` states;
- a new backend table must hold what it names or be swept by who holds it (shape 4) - the rule `ADR-2026-11-09` states
  and review 21 implemented.

The census's yield was one gap (`Z32`, fixed the same session), one rule (`graphics-architecture.md` 2.20) and one
retirement (`S26`). That is a different statement from "no problems": it says every remaining risk has a name, a
trigger and a place to be closed.

## 6. Gates

The fix is the only behavioural change; the prototype deletion removes three cases, and every other row is a reading.
Verified 2026-11-28: `test_graphics` **713/713** after the retirement (716 with the two new cases before it, 714 before
this round) and `test_vk` **240/240** on the fix; the three mutation arms above were each restored and rebuilt before
the next. The documentation gates were run after the page landed: `check_ai_docs.py --strict`,
`check_design_docs.py`, `check_cross_refs.py`.
