# graphics content lifecycle: release and caching

> **Status**: current
> **Last verified**: 2026-11-28
> **Scope**: host ownership of content objects, GPU residency release and caching policy, and the reclamation of upload staging.
> **Sources**: `RenderDevice.hpp` / `RenderEngine.hpp` / `ContentImages.hpp` /
> `vine/backend_vk/internal/FrameScratch.hpp` / `.../ContentCaches.hpp` / `.../ResidencyClaims.hpp` / `.../TargetTable.hpp`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

This file defines the current design of graphics content lifecycle: release and caching: first the objects and their boundaries, then features, interfaces, constraints and acceptance.

### 1.2 Goals

- Turn §2's design points into §3's features, §4's interfaces and §8's acceptance.
- Keep "what it is now" separate from "why it was decided that way": the reasons are in §2/§7, history is in git.

### 1.3 Non-goals

- This file does not repeat the full signatures from the headers; an interface gives the contract and a pointer only.
- This file keeps no closed history or per-round review records.

### 1.4 One-paragraph conclusion

> Content objects belong to the host, GPU residency belongs to the backend; release goes through an explicit entry point; the cache has no capacity cap and takes as much as the host uses.

## 2. Design Points

2.1 **Two lifetimes**: `Geometry` / `Material` / `ShaderProgram` / `Texture` are host objects; uploaded streams, compiled programs and pipelines,
    descriptors, targets and the frame's constant ring are backend residency.
2.2 **Release GPU residency, not the object**: after `releaseContent()` the object is still usable, and the next use rebuilds residency.
2.3 **Explicit management**: no LRU, no byte cap, no pressure eviction; the host says when to release.
2.4 **Never idle the device**: `releaseContent` forgets the entry now and hands its GPU objects to the device's completion park; a dropped or superseded object is ended by the frame end whose sweep finds no other holder (2.10). Only the destruction path uses a counted device idle.
2.5 **Every cache entry holds the identity its key names**: an instance-stream entry is keyed by its `Buffer`'s address **and holds that buffer**; a geometry entry is keyed by the address of the immutable `GeometryDescription` it built from and holds it (its streams are the entry's own copies); a program entry holds its description; a texture entry holds its `Texture`. The key is therefore an identity in both directions: no later object can be allocated at an address the entry still names, and `revision` compares versions of the one object the value came from (`../decisions/ADR-2026-11-09-address-keys-hold-the-object.md`; `vk-backend.md` 2.5 / 2.25).
    Residency lifetime **does not look at time**; one O(1) fact decides it: who still names the key. A host's `releaseContent` forgets it, and the backend's own frame end ends it when the cache is the key's last holder (`sweepUnheldStreams` / `sweepUnheldGeometries` / `sweepUnheldPrograms` / `sweepUnheldTextures`, `ContentCaches::endFrame`). An ended entry's Vulkan objects park behind the completion watermark, so no frame that resolved them destroys anything it may still submit.
    **Known gap**: the design's `StreamKey` sharing (one upload per buffer, shared by the geometries that read it) is NOT implemented - a geometry entry owns one copy per channel, so a shape revision re-uploads every channel (review 21 `Z28`, backlog `T74`). The per-frame cost of the sweep is O(#resident entries), not a function of content size: culling and hiding do not move a revision, so they never make a frame walk content.
