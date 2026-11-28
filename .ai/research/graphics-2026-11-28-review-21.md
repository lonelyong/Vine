# Graphics review 21 (2026-11-28): resource lifetime in a dynamic scene

> **Status**: one finding (`Z25`), high, **fixed the same day** with two cases and three mutations; two residuals recorded
> (`Z26`, `Z27`), neither fixed. §2 states the rule the fix moved, §3 what was read and why it is clean, §4 what was found
> and deliberately left.
>
> **Scope**: the resource-lifecycle half of the shipped backend - every table that keeps something between frames
> (`ContentCaches`' geometries / programs / pipelines / instance streams, `ContentImages`' textures and upload batches, the
> memory pool, the frame driver's scratch) - asked the question a dynamic scene asks: **content that changes while the
> session runs**. Reviews 19 and 20 read correctness seams (swapchain, feature enable list, state shadow, pool oracles);
> none of them asked what happens to the cache when the CONTENT moves.
>
> **Verified 2026-11-28**: `test_vk` 240/240 (two new cases, three mutations run), `test_graphics` 710/710, doc gates green
> (§5). Every claim below cites the line it was read at.
>
> **In one line**: the backend ends an entry when the HOST names it (or when its key is an address nobody holds), and never
> when the host has moved on - so a scene that regenerates its shape leaks one entry's streams, its host bytes and its
> description per regeneration, which two in-tree hosts do every time the FPS readout changes.

## 1. Why this theme, and what "read" means here

A static frame is the case every other round measured: collect, upload once, then pay nothing
(`VkResidencyTest.TheSecondFrameOfTheSameContentBuildsNothing`). A dynamic scene is the case where the caches have to
*have an ending*: nodes come and go, a mesh is regenerated, a texture is replaced, a document closes while the next one
opens. The question asked of each table was:

> **who can still name this entry, and where does the code learn that nobody can?**

`ContentCaches`' own note already answers it for one table - "a table entry holds its key, and ends with it" (`N11`) - and
that answer was implemented for the instance stream only, where the key is a `Buffer` ADDRESS and a leaked entry would turn
a recycled address into a stale pointer. The other three tables hold the same kind of share (a `GeometryDescription`, a
`ShaderProgramDescription`, a `Texture`) and had no ending at all, which is the shape of a leak rather than of a policy.

## 2. Z25 - a superseded identity is nobody's, and the cache kept it

### 2.1 Three tables that only the host's announcement could end

- `ContentCaches::Impl::geometries` (`ContentCaches.cpp:192`) is keyed by `const GeometryDescription*` and holds the
  description; the entry is inserted at `:802` (`ensureGeometry`'s `Build` path) and erased in exactly two places:
  `releaseGeometry` (`:2326`) and `destroyAll`. `releaseGeometry` is reached only from `Device::releaseContent(Geometry)`,
  which asks the LIVE object for its description (`Device.cpp:4029`: `geometry->description()`).
- `programs` / `pipelines` are the same shape: keyed by the program description (and its `ContentRef`), erased by
  `releaseProgram` (`:2345`) alone.
- `ContentImages::Impl::textures` (`ContentImages.cpp:149`) holds an `intrusive_ptr<const Texture>` - the same "the entry
  holds its key" property - and only `releaseTexture` erased it.

Two consequences, and they are different defects of one rule:

1. **A superseded revision is unreachable.** A geometry's revision is not a version number on one object; it is a NEW
   IDENTITY. `Geometry::description()` returns one value per revision (`DescriptionCache::get`, `DescriptionCache.hpp:67` -
   the cache keeps exactly one `cached_`, so the geometry moves its share to the new value), `bumpRevision()` (`Geometry.cpp:451`)
   builds the next one, and `Device::releaseContent` can only ever name the CURRENT one. The previous revision's entry is
   therefore not merely unannounced; **it cannot be announced**.
2. **A dropped object is unreachable too.** A host that closes a document drops the last `intrusive_ptr<Geometry>`; there is
   no object left to hand `releaseContent` (`raw_ptr<const Object>` - it needs a live object), so the entry, its streams and
   (through the entry's hold on the description, which holds `AttributeChannel::values`) **the host's own vertex buffers**
   stay for the session's life.

`graphics-content-lifecycle.md` 2.9 / F26 promised the opposite ("the host dropping the last reference ... and only then are
rows and residency swept"), and `N11` called the gap a residual on the grounds that "its rows are what leave the tree" - a
mechanism (`ContentStore`, row epochs, a declaration index) that the shipped backend does not have. `N11`'s justification was
the retired backend's, and it has been rewritten this round (see §2.4).

### 2.2 Where it leaks in the product

`FpsOverlay::writePattern` (`FpsOverlay.cpp:223`) regenerates the readout's positions on every published rate change:

```cpp
readout_->setPositions(packAttribute(positions));   // :250 - a NEW Buffer, because the row is one geometry
readout_->bumpRevision();                           // :251 - "the shape now names that buffer"
```

The comment above it states the intent exactly ("The vertex count is unchanged; what changes is WHERE the positions live (a
fresh buffer every update)"), and it is right about the picture. What it could not see is that the backend answered the
revision with a second geometry entry and never ended the first. Two in-tree hosts enable the readout:
`appfw/gui/Viewer.cpp:54` (`pipeline_options.fps.enabled = options.stats`) and `demo_plugin/DemoScene.cpp:1439`.

**Per rate change**: 21 bars x 24 vertices x 3 floats x 4 B = 6,048 B of vertex stream + 21 x 36 indices x 4 B = 3,024 B of
index stream = **9,072 B of device memory**, plus the retained host `Buffer<float>` (6,048 B) and the description. It is a
per-change figure, not a per-second one - but a host whose readout moves every frame pays it every frame, and the same
mechanism is what a host that regenerates a deforming mesh, a plot or a live tessellation pays.

### 2.3 The fix: one rule, four tables

The rule `N11` already stated for the instance stream is now the cache's rule for every table whose entry holds its key:

> **An entry that holds its own key ends at the frame boundary once that key has no other holder** - no time window, no
> capacity cap, no host verb. `useCount()` *is* the answer to "can anybody still name it", because naming it requires a
> share.

- `Impl::withdrawGeometry` (`ContentCaches.cpp:490`) is the one withdrawal body: forget the claim, take the entry out of the
  table, park its streams behind the entry's own share. `releaseGeometry` and the new sweep both run it, so "when does a
  geometry entry end" has one answer.
- `Impl::sweepUnheldGeometries` (`:526`) walks the table at the frame's end and withdraws entries whose description share is
  the last one. **Culling and hiding are untouched**: they do not move the revision, so the geometry still holds its
  description (F27 / F28's promise - becoming visible again retransmits nothing).
- `Impl::withdrawProgram` (`:558`) is `releaseProgram`'s body moved into the cache, and `Impl::sweepUnheldPrograms` (`:625`)
  ends a revision nobody holds - with every pipeline compiled for it. Its probe is a CENSUS, not a single share count: the
  table is keyed by `(description, variant)`, so one revision legitimately holds one share per variant
  (`Impl::ProgramCensus`); the working vector is reused so a steady frame allocates nothing.
- `ContentImages::sweepUnheldTextures` (`ContentImages.cpp:693`) is the same probe for the texture table. A live material
  holds its own share of each slot (`TextureSlotBinding::texture`) and a pass' imported input does too
  (`ImageInput::byTexture` - "the declaration is OWNING"), so content that still names a texture keeps its image.
- All four run once, from `ContentCaches::endFrame` (`:2498-2501`), next to the sweep that was already there. Cost:
  O(#resident entries) - one share count each, never a walk over content (the property `graphics-frame-pipeline.md` `I4`
  gives residency: it follows who still references the value).

### 2.4 The witnesses, and the mutations

- `VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced` is the product shape: draw, keep a command list that still
  names the old shape (what a cached publication or a recording frame has), regenerate the positions + `bumpRevision()`, draw,
  drop the held list, draw. **Red on today's code** at `after->live` 4 vs 2 ("one shape is resident, not one per
  regeneration"); green after the fix, and the held-list step also pins the opposite mistake (a sweep that drops what a frame
  is still about to submit).
- `VkResidencyTest.ContentTheHostNoLongerHoldsLeavesResidencyBehind` is the closing document: open A (geometry + program +
  sampled texture), open B, drop A's last references, draw B twice. Geometry 4 → 2, textures 1 → 0, programs' `retired` 0 → 1.
- **Mutations, each run separately** (all three arms off → both cases red; then geometry alone → the shape case green and the
  document case still red on textures and programs; then textures restored → red on programs alone). Every arm is load-bearing.
- One existing case had to change, and it is worth recording why: `VkResidenceWireTest.ARefusedBuildIsWithdrawnInsteadOfPublished`
  drew its warm-up geometry as a TEMPORARY (`programDraw(makeTriangle(), program)`), so "one key is resident" was an assertion
  about the leak itself. The geometry is now held by the case (`ResidenceWireTest.cpp:409`) and the assertion keeps its
  meaning.

### 2.5 What the fix does not cover

- **A shape edit still costs a full re-upload.** A geometry entry's streams are its own copies, so a new revision uploads every
  channel, not just the one whose buffer changed (`VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced` asserts the
  2). The design's `StreamKey` sharing (`graphics-content-lifecycle.md` 2.5) is not implemented in this backend; a 20 KiB
  mesh regenerated per frame pays 20 KiB per frame instead of 0. Recorded as `Z28` (§4).
- **Pipelines of a LIVE program revision** that no longer match a topology / attachment shape stay until the program goes
  (`graphics-content-lifecycle.md` 2.9's "known residue"). The sweep ends them with the program, not before it.

## 3. Read and clean (with the property that makes it so)

- **The memory pool gives a block back** (`MemoryAllocator.cpp:543-570`): a block that became entirely free is destroyed when
  another block of its type remains, so a one-off large upload or a closed target does not hold its reservation for the
  session; the last block of a type stays warm. This is why the leak above was invisible in `MemoryStats`: the pool was fine,
  the table was not.
- **Upload batches have their own ending** (`ContentImages::batchCompleted` / `batchAbandoned`): `pending` is promoted or
  parked, the batch's staging is destroyed on the evidence of its own command buffer, and the batch's book is erased -
  including on the abandoned path, which parks what a failed submit could still name.
- **The SDK already had this rule** (`PublicationStore::beginFrame`, `PublicationStore.cpp:66-92`): it drops the publications
  no frame names and the overrides of scenes whose handle is no longer alive - the same "ask the identity, not a time
  window" the backend now applies to content. Two mechanisms, one vocabulary (`G6`).
- **`ContentRegistry` handles are recycled, not accumulated** (`ContentRegistry.cpp:36-72`): a released slot returns to the
  free list and the generation moves, so churn costs no growth. `peakObjects()` is a high-water mark and says so.
- **The frame driver's scratch is destroyed with the driver** (`Device::releaseFrameDriver` → `FrameDriver::destroy` →
  `FrameScratch::destroy`: descriptor pool, ring buffer, cached sets), so opening and closing document windows does not keep
  one pool per window.
- **`ReadbackStaging`** opens one buffer/pool/fence per read and releases them in its destructor; nothing is cached across
  reads.
- **The descriptor-set cache** is swept every 120 frames with a 600-frame retention and a pool cap of 1024 sets, and a full
  pool refuses rather than evicting a live set (`ContentAbi.hpp:41-50`, `FrameScratch::beginFrame`). Its combinatorial cost
  curve is backlog `T69` and was not re-opened.

## 4. Recorded, not fixed

| # | Severity | What | Where it would bite |
|---|---|---|---|
| Z26 | low | `ContentImages::samplerFor` caches a `VkSampler` per distinct `Sampler` VALUE with no cap and no sweep - the one table whose key is a value, so no share can ever end it | a host that animates sampler state (an `lod_bias` ramp, a streaming filter switch) grows one device object per distinct value. Trigger: such a host appears, or `sets_live` / sampler count shows up in a memory report |
| Z27 | low (doc) | `graphics-content-lifecycle.md` still names the retired reference backend's tables (`ContentStore`, `StreamUploads`, `MaterialArena`, row epochs, a declaration index) as the mechanism - 2.5, 2.9, 2.10, F5, F26, §6.4. This round corrected the one claim the fix contradicts (2.10 / F26 / §6.4 now point at the shipped rule) and left the page's conversion as backlog `T73` | the next reader takes a mechanism that no longer exists for the current one - the same cost `N11` paid |
| Z28 | low (cost) | a geometry entry owns one copy of every stream, so a shape edit re-uploads channels it did not change; the design's `StreamKey` sharing is absent from this backend | a host that regenerates a shape per frame (the FPS readout is the in-tree one: 9 KB per change instead of 6 KB). Trigger: a measured upload cost after a shape edit, or a discrete-GPU reading (`T67`'s batch) |

## 5. Gates

- `build/bin/test_vk` - **240/240** (238 before; the two new cases in `VkResidencyTest`).
- `build/bin/test_graphics` - **710/710** (unchanged; the SDK was not touched).
- ASan + LeakSanitizer over the changed code: `VN_ASAN_TARGET=test_vk VN_ASAN_FILTER='VkResidencyTest.*' VN_ASAN_LEAKS=1
  VN_ASAN_LEAK_SCOPE='vn::backend_vk' scripts/asan_check.sh` - **12/12, RESULT: PASS, zero leak report**, which is the half a
  reading round cannot settle: the sweep erases a table entry while frames hold shares of it, and the parked destruction is
  the only place a mistake would surface.
- Mutations: all three sweep arms off → both new cases red; each arm restored alone → exactly its own assertion is the one
  that is red. Recorded in §2.4.
- Static gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py`, `check_doc_symbols.py`,
  `check_include_hygiene.py`, `check_export_annotations.py`.
- `test_gui` is **red in this environment, and not for this round's reason**: no framework `.qm` catalog is built here (Qt
  `lrelease` is missing - the CMake warning recorded in the repo notes), so every case that asserts a translated UI string
  fails (`DocumentViewTest.*`, `UserIOTest.PathCheckTable`, `MainWindowTest.TheApplicationButtonSpeaksTheUiLanguage`, ...),
  and the console-wait cases time out under a machine running a second build. No graphics case is among the 21 failures, and
  the round touches no appfw file.

## 6. What the next round should open

- The **frames-in-flight story of the sweep**: it runs at one driver's frame end while another driver may be recording. That
  is safe by construction here (the recording driver holds the description, so the entry is not unheld - better than the
  entry-share test), but no case exercises "two drivers, one of them drops the content mid-frame". `VkMultiSessionTest` has
  the harness for it.
- `Z26`'s table is the only one left whose ending is "never"; if a sampler is ever animated, it needs the same treatment
  (an intern with a cap, or a sweep keyed on nothing - a value has no holder to ask).
