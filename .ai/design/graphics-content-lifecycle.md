# graphics content lifecycle: release and caching

> **Status**: current
> **Last verified**: 2026-11-16
> **Scope**: host ownership of content objects, GPU residency release and caching policy, and the reclamation of upload staging.
> **Sources**: `RenderDevice.hpp` / `RenderEngine.hpp` / `MaterialImages.hpp` /
> `vine/backend_vsg/internal/MaterialArena.hpp` / `.../Streams.hpp` / `.../ResourceTable.hpp` / `BlockStorage.hpp`.
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

2.1 **Two lifetimes**: `Geometry` / `Material` / `ShaderProgram` / `Texture` are host objects; uploads, blocks, descriptors and
    compiled variants are backend residency.
2.2 **Release GPU residency, not the object**: after `releaseContent()` the object is still usable, and the next use rebuilds residency.
2.3 **Explicit management**: no LRU, no byte cap, no pressure eviction; the host says when to release.
2.4 **Never idle the device**: release is one word from the host (a mark), rows are reclaimed per epoch (2.10); only the destruction path uses a counted device idle.
2.5 **Streams are shared by buffer identity, and lifetime is "who still declares these bytes"**: `StreamKey` = buffer + revision + segment; the steady state retransmits nothing.
    An entry holds the `Buffer` its key names, so the address lives as long as the entry. Residency lifetime **does not look at time**; two O(1) facts decide it:
    the declaration index published by the content layer (`ContentFacts::declarations`, an
    address→declaration count `ContentStore` maintains synchronously on row changes) says "is there still live content declaring this buffer"; the entry's own handle says "is the revision still the one it uploaded".
    An entry with no declaration, or whose revision has moved on, parks and leaves (`vine/backend_vsg/internal/ResourceTable.hpp`). The per-frame cost is O(#resident entries),
    not a function of content size (culling does not delete rows, so scanning rows would turn the frame that draws nothing into the most expensive frame).
2.6 **Tables keyed by identity, and an entry must live while its key lives; only uploads are still keyed by address**: the geometry / material / program fact rows are keyed by the content handle
    `(id, revision)` (`ContentId.hpp`), and the compiled halves and descriptor sets are keyed by the same `ContentRef` (plus layout / variant /
    kind) — the whole class of error "another description at the same address" no longer exists for them, because the handle moves its generation on release and a stale reference
    answers gone outright. **The only table still keyed by address is the upload table** (`StreamUploads`' `StreamKey` = `Buffer` address + revision + segment,
    and `Buffer` is not part of the content object system): its entries **hold the `Buffer` their key names**, keep the entry when parking cannot be scheduled and retry next frame,
    and never hand the address back to the allocator early; the instance stream a plan reads is likewise held by the plan (see ADR-2026-11-09-address-keys-hold-the-object).
    Explicit release names it the same way: `releaseContent(const Buffer<float>& / const Buffer<uint32_t>&)` parks this stream's entry
    by address, with no need to invent an identity for `Buffer` (G1: at this layer the address is its only identity).
2.7 **Three words, three meanings** (read this table before the code, or "identity" will mean three things):
    - **name**: `Node::name()`, used only for lookup and debugging, **not** an identity.
    - **key (id + revision)**: the key of the cache tables. `id` is the content object's `Handle` (see `ContentId.hpp`), and `revision` says
      "this value"; the only table still keyed by address is the upload table (see 2.6).
    - **variant id**: a unique handle issued by `core::VariantPool`, used only to recognise "this one compiled result", and it may be invalidated across frames.

2.8 **The staging one upload relays through lives for "one upload", not for residency**: `releaseContent()` answers "does the host still want a GPU copy of this object kept",
while the host staging one upload relays through (in vsg, `TransferTask`'s `staging`: one `HOST_VISIBLE` buffer for the whole upload, only ever grown) has
**no second reader** once the copy has landed — it is neither the host object's residency nor a reusable cache, just temporary memory not yet given back to the system. Why: only once it is classified can anyone decide "when to give it back". It cannot enter `releaseContent`'s scope (that would decide residency on the host's behalf),
nor should it become a capacity cap / budget (`G8`: no consumer).
Boundary: **this staging is no longer retained, but the reusable minimum size stays** (see `../decisions/ADR-2026-11-13-staging-keeps-the-reusable-block.md`). It is the source of **one** `vkCmdCopyBuffer`, with no second reader; the moment the copy's fence completes it is dead weight.
So "when to give it back" is not a question anyone has to decide: give it back when the evidence (that block's fence) arrives, and the trigger is the **frame boundary**, with the host neither participating nor needing to know (F16–F21).
Satisfies `G1` / `G7` / `G10`: one rule, zero waiting; and the same thing no longer has a second name (there is no longer a "clear one upload" verb on `RenderDevice`).
The cost is written in D14: it relies on vsg's protected block state (the derived task reads/clears it).

2.9 **The three switches behind "no longer drawn" have different consequences**: explicit `releaseContent()` (the host says "I do not want residency any more"; an object, or a
    `Buffer` one stream reads),
   the host dropping the last reference (the object leaves the tree, and only then are rows and residency swept), and **merely not being drawn this frame** (hidden, culled by the frustum).
   The first two are decided by **holdings**; the third does not change holdings, so it releases **nothing**:
   - Table rows (`ContentStore`'s geometry / program / material) look only at holdings: while the object is there (the scene graph holds it) the row stays,
     so hiding/culling causes no build-row-drop-row churn (`ContentStore.cpp`'s `releaseAbandoned` requires `useCount() <= 1`).
   - Uploads (`StreamUploads`) also look only at **holdings**, and what they look at is the **declaration**: the declaration index published by the content layer
     (`ContentFacts::declarations`) says "is there still a live row declaring this buffer", and the entry's own handle says "is the revision still the one it
     uploaded". Hiding or culling does not delete the row ⇒ the address is still declared ⇒ the upload stays, and coming back **retransmits nothing**;
     the object leaving the tree ⇒ the row is deleted after the epoch window ⇒ the declaration disappears ⇒ the upload leaves with it. A buffer generation change (`Buffer::edit()`)
     also makes the old entry lose the revision half on its own, with no word from the geometry.
   - Textures (`MaterialImages`) follow **the same rule**: the declaration is a material row's slot (`declaresTexture`) or a pass's imported input for that frame
     (`ImageInput`, which counts even without a fact row), and the other half is the entry's own `Texture::revision()`; leaving does not park (the descriptor set
     holds the view / sampler itself), so it is asked in `ContentAssembly::beginFrame` together with the streams.
   - Compiled halves / descriptor sets are **session-level** and do not follow a pass: a half is pinned by **program@rev ∧ `declaresLayout`**
     (the "layout → live row count" the geometry table maintains, O(1)), and a descriptor set additionally by **the textures it samples** (a material row's `declaresTexture` +
     the texture revision in its own key). Both go through `ResourceTable::retireUnpinned`, and there is no longer a per-frame scan asking "is there still any geometry asking for this
     layout" (when a hand-built `ContentFacts` has not published the index it falls back to the scan under the optional-index rule, and production always publishes); "the last
     geometry using that layout leaves the tree ⇒ the half parks" is answered directly by the declaration index.
     **The scan belongs to the frame, not the pass**: both tables park once at the head of `ContentAssembly`'s frame (`ProgramHalves::sweep` /
     `BindingSets::sweep`), so a frame with zero draws still reclaims, and N passes do not scan the table N times.
     **Known residue**: the pin only covers the two halves of the key that say "did the content change" (program@rev, layout / texture),
     while topology, attachment shape and variant are in the key too — the old halves / old sets a change in those leaves behind do **not** leave because of the pin,
     but wait until the program or the layout disappears as well. Each such object is one compiled pipeline (tens of KB), and the count is bounded by
     program × layout × that program's variant/topology combinations, growing with the shape of the edit rather than with frames; closing it completely would need the pin to cover
     the whole key, which conflicts with `I4` (the O(#resident entries) criterion), and it is left as measurement-driven.
   - Material blocks (arena slots) follow **the same rule**: a slot is kept by **identity**, and every frame the declaration index is asked once
     (`ContentDeclarations::declaresMaterial`: the material table's per-identity row count) "is there still a live row"; if not, the slot is
     **freed that frame, immediately** (`MaterialArena::retireUnpinned`). It is not a park — the arena's copy rotation is that window,
     and what is given up is the right to reuse the slot, not bytes an in-flight frame will read.
   Why split it this way: merging "the object is gone" and "this frame did not draw it" into one switch must either pay the rebuild cost for culling churn or keep residency
   that has left the tree forever (`G1`: two questions, two answers). The stream step has already moved from "guess a third switch from a time window" to "ask only who still declares" (see the ADR).

2.10 **Content rows' lifetime is computed, not parked**: every geometry / program / material row records the frame it was last hit by a plan
    (epoch), and the frame boundary reclaims rows that meet two conditions: `last_epoch < completedFrame() + 1`, and **no longer the entry's current answer**.
    Two consequences:
    - A superseded revision (the entry already answers a new row) and a released entry (the host said release, or nobody holds it any more) leave after the window;
    - **Culled / hidden content does not lose its row**: it is still the entry's current answer, so it is still the source of the upload declaration (F27 / F28).
    An entry itself leaves when "the host does not want it" and "no row still answers" hold at the same time (`ContentStore`'s `retireOwner`: mark and discard in one pass);
    material arena slots do not look at this entry, only at **rows**: the frame head asks the declaration index, and the slot is freed after the last row leaves.
    `markUsed` / release tokens / park / `kept` no longer exist (the rules of `ResidencyTable` moved onto rows, see ADR-2026-11-11 paragraph 3).

## 3. Features

### 3.1 Explicit release

| # | Feature | Notes |
|---|---|---|
| F1 | `releaseContent(...)` | Releases the GPU residency of Geometry / Material (asset or `MaterialInstance`) / ShaderProgram / Texture, and of a single stream's `Buffer<float>` / `Buffer<uint32_t>` (a buffer is named by address; the geometry reading it is untouched). |
| F2 | Release mark | Release is only a mark; rows are reclaimed per epoch (2.10). The next use (`track` / `ensure*`) clears the mark, so nothing is deleted by mistake. |
| F3 | Textures deleted immediately | The texture cache deletes the entry immediately; rows are reclaimed per epoch, and the material arena slots per row (pin). |
| F4 | Rebuild | The next use rebuilds residency; it is not a leak. |
| F5 | Abandoned sweep | After the host drops its reference, `ContentStore::releaseAbandoned` marks it (once per frame, after content is recorded and before submission), and rows and entries are reclaimed per epoch; uploads, textures and material blocks leave through the frame's own declaration scan. |

### 3.2 Caching

| # | Feature | Notes |
|---|---|---|
| F6 | Texture cache | `MaterialImages` entries are pinned by **live content**: the material row's slot declaration (`ContentDeclarations::declaresTexture`) + the textures each pass imports that frame, plus the entry's own `revision()`; no LRU / cap. |
| F7 | Material arena | `copies` per slot (matching in-flight frames); a full slot answers `Refused` and evicts no live material; a slot's lifetime is the pin (a live material row declares this identity), and the frame head's `retireUnpinned` frees it. |
| F8 | Grow on demand | Before recording, swap in larger storage according to `ContentStore::materialCount()`; no hard cap. |
| F9 | Stream sharing | `StreamUploads` are shared by `StreamKey`; a steady-state frame uploads 0; entries are pinned by live geometry rows (`ResourceTable`). |
| F10 | Per-frame blocks | `BlockStorage` is a per-frame ring, slabs = in-flight frames + 1; a steady-state frame does not write. |
| F11 | Alignment | A block offset must be a multiple of the device's `minUniformBufferOffsetAlignment`. |
| F12 | Budget | Oversized / overflow / material refusal are refused and counted before any budget is spent. |

### 3.3 Observation

| # | Feature | Notes |
|---|---|---|
| F13 | Counters | `writes` / `bytesWritten` / `materialWrites` / `uploads` / `aliases` / `refusals` / `retired`. |
| F14 | Diagnostic episode | Each refused block / material is reported once; cache pressure no longer emits `ResourcePressure`. |
| F15 | Observable after release | Release → the next frame rebuilds; the case asserts residency disappears and then appears again. |

### 3.4 Upload staging

| # | Feature | Notes |
|---|---|---|
| F16 | Not retained | The host-visible buffer of one upload is released once the copy completes: `staging` and `buffer_data` are cleared (the state vsg itself handles when "not big enough, rebuild"), and the next upload into that block allocates again. |
| F17 | The evidence is that block's own fence | `block->fence->status() == VK_SUCCESS` means "the copy that read it has completed". No timeline window, no retirement queue, no `deviceWaitIdle` (see N8). |
| F18 | The trigger is the frame boundary | `VsgBackend::executeFrame()` runs `releaseFinishedUploadStaging()` once at its very start, asking each session about its own upload tasks. The host does not call it and does not need to know (F18 used to require the host to call it at a lifecycle point — withdrawn, see ADR-2026-11-10-upload-staging-is-not-retained). |
| F19 | The frame boundary must not block | A block whose fence has not completed is left to the next frame; a block that still has data to copy in `DataToCopy` is skipped too (the next upload reallocates anyway, so clearing early only wastes one allocation). |
| F20 | Per block, independently | `TransferTask` has `2 × numBuffers` blocks (early/late × 3) and uploads rotate by `frameIndex`, so both blocks can hold one model (measured: a 704 MB body → `SharedUsage` 1,228 MB = `2 × 614 MB`); clearing is per block too. |
| F21 | Observable | `VsgBackend::uploadStagingBytes()` / `api::OffscreenSession::stagedBytes()` report the current occupancy; the steady state is always 0 (while pending, it is the span between the upload instant and the next frame boundary). |
| F22 | Answered by the derived task | "Do I hold a block" is answered by the session's own upload task: it is a `detail::StagingTask` (derived from `vsg::TransferTask`, able to see and clear `staging` / `buffer_data` / `fence`), installed by `installStagingTask()` when the session starts. No device-level upload count (that is the device's metering, not the asked session's). |

### 3.5 The six "no longer drawn" situations

| # | Situation | Rows (tables) | Uploads (stream) | Halves / descriptor sets | Material slots | Triggered at |
|---|---|---|---|---|---|---|
| F23 | **material leaves the tree** | Marked at the next frame boundary after the host drops the reference; rows leave only after the epoch window | — | None (the set key does not include material) | After the row leaves, the frame head's pin scan `BlockStorage::retireUnpinnedMaterials` frees the slot | `ContentStore::releaseAbandoned` / `releaseContent` |
| F24 | **program leaves the tree** | Same as above | — | The half parks when program@rev is no longer answered by the table (the vsg pipeline it drives goes with it; pooled variants are only evicted FIFO 65) | — | Same as above |
| F25 | **pass detaches** | The backend only has the identity: `releasePass` forgets the id (never reissues it) | — | **Not released** (session-level; it leaves only with the program or the layout); the input set falls back after one frame | — | `RenderEngine::removePass` → `device()->releasePass()` + the owned target's `releaseRenderTarget()` |
| F26 | **geom leaves the tree** | Sweep-marked after the host drops the reference; rows leave after the epoch window ⇒ the pin disappears and the upload leaves through its own window | — | The last geometry row for the layout leaves the tree ⇒ `declaresLayout` disappears ⇒ the half parks (a program leaving the tree parks too) | `releaseAbandoned` |
| F27 | **geom hidden** (`setVisible(false)`) | **Kept** (the host still holds it) | **Kept**: the row is still there ⇒ the pin is still there; becoming visible again retransmits nothing | Kept | — | Affects collection only, triggers no release |
| F28 | **geom culled** (outside the frustum, per view) | Kept | Same as F27 — culling is per view, but it does not change "who declared these bytes" | Kept | — | Same as above |

Textures (`MaterialImages`) belong in the "uploads" column: their pin is likewise a declaration (a material row's slot) + the textures imported that frame + the entry's own
revision, so a hidden / culled material does not lose its textures, and only once the material row passes the epoch window do the textures leave (F26's mechanism, another table).

## 4. Public Interface

### 4.1 Release

| Interface | Purpose |
|---|---|
| `RenderDevice::releaseContent(raw_ptr<const Object>)` | Releases GPU residency |
| `RenderEngine::releaseContent(...)` | The engine forwards it |

Upload staging **has no entry point**: it reclaims itself at the frame boundary (F16–F19); the backend-side surface is `VsgBackend::uploadStagingBytes()` and
the per-frame observation `FrameReport::uploads` (`FrameUploads`).
`api::OffscreenSession::stagedBytes()` (read-only observation).

### 4.2 Caching and storage

| Interface | Purpose |
|---|---|
| `MaterialImages::acquire()` / `releaseTexture()` / `beginFrame()` / `released()` / `bytes()` | Texture cache (lifetime reads `ContentDeclarations::declaresTexture` + the textures imported that frame + the entry's own revision) |
| `core::MaterialArena::write()` / `capacityNeed()` / `live()` | Material blocks |
| `BlockStorage::beginFrame()` / `writeView()` / `writeDraw()` / `writeMaterial()` / `retireUnpinnedMaterials()` | Per-frame blocks |
| `BlockStorage::strides()` / `regions()` / `bytes()` | Layout and observation |
| `StreamUploads::acquireVertex()` / `acquireIndex()` / `beginFrame()` / `retired()` / `uploads()` | Stream uploads (entries held by `core::ResourceTable`; lifetime reads `ContentFacts::declarations` + the entry's own revision) |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | Content objects belong to the host, GPU residency to the backend; release goes through an explicit entry point. |
| C2 | The cache has no capacity cap; the host takes as much as it uses. |
| C3 | A full material slot refuses; live materials are never evicted. |
| C4 | Upload staging is neither residency nor a cache: reclaiming it changes no host object and no revision, and the next upload allocates for itself. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | `releaseContent()` releases GPU residency; it does not decide lifetime for the host. |
| R2 | Never idle the device; release is only a mark, and rows are reclaimed per epoch (2.10). Staging reclamation is lighter still: fence evidence (F17), zero waiting, zero idle, no retirement queue. |
| R3 | No global budget / trim / shrink is introduced. |
| R4 | Staging reclamation needs no host participation: it is not a policy but "give it back once used" (F18). There is no call site on the host side any more, and no switch. |

### 5.3 Invariants (N#)

| # | Caveat |
|---|---|
| N1 | Do not reintroduce LRU / a byte cap "to save memory"; the direction has been decided as explicit release. |
| N2 | Reusing a slot immediately is safe: `copies` per slot, and a new material writes the current copy. |
| N3 | A refusal does not evict live material: reusing a slot recorded in the same frame may draw wrong. |
| N4 | The release path does not idle the device; only the destruction path uses a counted idle. Staging reclamation is such a destruction path (see R2's exception). |
| N5 | Block storage growth only adds chunks and never moves old blocks (old spans must stay valid). |
| N6 | Alignment comes from device limits; do not hardcode 16 / 256. |
| N7 | There is no timer and no "collect after a few quiet frames": the criterion is that block's own fence (F17), and the frame boundary is only "when to look". |
| N8 | The evidence for destroying staging is neither a timeline window nor device idleness: a timeline needs frames (if the host stops it never expires), and `deviceWaitIdle` stops the whole device. |
| N9 | Staging reclamation **does no internal synchronisation**: what it reads and writes is state the frame path is reading (the task's blocks, the blocks' `staging` / `buffer_data` / `fence`). It only runs at the frame boundary (once per frame, one thread); calling it on another thread is undetectable. |
| N10 | Clear **only `staging`** (together with the `buffer_data` pointing at it); the block itself, the fence and the command buffer / pool all stay: deleting the element does not work (`*(frames[frameIndex])` is dereferenced directly), and replacing the whole task costs much more. |
| N11 | The derived task reads vsg's protected members (`_earlyDataToCopy` / `_lateDataToCopy` / `TransferBlock::staging`). The criterion only recognises "`staging != nullptr` ⇒ a piece of host memory is held", which is the same sentence as "this memory is still there"; an upstream rename = a compile error (loud), an upstream semantic change is the dangerous kind and is caught by the byte-count assertions in T8. |
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
- How to detect it: `VsgBackend::uploadStagingBytes()` / `stagedBytes()` is not 0 after things go quiet; `RetentionStats::device_waits`
  rises on this line.
- How to prevent it: only recognise that block's own `fence->status()` (F17), sweep once at the frame boundary (F18), and **add no extra condition** (E).

### 6.4 Hidden / culled content does **not** lose its uploads (only leaving the tree does)

- How it fails: treating "not drawn this frame" as "the object is gone", and so either adding a release to the leave-the-tree path (culling churn ⇒ retransmit every frame),
  or, conversely, believing hidden content forgets its uploads and adding a time window to it.
- How to detect it: `VsgBackend::releasedStreams()` only grows **after the row disappears**; for the same piece of geometry, "invisible → visible again" leaves
  `StreamUploads::uploads()` **unchanged**. The row count (`ContentStore`'s live objects) is unchanged too.
- How to prevent it: lifetime asks one question only — "is there still a live geometry row declaring these bytes?" (pin = buffer address + its current revision,
  enumerated from the geometry rows published by `ContentStore::tablesFor`). To give residency back early, use explicit `releaseContent()`;
  do not add a time window / capacity cap (`G8`: that guesses on the host's behalf).

### 6.5 Swapping the instance stream is a **description** change, and the geometry must report the revision itself

- How it fails: the host swaps the instance slice of a primitive (`Geometry::setInstanceTransforms`)
  without then reporting `Geometry::bumpRevision()`. That setter only advances the content epoch (`Node::invalidateBounds`) and
  does **not** touch `revision()`; the backend's geometry row, however, is only rebuilt when the revision moves (`ContentStore.cpp`'s
  `findGeometry(..., contentRefOf(identity, live.shape->revision()))`). So the plan reads the new buffer from the primitive every frame while
  the fact row still declares the old buffer, and the upload entry is **built and retired every frame**: the picture is right and the cost is silent.
- How to detect it: in a steady state where nothing changed and only the frame is redrawn, `StreamUploads::uploads()` and `retired()` both rise per frame,
  while `live()` does not (only all three together rule out "the content really changes every frame"); the replaced buffer never appears in the declaration index.
- How to prevent it: swapping the slice is "this description changed", so call `bumpRevision()` right after. Hiding/culling does **not** trigger this
  (the row is still there ⇒ the declaration is still there, see 6.4), so it only has consequences when the buffer **really is** swapped —
  the "cull 100 frames, zero retransmission" case does not pin it down; T10 does (before the report only the old stream is declared, after it both generations are, and once the old row leaves only the new stream is).

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | LRU / capacity cap / pressure eviction | Under 300 textures cycled in the same order, LRU is as bad as FIFO; a cap decides for the host. See [ADR](../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md). |
| D2 | Material FIFO eviction | Evicting live material draws wrong; a refusal is only the fallback. |
| D3 | Idling the device on release | Only the destruction path needs it; release is only a mark, and rows are reclaimed per epoch. |
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
| D15 | Replacing the whole `TransferTask` to release | It throws away each block's command pool / command buffer / fence, and the three hand-overs have to be got right again (`vsg-pitfalls.md` P42.1); clearing `staging` loses only that buffer. |
| D16 | Clearing only blocks larger than `minimumStagingBufferSize` (keeping a reusable small block) | ~~Rejected~~ **Adopted (T26)**: a host that uploads every frame reaches a steady state with zero allocations, and a one-off large block is still given back; see `../decisions/ADR-2026-11-13-staging-keeps-the-reusable-block.md`. |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `ContentStoreTest.*Release*` / `MaterialImagesTest.Release*`. |
| T2 | `VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt`. |
| T3 | `HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice`. |
| T4 | `BlockStorageTest.*` / `MaterialArenaTest.*` / `StreamUploadsTest.*`. |
| T5 | `UploadScratchTest.TheUploadScratchIsNotRetainedAndTheFramesGiveItBack`: frames after an upload do not raise idle; the upload's staging is reclaimed by later frames with **nobody calling** (every live block is a multiple of the reusable size; a one-off block larger than the minimum is given back); redrawing the same content uploads nothing; a new stream uploads again and falls back to the minimum again. |
| T6 | `OffscreenSessionTest.AReplacedTransferTaskLeavesTheSessionSubmitting`: the session has an askable task installed as soon as it starts (`stagedBytes()` has a value and is 0), emitting frames does not raise idle, and it can still submit afterwards. |
| T7 | `OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands`: the mechanism layer (a 256 MB working set falls back + a later upload is read back byte for byte; a task vsg built itself cannot answer → reports 0 after being installed → reports ≥ 4096 bytes after another upload). |
| T8 | `HeadlessBackendTest`: the device side no longer has a staging entry point (the verb is gone from the SDK, and `RenderDevice` has one virtual function fewer). |
| T9 | The six "no longer drawn" situations (§2.9 / F23–F28): `ContentStoreTest.*Release*` (leaving the tree and explicit release), `ContentStoreTest.ACulledGeometryKeepsItsUploadsForAHundredFrames` (cull 100 frames, zero retransmission), `ContentStoreTest.AStreamLeavesWithinTheParkingWindowAfterItsGeometryIsDropped` (`live()` drops within `slots + 1` frames of leaving the tree), `StreamUploadsTest.TwoGeometriesOfOneBufferShareOneUploadUntilBothLeave` (the pins of a shared buffer merge), `VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt` (rebuild after release), and `VsgBackendTest`'s pass-identity case (`releasePass` only forgets the id). Hidden / culled "the row is unchanged and the upload stays too" is pinned half by `StreamUploadsTest` and half by `ContentStoreTest`, not merged into one — those are two different facts. |
| T10 | The instance stream is a **declaration** too (§2.5 / §6.5): `ContentStoreTest.AnInstanceStreamIsDeclaredThroughItsPrimitiveAtTheRowsRevision` — the row carries the primitive's instance handle and declares it from that; after the slice is swapped but no new description is **not** reported, only the old stream is declared; after `bumpRevision()` the live row declares the new stream while the parked old row still declares the old one; once the old row leaves only the new stream remains. |
| T11 | Per-frame uploads/releases are an **increment** in the report (T25/T29): `FrameUploadsTest.TheReportAnswersWhatThisFrameUploadedAndReleased` — no answer before the frame; the first frame has `stream_uploads`/`stream_bytes` > 0; the steady state is all 0; `Buffer::bumpRevision()` uploads once more; swapping content reports `stream_releases` > 0. |

## 9. Status and Evolution

### 9.1 Current status

Current design; features and interfaces are as the tables above. F16–F22 (upload staging) have landed: `gfx_backend_vsg` sweeps
"fence completed" blocks at every frame boundary and releases their `staging` (zero waiting, zero idle), the SDK side has **no** corresponding verb, and the model viewer needs to call nothing either
(the `release_render_memory` command was deleted with it).

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items are kept by trigger condition.

### 9.3 Compatibility / migration

Old names / old paths appear only in §7's rejected options or in git history; new code is written against §4's interfaces.

## 10. References

`graphics-architecture.md` · `graphics-geometry.md` · `vsg-backend.md` · `../backlog/graphics.md`