2.6 **One identity vocabulary, two scopes**: a value that crosses the thread seam names content by `ContentRef{id, revision}` (`ContentId.hpp`) - the snapshot rows and `DrawItem` do - and a handle's generation moves on release, so a stale reference answers gone instead of resolving to the slot's next occupant. A BACKEND entry is keyed by the address of the immutable value it was built from (a description, a `Buffer`, a `Texture`) and **holds that value** for as long as the key is queryable, so the address cannot be recycled underneath it (2.5); there is no address-keyed table that does not hold its key. `releaseContent` names either form: an object, or the `Buffer` of one stream (a `Buffer` is not part of the content object system, so its address is its identity at this layer; `G1`).
2.7 **Three words, three meanings** (read this table before the code, or "identity" will mean three things):
    - **name**: `Node::name()`, used only for lookup and debugging, **not** an identity.
    - **key**: for a backend entry, the identity of the immutable value it was built from (the description's address, which the entry holds - 2.6); for a value
      crossing the seam, `(id, revision)` (`ContentRef`). A revision alone is never an identity: every object starts at 0, and two objects' counters collide.
    - **variant id**: the handle that recognises "this one compiled result" in the backend's own table, and it may be invalidated across frames.

2.8 **The staging one upload relays through lives for "one upload", not for residency**: `releaseContent()` answers "does the host still want a GPU copy of this object kept",
while the host staging one upload relays through (one host-visible buffer for the whole upload, only ever grown) has
**no second reader** once the copy has landed — it is neither the host object's residency nor a reusable cache, just temporary memory not yet given back to the system. Why: only once it is classified can anyone decide "when to give it back". It cannot enter `releaseContent`'s scope (that would decide residency on the host's behalf),
nor should it become a capacity cap / budget (`G8`: no consumer).
Boundary: **this staging is no longer retained, but the reusable minimum size stays** (see `../decisions/ADR-2026-11-13-staging-keeps-the-reusable-block.md`). It is the source of **one** upload copy, with no second reader; the moment that batch's command buffer completes it is dead weight.
So "when to give it back" is not a question anyone has to decide: give it back when the evidence (that batch's command buffer completing) arrives, and the trigger is the **frame boundary**, with the host neither participating nor needing to know (F16–F17).
Satisfies `G1` / `G7` / `G10`: one rule, zero waiting; and the same thing no longer has a second name (there is no longer a "clear one upload" verb on `RenderDevice`).
The cost: the backend must keep a batch's staging alive until that batch's submitted command buffer has completed — the per-batch bookkeeping (`ContentImages::BatchId`) is the whole of it.

2.9 **The three switches behind "no longer drawn" have different consequences**: explicit `releaseContent()` (the host says "I do not want residency any more"; an object, or a
   `Buffer` one stream reads), the host dropping the last reference (the object leaves the tree, and only then does the frame-end sweep end what nobody holds), and
   **merely not being drawn this frame** (hidden, culled by the frustum). The first two are decided by **holdings**; the third does not change holdings, so it releases **nothing**:
   - **Geometry entries**: an entry holds the shape it describes, and a snapshot row holds the shape while a scene names it. A dropped geometry's entry (and its
     streams) ends at the frame end where the cache's share is the last; a **superseded** revision ends the same way, because a shape edit builds a NEW identity
     `releaseContent` can never name.
   - **Instance streams**: keyed by the `Buffer`'s address and holding it (2.5); a host's release or the frame-end sweep ends the entry. Hiding or culling does not
     move the revision, so the buffer stays named and coming back **retransmits nothing**; a buffer edit (`Buffer::setRevision`) makes the entry's revision half stale,
     with no word from the geometry.
   - **Textures**: an entry holds the `Texture`. A live material's slot and a pass' imported input hold the texture, so a hidden / culled material keeps its image; the
     entry ends when the last outside share goes (`ContentImages::sweepUnheldTextures`).
   - **Compiled programs and pipelines**: a program entry holds the description and the modules and pipelines compiled for it; it ends when the last outside share goes,
     and takes every pipeline of that revision with it, because the table is keyed by `(description, variant)`.
     **Known residue**: a pipeline of a LIVE revision that no longer matches a topology / attachment shape stays until the program goes; it is bounded by
     program × variant/topology combinations and is left as measurement-driven (`vk-backend.md` N11).
   - **Material values**: nothing is kept between frames - the values are bytes in the frame's constant ring, written per draw (F23), so a material leaving the tree
     has no entry to end.
   Why split it this way: merging "the object is gone" and "this frame did not draw it" into one switch must either pay the rebuild cost for culling churn or keep residency
   that has left the tree forever (`G1`: two questions, two answers).

2.10 **Residency ends by holdings, at the frame boundary**: there are no rows and no epochs - every cache entry holds the object its key is (2.5 / 2.6), and
    `ContentCaches::endFrame` ends every entry whose key has **no holder other than the entry itself** (`vk-backend.md` 2.25). Two consequences:
    - A superseded revision (a shape edit builds a new identity the host can never name again) and a dropped object leave that way, with no host verb;
    - **Culled / hidden content keeps its entry**: it does not move a revision, so the geometry still holds the description and the entry stays the source
      of its streams - becoming visible again retransmits nothing (F27 / F28).
    Cost: O(#resident entries) once per frame, one share count per entry. The probe is `useCount()`, and a frame still recording, a cached publication and a
    host-held command list all hold the description, so the sweep can never take what a submission may still name (F2 still decides *when* the objects go).
    Two drivers do not change the probe: a frame end on one driver runs beside the other driver's recording body, and that body's commands hold
    what they resolved, so the entry stays until the body finishes and the next frame end ends it
    (`VkMultiSessionTest.AFrameEndBesideARecordingDriverKeepsTheContentThatDriverNames`).
    **Why not a policy**: a time window picks a number nobody can defend, and re-deriving "is this content alive" from the scene graph would put a second answer
    beside the host's own reference count (`G1`; `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`).

## 3. Features

### 3.1 Explicit release

| # | Feature | Notes |
|---|---|---|
| F1 | `releaseContent(...)` | Releases the GPU residency of Geometry / Material (asset or `MaterialInstance`) / ShaderProgram / Texture, and of a single stream's `Buffer<float>` / `Buffer<uint32_t>` (a buffer is named by address; the geometry reading it is untouched). |
| F2 | Release forgets now, parks later | `releaseContent` removes the entry on the spot (residency numbers answer immediately and the next frame uploads again), while its Vulkan objects park behind the device's completion watermark. Nothing waits on the host side. |
| F3 | Textures end by holdings | A texture entry is ended by `releaseContent(Texture)` or by the frame-end sweep when its last outside share goes; the image parks. A material that still names it keeps it. |
| F4 | Rebuild | The next use rebuilds residency; it is not a leak. |
| F5 | Frame-end sweep | After content is recorded and before submission, the frame tail ends every entry whose key has no holder other than the cache (F26's mechanism, every table); nothing is marked for a later epoch. |

### 3.2 Caching

| # | Feature | Notes |
|---|---|---|
| F6 | Texture cache | An entry holds its `Texture`; a live material's slot and a pass' imported input hold the texture, so hidden / culled content keeps its image; the entry ends with its last outside share. No LRU / cap. |
| F7 | Material values are frame bytes | A material keeps no GPU entry: its values are packed into each draw's constant slice, so "when does the material's residency end" has no answer to give - the slice dies with the frame. |
| F8 | Ring grows on demand | The frame's constant ring (`FrameScratch`) grows in chunks before recording, according to the frame it is about to build; no hard cap. |
| F9 | Streams are per entry (sharing is a known gap) | A geometry entry uploads its own copy of every channel, so a shape revision re-uploads every channel; the design's `StreamKey` sharing is NOT implemented (review 21 `Z28`, backlog `T74`). The instance-stream entry holds its buffer and is swept at the frame end. |
| F10 | Per-frame ring | `FrameScratch` is the frame's constant ring: slabs = in-flight drivers + 1, one per driver; a steady-state frame does not write. |
| F11 | Alignment | A block offset must be a multiple of the device's `minUniformBufferOffsetAlignment`. |
| F12 | Budget | Oversized / overflow / material refusal are refused and counted before any budget is spent. |

### 3.3 Observation

| # | Feature | Notes |
|---|---|---|
| F13 | Counters | `ContentCaches::Progress` (`stream_builds` / `stream_releases` / `streams_live`) and the device's descriptor-set numbers (`set_builds` / `sets_live`), plus `retirementNumbers()` for what the park holds. |
| F14 | Diagnostic episode | Each refused block / material is reported once; cache pressure no longer emits `ResourcePressure`. |
| F15 | Observable after release | Release → the next frame rebuilds; the case asserts residency disappears and then appears again. |

### 3.4 Upload staging

| # | Feature | Notes |
|---|---|---|
| F16 | Not retained | The staging buffer of one upload is released once the copy completes: the batch's staging is destroyed when its command buffer has completed (`ContentImages::releaseUploadStaging`), and the next upload allocates again. |
| F17 | The evidence is that batch's own completion | The upload batch's submitted command buffer having completed means "the copy that read it is done". No timeline window, no retirement queue, no `deviceWaitIdle` (see N8). |
| F18 | The trigger is the frame boundary | the frame tail that recorded the uploads calls `ContentCaches::endFrame(batch)` → `ContentImages::releaseFrame(batch)`, destroying that batch's staging and the images its revisions replaced; the warm-up's own batch (0) is released by `releaseUploadStaging(0)` after its fence. The host does not call it and does not need to know (an explicit host verb was withdrawn, see `ADR-2026-11-10-upload-staging-is-not-retained`). |
| F19 | The frame boundary must not block | A block whose fence has not completed is left to the next frame; a block that still has data to copy in `DataToCopy` is skipped too (the next upload reallocates anyway, so clearing early only wastes one allocation). |
| F20 | Per batch, independently | a batch is one frame driver (`ContentImages::BatchId` = `clock_window + 1`; 0 is the warm-up's), so releasing one batch never waits for a frame still recording under another — the reason a second driver does not inherit the first one's staging. |
| F21 | Deliberately not exposed | No SDK API reports upload-staging occupancy: the steady state is 0 bytes beyond the in-flight batch, and a counter nobody branches on would be a capability with no consumer (`G8`). |
| F22 | Answered by the batch | "Do I hold a staging" is the upload batch's own bookkeeping (`ContentImages::BatchId`): the staging of one batch is released when that batch's command buffer completes. No device-level upload count answers it (that is the device's metering, not the asked session's). |

### 3.5 The six "no longer drawn" situations

| # | Situation | Rows (tables) | Uploads (stream) | Halves / descriptor sets | Material slots | Triggered at |
|---|---|---|---|---|---|---|
| F23 | **material leaves the tree** | A material keeps nothing of its own here: its values are packed into each draw's uniform block, so the row and its GPU state are already the frame's (`vk-backend.md` 2.25) | — | None (the set key does not include material) | Nothing to free - the values lived in that frame's ring | the backend's content table |
| F24 | **program leaves the tree** | Its compiled variants and every pipeline compiled for that revision go with it, at the frame end that finds the revision unheld (`VkResidencyTest.ContentTheHostNoLongerHoldsLeavesResidencyBehind`) | — | The pipelines are the compiled halves here; a LIVE revision's variants that no longer match a shape still wait for the program (`vk-backend.md` N11) | — | `ContentCaches::endFrame` |
| F25 | **pass detaches** | The backend only has the identity: `releasePass` forgets the id (never reissues it) | — | **Not released** (session-level; it leaves only with the program or the layout); the input set falls back after one frame | — | `RenderEngine::removePass` → `device()->releasePass()` + the owned target's `releaseRenderTarget()` |
| F26 | **geom leaves the tree** | The host's last reference IS the announcement: the entry's share of the shape is then the last one, so the frame-end sweep withdraws the entry and its streams. The same sweep withdraws a **superseded** revision, which a shape edit produces and which `releaseContent` can never name | — | The last geometry holding a layout's pipeline shape leaves ⇒ the entry goes, and a program nobody holds takes its pipelines with it | — | `ContentCaches::endFrame` (`VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced` / `ContentTheHostNoLongerHoldsLeavesResidencyBehind`) |
| F27 | **geom hidden** (`setVisible(false)`) | **Kept** (the host still holds it) | **Kept**: the row is still there ⇒ the pin is still there; becoming visible again retransmits nothing | Kept | — | The node's gate is a hard one; the SCENE's own flag is the same answer one level up (`ScenePublisher::ensureRows` publishes an empty table, review 22 `Z29`). Neither triggers a release |
| F28 | **geom culled** (outside the frustum, per view) | Kept | Same as F27 — culling is per view, but it does not change "who declared these bytes" | Kept | — | Same as above |

Textures follow the same rule as geometry: an entry holds its `Texture`, a material that still names it (or a pass that imports it) keeps it, so a hidden / culled material does not lose its textures; the
entry ends at the frame end where its last outside share is gone (`ContentImages::sweepUnheldTextures`, F26's mechanism in another table).

## 4. Public Interface

### 4.1 Release

| Interface | Purpose |
|---|---|
| `RenderDevice::releaseContent(raw_ptr<const Object>)` | Releases GPU residency |
| `RenderEngine::releaseContent(...)` | The engine forwards it |

Upload staging **has no entry point**: it reclaims itself at the frame boundary (F16–F19). The observation is the per-frame `FrameReport::uploads` (`FrameUploads`); the device-level numbers are `contentCacheCounters()` (streams / images) and `retirementNumbers()` (what the park queue holds), and none of them reports staging occupancy (F21).

### 4.2 Caching and storage

| Interface | Purpose |
|---|---|
| `ContentImages::releaseTexture()` / `viewFor()` / `batchCompleted()` / `batchAbandoned()` / `progress()` | Texture cache and upload batches (lifetime reads the declarations + the textures imported that frame + the entry's own revision); a recorded copy is PENDING until its batch's fence was waited |
| `FrameScratch::beginFrame()` / `reserveSlice()` / `slice()` | The frame's constant ring and descriptor pool (written per frame, owned by the frame's driver) |
| `ContentCaches::ensureContentPipeline()` / `planCommand()` / `planScreen()` / `resolveTextures()` / `releaseGeometry()` | Identity-keyed caches (geometry / program / pipeline / streams), resolved at the frame head; textures live in `ContentImages` |
| `ResidencyClaims::admit()` / `publish()` / `forget()` | Who may BUILD a key: `Use` (published), `Build` (this claimant), `Copy` (a competitor is filling it) |
| `ReadbackStaging::open()` / `recordCopy()` / `run()` | One readback's own command pool, staging buffer and fence |
| `TargetTable::ensure()` / `claim()` / `lease()` / `peek()` | The target's object (map + shape + build/rebuild/repair) and who may draw it this frame |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | Content objects belong to the host, GPU residency to the backend; release goes through an explicit entry point. |
| C2 | The cache has no capacity cap; the host takes as much as it uses. |
| C3 | No entry is ended by pressure: only `releaseContent` or the frame-end sweep ends one; a refusal reports and answers nothing. |
| C4 | Upload staging is neither residency nor a cache: reclaiming it changes no host object and no revision, and the next upload allocates for itself. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | `releaseContent()` releases GPU residency; it does not decide lifetime for the host. |
| R2 | Never idle the device; `releaseContent` forgets the entry and parks its objects (F2). Staging reclamation is lighter still: fence evidence (F17), zero waiting, zero idle, no retirement queue. |
| R3 | No global budget / trim / shrink is introduced. |
| R4 | Staging reclamation needs no host participation: it is not a policy but "give it back once used" (F18). There is no call site on the host side any more, and no switch. |

### 5.3 Invariants (N#)

| # | Caveat |
|---|---|
| N1 | Do not reintroduce LRU / a byte cap "to save memory"; the direction has been decided as explicit release. |
| N2 | The sweep only ends entries whose last outside share is gone; a frame still recording, a cached publication and a host-held command list hold what they name. |
| N3 | A refusal never evicts a live entry (it would draw wrong or rebuild); it reports through the diagnostic route and answers nothing. |
| N4 | The release path does not idle the device; only the destruction path uses a counted idle. Staging reclamation is such a destruction path (see R2's exception). |
| N5 | The frame ring grows only by adding slabs and never moves an old slice (old spans must stay valid). |
| N6 | Alignment comes from device limits; do not hardcode 16 / 256. |
| N7 | There is no timer and no "collect after a few quiet frames": the criterion is that block's own fence (F17), and the frame boundary is only "when to look". |
| N8 | The evidence for destroying staging is neither a timeline window nor device idleness: a timeline needs frames (if the host stops it never expires), and `deviceWaitIdle` stops the whole device. |
| N9 | Staging reclamation **does no internal synchronisation**: what it reads and writes is state the frame path is reading (the task's blocks, the blocks' `staging` / `buffer_data` / `fence`). It only runs at the frame boundary (once per frame, one thread); calling it on another thread is undetectable. |
| N10 | Clear **only the upload's staging** (not the image it fed, not the pool): the staging is the temporary half, so destroying it early frees bytes still being copied and keeping it turns an upload into a residency. |
## 6. Caveats and Traps

### 6.1 One fact, one home

- How it fails: the same state is stored in two places; change one and the other goes stale.
- How to detect it: two writable values / two writable registries appear.
- How to prevent it: go back to §5's N#; keep one owner and make the rest read-only.

### 6.2 The backend/platform boundary

- How it fails: platform differences are written into the engine/application layer, and switching platform forks everywhere.
- How to detect it: an "if Windows/X11 then…" branch appears.
- How to prevent it: platform facts stay in the backend; the engine only consumes the compiled frame values.

### 6.3 Upload staging: when to give it back

- How it fails (A): taking a "timeline window" as evidence. The window counts frames, while the copy's completion is a fact of the device: a park needs `2 × slots + 1` frames to expire,
  and if the host stops it never expires (measured: after loading a large model 1.2 GB stayed resident for minutes, the process at 1% of one core / 0% GPU).
- How it fails (B): taking "device idle" as evidence. `deviceWaitIdle()` stops the whole device, while correctness only needs "this one copy is done".
- How it fails (C): making "is there anything to give back" a question the host must answer (an explicit verb). The most common host in the product (load, then do nothing)
  never calls it, so another "ask again = now" has to be added — one feature with two semantics.
- How it fails (D): using the device-level "how many uploads happened" in place of "does this session still hold staging". One session succeeding masks the other's answer.
- How it fails (E): adding a cheap "this DataToCopy still has data to copy, so do not clear it yet" check to the sweep. Such a table can stay non-empty for a long time with nothing pending
  (`_transferData()` returns early and does not clear it), so the sweep skips forever and the whole staging becomes resident again.
- How to detect it: `FrameReport::uploads` does not fall back to 0 after things go quiet, or `RetentionStats::device_waits` rises on this line.
- How to prevent it: only recognise that block's own `fence->status()` (F17), sweep once at the frame boundary (F18), and **add no extra condition** (E).

### 6.4 Hidden / culled content does **not** lose its uploads (only leaving the tree does)

- How it fails: treating "not drawn this frame" as "the object is gone", and so either adding a release to the leave-the-tree path (culling churn ⇒ retransmit every frame),
  or, conversely, believing hidden content forgets its uploads and adding a time window to it.
- How to detect it: `ContentCaches::Progress::stream_releases` only grows once the geometry's ENTRY leaves; for the same piece of geometry, "invisible → visible again" leaves
  `stream_builds` **unchanged**, and so does `streams_live`.
- How to prevent it: lifetime asks one question only — "does anything but this cache still name this entry's key?" — and the frame end asks it
  (`vk-backend.md` 2.25: a geometry entry holds the shape it describes, so a culled or hidden geometry still holds it, while a superseded or dropped one cannot).
  To give residency back early while keeping the object, use explicit `releaseContent()`;
  do not add a time window / capacity cap (`G8`: that guesses on the host's behalf).

### 6.5 Swapping the instance stream is a **description** change, and the geometry must report the revision itself

- How it fails: the host swaps the instance slice of a primitive (`Geometry::setInstanceTransforms`)
  without then reporting `Geometry::bumpRevision()`. That setter only advances the content epoch (`Node::invalidateBounds`) and
  does **not** touch `revision()`; the backend's geometry entry is keyed by the description identity, which only moves with the
  revision, while the commands the frame draws are built from the live primitives - so each frame names an instance buffer no entry
  has seen: it is uploaded, used for one frame, and swept at that frame's end. The picture is right; the cost is silent.
- How to detect it: in a steady state where nothing changed and only the frame is redrawn, `stream_builds` and `stream_releases` both rise per frame,
  while `streams_live` does not (only all three together rule out "the content really changes every frame"); the replaced buffer never becomes live.
- How to prevent it: swapping the slice is "this description changed", so call `bumpRevision()` right after. Hiding/culling does **not** trigger this
  (the row is still there ⇒ the declaration is still there, see 6.4), so it only has consequences when the buffer **really is** swapped —
  the "cull 100 frames, zero retransmission" case does not pin it down; T10 does (before the report only the old stream is declared, after it both generations are, and once the old row leaves only the new stream is).

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | LRU / capacity cap / pressure eviction | Under 300 textures cycled in the same order, LRU is as bad as FIFO; a cap decides for the host. See [ADR](../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md). |
| D2 | FIFO / pressure eviction | Evicting an entry another frame or publication still names draws wrong; a refusal is only the fallback. |
| D3 | Idling the device on release | Only the destruction path needs it; release forgets the entry and parks its objects. |
| D4 | Global budget / trim / shrink | No consumer; the host manages explicitly. |
| D5 | A seam that reads back old frames | F−N is an observation gap, not a production race; not done. |
| D6 | Making reclamation an explicit entry point (called by the host or a user command) | The most common host (load, then nothing) never calls it; and a park expiry needs `2 × slots + 1` frames, so another "ask again = one counted idle" has to be added. See [ADR](../decisions/ADR-2026-11-10-upload-staging-is-not-retained.md). |
| D7 | A timeline park (or `deviceWaitIdle`) as evidence | The evidence is that block's own fence, which is stronger than a window and cheaper than device idleness: zero waiting, zero idle. |
| D8 | The engine "kindly" calling reclamation once per frame | The engine should have no resource policy, and it cannot see "has this copy completed". Looking at the fence at the frame boundary, in the backend itself, is the right place for the same thing. |
| D9 | An environment variable as a switch | It is equivalent to "give it back once used", not an optional policy; a switch would only add one more line to explain. |
| D10 | Adding a per-frame upload count to `FrameCounters` for the engine to trigger on | An SDK field nobody reads (`G8`). **The observation surface is `FrameReport::uploads`** (`FrameUploads`: this frame's stream/texture uploads, bytes and releases); the engine still does not trigger on it. |
| D11 | Adding a forwarding entry point to the engine | The engine already has `device()`; one more entry point that only hops once is the same thing under two names (`G4`). |
| D12 | Registering only a "to be cleaned" mark and leaving the real cleaning to the next frame | Registering is not giving back; the frame boundary is that "next frame", and no extra state is needed. |
| D13 | Asking "does this DataToCopy still have data to copy" before clearing a block | It looks cheap, but such a table can stay non-empty for a long time with nothing pending, so the sweep skips forever (see 6.3 E). |
| D14 | Using the device-level upload count (`uploads()` / `count()`) to decide "do I hold a block" | The count answers for the device, not for the session being asked (see 6.3 D). Ask the session's own task instead (F22). |
| D15 | Replacing the whole upload batch to release | It throws away the batch's command pool / command buffer / fence and has to get the ordering right again; releasing only its staging loses just those bytes. |
| D16 | Clearing only blocks larger than `minimumStagingBufferSize` (keeping a reusable small block) | ~~Rejected~~ **Adopted (T26)**: a host that uploads every frame reaches a steady state with zero allocations, and a one-off large block is still given back; see `../decisions/ADR-2026-11-13-staging-keeps-the-reusable-block.md`. |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `ContentRegistryTest.ARepeatedReleaseDoesNotMoveTheGenerationTwice`, `VkResidencyTest.ReleasingAGeometryLowersResidencyAndTheNextFrameUploadsItAgain` / `ReleasingATextureLowersTheImageResidency`. |
| T2 | `VkResidencyTest.ReleasingAGeometryLowersResidencyAndTheNextFrameUploadsItAgain`. |
| T3 | `HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice`. |
| T4 | `VkUniformRingTest.*` / `VkMemoryPoolTest.ASteadyFrameAllocatesNothing` / `VkResidencyTest.TheSecondFrameOfTheSameContentBuildsNothing`. |
| T5 | `VkMemoryPoolTest.ASteadyFrameAllocatesNothing`: the first frame's staging buffers are back by the steady frame with **nobody calling** (the frame tail releases its own batch); redrawing the same content uploads nothing (`VkResidencyTest.TheSecondFrameOfTheSameContentBuildsNothing`), a release makes the next frame upload again (`ReleasingAGeometryLowersResidencyAndTheNextFrameUploadsItAgain`), and one driver's batch never frees another's (`VkMultiSessionTest.TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`). |
| T6 | `VkPrepareUploadTest.APreparedTexturedFrameUploadsNothingWhenItRuns` / `ASecondPrepareSubmitsNothing`: the upload happens in the preparation batch, the frame that follows uploads nothing, and a second preparation submits nothing. |
| T7 | `VkResidencyTest.ReleasingAGeometryLowersResidencyAndTheNextFrameUploadsItAgain`: after the residency is released and wanted again, the next frame uploads it again — the first upload's staging is gone, the data is not. |
| T8 | `HeadlessBackendTest`: the device side no longer has a staging entry point (the verb is gone from the SDK, and `RenderDevice` has one virtual function fewer). |
| T9 | The six "no longer drawn" situations (§2.9 / F23–F28): leaving the tree and explicit release (`VkResidencyTest.ReleasingAGeometryLowersResidencyAndTheNextFrameUploadsItAgain`, `VkResidenceWireTest.AReleasedGeometryIsForgottenSoTheNextFrameMayBuildItAgain`), one buffer of a shared set (`VkResidencyTest.ReleasingOneBufferLowersOnlyTheStreamsThatReadIt`), a program (`ReleasingAProgramLowersTheCompiledHalvesAndKeepsTheStreams`), a texture (`ReleasingATextureLowersTheImageResidency`), a target (`ReleasingATargetFreesItsAttachmentsAndTheNextFrameBuildsThemAgain`), and the park (`AReleaseIsHandedOverParkedAndDestroyedAfterTheFramesThatCouldNameIt`). Hidden / culled content is engine-side: it is absent from the frame, so no residency question is asked (`SceneTest.CollectCommandsCullsOutOfView`). |
| T10 | The instance stream is a **declaration** too (§2.5 / §6.5): `PrimitiveInstancingTest.EachPrimitiveOwnsItsInstanceStream` (the primitive owns the slice) and `VkResidenceWireTest.AVertexBufferThatAnnouncesANewRevisionIsUploadedAgain` (a revision is part of the key, so a row declares the bytes of the revision it names). |
| T11 | Per-frame uploads/releases are an **increment** in the report (T25/T29): `VkResidencyTest.TheSecondFrameOfTheSameContentBuildsNothing` (the first frame reports `stream_uploads == 2` with bytes > 0; the second reports 0) and `VkPrepareUploadTest.APreparedTexturedFrameUploadsNothingWhenItRuns` (no frame has run ⇒ no numbers; the prepared frame's uploads are reused). |
| T12 | Content that is **superseded or dropped** leaves residency by itself (`vk-backend.md` 2.25): `VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced` (a command list that still names the old shape keeps it resident; dropped, the next frame's end releases the entry and its bytes go) and `VkResidencyTest.ContentTheHostNoLongerHoldsLeavesResidencyBehind` (a closed document's geometry, texture and pipeline leave, the open document's stay and still draw). |
| T13 | A frame end **beside another driver's recording body** does not end what that body names (`vk-backend.md` 2.25): `VkMultiSessionTest.AFrameEndBesideARecordingDriverKeepsTheContentThatDriverNames` (driver B is held inside its record by a refused packet while the host drops the shape and driver A's frames end; the geometry row stays at 2 live for the whole recording and goes to 0 - bytes down - at the first frame end after B finished; withdrawing regardless of the share reddens the first half, skipping the geometry sweep reddens the second). |

## 9. Status and Evolution

### 9.1 Current status

Current design; features and interfaces are as the tables above. The reclamation half is the frame-end sweep of §2.10 (`vk-backend.md` 2.25): every cache entry
holds its key, so a superseded shape revision and a dropped object release their residency without a host verb. F16–F22 (upload staging) have landed: the shipped
backend's frame tail releases that batch's staging once its command buffer has completed (`ContentCaches::endFrame` → `ContentImages::releaseFrame`; zero waiting, zero idle),
the SDK side has **no** corresponding verb, and the model viewer needs to call nothing either (the `release_render_memory` command was deleted with it).

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items are kept by trigger condition.

### 9.3 Compatibility / migration

Old names / old paths appear only in §7's rejected options or in git history; new code is written against §4's interfaces.

## 10. References

`graphics-architecture.md` · `graphics-geometry.md` · `../backlog/graphics.md`
