# graphics threading model (current contract)

> **Status**: current. The mirror / delta / three channels / memo+epoch generation of shapes is gone, and the only cross-thread payload left is
> "an immutable snapshot produced by one publication" and "two atomic revisions". The history (why it used to be that way, which option was rejected) is in
> `../decisions/ADR-2026-11-12-adopt-graphics-next.md`, `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`
> and git history; the `C1–C15` and `N1–N14` entries that only served the deleted mechanism are retired, and their numbers are not reused (see §5.1, §5.3).
> **Last verified**: 2026-11-27
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
  the revision". `Geometry` / `ShaderProgram` / `Material` revisions stay plain numbers: the frame side reads the published description value
  (`GeometryDescription` / `ShaderProgramDescription` / `MaterialDescription`), not the live object — and the one verb another thread does reach,
  `X::description()`, measures that number inside `detail::DescriptionCache`'s lock, which is the C22 rule (§2.16).
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
- **Boundary**: a device verb is **the device's promise, not the host's duty** (`RenderDevice`'s class note): `createSurfaceTarget`,
  `setRenderTarget`, `createFrameDriver` / `releaseFrameDriver`, `shutdown` and the readbacks take the device's gate and WAIT for the frames in flight,
  and a call from inside a frame body is refused (`vk.device.reentry`); `releaseRenderTarget` / `releaseContent` park instead of waiting (`R6`).
  Two duties stay with the host, each because the framework has no object to gate: `device->initialize()` is the creator's and happens before any frame,
  and a window target's `setWindowHandle` belongs to the surface the host owns (call it between frames - `waitForFrame()` first, P3).
  `RenderEngine::setDevice` waits on its own before it hands the old driver back, because that driver is what the engine half is executing through - the rule
  `applyPlan` (refuse while busy) and `setFrameThreading` (refuse while the interval is held, 2.17) already followed.

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
- **A frame is being RECORDED from its first compile until its answer, and the pump owns that phase** (`G1` / `G4`): the
  content half (hooks, publication, `compileFrame`) and the engine half are one interval, so the session's shape is fixed
  for all of it - inline as well as threaded. `detail::FramePump` keeps the two facts (`compiling`, owned by the guard a
  build returns; `answering`, owned by the frame's answer) and answers `recording()`; a shape change
  (`RenderEngine::beginShapeChange`) holds `FramePump::phaseGate()` across its DECISION and its CHANGE, which is what
  stops a second thread from applying a plan into a compile that started after the check. The earlier predicate asked the
  runner, which is false inline and false through the whole content half, so the gate was open exactly where the compile
  reads the table (ADR-2026-11-26-a-frame-is-recorded-from-its-first-compile).
- **The frame's numbers are the frame's answer, and none of them is read live from a writer** (`G1` / `G7`): the pass
  costs ride the frame value and are published with the answer (ADR-2026-11-26-a-pass-cost-belongs-to-the-frame), and the
  backend's `FrameReport` is the same shape - the frame body writes the books it owns, `closeFrameBooks` publishes them, and
  `frameReport()` reads the published value (ADR-2026-11-26-the-frame-report-is-the-frames-answer). So a host may read
  "how did it go" from any thread at any time; `waitForFrame` decides WHICH frame those numbers describe, never whether
  they are safe to read.- **A pass' cost is a fact of the FRAME, and it travels with its answer** (`G1` / `G4` / `G10`): the content half times its
  describe share, the engine half its expansion, and both land on the frame value (`detail::PassCost`) - `publishFrameAnswer`
  hands them to the pump under the same lock as the rest of the answer. So `frameTable()` read right after
  `waitForFrame(N)` reports N's whole number, a frame still in flight publishes nothing, a `prepare()` cannot rewrite what a
  frame that ran cost, and the read joins costs to rows BY PASS IDENTITY (a plan change between the answer and the read
  hands no pass a neighbour's number). The engine half never writes the session's pass table, and the two threading modes
  publish at the same moment - the old "engine share lands at the next frame's start" shape broke all five
  (ADR-2026-11-26-a-pass-cost-belongs-to-the-frame).
- **The completion notification is a report, not a callback** (`G10`): a frame's **present outcome** is the `PresentOutcome` value, published with its frame number through
  `RenderEngine::presentRecord()` (packed into one atomic word, readable from any thread at any time). The engine thread **never** calls back into the UI, and never
  reads back from it. To "wait for frame N to be on screen" call `waitForFrame(N)`; to know "what happened to this frame" read `presentRecord()`.- **A frame is always ANSWERED, even when its engine half throws** (`G4` / `G11`): the half is handed over through `RenderEngine::answerEngineHalf`, which reports
  `engine.frame.failed` (Error) and answers `PresentOutcome::Failed` - the same `presentRecord` / `frameResult` / completion a drawn frame uses. Neither escaping shape
  is an answer: inline the exception used to reach the host out of `frame()`, and on a frame thread it left the thread function, which is `std::terminate`.
  `FrameRunner` adds one narrower guarantee that does not depend on the caller: a job that throws still releases the one-in-flight handshake (and is counted by
  `FrameRunner::failures()`), because a `has_request_` left set would block every later submit forever. The report travels through the host's sink, so the answer
  is published even when that sink throws: the route counts before it calls the sink, and the publication does not depend on the callback returning (a throwing
  sink is the host's to keep - `RenderEngine::setDiagnosticSink`).
- **The size travels with the frame, and the swap chain follows at the frame head**: `RenderArea::resize` does two things only — record this area's own size,
  and hand the geometry to the presenting view — and **does not touch** the target; `frame()` hands "this frame's size" to the target after assembly and before execution,
  and **only when the number changed**. So: a host resizing mid-frame does not wait for the device, and several resizes between two frames pay one swap-chain rebuild.
- **Shutdown** (F10): `stop()` posts "no more frames" → the engine half finishes the frame in hand (or discards it) → release the present target,
  the session and the device on the engine thread → join (bounded, an error on timeout). The destruction order follows N11's rule (the thread ends before the device and the tables). The UI may now freely destroy its own
  window, because the engine side has no edge back. The call itself is an INTERVAL ENTRY (§2.20 / C25): from a frame body it is refused, not
  half applied - the frame that is inside the engine is using what the teardown releases.
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
    new); all three are immutable values **cached by revision** through ONE type (`detail::DescriptionCache`, C22: several threads
    describing one object share one value).
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
    so two engines drawing the same geometry is **the same key**. **Every content description is safe for concurrent readers through one cache
    type** (`detail::DescriptionCache`, C22): several threads may describe one geometry, material or program at once and all get the same value —
    a rule the tree used to spell three times, twice without a lock (review 19 `Z7`).
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
- **A host verb may run mid-frame exactly when it is a VALUE or a LOCKED TABLE** (`G4` / C23): `RenderEngine::setDefaultContentProgram` is not gated -
  it takes the program's DESCRIPTION on the caller's thread and the backend reads that value under its own lock (`program_mutex`) - and the output table
  (`publish` / `unpublish` / `resolve` / `transientTarget`) is locked inside `detail::FrameResources`, with the frame head taking the bindings as a value
  (§2.18). Both are the same shape as `releaseContent` (which parks instead of waiting): a verb whose answer is "this is the value from now on" mutates
  nothing a frame is walking. Every device-facing verb that changes STRUCTURE is the gate's (see above).
- **The device's locks, and the one order between them** (`G10` / `G11`): each shared piece has its own lock, and exactly one pair nests - `queue_submit`
  before `clock_mutex` (`Device.cpp`'s two submit paths take them in that order, and nothing takes `clock_mutex` and then `queue_submit`; the fence wait,
  where the time goes, happens outside both). `TargetTable`'s `map_mutex_` and `claims_mutex_` are never held together (each verb takes one; `drain()`
  takes them in two scopes), `report_mutex` is recursive (a sink may report again) and `reentry_mutex` is a leaf taken for the ask-and-set and dropped
  before the report, and `program_mutex` / `books_mutex` / `loss_mutex`, the content cache's lock (taken only at the frame head) and the allocator's and
  the residency claims' locks are leaves. A change that needs a second lock inside one of them states the new order here, in this list, before it lands -
  that sentence is the whole defence against an ABBA, and it is cheaper than a lock-order detector.
- **The shipped backend**: `vk` keeps the frame machinery on the session/driver (collection staging, frame-head snapshot, the device gate's two sides, `FrameHead` value plans,
  a lock-free recording path), and wires `ResidencyClaims` into the frame (F4) — `ContentCaches::ensureGeometry` asks the table first, and the frame-scoped `FrameBuilds`
  publishes after the frame fence waits. The decision and its argument are in `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`, and the shape of the
  implementation is in `graphics-sessions.md` (types, lock table, state machines).

### 2.17 Entering the frame interval: an entry that cannot wait is refused (`G4` / `G6`)

- **What it is**: one interval per engine (the content half, then the answer) and per device executor (a frame body), and
  four ways a caller meets it. The rule is one question - *can the second caller be expressed as "after the current
  one"?* - and two answers:

  | Caller | Can it wait? | Answer | Mechanism |
  |---|---|---|---|
  | host device verb (`setRenderTarget`, `createSurfaceTarget`, the readbacks, `shutdown`) | yes - the body is bounded and will end | **waits** | `FrameGate::Exclusive` |
  | another `FrameDriver`'s frame body | not needed - the device serves several | **runs beside it** | `FrameGate::Shared` (`C21`) |
  | the thread that already holds the interval | no - it would wait for itself | **refused**: `EngineNotReadyReason::Reentrant`, `vk.device.reentry`, `vk.session.frame_reentry` | `FrameGate`'s empty lease / `FramePump::Build` |
  | a second HOST thread calling `frame()` / `prepare()` | no - two frames are a queue (`D12`), and the content half writes a single-writer world | **refused**: `EngineNotReadyReason::Concurrent` | `FramePump::claimInterval` |
  | `setFrameThreading` (a mode change) asked while a frame is being BUILT (its content half) | no - the switch back ends by JOINING the frame thread | **refused** by the same two reasons, reported as `engine.threading.refused_from_frame` / `refused_in_flight` | `FramePump::takeInterval` |
  | `RenderEngine::shutdown` (a teardown) | from a frame body no; a frame already ANSWERED yes - it is bounded | **refused** with `engine.shutdown.refused_from_frame` / `refused_in_flight`, nothing torn down; an answer in flight is waited for and joined | `FramePump::beginBuild` (the lease) + `FramePump::stop` (2.20) |

- **Why the last row is a refusal, not a wait**: a device body can be deferred behind another and still produce the
  same picture whenever it runs. A frame cannot: its content half is the host's own hooks and description, so waiting
  inside it is one host thread waiting for another host thread's hook, and the queue that would make that fair is the
  pipeline `D12` rejected. The peers answer the same way at their entry point - WebGPU's objects are thread-affine (an
  entry from a non-owning thread is a validation error), Unity refuses `Camera.Render` off the main thread, Godot raises
  a thread guard, D3D12's debug layer reports concurrent recording - and the peers that DO accept several producers
  (UE's `ENQUEUE_RENDER_COMMAND`, Godot's `CommandQueueMT`) accept them through a command QUEUE, which is the second
  path this design deliberately does not have (§2.3).
- **What one spelling buys**: `detail::FramePump::Build` is the engine's lease on the same interval the device gates,
  and it answers the same way - false, with a reason - so "refused, never queued" is one rule with two implementations
  instead of two policies that can drift. The two reasons stay apart (`G4`): `Reentrant` is a call from the thread that
  holds the interval (a hook or a diagnostic sink driving this engine), `Concurrent` is a call from another host thread.
  The claim is a compare-exchange, not a lock: a refused caller does not block, and two threads may still drive the
  engine in TURN - only not at once. **The turn is ordered, not merely tolerated**: the claim's release/acquire pair is
  what makes the next thread's write of the slots, of `current_` and of the frame clock read safely, which is why the
  same thread-local (`s_building_`) that names the refusal reason can be a plain pointer.
- **A MODE CHANGE IS ONE OF THOSE ENTRIES, not a bystander** (`G4` / `G6`): `setFrameThreading` replaces the fact a
  frame asks before it builds ("where does my other half run"), and switching back joins the thread an answer runs on -
  so `FramePump::setThreaded` takes the same interval (`takeInterval`) and answers the same two refusals, and the engine
  reports which one it was (`engine.threading.refused_from_frame` / `refused_in_flight`, once per episode). One
  asymmetry is deliberate: it refuses while a frame is being BUILT (the content half, `compiling_`) and WAITS for an
  ANSWER already in flight (`waitIdle`, as `shutdown` does) - the thread cannot be joined under the frame it is running,
  and an answer is bounded. It used to write `runner_` beside a frame's reads of it: a data race on a `unique_ptr`, and a
  silent no-op that left a host believing its frames had moved. The same hazard is why `runner_` is published by SHARED
  ownership: `waitForFrame` is allowed from any thread, so a waiter takes the runner object instead of naming a pointer
  that a mode change may free, and `FrameRunner::live_` (an atomic, set false after the join) answers "is anything still
  in flight" without reading the `std::thread` another thread is joining.
- **Boundary**: the rule covers ENTRY only. Waiting stays a plain wait for a caller with nothing to enter -
  `waitForFrame` from any thread, `releaseContent` / `releaseRenderTarget`, a host device verb - and that is what a
  second thread is for: several threads may wait for the SAME frame, and the one-in-flight handshake is the runner's, so
  no waiter waits on a notification sent to another. `applyPlan` keeps its own answer (`PlanRefusal::Busy`) because it
  edits the shape the interval protects, and it is refused under the same `phaseGate` the claim takes. Reading values
  (`frameNumber`, `presentRecord`) is never gated.

### 2.18 A door is gated or locked, never neither (`G1` / `G4` / `G6`)

- **What it is**: the engine has two families of state a host can reach while a frame records, and each family has ONE
  door rule, enforced where the state lives:
  - **the session shape** (pass declarations) is one critical section under `detail::FramePump::phaseGate()`
    (`RenderEngine::beginShapeChange`), and EVERY mutator of it stands behind that gate: `applyPlan` (in any form),
    `bindPassContent` / `PassRegistry::bindContent`, and the take-back `removePassesOwnedBy`. Each answers the caller a
    value - `PlanResult{Refused, Busy}`, `ContentBindOutcome::Status::Refused` - and reports one
    `engine.pass.changed_during_frame` per episode (C20).
  - **the image tables** (`detail::FrameResources`: the host's standing bindings, the frame's publications, the
    transients) belong to TWO worlds, so the lock is the type's own rule: every verb a host can reach takes
    `tables_mutex_` for a lookup, an insert or an eviction, never across a device call; and the frame head takes the
    host's bindings as a VALUE (`beginFrame`), so the seeding, the wiring checks and the resolution of one frame answer
    ONE set of bindings (C23).
- **Why**: the interval (C20) is a rule about which verbs may run while a frame records, and each mechanism had been
  written with one caller in mind - so when the engine grew a frame thread and public host doors, two of them stayed
  outside the rule. `publish` / `unpublish` / `resolve` / `transientTarget` touched tables a compile walks while
  `FrameResources` stated in its own doc "no synchronisation is needed", and `bindPassContent` rewrote "the same one
  storage a plan writes and the compile reads" without the gate `applyPlan` takes (review 19 `Z1` / `Z2`). Fixing the
  call sites one at a time would leave the next door free to make the same mistake; the fix is the rule, stated once per
  family and enforced by the type that owns the state (`G12`: the class boundary shows the concurrency model).
- **What the lock does NOT promise, and why that is not a gap** (`G4`): a borrowed answer (`resolve`, `transientTarget`)
  says "the instance that was live at this instant", never "it survives" - a compile that later replaces or retires a
  transient leaves a handle describing a target that has been let go, which is the contract the single-threaded path
  already states ("re-ask after a shape change"). And a locked table makes READERS safe, not writers: publishing a
  target is the host's act and editing a declaration is the gated act, exactly as C22 divides a description's readers
  from an edit.
- **One question, two answers, both stated**: `RenderEngine::resolve` answers the HOST - the live standing binding, so
  a host that just published asks what it just published - while the compile resolves against the frame's own copy
  (`FrameResources::resolveForFrame`), so the two can never disagree inside one frame.
- **The one lock order this introduces** (`G10` / `G11`): the shape gate (`FramePump::phaseGate()`) may be held while the
  image tables' lock is taken - `applyPlan` validates against `hostBindingNames()` - and the table's lock is never held
  across anything that takes the gate, because `FrameResources` calls nothing in the engine. So the order is
  `phaseGate` then `tables_mutex`, and nothing reverses it.
- **Where the remedy lives for a convenience layer**: `SceneView::setScene` re-binds its chain's presenting pass, so it
  takes the refusal seriously - it waits the frame out once (bounded, as a resize does before applying a plan) and, if
  the bind is still refused, reports `engine.view.content_not_bound` instead of letting the chain's content check
  accuse the wrong cause.
- **Acceptance**: T42 (the image tables), T43 (the shape's second door).

### 2.19 A take-back is accepted, never refused (`G4` / `G7` / `G10`)

- **What it is**: the shape changes a host can ask for divide by ONE question - *can the caller try again?* - and the two
  sides get different answers:

  | Ask | Caller | Answer while a frame records |
  |---|---|---|
  | `applyPlan`, `bindPassContent` (widening or rewriting the shape) | a host call, retryable | **refused** with a value + one report (`beginShapeChange`, 2.18) |
  | the take-back `removePassesOwnedBy` (from `~PassChain` / `~SceneView`) | a destructor, NOT retryable | **accepted and deferred** to the next frame head |

  A take-back between frames is applied at once, `removePass`'s backend release included. Asked inside one it is
  recorded (`detail::SessionShape::deferTakeBack`) and applied at the head of the next compile - the first instant the
  table is nobody's to walk - and by `shutdown()` after the pump stopped, so it never outlives the engine that held it.
- **Why**: the interval (C20) exists to keep a change out of a compile that is walking the table, and a refusal is the
  right answer only if the caller can act on it. `~PassChain` and `~SceneView` cannot: a destructor has no later call,
  and waiting is a deadlock when the frame being destroyed is the one destroying it. Refusing there meant the rows
  stayed in the frame with a **dead owner** and a chain already dropped, and the per-pass backend release never ran -
  reachable from the shipped host, which destroys the view immediately before the engine that stops the frame thread
  (review 19 `Z22`).
- **What a deferred take-back does NOT damage**: the frame already built or being answered keeps the shape it compiled,
  and the rows are self-sufficient - a `RenderPass` holds its camera, content, program and target by strong reference
  (`PassDesc`), so a dead owner's pass is drawn once more at most and the next frame no longer announces it. The drain
  runs after `beginFrame`'s `waitIdle`, so no compiled frame still names a row it removes.
- **The owner address is not an identity across time** (`G10`): `operator new` hands the next view the memory the last
  one freed, so a deferred request keyed by the address alone would delete the rows of the object that asked second.
  The record carries the **registration ticket** current at the instant it was asked (`Slot::registration`, a monotonic
  number stamped by `insertSlot`), and the drain removes only the rows older than it. The ticket is read under the same
  `phaseGate` every registration takes, which is why "the refusal and the ticket" cannot be split by a registration.
  One consequence, stated: a row the SAME address updates in place after asking is still older than the ask and goes
  with it - a live owner asking to be taken back and then re-declaring is a contradiction, not a case to serve.
- **No report**: `engine.pass.changed_during_frame` is the refusal's sentence and it tells the caller to wait or to
  change between frames - advice a destructor cannot take. The accepted-and-deferred answer is the whole answer.
- **The queue's own lock** (`G10` / `G11`): `deferred_take_backs_` is written from a destructor on any thread and read
  by a drain that runs inside a frame, so it has a leaf `std::mutex` - the phase gate is already held by the asker and is
  NOT this thread's to take in the drain. Lock order: `phaseGate` then `deferred_mutex_` (append), never the reverse;
  the drain holds it only across the swap and releases backend state after it, as `FrameResources` releases a transient
  after `tables_mutex_`.
- **Cost**: one lock-guarded vector append per dead owner (2.8 ns uncontended, the same plain mutex C22 / C23 use) and
  one `swap` per frame, empty in the steady state.
- **Boundary**: `applyPlan` and `bindPassContent` stay refusals - they are retryable, and the refusal is what tells the
  host to wait. Nothing ELSE becomes deferred: the take-back is the only ask whose caller is a destructor.
- **Acceptance**: T44.

### 2.20 A teardown is an interval entry, and a runner says where a job will run (`G1` / `G4` / `G10`)

- **What it is**: one question - *is this frame machinery being taken down, and may work still be handed to it?* - and
  three facts that used to answer it differently:
  - **the engine's teardown enters the interval**: `RenderEngine::shutdown()` takes the same lease `prepare()` takes
    (`FramePump::beginBuild`) and holds it across the WHOLE teardown (the pump stopped, the deferred take-backs
    applied, the transients released, the surface detached). A call from a frame body - a hook, or a diagnostic sink
    on the frame thread, which is where the vk backend reports a lost device - is refused with
    `engine.shutdown.refused_from_frame`; a call from another host thread while a frame is being built is refused with
    `engine.shutdown.refused_in_flight`; either way NOTHING is torn down, and the sentence is said once per episode.
  - **a runner has ONE life fact** (`detail::FrameRunner::State`: `Inline` / `Running` / `Stopping` / `Stopped`).
    `accepting()` answers "a job submitted now is handed to the engine thread"; `stop()` publishes `Stopping` under
    the same lock `submit` decides under, BEFORE it returns and whether or not it could join - so from the moment the
    stop is asked for (including from the frame thread, where the join belongs to somebody else) the next job runs on
    the caller's thread. A job handed over just before the stop is still run: the loop drains the request it was woken
    for and then exits.
  - **the pump publishes that answer**: `FramePump::threaded()` is `runner != nullptr && runner->accepting()` - the
    fact `frame()` reads to decide where its other half runs - so it stops being true when the stop is ASKED for, not
    when somebody joins.
- **Why**: the defect (review 19 `Z21`) was one answer short. A sink on the frame thread called `shutdown()`: the pump
  could not join the thread it was running on, so it returned before clearing its runner - and that runner's loop had
  left. `threaded()` still answered true (the object is stored), `submit` still took the threaded branch (`live_` was
  cleared only by the join), the job was accepted and never run, `has_request_` stayed set, and the next frame blocked
  for ever in `beginFrame`'s `waitIdle`. Behind it the teardown had already released the transients and detached the
  surface under the very frame that was inside it.
- **What it deliberately keeps** (`G4`): a stop asked from the frame thread still cannot join itself - the loop leaves
  after its job and a later stop from another thread performs the join (the destructor, `shutdown()`). And `wait` /
  `waitIdle` answer from the JOB IN FLIGHT, never from "is there a thread", so a stopping runner is not reported idle
  while its last job is still inside the device - which is the one-in-flight rule `beginFrame` relies on.
- **What it does not change**: `applyPlan` / `bindPassContent` / the take-back keep their answers (C20 / C24); a
  `frame()` handed over just before the stop is run. The interval entry closes the whole teardown, so a second host
  thread's `frame()` is refused (not queued) while one runs.
- **Cost**: none on the hot path - `threaded()` is one atomic load per frame, and `submit`'s locked path is the one the
  threaded mode already took.
- **Boundary**: the engine decides `Ran` / `Submitted` from `threaded()` before handing the frame over, and a mode
  change cannot land between the two decisions (2.17's interval).
- **Acceptance**: T45.

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

- **Behaviour**: the size is **the frame's own value** (handed to the target at the frame head, only when it changes); **a device verb waits for the frames in flight**
  (the device's gate - `createSurfaceTarget` / `setRenderTarget` / `createFrameDriver` / the readbacks / `shutdown`), while `initialize()` is the creator's before any
  frame and a window target's `setWindowHandle` stays a between-frames call the host makes after `waitForFrame()` (P3); each frame's present outcome is
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
- **The teardown is an INTERVAL ENTRY** (§2.20 / C25): `RenderEngine::shutdown()` takes the lease a prepare takes and holds it across the whole
  body, so a call from a frame body (a diagnostic sink on the frame thread) or from another host thread mid-build is REFUSED - `engine.shutdown.refused_from_frame` /
  `refused_in_flight`, once per episode, nothing torn down - while a frame ANSWERED on the engine thread is waited for and joined.
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
| C20 | The session shape (pass set, resource tables, backend retained state) changes **only between frames**: while this engine is BUILDING a frame (its content half - hooks, publication, `compileFrame`) or ANSWERING one (its engine half), and while a `prepare()` compiles, every mutator of it that EXPANDS or REWRITES the shape is refused - `applyPlan` (in any form) answers `PlanResult::Refused` + `PlanRefusal::Busy`, `bindPassContent` / `PassRegistry::bindContent` answer `ContentBindOutcome::Status::Refused` - and each reports **one** `PassProtocolViolation` per episode. The ONE exception is the take-back `removePassesOwnedBy`, whose caller cannot retry: it takes the same gate but is **never refused** - accepted and applied at the next frame head (C24, 2.19). The gate is `detail::FramePump::recording()`, and the decision plus the change are ONE critical section (`RenderEngine::beginShapeChange` holds `FramePump::phaseGate()`, the same lock a build is entered under) - so a change seen as allowed cannot be applied into a compile that started in between, and inline counts exactly like threaded. |
| C21 | **One device can be borrowed by several engines, and frames are concurrent per session**: content residency is shared by identity (one upload, one compile); one window has one target; a frame's machinery (command pool / fence / constant ring / descriptor pool) is **session-level**, so different sessions' frame bodies can run at the same time. Two frames of the same session are still serialised by its `FrameGate` / frame claim: another thread **waits**, and a same-thread reentry is **refused** and reported once (`PresentOutcome::Refused` / `PrepareReport::Outcome::Refused`). Device-level shared pieces each have their own lock (the cache is taken only at the frame head; the allocator / target map / clock / report sink); a frame outside a window takes the body gate exclusively, and a host verb is **the device's promise** (wait and count, or under `R6` park without waiting for a frame); **a host verb called from a frame-body thread is refused rather than waiting for itself** (`vk.device.reentry`; a readback inside a frame answers `InsideFrame`), and the refusal comes from **one type**: `graphics::FrameGate`'s shared/exclusive sides plus a per-thread nested-gate stack. Shared content's descriptions are safe for concurrent readers through one cache type (C22); shape editing is still the content thread's action. **Every frame also reports what it spent on the content lock** (`FrameReport::locking`: `acquisitions` is this frame's own phase count, `waits` how many of them really waited — the steady state is a constant with `waits` 0, measured the same way every run). |
| C22 | **A content description is built once per revision and shared by every reader, under one lock**: `Geometry::description()`, `MaterialValues::description()` and `ShaderProgram::description()` are one verb on one type (`detail::DescriptionCache`), which measures the source's revision, compares it with the cached value's and builds on a miss in ONE critical section — so several threads describing one object (two engines publishing one scene) get the same identity and no second value for one revision. The key is measured INSIDE the lock because a material instance's `revision()` resolves on read, and the lock is the plain kind (2.8 ns uncontended against 13.2 ns for the shared lock it replaced, measured 2026-11-27). An EDIT is still the owning thread's act: the cache makes readers safe, not a concurrent edit. |
| C23 | **A door is gated or locked, never neither: the two families a host can reach while a frame records each have ONE rule, enforced by the type that owns the state** (`G1` / `G4` / `G6`). The session shape's mutators all take `RenderEngine::beginShapeChange` (C20) - except the take-back, which takes the same gate and is deferred instead of refused (C24). The image tables (`detail::FrameResources`: the standing bindings, the frame's publications, the transients) take `tables_mutex_` in every verb a host can reach - `publish` / `unpublish` / `resolve` / `transientTarget` / `transientCount` may run during a frame and never tear - and the frame head copies the host's bindings (`beginFrame`), so the seeding, the wiring checks and the input resolution of one frame answer ONE set, while the host's own `resolve` still answers the live table (the compile resolves through `resolveForFrame`). The lock covers a lookup, an insert or an eviction, never a device call; it makes READERS safe, not a concurrent edit (as C22 does for descriptions), and a borrowed answer (`raw_ptr`) means "the instance that was live at this instant", not "it survives". |
| C24 | **A take-back is accepted, never refused, and it names the rows its owner had when it asked** (`G4` / `G7` / `G10`). `RenderEngine::removePassesOwnedBy` (reached from `~PassChain`, `PassRegistry::clearOwnedBy`, `~SceneView`) is the one shape change whose caller is a destructor and cannot retry: between frames it is applied at once (backend release included), and while a frame records it is recorded and applied at the **next compile head** - and by `shutdown()` after the pump stopped, so it never outlives the engine. It does NOT report `engine.pass.changed_during_frame`, because that sentence asks for a retry a destructor cannot make. The record carries the registration ticket current when it was asked (`Slot::registration`, stamped by `insertSlot` under the same `phaseGate`), so the drain removes the rows that existed THEN and never a row a new object at a recycled address registered meanwhile; a frame already built keeps the shape it compiled, and a drained removal runs the same `removePass` walk (and release) an immediate one does. `applyPlan` / `bindPassContent` stay refusals (C20) - they are retryable, and the refusal is the whole answer to a caller that can wait. |
| C25 | **A teardown is an interval entry, and a runner answers where a job will run before one is handed to it** (`G1` / `G4` / `G10`). `RenderEngine::shutdown()` takes the lease `prepare()` takes (`FramePump::beginBuild`) and holds it across the whole teardown (pump stopped, deferred take-backs applied, transients released, surface detached); a call from a frame body refuses with `engine.shutdown.refused_from_frame`, one from another host thread mid-build with `engine.shutdown.refused_in_flight` - one report per episode, and NOTHING is torn down - while a frame ANSWERED on the engine thread is waited for and joined, as a device verb waits. `detail::FrameRunner` has ONE life fact (`Inline` / `Running` / `Stopping` / `Stopped`): `accepting()` answers "a job submitted now is handed to the engine thread", `stop()` publishes `Stopping` under the lock `submit` decides under BEFORE it returns - including from the frame thread, where the join belongs to somebody else - a job handed over just before the stop is still run, and `wait` / `waitIdle` answer from the job in flight, never from "is there a thread". `FramePump::threaded()` publishes the runner's own answer, so the fact `frame()` reads stops being true when the stop is ASKED for, not when somebody joins. |
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

- How it fails: the pass table and the backend's retained state are swapped while a frame is being BUILT (the compile
  iterates the slot list) or ANSWERED (the backend names a pass the frame value carries), so the rows reference a released
  pass or a released transient target - a wrong picture at best, a use-after-free at worst.
- How to detect it: `applyPlan` answers `PlanResult::refused()` + `PlanRefusal::Busy` (C20), and the refusal speaks once
  per episode (`engine.pass.changed_during_frame`). "Busy" means "a frame is building or answering", NOT "the frame
  thread is busy" - an inline engine is refused too, and so is a change from a second thread while the paint thread
  compiles.
- How to prevent it: change the shape between frames (`graphics-frame-pipeline.md` F6) - or retry on `Busy`, as a
  bounded `waitForFrame` + re-apply loop (the demo's `addDemoPass` is the shape of it).

### 6.3 P3 Doing device verbs mid-frame

- How it fails: a verb that is NOT gated changes the session while the engine thread is recording it. Today that is `device->initialize()` (the creator's,
  before any frame) and a window target's `setWindowHandle` (the host owns the surface object).
- How to detect it: TSan, or a crash that reproduces. The gated verbs cannot fail this way - they wait, and a call from inside a frame body is refused with
  `vk.device.reentry` instead of deadlocking.
- How to prevent it: the host calls `waitForFrame()` first for those two; the size reaches the frame only as a value through `RenderArea::resize` (§2.6).

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
| T36 | a shape change inside a build is refused (inline, threaded, and from a second thread) | `PlanTest.AFrameRefusesAPlanFromInsideItsOwnCompile` / `PlanTest.APrepareRefusesAPlanFromInsideItsOwnCompile` / `PlanTest.APlanFromAnotherThreadIsRefusedWhileThisOneCompiles` + `FramePumpTest.ThePhaseCoversTheBuildAndTheAnswer` | implemented |
| T37 | the frame report is a closed frame's value, readable while the next frame runs | `VkConcurrentFrameTest.TheFrameReportIsTheFrameThatEndedWhileTheNextOneRuns` (TSan: 15 reports with the live-books read, 0 with the published one) | implemented |
| T38 | a second entry into the interval is a value, not a race: the other host thread gets `Concurrent`, the same thread gets `Reentrant`, and both take no frame number | `RenderEngineTest.AFrameFromASecondHostThreadIsRefusedInsteadOfRacingTheCompile` | implemented |
| T39 | two host threads DRIVE IN TURN: every frame is numbered once, a prepare from the second thread is refused while the first builds, and several threads waiting for one frame are all answered | `FramePumpTest.TheIntervalAnswersWhyASecondEntryWasRefused` / `FramePumpTest.TwoHostThreadsDriveThePumpInTurnAndNumberEveryFrameOnce` / `RenderEngineTest.APrepareFromInsideAPreparesCompileIsRefusedInsteadOfWaitingForItself` / `RenderEngineTest.TwoHostThreadsPreparingAtOnceLeaveOneAnswered` / `RenderEngineTest.ASecondHostThreadDrivesTheNextFrameOnceTheIntervalIsFree` / `RenderEngineTest.TwoThreadsWaitingForOneFrameAreBothAnswered` | implemented |
| T40 | a mode change is an interval verb: refused by the same two reasons while a build holds it (the cause reported once), applied between frames, the mode unchanged - it WAITS for an answer already in flight instead, and a build that leaves by an exception leaves the interval free for another thread | `FramePumpTest.AThreadModeChangeIsAnIntervalVerb` / `FramePumpTest.ABuildThatLeavesByAnExceptionLeavesTheIntervalFree` / `FramePumpTest.OneThreadMayHoldTwoPumpsAndEachKeepsItsOwnInterval` / `RenderEngineTest.ASecondThreadCannotChangeTheFrameThreadWhileAFrameIsInFlight` / `RenderEngineTest.SwitchingBackToInlineWaitsForAnAnswerInsteadOfRefusingIt` / `RenderEngineTest.ASinkDrivingAnotherEngineLeavesTheFirstOneWhole` | implemented |
| T41 | a description is one value per revision under concurrent readers: several threads describing one program, one material and one geometry get one identity each, a new revision is a new value, and `peek` never builds | `DescriptionCacheTest.OneBuilderAnswersEveryConcurrentReaderTheSameValue` / `DescriptionCacheTest.ANewRevisionIsANewValueAndTheOldOneKeepsItsRevision` / `DescriptionCacheTest.PeekNeverBuildsAndAnswersNullForAMovedRevision` / `DescriptionCacheTest.ThreadsDescribingOneProgramOneMaterialAndOneGeometryShareOneValue` + `scripts/tsan_check.sh` | implemented |
| T42 | the image tables are gated by one lock and one frame-head value: a host publishing, withdrawing, resolving and reading transients while a frame compiles never tears the table, and one frame answers the bindings its head took | `FrameResourcesTest.AHostSwappingBindingsWhileAFrameCompilesNeverTearsTheTables` / `FrameResourcesTest.TheFrameAnswersTheBindingsItsHeadTook` + `scripts/tsan_check.sh` | implemented |
| T43 | a content bind is a shape change: refused from inside a compile (the declaration unchanged, one report), accepted between frames, and two passes serving one name from different targets are reported once | `PlanTest.AFrameRefusesAContentBindFromInsideItsOwnCompile` / `PlanTest.TwoPassesPublishingOneNameAreReportedAndTheLastOneServesIt` | implemented |
| T44 | a take-back is never refused: asked from inside a compile it leaves the row the compile walks, reports no refusal, lands at the next frame head with its backend release, removes only the rows that existed when it asked (a row a later object registers at the same address survives), applies at once between frames, is applied by `shutdown()` when no frame came, and WAITS while the engine half answers a frame (the shipped host's order) | `PlanTest.ATakeBackAskedInsideAFrameLandsOnTheNextFrame` / `PlanTest.ATakeBackThatWaitedLeavesRowsRegisteredAfterTheAskAlone` / `PlanTest.ATakeBackStillWaitingWhenTheEngineStopsIsApplied` / `PlanTest.ATakeBackWhileAFrameIsAnsweredWaitsForTheNextHead` | implemented |
| T45 | a teardown is an interval entry and a runner says where a job will run: a shutdown from the frame thread (a sink reacting to a device report) is refused with the cause, said once, and tears nothing down - the engine keeps driving and the next frame is answered; a shutdown from another host thread mid-build is refused the same way and lands between frames; a stop asked from the frame thread leaves an inline runner and an inline pump BEFORE the join, and the next job runs instead of being handed to a loop that left | `RenderEngineTest.AShutdownFromTheFrameThreadIsRefusedAndTheEngineKeepsRunning` / `RenderEngineTest.ASecondThreadCannotShutTheEngineDownWhileAFrameIsBeingBuilt` / `FrameRunnerReentryTest.ARunnerAStopHasLeftRunsTheNextJobOnTheCaller` / `FramePumpTest.AStopAskedFromTheFrameThreadLeavesAnInlinePump` | implemented |

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
- `src/viz/graphics/sdk/vine/graphics/DescriptionCache.hpp` (the one cache behind `Geometry::description()` /
  `MaterialValues::description()` / `ShaderProgram::description()`: C22's rule, and `peek` for a reader that must not build)
- `src/viz/graphics/src/ScenePublisher.{hpp,cpp}` (the scene's derivation side: baked rows, per-view memo, the published value)
- `src/viz/graphics/src/SceneRows.{hpp,cpp}` (`SceneBake` → the single gate into a snapshot)
- `src/viz/graphics/src/SnapshotSource.{hpp,cpp}` (the snapshot's `DrawListSource` face)
- `src/viz/graphics/src/PublicationStore.{hpp,cpp}` (the content side's publication/binding and the `SnapshotSource` table)
- `src/viz/graphics/src/FrameResources.{hpp,cpp}` (the image tables: the host's standing bindings, the
  frame's publications and transients, and the one lock over them - C23)
- `src/viz/graphics/src/FrameExpand.{hpp,cpp}` (`bindSceneDraws` / `expandPass` / `expandFrame`)
- `src/viz/graphics/src/FrameRunner.{hpp,cpp}` (the engine thread)
- `src/viz/graphics/src/FramePump.{hpp,cpp}` (the interval: the lease a frame / a prepare / a teardown takes, the phase, and `threaded()` - "where will this job run" - C25)
- `src/viz/graphics/src/EngineCompile.cpp` (the content half: hooks / publication / describe / the engine half's entry)
- `src/viz/graphics/src/SessionShape.{hpp,cpp}` (the slot table: the registration ticket, `deferTakeBack` / `applyDeferredTakeBacks` - C24)
- `src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp` (`bindSnapshot` / `setFrameThreading` / `waitForFrame` /
  `presentRecord` / `presentedCamera`)
- `src/viz/graphics/src/DrawListMemo.{hpp,cpp}` (per-view list reuse and its two atomic counters)
- `src/fw/appfw/src/gui/SurfaceWindow.cpp` (the host's two run positions and the attach timing)

### 10.2 Cases

- `tests/test_graphics/PublicationStoreTest.cpp` (T3 / T31's three-state publication)
- `tests/test_graphics/FrameExpandTest.cpp` (T34 / T35: the frame-level seam, a bound publication is the truth, no publication draws nothing)
- `tests/test_graphics/FrameRunnerTest.cpp` (T10 / T12 / T16, and T45's says-where-a-job-runs pump case)
- `tests/test_graphics/SnapshotShadowTest.cpp` (T3 / T17 / T34)
- `tests/test_graphics/DescriptionCacheTest.cpp` (T41: one build per revision, one identity for every concurrent reader)
- `tests/test_graphics/FrameResourcesTest.cpp` (T42: the head's value, the collision answer, and the lock under two threads)
- `tests/test_graphics/PlanTest.cpp` (T36 / T43 / T44: the gate's refusals and the take-back that is never refused)
- `tests/test_graphics/HeadlessBackendTest.cpp` (the two halves and the warm-up)
- `tests/test_graphics/GraphicsTest.cpp` (memo reuse and T9's surface size)

### 10.3 Documents and ADRs

- `../decisions/ADR-2026-11-12-adopt-graphics-next.md` (four layers / three verbs / invariants; this file is its threading layer)
- `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md` (the split and order of the snapshot taking over collection)
- `../decisions/ADR-2026-11-27-a-description-is-built-once-behind-one-lock.md` (C22: one cache type, one lock, and the
  rejected per-class copies / shared-lock / lock-free shapes)
- `../decisions/ADR-2026-11-27-a-door-is-gated-or-locked.md` (C23 / C20: the two families of shared state, why one
  door was locked and the other gated, and the rejected "copy Geometry's answer" / "state the rule in five docs" shapes)
- `../decisions/ADR-2026-11-27-a-take-back-is-accepted-never-refused.md` (C24: why a destructor's ask cannot be a
  refusal, the deferred removal at the frame head, the registration ticket that survives an address reuse, and the
  rejected "wait in the destructor" / "order the view's death behind the frame" / "retry from the host" shapes)
- `../decisions/ADR-2026-11-27-a-teardown-is-an-interval-entry.md` (C25: why a shutdown from a frame body is refused,
  the one runner state, and the rejected "clear the flag but keep pointing at the runner" / "defer the teardown" /
  "wait in the sink" shapes)
- `../decisions/ADR-2026-11-10-render-world-mirrors-content.md` (**history**: the original mirror-shape decision, superseded by the two above)
- `graphics-frame-pipeline.md` (the target shape and the step-by-step migration in §9.3)
- `vk-backend.md` (backend implementation and synchronisation details)
- `appfw-render-surface.md` (which half the area and the window belong to)

### 10.4 External sources

- Qt Quick's `sync()` / Unreal's `ENQUEUE_RENDER_COMMAND` (compared in §2.9; this repository brings in no third party).
