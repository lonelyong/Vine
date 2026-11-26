# graphics threading model (current contract)

> **Status**: current. The mirror / delta / three channels / memo+epoch generation of shapes is gone, and the only cross-thread payload left is
> "an immutable snapshot produced by one publication" and "two atomic revisions". The history (why it used to be that way, which option was rejected) is in
> `../decisions/ADR-2026-11-12-adopt-graphics-next.md`, `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`
> and git history; the `C1–C15` and `N1–N14` entries that only served the deleted mechanism are retired, and their numbers are not reused (see §5.1, §5.3).
> **Last verified**: 2026-11-15
> **Scope**: who owns what of one frame's work on each of the two threads — what the content side and the engine side own, which values may cross the seam, who owns the surface and the swap chain,
> where parallelism sits, and how shutdown finishes.
> **Excludes** appfw's general async (`appfw-startup-phases.md`, `async-design.md`) and the Vulkan synchronisation details (`vk-backend.md`).
> **Sources**: `src/viz/graphics/src/PublicationStore.{hpp,cpp}` (the content-side publication and binding tables),
> `src/viz/graphics/src/FrameExpand.{hpp,cpp}` (the frame's binding and expansion: `bindSceneDraws` / `expandFrame`),
> `src/viz/graphics/src/SnapshotSource.{hpp,cpp}` (the snapshot's `DrawListSource` face),
> `src/viz/graphics/src/FrameRunner.{hpp,cpp}` (the engine thread),
> `sdk/vine/graphics/Scene.hpp` (`publish`) and `sdk/vine/graphics/RenderEngine.hpp` (`bindSnapshot` /
> `setFrameThreading` / `waitForFrame` / `presentRecord` / `presentedCamera`).
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

Content (scene graph, geometry, material, camera) is edited synchronously by the host on the UI thread — that is the document editor's core feel and cannot become asynchronous;
while a frame's GPU-side work (compiling, uploading, recording, submitting, presenting, residency accounting) must be able to run in parallel with interaction, or the UI and rendering lock each other out.
Measured (Release): 2000 drawables with a moving camera, the frame's select 337–340 µs, production miss (`SnapshotSource::collectDrawItems`) 435–443 µs, and 10000 drawables 2.3–3.1 ms / 3.4–3.6 ms (`tests/test_graphics/SelectTest.cpp`'s `MeasureWhatOneSelectCostsWithAMovingCamera`, 2026-11-13; the old 1597 µs/frame timed the direct collector that is no longer on the frame path), the reference backend's fixture measured 33 allocations / one recorded triangle before it retired; vk's recording window - 85 per steady frame with one draw packet - and the whole chain frame - ≈1966 - are measured apart in `vk-backend.md` 9.1).

The hard part is not "starting a thread" but **who writes the content**: the backend's "fact rows" read from live content (`Geometry::revision()`, a primitive's instance handle),
and collection walks the scene graph. As soon as rendering is on another thread, the two sides must have some relationship: share (locks), copy (a per-frame snapshot), or hand over (one writer).

This module chooses: the content side folds the mutable tree into one **immutable value** at the moment of **publication**, and after that the two sides meet only through that value.
There is exactly one answer to "when does this copy expire": the publication value itself (see §2.2).

### 1.2 Goals

- A frame's GPU-side work (upload, record, submit, present, residency accounting, culling) runs on the **engine thread**, and the content thread does not carry it.
- **The content editing API is unchanged**: the host keeps reading and writing the scene graph synchronously on the UI thread.
- Only **immutable values** and **report values** cross the seam; there is no shared mutable state anywhere.
- A publication is **one atomic pointer swap**; version, mirror, delta and memo are no longer concepts.
- The relationship between the picture and the content is **computable**: a frame draws the publication bound at the moment it started, not "whatever the content happened to be while it ran".
- `inline` and `engine thread` share one code path.

### 1.3 Non-goals

- No general job system / task graph (`async` and `ThreadPool` already exist).
- No locks added to `Node` / `Geometry` / `Buffer` (D2).
- Content editing does not become asynchronous (D1).
- No per-pass parallel recording at this layer (§2.7); parallelism only over pure values in the derivation layer.
- No remote rendering / recording (`OwnedFrame`'s consumers are already divided into three scenarios).

### 1.4 One-paragraph conclusion

> The content side **publishes** the mutable tree into an immutable snapshot (one pointer swap), and the engine half only reads that value;
> each of the two threads has its writer, the only things crossing the boundary are "the published value" and "the completion report", and byte content is polled through two atomic revisions.

## 2. Design Points

### 2.1 Two worlds, one writer each

- **What it is**: the **content world** = `Scene` / `Node` / `Geometry` / `Buffer` / `Program` / `Material` / `Texture` / `Camera`,
  written only by the content thread; the **engine world** = the snapshot's `SnapshotSource`, the fact tables, the residency tables (`ResidencyClaims`
  and the backend's content caches: streams, images, materials, compiled pipelines), the recorded command stream and the swap chain, written only by the engine thread.
- **Why**: two writers sharing one mutable graph can only rely on locks — locks enter every read path and conflict directly with "zero allocation in the steady state" (`G1` / `G10`).
  One writer each plus a value channel is the only shape that keeps both sides lock-free.
- **Boundary**: the only cross-seam reads are "the published immutable value" (§2.2) and the values snapshotted with the pass (§2.14). The engine side **does not read** any mutable field of a content object.

### 2.2 The only cross-thread payload: the immutable snapshot one publication produces

- **What it is**: `Scene::publish()` folds the mutable tree into an immutable snapshot (the row table is chunked by 32 rows, and a write copies only the changed chunks),
  and returns a `SceneSnapshotRef` handle; `RenderEngine::bindSnapshot(scene, ref)` says "this engine draws from this one on".
  The engine half swaps each draw's `content` for that content's `SnapshotSource`, after which **the frame names nothing the content owns**.
- **Why**: one value replaces five answers to "when does this copy expire" (`G1`); publication is the only mutable → immutable conversion, so "an old frame draws the old value"
  is not a convention but value semantics; and derivation is a pure function, hence parallelisable, dumpable and replayable (`G11`).
- **Boundary**: the snapshot's content, chunking and selector are **implementation** (`detail::SceneSnapshot`); the public face is one opaque handle plus `revision()`.
- **What values the rows carry**: each command carries `geometry_ref` / `material_ref` (identity + version) and three immutable descriptions
  (`GeometryDescription` / `MaterialDescription` / `ShaderProgramDescription`); a pass describing itself also snapshots the
  `LightValue` array and the program override's text. That is §2.14's payload, and the recording side only reads it.
- **Whose verb a publication is**: `publish()` is the content side's verb (content thread, the only writer) and the **only mandatory verb**: when a frame is built
  it takes the scene's value at that moment (`Scene::publishedSnapshot`), so the host can draw as soon as it has published. `bindSnapshot()` only **overrides**
  ("this engine draws this one"), for two engines drawing different publications; both land in `detail::PublicationStore`, and the engine half's only call is
  `beginFrame` (which opens the memo token, see §2.15).

### 2.3 Two channels: the published snapshot and the completion report

| Channel | Direction | What it carries | Failure shape |
|---|---|---|---|
| **publication** (snapshot) | content → engine | immutable value: row table + material values + boxes + light set, with a `revision` | a publication landing mid-frame does not tear: the frame uses the one bound at the moment it started |
| **report** (report) | engine → content/UI | values: `PresentRecord` (frame number + present outcome), `presentedCamera()`, `FrameReport`, diagnostics | what the host reads is always one complete version, with its frame number |

- **Why only two**: a change announcement ("delta") is not a channel — a change is the next publication's content, and there is no second way to say
  "what I changed"; handoff (ownership transfer) is the **thread pool/loader**'s general shape, not this module's channel. No channel without a consumer (`G8`).
- **Boundary**: publication has no return value and does not queue — it is **one pointer swap**, and "full" is not a state it has.

### 2.4 Byte content: the counters are atomic, the bytes still do not take the value channel

- **What it is**: a geometry's **shape** (channel set, slices, primitives) goes through the published description value; a geometry's/texture's **bytes** do not — bytes are written on the writer's own
  thread (a loading worker, a streaming write), and a residency entry asks "is the revision I uploaded still the one it uploaded" (`ContentCaches::Progress`' `streams_live` and the revision in the
  residency key).
- **Where the synchronisation is**: `Buffer::revision` and `Texture::revision` are `std::atomic<std::uint64_t>` — the writer (the `Edit` scope
  ending / `setRevision` / one fill) releases, the reader (`revision()`) acquires, so "observing the new revision" means "seeing the bytes written before
  the revision". `Geometry` / `ShaderProgram` / `Material` revisions need not be atomic: the frame side reads the published
  description value (`GeometryDescription` / `ShaderProgramDescription` / `MaterialDescription`), not the live object.
- **Why keep polling**: turning every `Buffer::edit()` into a publication would couple "edit" and "frame" tightly, and a publication would have to carry thousands per frame.
- **Boundary**: the atomic covers only "the counter + the bytes before it"; a host that keeps writing the same bytes **after** announcing (without a new revision) is still outside the contract —
  the frame reads what was written. The frame payload's **content** (shape, material values, program text, lights) does not use this channel; it is all §2.14's values.
  The storage itself is the writer's to keep still: rewriting the same elements and announcing is the streaming path, while growing or replacing them must wait for the frame (or be handed over as a new `Buffer` object).
  `Buffer` holds no lock for this — `ADR-2026-11-26-buffer-bytes-are-caller-managed` is the decision and `Buffer<T>::Edit`'s warning is where a host reads it.
  **Regression case**: a threaded engine answers the same picture and says when it landed (`RenderEngineTest.AFrameThreadedEngineAnswersTheSamePictureAndSaysWhenItLanded`), and a host verb
  racing another driver's frame waits instead of racing (`VkMultiSessionTest.AHostVerbWaitsForTheOtherDriversFrameInsteadOfRacingIt`); both must be green in a normal build and under TSan
  (before the atomics, TSan reported the `bumpRevision()` vs `revision()` pair).

### 2.5 A publication is a pointer swap, not a transaction

- **What it is**: outside a batch, `Scene::publish()` compares the current rows' `(epoch, boxes, materials)`; if unchanged it answers `Unchanged` and returns the same value;
  if changed it builds a new snapshot (sharing the unchanged chunks) and swaps the handle `Scene` holds. `beginEdit()` / `endEdit()` only delimit **visibility**:
  a publication inside a batch answers `InsideBatch`, and the first publication outside it lands the whole batch as **one** publication.
- **Why**: a batch is not a transaction (there is no rollback); it only delimits "what a frame can see" (P4). Making publication one swap is what lets "an in-flight frame reads the old value"
  need no lifetime negotiation at all.
- **Boundary**: what an overlay's own `update()` (`FrameUpdate`, called by the host/chain before the frame) writes becomes visible in the next publication; all passes of one frame see **the same** publication
  (N16), because binding happens at the frame head rather than in each pass.

### 2.6 The surface belongs to the content side (UI), the swap chain to the engine side

- **What it is**: a surface is created/destroyed on the thread that owns the native window (Qt's handle and display belong to Qt); `acquire / present / swap-chain rebuild`
  happen only on the engine thread. The host hands the size to the area as a **value** (`RenderArea::resize`), and the engine hands "this frame's size" to the target at the **frame head**,
  and only when the number changed.
- **Why**: the handle's lifetime belongs to the host, the swap chain's lifetime to the device; the one point where they cross (the rebuild) must have a single writer (`G2`).
- **Boundary**: device verbs (`createSurfaceTarget` / `setWindowHandle` / `device->initialize`) are **the host's between-frames** operations,
  and doing them while a frame is in flight is undefined behaviour — the host calls `waitForFrame()` first (P3). One of them the framework makes safe on its own:
  `RenderEngine::setDevice` waits for the frame in flight before it hands the old driver back, because that driver is what the engine half is executing through - the rule
  `applyPlan` (refuse while busy) and `setFrameThreading(false)` (wait) already followed.

### 2.7 Parallelism lives only in the derivation layer

- **What it is**: the only candidate for parallelism is the "select" step (layer pruning + frustum + command construction + sort keys), and it works on **immutable values**.
- **Why**: a pure value is the precondition for parallelism; the device half still runs on the engine thread (C16), and the backend's shared caches are written under their own locks rather than in parallel.
- **Boundary**: if a backend records in parallel itself, the determinism of the merged result is the backend's to prove (`graphics-frame-pipeline.md` R20).

### 2.8 When idle the engine thread waits, and the UI does not queue frames

- **What it is**: the engine thread blocks on "the next deadline / a new frame arriving"; with no frame it does nothing.
- **Why**: a frame has exactly one source (`frame()`), so "is there work" needs no queue to express; and the UI's paint cadence is not controlled in reverse by the device.
- **Boundary**: `waitForFrame(n)` is the only entry point that makes the caller wait for the device (screenshots, resize settling, exit, measurement).

### 2.9 Compared with other libraries: why "the value of one publication", not "a per-frame extract"

| Library | Content side | Render side | Relationship |
|---|---|---|---|
| Unreal | game thread (`UWorld`/actors) | the render thread's `FScene` + proxies, changes through `ENQUEUE_RENDER_COMMAND` | the same shape (§2.2) |
| Qt Quick | the GUI thread owns `QSGNode` | the render thread `sync()`s its own copy every frame | **O(n) sync per frame**; this design avoids it with publication |
| Godot | main-thread nodes | `RenderingServer` through `CommandQueueMT` + RID | likewise a command queue + stable handles; RID ≈ this design's `NodeId` |
| Unity | `Transform` is only a handle, the native layer is authoritative | the render side processes transform changes in batches | likewise "batch the changes", which confirms that "publish per batch" is necessary |
| wgpu / Dawn | the application thread submits commands | internally tracks resource lifetimes | borrows "internal references and user references kept apart"; this repository uses `PinnedResources` + a retirement queue |

- **Why not a per-frame extract**: the extract/`sync()` shape is an O(drawables) copy every frame, whereas this design's goal is precisely to take the **per-frame**
  cost off the UI thread; one publication pays O(the changed part) only when **editing**.

### 2.10 A frame is first a described value, and expansion is another step

- **What it is**: the content half produces a `FrameDescription` (each pass's view, inputs and what it draws), and the engine half expands it into a command list,
  executes and presents it. Only values cross between the two steps.
- **Why**: this seam is the thread boundary (`G3` / `G7`): the content half can stay on the content thread, the engine half can move to the engine thread, and
  "what a frame is" has one definition.
- **Boundary**: the pointers in a `FrameDescription` are all borrowed (camera, commands, target, program) and are valid only for the duration of this call;
  what the recording side reads, however, is the **value** beside them (`light_values`, `program_override_description`, the three descriptions in a command),
  so a borrowed pointer going stale does not change "what this frame draws".

### 2.11 One frame's ownership: three things, one owner each

| Thing | Holder | Sole writer | Shape across the boundary |
|---|---|---|---|
| `Scene` / `Node` / geometry / material / camera | host (content thread) | content thread | publishes an immutable snapshot |
| `SceneSnapshotRef` + `SnapshotSource`'s current value | engine (one per scene, see §2.13) | the publisher (content thread); the engine thread reads only | value (a shared immutable snapshot) |
| fact tables / residency tables / frame values / retirement queue / command graph / swap chain | engine (session) | engine thread | — |

- **Why**: one owner per thing (`G1`); no object between the two worlds has "half ownership", so there is no "who frees first" question either (`G10`).
- **Boundary**: the engine side holding a share of the snapshot only means "what it reads is one complete version"; it does **not** thereby own the content, and the content changing does not affect the version it holds.

### 2.12 One frame's two halves, one thread each (`G2` / `G4` / `G10`)

- **The ownership table** (`G1`: one definition, one writer per thing)

| Thing | Holder | Sole writer | Shape across the boundary |
|---|---|---|---|
| native window handle / display | UI (Qt) | UI | value (size, visibility) |
| `RenderArea` (the area) | engine (the share `createArea` returns) | engine thread | — |
| `WindowPresentTarget` + swap chain | engine (made by the device, `createSurfaceTarget`) | engine thread | — |
| `RenderDevice` / session / frame values / retirement queue | engine | engine thread | — |
| the frame's **content half**: frame number, `compileFrame`, publication (once per frame per scene), `describe` (an overlay's `FrameUpdate::onFrame()`, before the frame, called by the host/chain) | content thread | content thread | the frame value is handed over |
| the frame's **engine half**: surface events, `expandFrame` (expanding the bound views), `executeFrame`, present | engine thread | engine thread | — |

- **One frame, two halves, one frame in flight on each side**: while the content thread builds frame N+1 the engine thread is running frame N, **so the picture is one frame behind the content**
  (N15). That is the only semantic cost, and what it buys is that a UI-side call costs only O(passes): no expand (O(drawables)), no present
  (which may wait for v-sync). Frame values therefore rotate between two slots (`frame_slots_`), the frame number travels with the frame (`detail::EngineFrame::number`), and
  `frame()` **only waits for the previous frame's engine half** before building (`waitIdle`, usually already done), and answers `waitForFrame` with the completion state.
- **What being one frame ahead means for interaction** (written down because it can be felt):
  - the camera is content and `CameraManipulator` is **event-driven** (no inertia, no dt), so **an operation takes effect at once**, with a constant one-frame delay;
  - the picture shows **the camera snapshot taken one frame ago**. At 60 fps 16 ms is imperceptible; at 10 fps it is 100 ms, and it feels like "drag one step, jump one cell"
    — a low frame rate is like that anyway; the extra is only this one frame;
  - **a content query must be computed with "the frame the user saw"** (N17): `presentedCamera()` publishes the camera snapshot of the frame that was answered,
    and the host hands it to the manipulator through `SceneView::setPresentedCamera`; with no published value it falls back to the current camera (a still picture, inline).
  - **the UI only "stops paying for expand and present", it is not "no longer throttled by rendering"**: every frame request waits for **the previous frame's engine half**, so at a low frame rate
    the UI's paint cadence is dragged along; input events still land normally between two requests (the accurate reading of R8).
- **"Wait until this frame" is an explicit action** (`G4`): `waitForFrame(frame_id)` — for screenshots, after a resize settles, for exit, for measurement; the default path
  `frame()` only guarantees "built and handed over".
- **The completion notification is a report, not a callback** (`G10`): a frame's **present outcome** is the `PresentOutcome` value, published with its frame number through
  `RenderEngine::presentRecord()` (packed into one atomic word, readable from any thread at any time). The engine thread **never** calls back into the UI, and never
  reads back from it. To "wait for frame N to be on screen" call `waitForFrame(N)`; to know "what happened to this frame" read `presentRecord()`.
- **A frame is always ANSWERED, even when its engine half throws** (`G4` / `G11`): the half is handed over through `RenderEngine::answerEngineHalf`, which reports
  `engine.frame.failed` (Error) and answers `PresentOutcome::Failed` - the same `presentRecord` / `frameResult` / completion a drawn frame uses. Neither escaping shape
  is an answer: inline the exception used to reach the host out of `frame()`, and on a frame thread it left the thread function, which is `std::terminate`.
  `FrameRunner` adds one narrower guarantee that does not depend on the caller: a job that throws still releases the one-in-flight handshake (and is counted by
  `FrameRunner::failures()`), because a `has_request_` left set would block every later submit forever.
- **The size travels with the frame, and the swap chain follows at the frame head**: `RenderArea::resize` does two things only — record this area's own size,
  and hand the geometry to the presenting view — and **does not touch** the target; `frame()` hands "this frame's size" to the target after assembly and before execution,
  and **only when the number changed**. So: a host resizing mid-frame does not wait for the device, and several resizes between two frames pay one swap-chain rebuild.
- **Shutdown** (F10): `stop()` posts "no more frames" → the engine half finishes the frame in hand (or discards it) → release the present target,
  the session and the device on the engine thread → join (bounded, an error on timeout). The destruction order follows N11's rule (the thread ends before the device and the tables). The UI may now freely destroy its own
  window, because the engine side has no edge back.
- **One publication per frame**: hooks run in the content half and publications happen per pass, but the engine half's bind reads the current snapshot once at the **frame head** ⇒ all of that frame's
  passes expand against the same value. That is, **what one hook writes is visible to all passes of that frame**, not "only to the passes after it" (N16).
  If a pass must not see some content, that is a **declaration** question (give it its own scene), not a scheduling one.
- **Rejected**: D8 (move only expand and leave `executeFrame` on the UI — half a thread), D9 (run it through in place once built — that is inline),
  D10 (pipeline depth > 1), D11 (a UI-thread `onPresented` callback). Reasons in §7.
- **Acceptance**: T9 / T10, see §3.1.3, §3.1.4, plus "both run positions draw the same picture" (T12).

### 2.13 Publication is the content writer's verb, binding is an override

- **What it is**: whoever writes content (the host, `SceneView::publishContent`, an overlay's `FrameUpdate::onFrame()`) calls `Scene::publish()`;
  when a frame is built it takes `Scene::publishedSnapshot()` (or the bound value if one was bound). `RenderEngine::bindSnapshot(scene, ref)` is an
  **override**: the host uses it to draw an older or another publication, and it stays in effect until the next bind (a null handle = draw nothing).
- **Why**: publication is the content side's verb and binding is the engine side's verb (`G2`); making "which publication a frame draws" a value taken at the moment the frame starts is what lets the host say
  "this frame draws the one I just published" rather than "whatever happened to be there while it ran" (`G4`) — and that guarantee comes from the value (`PublicationStore` holds one source per
  `(content, revision)`), not from an implicit "wait first, then publish" convention.
- **Boundary**: one engine keeps one override per scene; the override is **per engine**, so two engines can each draw their own (an editor + offscreen baking).
  With no override both engines draw the same latest publication.
- **A scene that was never published**: the frame does not publish on its behalf and does not read its live content: the draw gets no source and draws nothing, and it reports once
  `ContentSkipped` ("pass 'x' draws a scene that was never published").

### 2.14 The cross-seam payload is values: three descriptions + `LightValue` + `ContentRef`

- **What it is**: content objects each have a "value half" that crosses with their identity:
  - `GeometryDescription` (shape), `MaterialDescription` (material values), `ShaderProgramDescription` (program text + id + revision,
    new); all three are immutable values **cached by revision** (`X::description()`).
  - `ContentRef` (`id + revision`) is the fact tables' key: a `DrawItem` carries `geometry_ref` / `material_ref`, and `program_description`
    carries its own id+revision.
  - `LightValue` (new) is the light's value half; when a pass describes itself (`RenderPass::describe` / `FullscreenPass::describe`) it snapshots the scene's lights into
    `DrawPacket::light_values`, and the program override at the same time into `PassDraw::program_override_description`.
- **Why** (`G1` / `G4` / `G10`): the **only class of real UB** is the UI changing lights/material/program while the engine thread reads the same non-atomic field (R3-1).
  The value channel makes "frame N draws the value it described with" value semantics rather than a convention; and each command saves 3 `intrusive_ptr` atomic increments/decrements.
  "Make the revision atomic" fixes one field only, and neither the lights nor the program text, and is **rejected** (`D12`).
- **The diagnostic route was the second shape of that UB, and is no longer** (`G6`): a backend reports on the engine thread while the host may report its own findings at any
  time (`DiagnosticsHandle::report`), so the route's counters are relaxed atomics and its sink is copied out under a lock and invoked with none held. A sink may therefore be
  entered by two threads at once: one that touches shared state has to serialise it (§2.4).
- **Boundary**: the lights' live-pointer channel is gone: `DrawPacket::lights` and `FrameRecorder`'s object-shaped
  `setLights` are both gone, and `light_values` is the only channel. The live pointers that remain are `DrawItem::geometry/material/program`
  and `PassDraw::program_override`: they carry **identity and lifetime** (stream binding needs objects, residency accounting needs shares, `releaseContent` needs to ask "who else
  holds it"), and the recording side reads only the **invariant** (id) from them — revision / isTransparent / light values all come from values. Deleting them would need a
  content registration channel (the trigger of T30/T22), and there is no consumer today (`G8`).
  Program/material values are re-taken at the publication boundary through `detail::refreshValues` (they do not go through node mutators, and the epoch cannot see them).

### 2.15 Publication and expansion are two halves, and the frame value carries only the publication (`G2` / `G12`)

- **What it is**: `detail::PublicationStore` holds the table "one `SnapshotSource` per `(content, publication)`" and its lock,
  and the verbs are all on the content side — `bindSnapshot` / `publicationFor` / `sourceFor` (plus the table-level `beginFrame`, the engine half's single call).
  The frame's binding and expansion are in `detail::FrameExpand`: `bindSceneDraws` points each scene draw at its publication source, and `expandFrame`
  reads values only and collects per view.
- **Why one source per publication**: a source is one immutable value's `DrawListSource` face. A new publication gets a new source, so a frame already built keeps drawing
  the one it took, and a publication landing mid-frame cannot change its picture; an unchanged publication reuses the same source, so a static scene's per-view memo survives as usual.
  A source is named by only one or two frames, and `beginFrame` drops the ones nobody draws any more (there is one in-flight frame ⇒ at most two per content).
- **Why**: publication (content side) is divided from bind + expand (engine side) into `PublicationStore` / `FrameExpander`, so there is no back edge
  "the content half must know the engine half's objects". Binding belongs to the content side as well: `RenderPass::describe` writes only the VIEW,
  `buildFrameValue` binds after describe (`detail::bindSceneDraws`), so: the content side owns publication **and** binding, the engine side only
  expands values, and the frame value carries no live content across threads (`G2`); `Scene` is no longer a `DrawListSource` either (`G12`: the author tree is not the frame's source).
- **Boundary**: what was split is objects, not threads. `FrameRunner` still hands only the engine half's expansion to the engine thread; `bindSnapshot` must still happen
  between frames (`PublicationStore`'s class comment pins this invariant), so frames may not overlap before "publication creation" enters the handshake.
- **Publication granularity**: the host/chain runs the overlays' `update()` (`FrameUpdate`) **before** the frame, and `buildFrameValue` takes the publication "once per frame per scene" and then describes. The same
  scene's N passes therefore publish once; and "describe first, publish after" would put two instants inside one frame, so the update and the publication must finish before the frame
  (that is how N16's "all passes see the same one" holds).
- **A scene that was never published**: the frame does not publish on its behalf and does not read its live content — the draw gets no source and expansion collects nothing (the backend records nothing),
  which is the honest answer to "you handed over no picture".
- **Acceptance**: `T34` / `T17` / `FrameRunnerTest.ManyFramesThroughTheEngineThreadStayEquivalent`;
  `FrameExpandTest.ADrawNoPublicationAnswersDrawsNothing` pins "no publication, nothing drawn".

### 2.16 The three faces of a shared device: content shared, targets not shared, frames concurrent per session (`G1` / `G4`)

- **What it is** (today's three facts, each with something checkable behind it):
  - **content sharing already holds**: one `RenderDevice` can be borrowed by several `RenderEngine`s (`setDevice`'s contract: the engine does not
    `initialize` / does not `shutdown` the device), and residency is cached by **identity** (geometry = `GeometryDescription`'s address + revision),
    so two engines drawing the same geometry is **the same key**. `Geometry::description()`'s lazy cache is safe for concurrent readers (a shared lock;
    found and fixed by TSan).
  - **targets are not shared**: one window, one target (`createSurfaceTarget`'s `WindowOccupied`), and an image has only
    one layout at any instant; an engine's transient is owned by compile and private by nature. Offscreen targets are governed by a **target claim**: one holder per frame,
    and a conflict only skips that pass and reports once.
  - **frames are concurrent per session**: a frame's machinery (command pool / fence / constant ring / descriptor pool / frame gate)
    is **session-level**, and different sessions' frame bodies **really do run at the same time** (with one stopped by a diagnostic sink, another driver's frame completes whole);
    two frames of the same session are still serialised by its `FrameGate` (another thread waits, a same-thread reentry is refused and reported once). Device-level shared pieces each have their own lock
    (the cache is taken only at the frame head; the allocator / target map / clock / report sink have one each), and a frame outside a window takes the body gate exclusively; a host verb is
    **the device's promise**: either wait (and count it in `deviceWaits()`, as readback does), or, under the park rule, not wait for a frame (release, `R6`).
- **Why**: the content cache has an "uploading" state (`ResidencyClaims`'s three states; see `graphics-sessions.md` §9.2), and the cache lock already serialises
  concurrent uploads, so "concurrency" has a definition: two engines building the same content for the first time at once **serialise** on the cache lock and do not
  get two identities. The remaining shared pieces are each protected by their own lock.
- **Boundary**: appfw does not use sharing yet (`SurfaceWindow` builds one device per window, and `docs/guides/render-views.md` §7 records
  "later"); the case `OneDeviceServesSeveralAreasWithoutCrossingThem` proves **sharing** (two frames run one after the other), and
  `VkMultiSessionTest`'s two cases are what prove **concurrency** (two threads with N frames each; with one frame stopped the other completes whole).
- **The shipped backend**: `vk` keeps the frame machinery on the session/driver (collection staging, frame-head snapshot, the device gate's two sides, `FrameHead` value plans,
  a lock-free recording path), and wires `ResidencyClaims` into the frame (F4) — `ContentCaches::ensureGeometry` asks the table first, and the frame-scoped `FrameBuilds`
  publishes after the frame fence waits. The decision and its argument are in `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`, and the shape of the
  implementation is in `graphics-sessions.md` (types, lock table, state machines).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Establishing the two worlds | the host builds the content; the engine half gets that content's published snapshot at the frame head | Landed |
| F2 | The publication protocol | one publication = one pointer swap; a three-state answer + a snapshot handle | Landed (5b-2a) |
| F3 | Scene-graph structure updates | attach / detach / reparent / root swap **need no notification at all**: the next publication's content is the new structure | Landed |
| F4 | Node state updates | placement / opacity / layers / drawOrder fold into the next publication; hidden = no row produced | Landed |
| F5 | Geometry, material and program value updates | rows carry `(id, revision)` and values; an old frame reads old values | Landed |
| F6 | Camera updates | the camera is content and an event takes effect at once; a query uses the snapshot of the frame that was answered (N17) | Landed |
| F7 | Byte content and streaming | `Buffer::edit()` only advances the revision (the revision is atomic; writers release, readers acquire); the engine side polls through residency entries (see §2.4) | Landed |
| F8 | Per frame | content half: hooks → publication → describe; engine half: bind → expand → execute → present → report | Landed |
| F9 | Surface events and the swap chain | the size is the frame's own value; device verbs are between-frame operations; the present outcome is a value | Landed |
| F10 | Shutdown and teardown | stop frames → the engine half finishes → the engine thread releases → join | Landed |
| F11 | Parallel select | the derivation layer goes parallel per chunk, bit-identical to serial | Not done (a pure function is its precondition) |
| F12 | `inline` mode | no thread is built; the "engine thread" is the calling thread | Landed (default) |
| F13 | Violations are diagnosable | publication is the only mutable → immutable conversion; pure derivation + item-by-item diff cases are the criterion | Landed (T34 / T17) |

### 3.1.1 F1 / F2 / F3 / F4 / F5: publication is the content side's "update protocol"

- **Behaviour**: the host edits content (structure, placement, state, geometry, material) and then calls `publish()`. One publication turns the
  new structure, the new folded values, the new material values and the light set into one new snapshot together; unchanged chunks are shared with the new values, and unchanged rows are not copied.
- **Constraint** (C18 / C19): no publication inside a batch; material crosses the frame as a **value**.
- **Requirement** (R9 / R13 / R17): one swap, an in-flight frame unchanged item by item, a batch = one publication.
- **Acceptance** (T3 / T17 / T31 / T34): `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture`,
  `FrameExpandTest.AFrameExpandsTheSamePictureTheSceneWould`,
  `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch`,
  `SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith`,
  `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames`.
- **Caveat** (§2.4): the byte revision only answers "the bytes changed", not "where to read them"; changes of shape and values go through publication (§2.14).

### 3.1.2 F8 Per frame

- **Behaviour**: the host calls `update()` first (`SceneView::update` lets the chain's overlays each update, `FrameUpdate`) and then `publish()` (`SceneView::publishContent` publishes the view and the chain-owned scenes); the content half
  (`RenderEngine::buildContentHalf`) only describes each pass in run order,
  describes a pass into a `FrameDescription`, and binds each scene draw to the publication at the moment the frame started; the engine half (`runEngineHalf`)
  expands the commands, hands them to the executor to execute, and presents.
- **Constraint** (C16): one thread per half; one engine thread touches the device.
- **Requirement** (R16): on the content thread `frame()` costs only O(passes) plus one handshake; waiting must be explicit.
- **Acceptance** (T12 / T16): `FrameRunnerTest.ManyFramesThroughTheEngineThreadStayEquivalent` (64 frames identical item by item),
  `HeadlessBackendTest.PrepareHandsOverTheSameFrameTheNextFrameRunsAndRunsNothing`.
- **Caveat** (N15): the picture is one frame behind the content.

### 3.1.3 F9 Surface events and the swap chain

- **Behaviour**: the size is **the frame's own value** (handed to the target at the frame head, only when it changes); **device verbs (`createSurfaceTarget` /
  `setWindowHandle` / `device->initialize`) are between-frame operations**, and the host calls `waitForFrame()` first; each frame's present outcome is
  the `PresentOutcome{Presented, Skipped, Lost}` value (`RenderEngine::presentRecord()`); `Lost` is repaired by **the engine** —
  forget the announced size, re-announce it on the next frame, and the target rebuilds the swap chain accordingly (counting one device wait); **the host** only re-attaches when the handle really changed/became null,
  and does **not** change the area's state because of one lost picture (hiding would stop the repair frame).
- **Constraint** (C17): present and swap-chain rebuild happen on the engine thread only; the UI holds no swap-chain handle and writes no engine-side state.
- **Requirement** (R9 / R16): a rebuild only at the frame head and only with a size change; a device refusal skips this frame without counting an error; `deviceWaits` and the skip reason are visible;
  a UI call does not block on present or a swap-chain rebuild.
- **Acceptance** (T9): `RenderEngineTest.TheAreasSizeAnnouncedBeforeInitializeReachesTheBackend`,
  `RenderEngineTest.TheFramesPictureOutcomeIsAFactOfTheFrame`.
- **Caveat** (§6.3): the Wayland fallback for "no extent until mapped" is still decided on the UI side (`appfw-render-surface.md` F5); also N15.

### 3.1.4 F10 Shutdown and teardown

- **Behaviour**: `stop()`: post "no more frames" → the engine half finishes the frame in hand (or discards it) → release the present target /
  session / device on the **engine thread** → the thread exits → join (bounded, an error on timeout).
- **Constraint**: the engine thread **must not** wait synchronously for the UI thread (or it deadlocks against "the UI must not wait for a worker").
- **Requirement** (R10): everything the engine side allocated is released after `stop()` (the ASan criterion); unprocessed frames are observable.
- **Acceptance** (T10): `stop()` leaks nothing; a repeated `setFrameThreading` is stable.
- **Caveat**: the destruction order: the engine thread must end before the device and the tables.

### 3.1.5 F11 / F12 / F13

- **F11 parallel select**: the candidate is the derivation layer's "select" step (pure values); the threshold starts at 2000 drawables (R11). Acceptance: for one snapshot and one view,
  the command sequences at P=1 and P=N are equal element by element (`SelectTest.SameInputSameList`, not done).
- **F12 `inline`**: the default; `frame()` runs both halves in place and answers `Ran`. The existing cases and the headless path use it without changing a line (R12).
- **F13 violations are diagnosable**: this layer now has only one "mutable → immutable" boundary (publication), so the criterion is **value equality**: the published source and a direct collection agree item by item across
  600 frames (T34), and an old frame reads old values (T17). A missed publication shows up as "the picture freezes on the old image", which T34 catches.

## 4. Public Interface

### 4.1 The content side (`vn::graphics`)

| Interface | Contract | Notes |
|---|---|---|
| `Scene::publish() → ScenePublishResult` | one publication: `Posted` / `Unchanged` / `InsideBatch`, the answer carrying that `SceneSnapshotRef` | `Scene.hpp` |
| `ScenePublishResult::snapshot` | the one that is the answer after this call (inside a batch = the one at the batch's start) | same |
| `Scene::beginEdit()` / `endEdit()` | the batch boundaries; only the outermost releases a publication | exists |
| `Node::id()` / `visible` / the state setters | the content editing API, all of it on the content thread | exists |

### 4.2 The engine side (`vn::graphics`)

| Interface | Contract | Notes |
|---|---|---|
| `Scene::publishedSnapshot()` | the handle of the scene's latest publication: what a frame build takes by default | new |
| `RenderEngine::bindSnapshot(scene, ref)` | **override**: this engine draws `ref` instead (until the next bind) | new; it is only an override, not a required step |
| `SceneView::publishContent()` / `PassChain::publishContent()` | the host's once-per-frame publication (the view + the chain-owned scenes) | new |
| `RenderEngine::frame() → FrameCallOutcome` | build + hand over (`Submitted`) or run in place (`Ran`); `NotReady` + `EngineNotReadyReason` means nothing is driving it | exists |
| `RenderEngine::setFrameThreading(FrameThreading)` | `Inline` (default) / `EngineThread`; decides the lifetime | exists |
| `RenderEngine::waitForFrame(n, timeout)` | the only explicit wait | exists |
| `RenderEngine::presentRecord()` / `presentedCamera()` | the previous frame's present outcome / that frame's camera snapshot (N17); both are the two halves of `frameResult()` | exists |
| `RenderEngine::frameResult() → FrameResult` | every fact of the previous frame: `query` (snapshot / view) + passes / draws / diagnostics / present | new (F12) |
| `RenderEngine::prepare()` | a warm-up that builds without submitting (see `graphics-frame-pipeline.md` F7) | exists |

### 4.3 What does not change

- The content editing API (`Scene` / `Node` / `Geometry` / `Material`) is unchanged, and every call is synchronous on the content thread.
- `DrawListSource` ("what this view draws") is unchanged: `SnapshotSource` is the production implementation (a frame only draws publications), and a host may implement it too;
  `Scene` is not its implementation — `Scene::collectDrawItems` is the content thread's query, answering what the current publication draws for one view,
  and it shares the one `detail::selectSnapshot` rule with the frame's source (not a second walk).

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

**A note on numbering**: `C1–C15` belonged to the mirror / delta generation of shapes and are **retired** with the 5b-2b deletion, and their numbers are not reused (old references to `C8` / `C12` /
`C14` / `C15` in code mean the history). The current constraints are:

| # | Constraint |
|---|---|
| C16 | One frame's two halves go to one thread each: the content half (hooks / publication / describe) on the content thread, the engine half (bind / expand / execute / present) on the engine thread; the frame value rotates between two slots, with one frame in flight per side. |
| C17 | A surface event is applied once at the frame head; the swap chain is rebuilt on the engine thread only; the UI side writes no engine-side state. |
| C18 | The version does not advance inside a batch: after `beginEdit()` and before the outermost `endEdit()`, publication stays where it is — a frame landing mid-batch **publishes nothing** and draws the state at the batch's start; the whole batch lands as **one** publication on the frame after `endEdit()`. |
| C19 | Material crosses the frame as a **value**: a command carries a `MaterialDescriptionPtr` (`revision` + resolved parameters), and the engine side reads the value only, not `Material::resolved()`. The values are taken on the content side, and not inside a batch. |
| C20 | The session shape (pass set, resource tables, backend retained state) changes **only between frames**: with a frame in flight, `applyPlan` (in any form) always answers `PlanResult::Refused` + `PlanRefusal::Busy` (`sessionChangeAllowed`), and reports **one** `PassProtocolViolation` per episode. |
| C21 | **One device can be borrowed by several engines, and frames are concurrent per session**: content residency is shared by identity (one upload, one compile); one window has one target; a frame's machinery (command pool / fence / constant ring / descriptor pool) is **session-level**, so different sessions' frame bodies can run at the same time. Two frames of the same session are still serialised by its `FrameGate` / frame claim: another thread **waits**, and a same-thread reentry is **refused** and reported once (`PresentOutcome::Refused` / `PrepareReport::Outcome::Refused`). Device-level shared pieces each have their own lock (the cache is taken only at the frame head; the allocator / target map / clock / report sink); a frame outside a window takes the body gate exclusively, and a host verb is **the device's promise** (wait and count, or under `R6` park without waiting for a frame); **a host verb called from a frame-body thread is refused rather than waiting for itself** (`vk.device.reentry`; a readback inside a frame answers `InsideFrame`), and the refusal comes from **one type**: `graphics::FrameGate`'s shared/exclusive sides plus a per-thread nested-gate stack. Shared content's `Geometry::description()` cache is safe for concurrent readers (found and fixed by TSan); shape editing is still the content thread's action. **Every frame also reports what it spent on the content lock** (`FrameReport::locking`: `acquisitions` is this frame's own phase count, `waits` how many of them really waited — the steady state is a constant with `waits` 0, measured the same way every run). |
### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R8 | A UI call does not block waiting for present; the only thing it ever waits for is "the previous frame's engine half", and "this frame is on screen" requires an explicit `waitForFrame`. |
| R9 | A publication is one atomic pointer swap costing O(changed chunks + changed material values). |
| R10 | No leak on the engine side after `stop()`. |
| R11 | Small scenes do not go parallel (the threshold starts at 2000 drawables). |
| R12 | The existing cases and the headless path change not one line under `inline`. |
| R13 | An in-flight frame reads old values: editing material / geometry / program changes nothing of the in-flight frame's content item by item. |
| R15 | When a robot's 28~70 joints change every frame, the engine side recomputes only in proportion to the chain, with zero allocation per frame. |
| R16 | After threading, `frame()` costs only O(passes) (build + hand over) plus one handshake; expand and present are off the caller's critical path. |
| R17 | One simulation step / one assembly = **one publication**, not one per joint. |
| R18 | Concurrent frames on a shared device must be **provable**: (a) two sessions' frame bodies really do overlap in time (with one stopped by a diagnostic sink the other completes whole, `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame`); (b) **the recording path is lock-free** (`contentCacheCounters().cache_lock_acquisitions` is a fixed 4 per frame and does not grow with the draw count); (c) **TSan evidence**: `scripts/tsan_check.sh` prints **0 race reports** per suite (the 2026-11-17 reading: `test_vk` 169/169 and `test_graphics` 617/617, 0 reports; the third suite then present has since retired). |

**The three commands of R18(c)** (rerun after every concurrency change; the numbers move with the case count):

```bash
VN_TSAN_TARGET=test_vk  scripts/tsan_check.sh
scripts/tsan_check.sh                                   # test_graphics
VN_TSAN_TARGET=test_vk VN_TSAN_FILTER='-*Window*:*Probe*:*Plugin*' scripts/tsan_check.sh
```

### 5.3 Invariants (N#)

**A note on numbering**: `N1–N14` belonged to the mirror generation of shapes and are retired; N15–N17 are still current (code refers to them).

| # | Invariant | Which bug it kills |
|---|---|---|
| N15 | After threading the picture is one frame behind the content (the engine half runs the previous frame); `waitForFrame` is the only way to "wait for that frame". | taking "submitted" for "on screen" |
| N16 | Within one frame there is only one publication: what a hook writes is visible to all of that frame's passes. | half-new, half-old inside a frame |
| N17 | The camera is on the content side and an event takes effect at once, so the picture is the previous frame's camera snapshot. Therefore **a query that turns a pointer into a world point (picking, anchors) uses the camera of "the frame the user saw"**: `RenderEngine::frameResult().query` (`view` + the `snapshot` it draws) publishes it, `presentedCamera()` is shorthand for its `view`; it reaches the manipulator through `SceneView::setPresentedCamera`; with no published value it falls back to the current camera. | at a low frame rate the pivot/selection is one frame off |
| N18 | The default content program belongs to **the session** (replacing "one device-level slot"): when two engines declare different default programs each shades with its own (`VkMultiSessionTest.TwoDriversShadeWithTheirOwnDefaultProgram`); diagnostic routing was already multi-way (each engine gets its own copy, `setDiagnosticsRoute`). | the second engine quietly changes the first one's shading |

## 6. Caveats and Traps

### 6.1 P1 Taking submitted for on screen

- How it fails: `frame()` returning is taken to mean the picture already shows this frame; a screenshot/measurement reads the previous one.
- How to detect it: comparing `presentRecord().frame` with `frameNumber()`.
- How to prevent it: to get "this frame on screen" call `waitForFrame(n)` (R8 / N15).

### 6.2 P2 Changing the session shape mid-frame

- How it fails: the pass table and the backend's retained state are swapped during recording, and commands reference released objects.
- How to detect it: `applyPlan` answers `PlanResult::refused()` + `PlanRefusal::Busy` (C20; it means "a frame is recording in the engine", and the SDK has no `Session` type).
- How to prevent it: the shape changes only between frames (`graphics-frame-pipeline.md` F6).

### 6.3 P3 Doing device verbs mid-frame

- How it fails: `createSurfaceTarget` / `setWindowHandle` / `initialize` change the session the engine thread is recording.
- How to detect it: TSan, or a crash that reproduces.
- How to prevent it: the host calls `waitForFrame()` first; the size reaches the frame only as a value through `RenderArea::resize` (§2.6).

### 6.4 P4 Treating a batch as a transaction

- How it fails: the caller expects a failure inside the batch to roll back, whereas the batch only delimits visibility.
- How to detect it: the semantics of T31.
- How to prevent it: the documentation says "a batch is not a transaction" (`Scene::beginEdit`'s comment).

### 6.5 P5 Querying with the live camera

- How it fails: the camera moves between frames, and the query answers the previous frame's picture with the new camera.
- How to detect it: T20 (`graphics-frame-pipeline.md`).
- How to prevent it: the query takes `presentedCamera()` (N17).

### 6.6 P6 Requesting a frame inside `FrameUpdate::onFrame()`

- How it fails: the update before a frame asks for one more frame, reentering compilation.
- How to detect it: non-consecutive frame numbers / a crash.
- How to prevent it: update writes content only and does not drive the engine (F8).

### 6.7 P7 Putting parallelism into the engine half

- How it fails: two threads touch one device's shared state at the same time.
- How to detect it: TSan.
- How to prevent it: parallelism only over pure values in the derivation layer (§2.7 / C16).

### 6.8 P8 `AllocationGate` and the parallel section

- How it fails: the parallel section's allocations are counted inside the "zero allocation per frame" window, and the gate turns into a false positive.
- How to detect it: the gate cases start failing once parallelism lands.
- How to prevent it: the parallel section stays out of `AllocationGate`'s window (F11).

### 6.9 P9 Putting `*collectDrawItems(...)` straight into a range-for

- How it fails: the temporary `shared_ptr` is destroyed when the loop ends while the loop body holds a reference inside it — dangling.
- How to detect it: ASan / random crashes.
- How to prevent it: store the answer in a named variable first.

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | Making content editing asynchronous (submitting to a queue) | a document editor's core feel is synchronous; it pushes the complexity onto every user (`G7`) | ADR-2026-11-12 |
| D2 | Locking the scene graph | locks enter every read path and conflict with zero allocation in the steady state (`G1` / `G10`) | ADR-2026-11-12 |
| D3 | A per-frame extract / sync | an O(drawables) copy every frame, leaving the cost on the content thread (`G10`) | ADR-2026-11-12 |
| D7 | A persistent mirror + delta channel | it was once this file's position; the root of five "when does this copy expire" answers, deleted at 5b-2b (`G1`) | ADR-2026-11-12 |
| D8 | Moving only expand and leaving `executeFrame` on the UI | half a thread: the UI still pays present's wait (`G3`) | ADR-2026-11-12 |
| D9 | Running it through in place once built | that is `inline`, and there is no parallelism to speak of | ADR-2026-11-12 |
| D10 | Pipeline depth > 1 | one more in-flight frame buys memory and latency with no measured gain (`G8`) | ADR-2026-11-12 |
| D11 | A UI-thread `onPresented` callback | the engine thread reads back into the UI, and the two writers meet again (`G2`) | ADR-2026-11-12 |
| D12 | A surface-event channel | one frame in flight + carried values already deliver every value the engine half needs (`G8`) | ADR-2026-11-12 |
| D13 | A global lock around every device verb | it turns the complexity into "every call may block" (`G7`), and the lock's granularity is unrelated to the resource (`G1`: treating "thread safety" as one concept) | `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` |

## 8. Tests and Acceptance

| # | Criterion | Case / command | Status |
|---|---|---|---|
| T3 | a batch = one publication | `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture` + `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` | implemented |
| T9 | size and present outcome | `RenderEngineTest.TheAreasSizeAnnouncedBeforeInitializeReachesTheBackend` / `TheFramesPictureOutcomeIsAFactOfTheFrame` | implemented |
| T10 | the shutdown phase | `FrameRunnerTest`'s stop / restart cases + an ASan run | implemented |
| T12 | both run positions draw the same picture | `FrameRunnerTest.ManyFramesThroughTheEngineThreadStayEquivalent` (64 frames identical item by item) | implemented |
| T16 | `frame()` costs only O(passes) | `FrameRunnerTest`'s inline / threaded count assertions | implemented |
| T17 | an old frame reads the old revision | `SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith` | implemented |
| T31 | publish inside a batch | see T3 (the same set of cases) | implemented |
| T34 | the published source equals a direct collection item by item | `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames` + `FrameExpandTest.AFrameExpandsTheSamePictureTheSceneWould` | implemented |
| T35 | a bound publication is the rendering truth | `FrameExpandTest.ABoundPublicationIsThePictureTheFrameDraws` | implemented |
| T13 | no data races | `scripts/tsan_check.sh` | run (2026-11-16: the then-present suites each 0 reports) |

## 9. Status and Evolution

### 9.1 Current status

- **Landed**: the two worlds and publication (F1–F7, F12), the frame's two halves and the engine thread (`setFrameThreading` / `waitForFrame` /
  `presentRecord` / `presentedCamera`), surface and swap-chain ownership (F9), the shutdown phase (F10), the host taking over publication (§2.13).
- **Not done**: F11 (derivation-layer parallelism) — it is the natural extension of a pure function, and it is not built before there is a consumer (`G8`).
- **Publication is never implicit inside a frame**: publication is the content writer's verb, and a frame takes the scene's value at build time;
  a scene that was never published is not drawn and reports once `ContentSkipped` (§2.13). `bindSnapshot` has only the meaning "override".

### 9.2 Known gaps

- **Derivation-layer parallelism** (F11): done only when measurement shows "the select step takes more than the budget of a frame".
- **The public face has no readback of "which publication this frame drew"**: `FrameResult`'s target shape (`graphics-frame-pipeline.md` F12) will add
  `snapshot` / `view`; the present equivalent is `presentRecord().frame` plus the host's own `SceneSnapshotRef`.

### 9.3 Compatibility / migration

This file describes the **present**, not the target. The target shape and its remaining work are in `graphics-frame-pipeline.md` §9.3 and `../backlog/graphics.md`.

## 10. References

### 10.1 Code

- `src/viz/graphics/sdk/vine/graphics/Scene.hpp` / `src/viz/graphics/src/Scene.cpp` (`publish` / batches / the published value)
- `src/viz/graphics/sdk/vine/graphics/SceneSnapshotRef.hpp` / `src/viz/graphics/src/SceneSnapshotRef.cpp` (the snapshot handle)
- `src/viz/graphics/src/SceneSnapshot.{hpp,cpp}` (the snapshot value, chunking, `selectSnapshot`)
- `src/viz/graphics/src/ScenePublisher.{hpp,cpp}` (the scene's derivation side: baked rows, per-view memo, the published value)
- `src/viz/graphics/src/SceneRows.{hpp,cpp}` (`SceneBake` → the single gate into a snapshot)
- `src/viz/graphics/src/SnapshotSource.{hpp,cpp}` (the snapshot's `DrawListSource` face)
- `src/viz/graphics/src/PublicationStore.{hpp,cpp}` (the content side's publication/binding and the `SnapshotSource` table)
- `src/viz/graphics/src/FrameExpand.{hpp,cpp}` (`bindSceneDraws` / `expandPass` / `expandFrame`)
- `src/viz/graphics/src/FrameRunner.{hpp,cpp}` (the engine thread)
- `src/viz/graphics/src/EngineCompile.cpp` (the content half: hooks / publication / describe / the engine half's entry)
- `src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp` (`bindSnapshot` / `setFrameThreading` / `waitForFrame` /
  `presentRecord` / `presentedCamera`)
- `src/viz/graphics/src/DrawListMemo.{hpp,cpp}` (per-view list reuse and its two atomic counters)
- `src/fw/appfw/src/gui/SurfaceWindow.cpp` (the host's two run positions and the attach timing)

### 10.2 Cases

- `tests/test_graphics/PublicationStoreTest.cpp` (T3 / T31's three-state publication)
- `tests/test_graphics/FrameExpandTest.cpp` (T34 / T35: the frame-level seam, a bound publication is the truth, no publication draws nothing)
- `tests/test_graphics/FrameRunnerTest.cpp` (T10 / T12 / T16)
- `tests/test_graphics/SnapshotShadowTest.cpp` (T3 / T17 / T34)
- `tests/test_graphics/HeadlessBackendTest.cpp` (the two halves and the warm-up)
- `tests/test_graphics/GraphicsTest.cpp` (memo reuse and T9's surface size)

### 10.3 Documents and ADRs

- `../decisions/ADR-2026-11-12-adopt-graphics-next.md` (four layers / three verbs / invariants; this file is its threading layer)
- `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md` (the split and order of the snapshot taking over collection)
- `../decisions/ADR-2026-11-10-render-world-mirrors-content.md` (**history**: the original mirror-shape decision, superseded by the two above)
- `graphics-frame-pipeline.md` (the target shape and the step-by-step migration in §9.3)
- `vk-backend.md` (backend implementation and synchronisation details)
- `appfw-render-surface.md` (which half the area and the window belong to)

### 10.4 External sources

- Qt Quick's `sync()` / Unreal's `ENQUEUE_RENDER_COMMAND` (compared in §2.9; this repository brings in no third party).
