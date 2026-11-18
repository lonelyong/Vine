# graphics / vsg backend — current-state notes

> **Current state only**: mechanisms, criteria and traps, plus “why it was decided that way”. Per-slice history and the generation before the rewrite are in git;
> the design docs are in `.ai/design/`, and **the only to-do list** is `.ai/backlog/graphics.md`.
> The finer backend contract is `.ai/design/vsg-backend.md`; the trap table is `.ai/design/vsg-pitfalls.md`.

## 1. Layering and one frame

- SDK: `src/viz/graphics/sdk/vine/graphics`; vsg backend: `src/plugins/gfx_backend_vsg` (`core/` pure policy + `api/` vsg adaptation + `support/`).
- `RenderEngine`: lifetime / `frame()` / device borrowing / size / capabilities and reports; the three facades are `PassRegistry`, `OutputRegistry`, `DiagnosticsRoute`.
- `frame()` returns `FrameOutcome` (`Ran` / `Submitted`; `NotReady` + `EngineNotReady`), `applyPlan` returns the three-state `PlanResult`; a pass's content lives in `PassDesc::content`, and `bindPassContent` returns whether it was hit.
- **The sort key is `detail::SortKey`** (`DrawListBake.hpp`): drawOrder → layer (smallest layer number) → opaque/transparent → quantised camera-space depth (1/4096 units) → row/primitive index. The representative point is the **world box centre** (no box falls back to the matrix origin), and `sortCollected` uses `std::sort` (`tie` guarantees no equal keys).
- **The frame result is a value**: `RenderEngine::frameResult()` → `FrameResult{frame, present, query{snapshot, view}, passes, draws, diagnostics}`, written once when the frame is answered (`frame_result_mutex_`); `query.view` is a `View` value (camera snapshot + device pixel area + layer mask), and `presentedCamera()` is its camera half. The `poses` half waits for F10.
- **The sort key is `detail::SortKey`** (`DrawListBake.hpp`): drawOrder → layer (smallest layer number) → opaque/transparent → quantised camera-space depth (1/4096 units) → row/primitive index. The representative point is the **world box centre** (no box falls back to the matrix origin), and `sortCollected` uses `std::sort` (`tie` guarantees no equal keys).
- **The frame result is a value**: `RenderEngine::frameResult()` → `FrameResult{frame, present, query{snapshot, view}, passes, draws, diagnostics}`, written once when the frame is answered (`frame_result_mutex_`); `query.view` is a `View` value (camera snapshot + device pixel area + layer mask), and `presentedCamera()` is its camera half. The `pose` half waits for F10.
- Type tests on the `Object`-derived nodes / passes / geometry use `obj_cast<T>` (core's meta-type chain), never `dynamic_cast`; `CameraManipulator` and third-party vsg types are the exceptions.
- `PassChain`: one picture's recipe + registration ownership. `PassChain::build(engine, ChainDesc)` builds the chain and registers the passes; the destructor hands its passes back to the engine; `SceneView::setChain` adopts it.
- One frame: `beginFrame` → every pass (`beginPass` / `setPassOrder` / `setPassInputs` / `execute` / `endPass`) → `endFrame` → present.
- Scheduling happens only in the engine (`detail::FrameScheduler`: iterative Tarjan + a heap ready queue, zero allocation in steady state); the backend materialises the `FrameDescription.passes` it is handed and never reorders or reports a cycle.
- Backend contract: `RenderBackend.hpp` + `ShaderAbi.hpp` (four zero-dependency L1 blocks) + `LightAbi.hpp` (the light block and packing); the types of the `backend/` half live in `core/`, `api/`.
- **One frame has two halves**: the content half (hooks / publish / describe) on the calling thread and the engine half (drain / bind / expand / execute /
  present) on the engine thread; two frame values rotate and the frame number travels with the frame (`detail::EngineFrame`). **The host is threaded by default**
  (`SurfaceWindow` starts with `setFrameThreading(EngineThread)`); `RenderControl::setFrameThreading(Inline)` is the fallback.
- The perceptible cost of threading (do not report it as a bug): the picture is **one frame behind** the content (N15); `frame()` waits for at most **one engine half** (including
  acquire/present), so the UI is throttled by the render cadence — "does not block" means it does not pay the expansion and the whole frame, not that it is not throttled.
- **Edit batches**: `Scene::beginEdit()` / `endEdit()` (or a `Scene::Edit` scope) hold back the mirror-compared version — frames inside the batch do not publish and
  draw the state from when the batch began, and the whole batch lands at once in the frame after it (C18/R17, a hard requirement of robot joint state). Queries (picking, `boundingBox`,
  direct collection) still see the current content inside the batch; `version()` returns `published_epoch_/published_boxes_`, not `bake_epoch_`.
- **Pointer queries are answered against "the frame the user saw"** (N17): `RenderEngine::presentedCamera()` publishes the camera snapshot of the frame that was answered,
  `SceneView::setPresentedCamera` hands it to the manipulator, and `OrbitCameraManipulator::makeScreenRay` prefers it; with no published value it falls back to
  the current camera (static picture / inline). One pixel → a world ray is `CameraSnapshot::screenRay` (back-project both ends; perspective and orthographic use the same path).
- The present outcome is a value: `PresentOutcome{Presented, Skipped, Lost, Refused}` + `RenderEngine::presentRecord()` (frame number + outcome, one atomic word).
  `Refused` means “the device did not record this frame at all”: the calling thread is already inside a frame (next bullet); it is neither `Skipped` (nothing to present) nor `Lost` (there is a surface but the picture did not arrive).
  `Lost` is repaired by the **engine** (forget the announced size → report it again next frame → the backend rebuilds the swap chain); the host only logs one line, does not tear down the session and does not hide the area
  (hiding the area would stop the repair frame). Device verbs (`createSurfaceTarget` / `setWindowHandle` / `initialize`) are **between-frame** operations.
- **One frame at a time is a “checked fact”, not a comment (2026-11-15, T47)**: `backend/FrameGate.hpp` — `executeFrame` /
  `prepareFrame` takes a **lease** on entry: another thread **waits**, the same thread re-entering is **refused** (`PresentOutcome::Refused` /
  `PrepareReport::Outcome::Refused`, one `*.session.frame_reentry` diagnostic). Both backends share one implementation, so the host's same
  mistake gets the same answer on either side. Why a lease rather than `enter/leave`: a frame has several early returns, and forgetting `leave` wedges the next frame,
  while a lease makes “forgetting” inexpressible. It guards only the **frame machine** (command pool / fence / ring / descriptor pool); host verbs (`setRenderTarget` /
  `releaseContent`) are still between-frame calls.
- Collection memo: `detail::DrawListMemo` is shared by `Scene` and `RenderWorldSource` (key = view projection / eye point / viewport / layer mask, state =
  table revision, token = frame number, one frame of grace; token 0 = do not record). The witness is on the engine side: `RenderEngine::contentCollectCount/ReuseCount`.
- Race / lifetime gate: `scripts/tsan_check.sh` (`build-tsan`, `-fsanitize=thread`; only **0 warnings** passes). It has caught
  the use-after-free in `removePass` and "`*collectRenderCommands(...)` straight into a range-for" (the temporary `shared_ptr` dies when the range
  initialisation ends).

## 2. Content model

- `Geometry` = a set of channels; `Primitive` = one draw (topology + index span + material / program + instance stream).
- Indices and the instance stream are written only on `Primitive`; `Geometry::material` / `program` are **geometry-level defaults** (the primitive's own value wins, otherwise the nearest `StateNode`).
- A channel = `BufferSlice<float>` + components. `setPositions` / `setNormals` / `setTexcoords2` / `setTexcoords3` share a buffer; an arena uses slices.
- Instance stream: 16 floats per instance (column-major mat4); `visible_instances` is a draw parameter and is not re-uploaded; `VN_INSTANCED` decides binding 4 and the vertex layout.
- revision: **shape belongs to geometry** (`Geometry::setRevision` / `bumpRevision`), **bytes belong to the buffer** (`Buffer::setRevision`). The upload cache keys on the buffer revision only; the geometry revision only drives fact-row rebuilds.
- Identity: `vn::graphics::Handle` (gen24/domain8/index32) + `ContentRef{id, revision}`, public header `sdk/vine/graphics/ContentId.hpp`. `Node::id()` / `MaterialBase::id()` are allocated at construction and released at destruction; `Row::identity`, `RenderWorld::RowId`, delta, `RenderCommand::node`, `CompiledCommand::geometry`, `GeometryFacts::geometry` and the material table all key on it. **A table builds rows by `(id, revision)`**, and a row of an old revision keeps answering inside the parking window. An address is no longer an identity (`ADR-2026-11-11-content-identity-is-a-handle`); `Buffer`/stream upload still is an address + `ADR-2026-11-09`'s “holding is identity” (content objects, plans and compiled halves have all moved to `ContentRef`).
- Picking tree: `Geometry::prepareBvh(draw, builder)` defaults to `pickTreeBuilder()` (`kPickTreeLeafSize = 64`), and the built tree is **sealed** (`Bvh::seal()`) — a 14M-triangle tree with a self-chosen leaf capacity of 4 went from 1.36 GB down to 88 MB. A sealed tree's query callback gets the leaf box as its second argument, which picking never uses anyway.
- Collection: `Scene::collectRenderCommands` emits one command per primitive, snapshotting world / state / material / program / instance; sorted by `drawOrder`, opaque front-to-back and transparent back-to-front.
- Bounding boxes: the `Geometry::localBounds` cache key = positions buffer + its revision + segment + geometry revision; `subtreeBounds` in the node's own space; `Scene::boundingBox` counts only content that is visible and matches the layer mask.
- Box/line/plane types live in `vn::geometry`: `Aabb<T>` (`Aabbf/Aabbd/Aabbi`), `Obb<T>`, `Line<T>`, `Plane<T>`; `math` has numbers and transforms only — no `Rect3`, no `math::Aabb*`. See `.ai/design/geometry-primitives.md`.
- OBB fitting: `Obb::computePca` / `computeFixedOrientation`; the eigendecomposition is `math::symmetricEigen` (ascending spectrum, upper triangle only), and matrix → quaternion is `math::toQuaternion`. The minimum-volume OBB and `merge` are not done.

## 3. Targets, layers, colour, image sources

- `RenderTarget` is a **description**; the backend creates images from it. `RenderTarget::setSamples` is the only MSAA entry, and the sample count goes into `TargetShape` ⇒ pipeline key ⇒ attachment/image. Colour and depth each resolve to one single-sample image (depth `sample0`, no averaging).
- **A pass's target and its publication are each a value** (B14): `PassDesc::target` = `Target{Kind::Window|Explicit|Transient, target, name, extent, shape}` (`Target.hpp`), `PassDesc::output` = `Output{name, whole_target, image}` (`Output.hpp`); the six `writes*` / `output_*` fields are deleted. `Target::isValid` / `Output::isValid` are checked by `validatePlan` (`InvalidTarget` / `InvalidOutput`); a transient resolved by compile goes into `RenderPass::setResolvedTarget` and **does not** rewrite the declaration.
- **The seam no longer spells Vulkan formats and subpasses** (B8, 2026-11-17): the device format of `TargetShape` / `RenderPassCompatibility` is `DeviceFormat` (an opaque code, 0 = `kUndefined` = “no such attachment / not learned”), and the render-pass structure is a backend-computed `RenderPassKey` (0 = `kNotStated`; both backends emit `kSingleSubpassRenderPass` today). Both ends only compare, never decode; `RenderPassKey` is the **structure half** and takes part in equality **beside** format/depth/sample count, not as their hash. `ImageLayout` is still the SDK's Vulkan semantics and has not been folded in yet.
- Layers: `Node::setLayers` + `PassDesc::layer_mask`; the nearest declaration wins; `Node::subtreeLayerUnion` is a lazy cache driven by the layer epoch, and the three walks test `(union & mask) == 0` right after visibility.
- Colour: `Colorf` is linear light, `Color` is sRGB-encoded bytes; `toLinear()` / `toColor()` are the pair of conversions; clear colour and pass colour use linear values.
- Image sources: `ImageInput` / `ImageSource`; produced (`ImageRef`: target + attachment) and imported (`Texture`) lifetimes are not merged. `MaterialImages` uploads by key `(Texture*, revision)`, and samplers are cached by value.

## 4. vsg platform facts (hard constraints)

- Vulkan 1.4 floor; EDS2 / EDS3 + 3 fn-pointer entries; **no dynamic rendering** ⇒ render pass + load-op variants.
- vsg compiles pipelines per viewID ⇒ every pass's state registry must be private.
- push constant 128 B; reverse-Z (far = 0); world CCW vs framebuffer CW ⇒ `frontFace = CLOCKWISE`.
- X11 / Win32; **no Wayland**.
- The in-flight slot count is vsg `Viewer`'s hard-coded 3: learned frame by frame during startup (`probeSlotCount` / `SlotTracker`), and nothing is parked before it is learned.
- Retirement window = `slots + 1`; `RetirementQueue::setSlots()` updates in place — do not assign a new queue (that zeroes the counters).

## 5. Traps (do not step in again)

- `vn::String` wraps `std::u8string`; write literals as `u8"..."`, and printf/gtest use `reinterpret_cast<const char*>(s.data()), s.size()`.
- `PassDesc::camera` is an **owning** `intrusive_ptr<Camera>`; passing a stack camera's address double-frees.
- `vsg::Viewer` has no `device` member; stop the device with `viewer->deviceWaitIdle()`.
- `vsg::Instance::create` **throws** (it does not return null); a version-fallback loop must catch.
- `vsg::RenderGraph`'s default `renderArea` is zero-sized; it must be set explicitly, or the pass records but clears nothing.
- `vsg::DescriptorBuffer::create` takes its arguments as `(bufferInfoList, DESTINATION BINDING, array element, type)`.
- The dynamic-state block slot must be `detail::kDynamicStateSlot` (15), or it replaces the pipeline bind.
- `VkSubpassDependency`'s stage/access masks are part of render-pass compatibility; when borrowing depth the dependency must be a superset of the shape.
- Device extensions must be truly enabled (EDS3's polygon mode / color blend); setting the feature bit alone is not enough, and only the validation layer sees it.
- Vulkan NDC z is the depth (there is no OpenGL `z*0.5+0.5`); reverse-Z far = 0.0, so a depth clear must not clear to 1.0.
- Dynamic `vkCmdSetPrimitiveTopology` may only stay inside the pipeline's topology class; `Topology` goes into `PipelineKey`.
- `POINT_LIST` requires the vertex stage to write `gl_PointSize` statically; the built-in programs write 1.0.
- Vertex/index upload is an **alias, not a copy**: `StreamUploads` uses `VsgBufferView` + a real `vsg::floatArray` / `vsg::uintArray` to read the model bytes
  (a vertex channel aliases its segment, the indices alias the **whole buffer** — the span is stated by the draw). The model may die first: array → view → buffer is one owning chain.
  Binding a `vsg::Data` subclass **directly** as the array **does not work** (the validation layer is clean but nothing draws); it must be wrapped in a real array first.
  Only with no buffer handle, or a span that does not belong to that buffer, does it fall back to a copy (the copy path must stay — the tests call it that way).
- After a vsg upload **the process carries an extra host-memory allocation the size of the data** (for a 14M-triangle model: 1.23 GB
  `SharedUsage` beyond the model's 1.16 GB, plus a same-sized `DedicatedUsage`). **The ownership is settled**: in `TransferTask::transferData()`,
  `staging = vsg::createBufferAndMemory(device, totalSize, TRANSFER_SRC, EXCLUSIVE, HOST_VISIBLE|HOST_COHERENT)`
  — one host-visible buffer for the whole transfer, kept in `TransferBlock::staging`, which grows but never shrinks while `staging->size < totalSize`
  and is not returned after the transfer; it does **not** go through `stagingMemoryBufferPools`, so `releaseUnusedPools()` cannot reach it (the measured application numbers did not change by a digit).
  It is freed when the session (viewport/document) is destroyed; vsg has no getenv/config that turns this path off (the source only reads getenv in FileSystem).
  **Solved (F16.2, settled 2026-11-10)**: **staging is not retained**. `detail::StagingTask` (derived from `vsg::TransferTask`, installed by
  `installStagingTask()` when the session comes up) clears `staging` + `buffer_data` of every block whose `fence->status() == VK_SUCCESS` at **every frame boundary**
  (the top of `VsgBackend::executeFrame()`) — exactly vsg's own “rebuild when too small” starting point, so the next upload into that block reallocates.
  The evidence is that block's own fence, so there is **zero wait, zero park, zero `deviceWaitIdle`**; an unfinished copy is left to the next frame.
  **Do not add extra conditions**: the sweep once asked `to_copy->containsDataToTransfer()` first (“skip while data is still to be copied”),
  while vsg's `_transferData()` returns early when `requiresCopy()` is false and **does not clean up** `dataMap` → that condition can stay true forever, the sweep skips forever,
  and the 1.2 GB comes back resident (the flaky case caught exactly this).
  **Each block is independent**: `TransferTask` has `2 × numBuffers` blocks and uploads rotate by `frameIndex`, so two blocks may each hold one model
  (measured: a 704 MB body → `SharedUsage` 1,228 MB = `2 × 614 MB`).
  **Observable**: `VsgBackend::uploadStagingBytes()` / `api::Session::stagedBytes()` / `api::OffscreenSession::stagedBytes()`;
  steady state is constantly 0, and `RetentionStats::device_waits` never rises on this path.
  **The cost**: a host that uploads every frame reallocates a block every frame (16 MB minimum) — no such host exists in the repository today; the trigger condition is in the backlog.
  **Swapping the task** (`installStagingTask`) is now only for “install a task that can clear blocks”; the four hand-offs are still all required (see P42.1).
  **How to read the numbers**: `Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory` instance names are `pid_<id>_luid_...`
  (filtering by `*Vine*` yields 0 rows); `DedicatedUsage` = video memory (counts as private, **not** in ws), `SharedUsage` = system memory
  (in ws). Lowering it means uploading fewer bytes (fewer faces / an indexed format) or letting the staging be reclaimed.
- Shader naming: files keep the `builtin_*` / `vine_*` prefixes; functions are `vnXxx`, and variables, block instances, stage outputs and samplers all carry the `vn_` prefix.
- A sampler name **is** an ABI key (`ContentImages::imageOriginOf` / `MaterialSlotCheck`): renaming it in the shader must change the table in the same commit;
  changing one side does not error, it just swaps the drawable's image for a pass input (`vn_shadow_map`, the five material slots, `vn_skyMap`).
- Transparency: the engine's only transparency channel is `Node::opacity`; materials do not contribute alpha; blend is always on and `enabled` only selects the factors.
- Device readback pixels are in BGR byte order; assert a single channel with `isRed()` and friends, never by reading `pixel[0]`.
- Changing a public signature or a name requires a full build; an old plugin `.so` reports `undefined symbol`.
- **A hidden / culled upload is not dropped — leaving the tree drops it**: the lifetime is two O(1) facts — the declaration index published by the content layer
  (`ContentFacts::declarations`, an address → declaration count maintained in step with the `ContentStore`'s geometry-row changes) says “is a live row
  still declaring this buffer”; the entry's own handle says “is the revision still the one it uploaded”. So culling for 100 frames re-uploads nothing,
  and an `edit()` generation change leaves on its own; when the object leaves the tree ⇒ the row is deleted `slots + 1` frames later ⇒ the declaration disappears ⇒ the upload leaves with it.
  Each frame walks only the resident entries, never the content rows (walking rows makes a “nothing drawn” frame the most expensive one). See `.ai/design/graphics-content-lifecycle.md` §2.5 / §2.9.
- **Two build traps in non-Debug configurations (measured 2026-11-10)**:
  - vcpkg's glslang exports only the `DEBUG` / `RELEASE` configurations; RelWithDebInfo / MinSizeRel have no exact match, so CMake falls back to the **first** (DEBUG)
    ⇒ the plugin links the `/MDd` glslang targets into the `/MD` DLL: `LNK2038` (`_ITERATOR_DEBUG_LEVEL`, `RuntimeLibrary`) + unresolved `__imp__CrtDbgReport`.
    `gfx_backend_vsg/CMakeLists.txt` sets `MAP_IMPORTED_CONFIG_RELWITHDEBINFO|MINSIZEREL = RELEASE` on `glslang::glslang` / `::SPIRV` / `::glslang-default-resource-limits`;
    a future vcpkg package that ships only two configurations needs the same mapping on the consumer side.
  - `tests/test_graphics/GraphicsTest.cpp` is about 650 KB, and in optimised configurations its section count exceeds 65535 (`C1128`) ⇒ the MSVC options in `tests/CMakeLists.txt` all carry `/bigobj`.

- **The three landing points of a material change (measured 2026-11-11, do not step in again)**: a material is the last content fact that “crosses a frame by value” (C19). After
  `Material::setXxx` the value must land through **two read points**: `SceneRows::publish` (the publication boundary, the version the mirror/engine side sees)
  and `detail::refreshMaterials` at the top of `Scene::collectRenderCommands` (clears the memo and raises `Version::materials` only when the version moved).
  **Changing only one lags the picture**: publish only ⇒ direct/mirror collection hits the old memo; collect only ⇒ the mirror never republishes. Inside a batch (`edit_depth_ != 0`)
  neither reads, and that is how C18 takes effect for materials.
  Also: `RenderCommand::materialDescription()` falls back to `material->description()` for hand-built commands (the same shape as
  `geometryDescription()`), so a hand-built command still “lands next frame”, but that path belongs to the host (do not change it while recording).

- **Allocation-failure classification lives in `Device` alone (2026-11-15, audit A1/A8)**: `MemoryAllocator` only reports facts
  (`MemoryRefusal` + the driver's raw `VkResult` + requested bytes/type/pool numbers, packed into `MemoryRefusalReport`), and **every allocation
  including successful ones** goes to the sink `Device` installed in `initialize()`; `Device::noteMemoryRefusal` is the only classification point (`DeviceLost`
  → `noteDeviceLost`, the session ends). Stop letting call sites attribute it themselves: five of the six allocation sites only look at “did it work”, and handing them the reason means
  it gets dropped — **device loss was once read all the way down as “out of video memory”**. The failure path is injected through `Device::setMemoryBlockCreator`
  (`vkAllocateMemory` is the only block-creation point, and it does not fail on a healthy machine); the three mutations are each killed by
  `VkMemoryAllocatorTest.ARefusedBlockSaysWhichKindOfFailureItIs` /
  `VkMemoryFailureTest.ALostDeviceDuringAnAllocationEndsTheSession` /
  `VkMemoryFailureTest.AnExhaustedHeapIsOneReportWithItsNumbersAndTheSessionSurvives`.
  When writing such a case, first ask “will the request really create a new block”: on an integrated device device-local and host-visible may be the **same**
  memory type, so a small target slices into an existing block and the injection point never triggers (the case uses 4096×4096 RGBA8 = 64 MiB to get past it).

## 6. Evidence and gates

- **The frame report can answer "which pass is expensive" (2026-11-15, A5)**: in `FrameReport::timing`, `FrameTiming::passes` is `{pass name, gpu_micros}` in execution order,
  one boundary per pass (written before its uploads, so the transitions/copies it demands count against it), and the segments sum to
  ≤ the whole frame; the query pool grows **between frames** to "the largest pass count seen". Sampling happens every 30 frames, and an unsampled frame's `timing` **is absent**.
  Do not charge the whole frame to every pass — that is exactly the wrong method `VkPassTimingTest` pins down.
- **S6c-2b landed (2026-11-16, R6/T6: release no longer waits for a frame)**: `releaseContent` (three overloads) and `releaseRenderTarget` left the body gate — they wait for
  **one cache operation** (the lock frame-head resolution holds), not a whole frame. Safety comes from existing mechanisms: the cache lock (frame-head resolution and release exclude each other) + park (a handle a frame resolved lives to
  the watermark) + an open frame holding the watermark. For this, the `targets` entries became `shared_ptr<TargetImages>`: **the old `releaseRenderTarget` moved the value out of the map node
  and then erased it, so the `TargetImages*` a frame held dangled on the spot**; shared ownership gives “a frame that already resolved it” a valid object after the release, and the park reference
  presses it down to the watermark. `released_nodes` became atomic (two host threads may release at once); the queue's readers (`retirementNumbers()`, `deviceWaits()` in
  `frameReport()`) also take `clock_mutex` — now that release no longer takes the body gate, they run concurrently with park. Evidence: `VkMultiSessionTest.AReleaseDoesNotWait…`
  (with a frame stopped by the sink, the host releases content that frame resolved + one built target and must return within 2 s; the frame then still finishes, `device_waits` does not rise and park is not released);
  mutation (adding the body gate back to `releaseContent`) ⇒ a 2 s timeout goes red. Lesson: **an R6 case must be written with “another thread + a deadline”** — a direct call would wedge the whole suite on a regression,
  while a timeout turns the regression into one red assertion; the failure path must also unlock first and then join both threads.
- **An environment fact (re-confirmed 2026-11-16)**: `HostWindowReadTest.AReadOfWhatTheWindowShowsComesBackAndReportsNoError` fails in the **full**
  `test_vsg` (the server does not report viewable for 10 s, `readError=8 BadMatch`) and **passes in 1 ms when run alone**; an A/B with `git stash` on the previous commit
  **fails the same way** ⇒ unrelated to this change, it is X-server timing/environment (the same class was recorded earlier in this file). Do not change code for it; run it alone + A/B first.
- **S6c-2a landed (2026-11-16, frame bodies truly parallel)**: the device's big lock became a `std::shared_mutex body_gate` — **frame bodies SHARED** (two drivers
  recording/submitting at once), **a frame leaving the window and host verbs EXCLUSIVE** (may wait for frames, does not compete), and `shutdown` EXCLUSIVE too. To make that work: `FrameTimeline`
  **opens frames multiply** (`begin()` mints a new number each time, `submitted`/`abandoned` close by token — the old single slot let the second driver take the first one's token,
  collapsing two frames into one in the watermark comparison); `DeviceClock`/`RetirementQueue` are serialised by the device's `clock_mutex` (**the lock lives on the device, not on the SDK value types**:
  vsg is single-threaded and has no consumer, `G8`; but both must still state "the caller serialises"); the allocator / target map / report sink / per-session default program /
  surface list each got a small lock; sampled targets (screen sources, pass inputs) also take a lease; texture staging and replaced images are owned per driver by `ContentImages::BatchId`,
  and a **replaced image is parked to the watermark** (another driver's in-flight frame may still hold the old view). Deterministic evidence:
  `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame` — a diagnostic sink stops one frame body, and another driver's frame must complete as a whole;
  mutation back to an exclusive gate ⇒ red after 5 s. Three lessons: ① **after adding a lock, check for self-calls**: `MemoryAllocator::allocate` called `opened()`/`stats()`
  (both taking the same non-recursive lock) and self-deadlocked; the fix is "implementation inside the lock + public verbs only wrap the lock"; ② **do not use "was entered at some point" as overlap evidence**: the first witness read
  `sink_entered` after the second frame returned, but a serialised device would also make it true after 5 s — the criterion must be set by **the stopped frame inside its critical section**
  ("another frame finished while I had not left yet"); ③ **open frames multiply only after the timeline is changed**: changing the body gate without the token let two drivers share a frame number, and the watermark and park all
  lost their meaning.
- **S8b landed (2026-11-16, T48 step eight, middle: vsg's executor frame state + driver seam)**: the target registry was split out of the executor into
  `VsgTargetRegistry` (one per device, borrowed entries), and each driver's executor borrows it; `createSession()` hands out `VsgDriver`
  (a `RenderFrameExecutor` implementation owning one `VsgExecutor` and its own default content program), owned by the device, destroyed by `releaseSession` and
  taken away by `shutdown`. `VsgBackend::executeFrame` / `prepareFrame` became two forwarding lines, with the truth in `executeFrameFor(executor, ...)` /
  `prepareFrameFor(executor, ...)`; `Data::driving_executor` is set when a frame opens and cleared at every exit like `Data::scratch`, and
  `presentWindow` gets this frame's driver back from it. **The default program belongs to the driver (C8)**: the value lives in `VsgExecutor` and is handed to the driving session's recorder when a frame opens;
  device-level `setDefaultContentProgram` only sets the device's own driver, while store registration stays the device's. **Evidence**:
  `VsgBackendTest.TwoDriversKeepTheirOwnDefaultProgram` — the two defaults do not overwrite each other, the same target is red for A and green for B, and B carries on after A is released;
  `test_vsg` 558/558, parity 13/13. **Remaining S8c**: the device cache lock / frame-head snapshot + unblocking the frame gate (truly parallel frame bodies); until then the frame gate stays serial.
- **S8a landed (2026-11-16, T48 step eight, first half: vsg's collection scratch moves into the session)**: the new `internal/FrameScratch.hpp` gathers vsg's
  arena / recorder / compiler / `facts` / `light_scratch` / `input_*` / `packets` / `frame`; **the window session and the offscreen session each hold one**
  (`SessionOptions` / `OffscreenSessionOptions` gained `observe`, null = the session makes its own, and tests that create sessions themselves behave as before). The device keeps only
  `Data::scratch`, the "handle of the driving session": set when a frame opens, cleared at every exit by `ScratchCloser`; `commitFrameThrough` gets the same one back from the session arguments.
  Device-level settings (`setDefaultContentProgram` / `releaseRenderTarget`) apply to **every** session's scratch, and the default program is re-applied when a frame opens.
  **The trap it caught**: the default program used to be written only into the device's single recorder — when a session came up after the setting (or after another initialize), its recorder had never heard of it and
  the picture reported "the content layer was never told about this identity". The fix = one pointer copy when the frame picks its scratch
  (`Data::default_program_text` caches the text, avoiding a description rebuild every frame). `VsgExecutor.hpp` no longer includes `Session.hpp` <!-- drift-ok -->
  (the executor needs a reference only; this breaks the FrameScratch→Executor→Session cycle). Verification: `test_vsg` 557/557, `BackendParityTest` 13/13,
  `test_graphics` 587, `test_vk` 133; TSan subset 0 reports; the ASan full run's 736 B / 10 leaks **A/B-proven to exist on the old commit too** (the tests' own
  `Buffer` leak, unrelated to this change). **Remaining S8b**: where `VsgExecutor`'s frame state (`window_` / `window_recorded_` / `recorded_` /
  `profile_entries_` / `skipped_`) and `driving` go, the device cache lock / frame-head snapshot, and unblocking the open-frame handle (truly parallel frame bodies).
- **S9 landed (2026-11-16, T48 close-out: F10 + TSan + docs)**: three contract texts were rewritten to **what the code guarantees now** — `C21` / `R18` / `N18` went from
  "frames do not run concurrently / one device-level slot / the record path locks" to "**frames run concurrently per session** (the same session is still serialised by `FrameGate`) / the default program belongs to the session / the record path takes no lock",
  and the threading sections of `RenderDevice.hpp` and `RenderFrameExecutor.hpp` were brought in step (vk `Device.hpp`'s `@warning` was written in S7). The ADR gained an “implementation status”, and
  audit items A6 (partial: the `Targets`/`Report` units are still not moved), A7 (not triggered, waiting for a second API family), A9 (TSan, landed), A11 (retirement points moved up, landed)
  were each marked. **TSan evidence (three runs, 0 reports)**: `test_vk` 133/133, `test_graphics` 587/587, `test_vsg` (window/probe/plugin suites excluded,
  see `tsan_check.sh` usage) each with 0 race reports. **The tool caught one real race**: `Geometry::description()`'s lazy cache was unprotected — two engines'
  content threads describing the same geometry at once (exactly what the T10 case does) would read `description_` concurrently and each build it. The fix is **a shared lock for readers + an exclusive lock for the single builder**
  (`description_mutex_`, one shared_lock on the hit path); shape edits remain a content-thread action. Two more TSan-tree compatibility fixes: the allocation-count case
  `ADeliberateAllocationIsCaught` also needs `GTEST_SKIP` when `!countsAvailable()` (one was missed, and it goes red under TSan); `VsgBackendPluginTest`
  does not enter the TSan tree (no deployed plugin .so; the filter is written into the script's comments). Backlog `T34` therefore closed (deleted from the backlog). **Lesson**: the value of a sanitizer
  gate is "running the path nobody ran" — this time it caught only one defect, but that defect was exactly the ADR's core promise (two engines sharing content).
- **S7 landed (2026-11-16, T48 step seven: readback becomes independent)**: `ReadbackStaging` now owns **its own command pool and fence** (one set per copy; against
  one GPU round trip, creation/destruction is noise), and readback no longer borrows `Session`'s command pool/fence (C7). The wait is still the device's "between frames" (body gate EXCLUSIVE),
  but **every readback counts once** in `deviceWaits()` (the same reading as the reference backend's `waitDeviceIdle`, G6); **a refusal answerable before the wait** (unknown/unbuilt
  target, out-of-range attachment, unsupported format, borrowed depth) is answered in phase one and does not stop the device; phase two re-resolves inside the gate (a frame may rebuild a target while waiting).
  A read called inside a frame body (sink/hook) answers `Failed`: `s_frame_depth` (`thread_local` frame-body depth, `FrameBodyDepth` RAII) turns "waiting for myself"
  into an answerable refusal. **It also fixed S6c-2a's external-synchronisation gap**: `vkQueueSubmit` and `vkDeviceWaitIdle` now go through `Impl::queue_submit`
  (the ADR's hard-constraint table had stated the submit lock all along, but the code lacked it — two drivers submitting at once is a Vulkan external-synchronisation violation; readback is the third submitter, so it had to be fixed first).
  Evidence: `VkMultiSessionTest` gained 3 cases (two drivers draw interleaved with readbacks and each gets its own last frame; a readback must wait while a frame is stopped by the sink,
  then reads that frame's pixels and the count goes +1; a read inside a frame body answers `Failed` and does not touch the host vector); mutations 2/2 red (body gate to SHARED ⇒ the wait case goes red; removing
  `noteDeviceWait` ⇒ the count assertion goes red). `test_vk` **133/133** (Debug). Three lessons: ① **`retirementNumbers()` is itself a host verb**
  (takes the body gate EXCLUSIVE) — a test that reads the counters after a stopped frame waits for the frame to end first; take the snapshot before stopping the frame; ② **phase one's field reads are safe**
  because the whole `ensureTarget` body holds `targets_mutex` (`built()`/`colorCount()`/format change only on build/resize); confirm that before
  reading `TargetImages` outside the gate; ③ **adding a submitter means adding the submit lock first**: `vkQueueSubmit`'s object-level external synchronisation does not vanish because "each has its own fence".
- **S6c-1 landed (2026-11-15, T48 step six c, first half: the frame-head snapshot)**: the device cache got **one single lock** (`ContentCaches::Lock`,
  non-reentrant; a `Lock&` parameter turns "I hold the lock" into compile-time evidence), and a frame takes it once at the **frame head**: `planCommand`/`planScreen`/`resolveTextures`
  write pipeline, vertex/index/instance buffer handles, material, texture view/sampler **by value** into `CommandPlan`/`ResolvedTextures`,
  and texture uploads are recorded into the frame command buffer at the frame head too; `record`/`recordScreen` only read those values. Observation point: `contentCacheCounters().cache_lock_acquisitions`
  — a fixed **4** for the whole frame (frame head 1 + books 2 + `endFrame` 1), **not growing with the draw count**; `VkFrameHeadTest` gives the same number for 1 and 6 commands,
  and the mutation (taking the lock per draw on the record path) ⇒ 6 → 10 red. Three lessons: ① **the plan must be a value** — storing cache-entry pointers in the plan is a lifetime defect the moment "a release can
  slip into the frame"; ② **texture uploads can only happen at the frame head** (recording copy/barrier inside a render pass is illegal), so `preparePass` is left to warm-up only;
  ③ **a non-reentrant lock demands a clear split between "take a `Lock&`" and "take it yourself"** — `progress()` and `imageProgress()` each take the lock, so the frame's
  books use a `numbers()` that reads both halves in one go; otherwise calling them on a path that holds the lock is a deadlock.
- **S6b follow-up (2026-11-15): host verbs take the same device lock too**. With several drivers, "between frames" is no longer a moment the host can name, so
  `readColorBuffer` / `readDepthBuffer` / `releaseContent` ×3 / `releaseRenderTarget` / `setDefaultContentProgram` /
  `setRenderTarget` / `createSurfaceTarget` / `setMemoryBlockCreator` / diagnostic reads all take `Impl::frame_mutex`: they may wait for
  an in-flight frame body but do not compete with it. `device_lost` became `std::atomic<bool>` (one thread writes, another reads). The contract text was brought in step: the vk header says "one
  device body at a time, host verbs serialise", and the SDK says "a host verb is the device's promise (it may wait, or explicitly says between-frame), not something the host has to guess".
  Criteria: `VkMultiSessionTest` 6 cases (new: while another driver runs 30 frames the main thread does 10 readbacks + 5 releases without failing and the picture stays right;
  switching this driver's default program while another driver runs frames takes effect on the next frame), `test_vk` 127/127, ASan gate PASS. **Stated honestly**: the lock's correctness rests on
  structure and ASan; a mutation would be timing-dependent (not a deterministic red), and this class of guard is "defence in depth" — say so in the report instead of pretending there is a red mutation.
- **S6b landed (2026-11-15, T48 step six b, serial version)**: vk really supports multiple drivers — `Device::createSession()` creates a `Session`
  (one created before `initialize()` is brought up by it; on an already initialised device it is created on the spot with its scratch); `releaseSession()` destroys and forgets it, and the device's own
  driver cannot be returned. The device holds one `frame_mutex`: sessions each have their command pool/fence/timestamp pool/frame gate/scratch/default program, but the cache/allocator/queue do not yet hold
  between two running frames ⇒ **frame bodies are serial** (C10: write it serial first). `VkMultiSessionTest` 4 cases (T8 two drivers with different default programs each draw their own,
  T10 two threads with 6 frames each get their own correct picture, R1 borrow early use late, release and re-borrow), mutations 2/2 red; `test_vk` 125/125, ASan gate PASS.
  **Trap**: calling `createSession()` after the device is initialised must **build the machine on the spot** (otherwise `vkAllocateCommandBuffers` uses an empty pool and segfaults;
  the first version blew up exactly that way). **Still not done (S6c)**: the frame-head snapshot, resident entries living to the watermark, R6 non-blocking release, and `ensureGeometry` wired to `ResidenceTable`.
- **S6a landed (2026-11-15, T48 step six a)**: the SDK's session seam — `RenderDevice::createSession()` defaults to `&executor()`,
  `releaseSession()` defaults to a no-op; `setDefaultContentProgram` **moved** from `RenderDevice` to `RenderFrameExecutor` (default no-op;
  `RenderBackend` is no longer ambiguous). `RenderEngine` holds `session_`: `setDevice` borrows it, changing device / destruction returns it, and frames and the default program both go through it. vk's default
  program moved from `Impl` to `Session` (one per driver, and `Device` overrides it to forward to its own). Criteria: `test_graphics` 587 (+1 borrow/return),
  `test_vk` 121 (+1 default shape), `test_vsg` 556, mutation 1/1 red (destructor does not return). **Trap (must remember)**: after changing the SDK's **vtable**,
  **the dynamic plugin must be rebuilt** — the old `gfx_backend_vsgd.so` segfaults in `VsgBackendPluginTest` (vtable layout mismatch);
  `ninja -C build gfx_backend_vsg gfx_backend_vk` makes it all green again. **Still not done (S6b)**: vk still answers a single default driver; a second driver waits for
  the cache/allocation/submit locks (or the frame-head snapshot), `ensureGeometry` wired to `ResidenceTable`, and the replacement path moving to park.
- **S5 landed (2026-11-15, T48 step five)**: `internal/TargetClaims.hpp` — the device holds `RenderTarget* → SessionId`;
  `begin(session)` issues a frame-scoped `Lease` (its destructor is the only release point, `P3`), and `executeFrame` / `prepareFrame` claim **before `ensure_target`**
  (a rebuild belongs to the holder only, `R5`); a conflict skips just that pass and reports `vk.target.claim_conflict` (one per (target, episode):
  the episode ends after a conflict-free frame or warm-up). Device-level observation point `Device::targetClaims()`. Criteria: `TargetClaimsTest` 7 cases +
  `TargetClaimFrameTest` 2 cases (a lease posing as “another driver” makes the pass be skipped, two attempts report one line, a new conflict reports again after a clean frame), mutations
  **2/2 red** (lease not released / episode not ended), `test_vk` 120/120. **Trap**: the claim in `Session` must come before `ensure_target`;
  one step later and two sessions can rebuild the same target's attachments interleaved.
- **S4 landed (2026-11-15, T48 step four)**: `internal/ResidenceTable.hpp` is the state machine for “who may build this key” (no Vulkan types): <!-- drift-ok -->
  `admit(key, session)` → `Use` / `Build` (a move-only `Claim`) / `Copy(owner)`; `Claim::publish()` is the owner's completion statement, and a destructor without a decision
  ⇒ the entry falls back to `Absent` (rule 3 is enforced by the type); tokens are monotonic and never reused, so after `forget` an old claim's `publish` must fail and counts as
  `Numbers::stale`. Observation points: `numbers()`'s `claims` / `published` / `withdrawn` / `copies` / `stale` / `uploading`
  (the last must be constantly 0). Criteria: `ResidenceTableTest` **8 cases** (including 8 threads racing one key and exactly one `Build`), mutations **2/2 red** (removing the destructor withdrawal /
  removing the token comparison), `test_vk` 111/111. **Not wired up**: `ensureGeometry` still takes the old path; it is wired only when S6 passes the session id (half-wired is worse than unwired).
  It is **not** the SDK's `ResourceTable`: that one answers “may this resident value be placed” per pin, this one answers “who may build it”.
- **S3 landed (2026-11-15, T48 step three)**: the content path was split into three — `ContentCaches` (**device**: programs /
  geometries / instance streams / pipelines / layouts / `ContentImages` / the pipeline cache / `Progress` / `release*`),
  `FrameScratch` (**driver**: constant ring + descriptor pool + set table + `frame_index` + `ring_builds` / `set_builds`),
  `ContentDraw` (**recorder**: holds borrowed references to both, keeping only `prepare` / `prepareScreen` / `record` / `recordScreen`).
  `Session` owns `scratch` + `recorder`, and `Impl` keeps only `caches`; `createScratch()` is called after the layout is built, and on teardown
  **scratch goes before caches** (a set's layout is borrowed). Criteria: `test_vk` 103/103 (Debug + ASan), forced rebuild 0 warnings;
  mutation (`FrameScratch::create` not building the ring) ⇒ `ATriangleIsDrawnIntoTheTargetAndProbed` red, green again once restored.
  **Trap**: inside `Session`, `owner` is the **device** — `owner.scratch` / `owner.recorder` are wrong; the ring/recorder are `Session`'s own members;
  only `owner.caches` crosses that line. **T3's lock counting** was not done: a single session has no second reader, so locking would be a dead mechanism (`G8`); it lands with S4's owner/competitor.
- **S2b landed (2026-11-15)**: `executor()` answers `Device::Session`, the frame body lives in it too, and `Device` keeps only three forwards.
  The frame body does not call `Device` members directly: reporting (protected, which the nested class cannot reach) / building a target / declaring loss go through the three faces
  `report` / `ensure_target` / `note_device_lost` on `Impl` (assembled when the device is destroyed). `initialized()` inside the frame body became
  `owner.device != VK_NULL_HANDLE && !owner.device_lost`.
- **S2a landed (2026-11-15)**: the frame machine lives in `Device::Session` (command pool / fence / timestamp pool / `FrameBooks` /
  `FrameGate` / clock window); the device owns it and `destroy()`s it in `shutdown()` (**without resetting the pointer**: after shutdown a report must answer
  “nothing was measured” rather than crash). The frame body (`executeFrame` / `prepareFrame` / `frameReport`) is still in `Device` — S2b;
  its two obstacles were located: the ~25 `reportDiagnostic` calls in the frame body are protected members of `RenderDevice` (the nested class cannot reach them,
  so an assembled sink on `Impl` is needed), and `initialized()` must become `owner.device != VK_NULL_HANDLE`.
- **S1 landed (2026-11-15, T48 step one)**: vk adopted `backend/DeviceClock.hpp` + `RetirementQueue` (a session = a clock window,
  `slots = 1`). `releaseContent` / `releaseRenderTarget` are **two things**: the cache entry is **forgotten on the spot** (the resident count drops at once, re-upload next frame),
  and the Vulkan object **goes to the queue** to wait for the watermark (one slot ⇒ `submitted + 2`, destroyed at most two frames later). Observation point `Device::retirementNumbers()`
  (`pending` / `released` / `device_waits`, the last of which must be constantly 0). `shutdown()` idles first, then clears the seam and calls `releaseAll()`
  (the teardown verb added to the SDK) — skip it and parked objects leak with the session (verified by mutation).
  **Still not done**: the replacement path (the old buffer on re-upload, ring rebuilds) still destroys immediately; S6 must park it too.
- **T48's implementation design is in `.ai/design/graphics-sessions.md` (status: in progress, S1–S3 landed, 2026-11-15)**: a session = a **frame driver** (not a window); the frame-head snapshot makes the record path lock-free; residency is three-state “publish or withdraw”;
  a target claim conflict skips only that pass; retirement **reuses** `DeviceClock` + `RetirementQueue` (vsg already uses them).
  One correction: vsg's `api::Session` is a **window session**, and the collection/compile scratch (`recorder` / `compiler` / `arena` /
  `facts` / `packets` / `frame` / `driving`) is still a **device-level singleton** in `VsgBackend::Data` — the ADR's “vsg is closer” was only half right.
- **Diagnostics carry a stable code (2026-11-15, audit A3)**: `RenderDiagnostic`'s `code` (`vk.<class>.<cause>`) is what the host and the tests
  match on; `message` may be reworded, the code may not; `subject` says “which target this report is about”, and `limit` is the
  `requested` / `supported` pair. The vocabulary is registered in `scripts/diagnostic_codes.txt` (checked both ways: what is used must be registered,
  what is registered must be used), and `check_diagnostic_formats.py` uses `scripts/diagnostic_code_baseline.txt` to set a **cap** on call sites not yet converted
  — a newly written report without a code goes red. **Criterion**: **constructing** a diagnostic (≥3 arguments) must write a code; **forwarding** a `RenderDiagnostic` (`reportDiagnostic(diagnostic)`) does not count — the value already carries the code.
  On 2026-11-15 there were **61 codes in seven vk families + 22 in four vsg families + 13 engine codes = 96**, and both backends are fully converted; the remaining 22 are the engine's own reports (`RenderEngine` 13 / `RenderArea` 5 / four small files with 1 each).
  **The reason is named at the layer that knows it**: vk's content refusal is `ContentRefusal` (category + code + sentence, first one wins) and the device only forwards it; vsg's readback refusal is mapped by `readbackRefusalDiagnostic`, a switch with no `default`, so a new enum value must name a code.

- `test_graphics` (model-independent), `test_vsg` (the backend, including real-device cases), `test_gui`, `test_core`.
- `scripts/vsg_rewrite_gate.sh`: the suite run twice with 0 VUID / 0 SYNC-HAZARD, closed phase lines, and the application picture percentage.
- Static gates: `check_ai_docs.py`, `check_doc_symbols.py`, `check_include_hygiene.py`, `check_export_annotations.py`, `check_diagnostic_formats.py`, `check_public_docs.py`,
  `check_api_style.py` (API style: no setters/callbacks in values, no `getXxx` accessors, `bool` + out-parameter may not coexist, an `XxxBuilder` must have `build()`;
  4 existing findings are registered in the script's `BASELINE` and are deleted as they are fixed).
- `scripts/asan_check.sh` / `VN_ASAN_*`; only `skipped == 0` counts as having run the device battery.
- Measuring a backend's per-frame cost: `test_vsg --gtest_filter='*MeasureWhatEachKind*'` (prints µs/frame + `builds`/`uploads`/`created`; needs a real window and is skipped headless); the cost model and allocation caps are in `.ai/design/vsg-backend.md` §2.10 / N4 / T5.

## 2026-11-11 — graphics-next proposal set (not adopted; continue here next time)

- Six documents, the outline plus five expansions: `-requirements` (`R1–R30`,
  `N1–N18`, `C1–C12`, `T1–T16`), `-design` (type drafts + hot/cold partitioning + seven algorithms + 7 open questions),
  `-benefits` (benefits/costs `P1–P7` / three migration batches), `-api` (`P1–P32` + `M1–M10` + the three style tiers + where 49 methods go),
  `-hot-paths` (performance budget, animation pose, post-processing dependency graph, extensions, `N11–N18`). Backlog `T32`; **the current design is unchanged**.
- Three judgements settled in the discussion and worth reusing directly next time:
  1. **Three style tiers**: cross-thread / enters a plan / compared ⇒ value (no setters, `withXxx`); has identity / single-threaded / incrementally edited ⇒ object (Qt accessors);
     exists only at creation ⇒ builder (`build()` produces a value). `RenderPass` / `PassChain` / `ShaderProgram` / `RenderControl`
     **cross tiers** today (value semantics, builder, value, adapter respectively).
  2. **Animation does not go through publication**: `Instance::setMatrix` is an edit (hot block CoW + publish); per-frame animation goes through the in-frame `PoseSet`
     (snapshot untouched, one matrix multiply per row). This keeps “half a configuration on screen” out of the animation path and removes backlog `T6` along the way.
  3. **hot/cold partitioning** is the premise that lets “folding stays in the snapshot” and “publish O(change)” hold at once: animation copies only the hot block (~10 KB per block),
     while cold blocks (identity/state/references) are shared zero-copy.
- New gate: `scripts/check_api_style.py` (four rules; 0 new / 4 baseline today).
- The only **production-code** change this session: `af2de738` pulled `sessionChangeAllowed` back to private (I had made it public by mistake).
- The first command next time: `-hot-paths` §8 step 1 — turn the `Measure*` cases from printing into threshold assertions (no architecture change, make performance regressions visible).

## 2026-11-11 — graphics-next second-round audit (action order rewritten)

- Added the second-round audit: `X1–X10` cross-document contradictions, `A1–A13` design gaps,
  `R31–R39` / `T17–T31` / `K7–K11` additions. **The proposal is still “not adopted”; the current design is unchanged.**
- The most critical one, A1: the new design's `DrawItem` carries only `id` and loses the revision; when content changes, an in-flight old frame looks up the new value by id,
  directly violating `R9`/`I1`. Today's `RenderCommand` instead carries `GeometryDescriptionPtr`/`MaterialDescriptionPtr`
  (`ContentStore` also keeps rows for old revisions until the frame completes). Fix: `ContentRef{id,rev}` or store value pointers in the cold block;
  the engine table keys on `(id, generation, revision)`. **Fix this before talking about a snapshot prototype.**
- The rest that must be fixed first: A2 picking identity is `InstanceId` + `FrameResult` lacks View/pose; A3 the shared prototype needs “one writer domain +
  comparing the prototype revision at publish”; A4 `Instance` must keep its prototype alive; A5 prototype instantiation's “10 matrix multiplies” does not hold (5000×10 is 50,000,
  and memory multiplies per instance); A6 `DrawItem ≤64 B` is already exceeded by its fields and `sort_key` is 32 bits short; A7 copying the publication metadata wholesale is not O(change);
  A8 culling `chain[0]` blocks does not hold and the `local_box_union` field does not exist.
- Evidence discipline: 2519 = the proposal baseline (8 files .hpp+.cpp, cpp only 1212; the current prototype is 2702 after adding select/facts, K5 still uses 2519 as the denominator); ContentStore 1016 lines, lifetime tokens 75 (six tokens) / 100 (with parked);
  RenderEngine 1309 lines; test_graphics 548 (at proposal time; 559 now) / test_vsg 537 / ctest 30 suites; 2000 drawables re-measured about 2074–2128 µs (204 µs for 200);
  “47 includes” is only 23 files today, “126 bools” is 110/152 by scope — numbers must carry their command, or they drift.
- Action order (replacing “the first command next time” above): ① fix the X/A1–A4 documents + add K7–K11; ② `-hot-paths` §8 step 1 (Measure thresholds)
  + the evidence script; ③ `detail::` snapshot + pure select (**without prototype instantiation**, content references carry a revision); ④ lifetime/registry; ⑤ plan values +
  `prepare` + one Session with several Surfaces; ⑥ prototype instantiation as a separate experiment (first settle A5's shared local rows + per-instance transform).
- One-sentence conclusion: an immutable value that crosses layers must land on a reference — **a reference carrying only an id is not an immutable value**, it just pushes the version problem into the engine table.

### Landing (same session, 2026-11-11)

- **Step 0 done**: `X1–X10` / `A1–A13` in the six proposals were revised per the audit — `ContentRef{id,rev}`,
  `DrawItem` 24 B + a 64-bit `sort_key`, `world_bound` block culling, `PoseEntry` subtree propagation, `FrameQuery`/`PickResult`,
  `prepare`, one Session with several Surfaces, the `Result<void>` specialisation, `C2b` writer domain, `R31–R39`/`T17–T31`/`K7–K11`.
  The proposal is still “not adopted”; `.ai/design` was not touched.
- **Step 1 done**: `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` now has a threshold
  `drawables*3+1000` µs (sanitizers only print); measured 200→204 µs / 2000→2128 µs, both pass. Added
  `scripts/measure_graphics_claims.py`: bridge 2519 (at proposal time; 2700 now), ContentStore 1016/75/100,
  RenderEngine 1309/50 public/22 includes, SDK 75/99, bools 110/152, tests 548/537/30 (at proposal time; 559/537/30 now).
- Next time start at step 2: `detail::Snapshot` + a pure `select`, **content references carry a revision, no prototype instantiation yet**;
  run `K1–K3` + `K7` first. Do not skip `ContentRef` — that was the core correction of this audit round.

### Step 2: chunked snapshot + incremental publication (2026-11-11, Debug results)

- `SceneSnapshot` is now an immutable snapshot of 32-row chunks: `makeSceneSnapshot` reuses unchanged chunks from the previous version;
  the `SceneRows::snapshot` cache hangs off the internal `SceneBake` (moved over on a full rebuild), and `Scene`'s public layout is unchanged.
- `Scene::ensureRows` gained an incremental path: find edited rows by each row's `Node::contentEpoch`; check structure/visibility/parent order
  fully; only the hot fields local / opacity / layers / draw order / view placement may refresh in place,
  everything else (state / geometry / material / primitive / box) falls back to a full `buildRows`.
- Five `SnapshotTest` cases pass: pure-select equivalence, old revision, transform increment, 2000-row timing, resident memory; `test_graphics` 559 pass (plus 6 in `ContentRegistryTest` / `ResidencyTableTest`).
- **1 changed row out of 2000**: Debug publish ≈ 2.2 ms / select ≈ 1.3 ms; **RelWithDebInfo publish ≈ 154–203 µs /
  select ≈ 293–307 µs**, both K1 (≤500 µs) and K3 (≤×1.2) pass; shared 62/63 chunks.
- **K2/N3 pass**: `MeasureWhatASnapshotKeepsResident` measures the snapshot resident at about **1.03× the row table** (K2 ≤3×, N3 ≤2×).
- **N2 was adjusted to ≤250 µs** (2026-11-11): 50 µs needs content-side dirty notification, which conflicts with “Node has no back pointer to Scene”;
  203 µs is inside 250 µs, and the RelWithDebInfo timing test now asserts N2. Next: step 3
  (the engine table of `ContentRegistry` + `ContentRef{id,rev}`). Do not take Debug's 2.2 ms as the final number.

### Step 3a (identity/residency prototype; 2026-11-11)

- New internal `ContentRegistry.{hpp,cpp}`: `Handle` (gen24|domain8|index32), typed `Id<Tag>`,
  `allocate<Tag>()`, generation/domain checks, 24-bit wraparound retirement slots; `ContentRef{id,rev}`;
  `ResidencyTable` (collectable/reclaim when `last_epoch < oldest_live`, revision is part of the key).
- Added 6 cases in `ContentRegistryTest` / `ResidencyTableTest` (stale handle, cross domain, duplicate release,
  collectable enumerable, revision key, touch extends life); `test_graphics` 559 pass.
- **3b settled on B by design (public identity = `Handle`), first segment landed** (2026-11-11): new public
  `sdk/vine/graphics/ContentId.hpp` (`Handle`/`Id<Tag>`/`ContentRef`/`ContentRegistry::instance()` + `std::hash`),
  and `Node` holds a `NodeId` (allocated in the ctor / released in the dtor, `Node::id()`). Row identity moved to `NodeId` everywhere: `Row::identity`,
  `RenderWorld::RowId`, `Delta`, the mirror delta, `SceneSnapshot::identities`; public `RenderCommand::row`
  (`const void*`) → `RenderCommand::node` (`NodeId`), and `Collected/CompiledCommand`, `GeometryFacts`,
  `ContentStore`'s geometry table key on `NodeId` accordingly. A hand-written command with no node uses `geometry->id()`.
  Evidence: `test_graphics` 559 / `test_vsg` 537 / ctest 30/30. After the change **clean the old objects before building** (the `Node`/`RenderCommand` layout changed).
  Still not done: ① `Geometry`/`MaterialBase`/`ShaderProgram`'s own ids, so `ContentRef` becomes the only key (the geometry table is
  `(NodeId, rev)` and the material table still keys on the address = A1's residue in the backend); ② merging the plan row's geometry into `ContentRef geometry`;
  ③ replacing `markUsed`/parking/`kept` with `ResidencyTable` epoch reclaim. Do not half-migrate `ContentStore` between those three steps.

### Step 4 (material identity; 2026-11-11)

- `MaterialBase::id()` (allocated in the ctor / released in the dtor). `MaterialFacts::material` became a `ContentRef` (revision in the key,
  the separate `revision` field deleted); `CompiledCommand`/`CollectedCommand::material` are `ContentRef` too.
- `ContentStore::ensureMaterial` now **appends a row per revision** and deletes old rows after parking (no more in-place replacement + `RetiredMaterial`/`kept`);
  `materialEntries()` is “one row per revision”. `MaterialArena`/`BlockStorage`/`ContentSweep`/`released_materials` key on `MaterialId`.
- The material revision rule in `FrameRecorder::snapshotCommands`: a published value uses its value revision, otherwise the object's current revision —
  it must agree with `ensureMaterial`'s rule, or hand-written commands and the table miss each other forever (measured).
- **`ContentRef::revision` is `std::uint64_t`** (the design draft said uint32): every `revision()` in the SDK is 64-bit.
- Test helper: `tests/test_vsg/TestMaterialFacts.hpp` (`materialId()`/`materialRef()`/object → `ContentRef`).
- Next: `ShaderProgram::id()` + `ProgramRef` → `ContentRef` (the program table also keys on `(id,rev,variant,kind)`);
  merge the plan row's `geometry`+`geometry_revision` into `ContentRef geometry`; then epoch reclaim.
- This step changed only the internal `SceneBake` / `SceneSnapshot` / `Scene::ensureRows` / `DrawListBake`; the public API is unchanged.
  All of `Scene`'s caches/epochs live in the internal `SceneBake`, to avoid changing the public class layout (temporarily adding a field once crashed test_vsg's ABI; reverted).

### Step 5 (program identity; 2026-11-12)

- `ShaderProgram::id()` (allocated in the ctor / released in the dtor). The `ProgramRef` type is deleted: plans/records (`CollectedCommand`,
  `CollectedDraw`, `CollectedFrame::default_program`, `CompiledCommand`, `CompiledDraw`, `CompiledFrame`),
  `PipelineKey::program`, `Scope::Entry::program`, `ContentHalves::Key::program`, `ContentSets::Key::program`
  are all `ContentRef`.
- `ProgramFacts::program` is a `ContentRef` (the separate `revision` and `generation` fields are deleted); the program table builds rows by
  `(ProgramId, revision, variant, kind)`. `ContentStore`'s live table keys on `ProgramId`; `ensureProgram`
  takes the revision from the tracked object (the plan's revision does not take part in building a row; the rule is still “a row describes what the object is now”).
- Diagnostics: `ContentFacts::program_names` (one `ProgramNameFacts` per identity) + `programName()`. Reason: a plan carries only
  a handle, and a program whose “text was rejected” has no `ProgramFacts` row, yet `describeMissingContentHalf` must still say which program it was.
- Tests: `test_graphics` 559 / `test_vsg` 538 all green; `ContentHalvesTest.AnotherProgramIdentityIsAnotherHalf`
  pins “two program objects with the same text and the same revision do not share a half” (it used `generation` before, the handle now).
- Next: merge the plan row's `geometry` + `geometry_revision` into `ContentRef geometry`; then replace `markUsed`/parking with
  `ResidencyTable` epoch reclaim. After `ProgramFacts::generation` is deleted, `ADR-2026-11-09`'s “holding is identity” is left only in the upload table
  (`Buffer`).

### Geometry rows merged into `ContentRef` (landed 2026-11-12)

- Plan row: `CollectedCommand` / `CompiledCommand`'s `geometry` (`NodeId`) + `geometry_revision` merged into one
  `vn::graphics::ContentRef geometry`. The only producer is still `FrameRecorder::snapshotCommands` (identity from
  `RenderCommand::node`, falling back to `geometry->id()`; revision from `description->revision()`),
  and `FrameCompiler::resolveCommands` only copies.
- Facts and tables: `GeometryFacts`'s `geometry` + `revision` merged into the same ref; `GeometryKey` / `compareGeometry` /
  `orderGeometryRows` and `findGeometry(facts, ContentRef)` use one key; `ContentStore`'s `eraseGeometryRows`,
  `liveGeometry`, `ensureGeometry` and the geometry table all use one ref. `ensureGeometry` still builds rows from `live.shape->revision()`
  (the plan's revision does not take part; the rule matches program/material).
- **Types**: geometry's identity is the `NodeId` that `Geometry` inherits from `Node` (`Geometry::id()` = `Node::id()`),
  and `ContentRef::revision` is the geometry revision. **Do not invent a `GeometryId` for this** (`ContentId.hpp` has the tag, but nothing
  allocates it today); for a typed view use `ref.as<vn::graphics::NodeTag>()`.
- Consumers only changed their parameters: `ContentHalves::halvesFor`, `ContentPass` (two `findGeometry` calls), `ContentSets::setsFor`.
  `ContentHalves::Key` / `ContentSets::Key` carry no geometry identity (only layout / set / source), so those keys did not move;
  `RenderWorld` / `SceneSnapshot` / `Scene` row identity did not move either (they were already `NodeId` keys).
- Test helpers (`TestGeometryFacts.hpp`, the hand-built plans in `ContentStoreTest` / `ContentSweepTest` / `ContentHalvesTest`) keep the “object + revision” entry
  but build one ref internally — the plan-row API has one ref only.
- Verification: `test_graphics` / `test_vsg` / ctest 30/30 all green.
- Next (landed, see the next section): `ResidencyTable`'s epoch reclaim replaces `ContentStore`'s `markUsed`/parking/`kept`.

### Row lifetime moved to epoch reclaim (landed 2026-11-12)

- `ContentStore`'s `markUsed` / `release_token` / `release_requested` / `retained` / park are all deleted. Every
  geometry / program / material row carries `last_epoch` (the last frame a plan hit it, stamped per row by `tablesFor`);
  `Data::reclaim()` runs once at the top of each frame's `tablesFor`: rows with `last_epoch < completedFrame() + 1` **and no longer the entry's
  current answer** (`isCurrent`) leave. `RetirementQueue` is still only for GPU objects (uploads, halves, descriptor sets).
- **The “current answer” half cannot be omitted**: culled / hidden content is not hit by the plan this frame, but it is still the live entry's current answer, so the row (and the upload
  it declares) stays; otherwise `ACulledGeometryKeepsItsUploadsForAHundredFrames` and design F27/F28 would break.
  Released entries and superseded revisions are no longer the current answer and go by epoch (no time window).
- Entries (`Live*`) outlive one more step: a `released` mark (`release*` / `releaseAbandoned`) + no rows ⇒ `dropReleased()`
  drops the entry; “no rows” is answered by the table's per-identity row count (`Table::rowsOf`). A material arena slot goes into
  `material_slots_pending` when its entry is dropped, and the next `releaseAbandoned` hands it to `BlockStorage::releaseMaterial` (one frame later than the old park).
- `Table<Row, Storage, MakeOrder, KeyOf>`: `append(row, storage, epoch)` / `stamp(row, epoch)` /
  `reclaimBefore(oldest_live, keep, onErase)` / `rowsOf(key)`; `epochs_` moves together with `rows_`/`storage_`.
- API: `tablesFor(frame, timeline)`, `releaseGeometry/Program/Material` no longer take a timeline/retirement,
  `releaseAbandoned(released_materials)`, `releaseAbandonedContent(store, images, released_materials)`;
  `ContentStore::retained()` is deleted.
- Tests: the `Frames` helper (`ContentStoreTest`) drives begin/submit/`completeUpTo(frame - slots)`; old park cases were renamed
  to "...OnceNoPlanNamesThem", `AReleaseThatCannotBeParked...` was deleted, and
  `AReleaseWithoutCompletionEvidenceKeepsTheRows` and `AnAbandonedEntryKeepsItsObjectUntilItsRowsLeave` were added.
- Evidence: `measure_graphics_claims.py`'s lifetime tokens 75 → **1**; `test_vsg` 537 / `test_graphics` 559 / ctest 30/30.
- Trap: `tablesFor` returns a reference to the store's `d->facts`, which several records rewrite; read assertions immediately after each record.

## 2026-11-12 — `releaseContent` entries completed (MaterialBase + one stream)

- **`Material` → `MaterialBase`**: `RenderCommand::material` is a `MaterialBasePtr`, and `MaterialInstance : MaterialBase`
  is **not** a `Material`; the old concrete-class `obj_cast` made every instance fall through to the end of the dispatch and return false (rows and arena slots never released).
  `VsgBackendTest.ReleasingAMaterialInstanceDropsItsResidency` verifies it the other way: changing it back to `Material` goes red.
- **buffer dispatch**: `Buffer<T>` is not an `Object` (design 2.6 deliberately keeps it outside the content-object system), so
  `releaseContent(raw_ptr<const Object>)` cannot receive it at all — what was added is **two reference-parameter overloads**
  (`const vn::Buffer<float>&` / `const vn::Buffer<std::uint32_t>&`), one layer each through `RenderDevice` → `RenderEngine` →
  `VsgBackend`. **References**, not pointers: `releaseContent(nullptr)` still resolves to the Object overload (three pointer overloads would make
  that null assertion in `HeadlessBackendTest` ambiguous outright), and “a buffer that does not exist has no residency to drop” is not an input in the first place.
- Implementation: `backend/ResourceTable::retireIf(drop, timeline, retirement)` (`retireUnpinned` is now just its negation) +
  `StreamUploads::releaseBuffer(address, timeline, retirement)` (both tables look up by `key.buffer`, parking rather than destroying;
  before the parking window is learned it is kept and counted as retained). `VsgBackend::releaseStreamBuffer` uses the **device's** clock/queue, not a window's.
- Semantics: only this one stream's GPU copy is dropped; the geometry's rows and tables stay, and the next frame that draws it re-uploads. To drop all streams of a geometry → drop the geometry.
- Evidence: `VsgBackendTest.ReleasingAStreamBufferDropsTheUploadButNotTheGeometry` (`uploads().live()` 2 → 1 →
  2, `geometryEntries()` stays 1, `uploads()` rises again); `test_vsg` 539 all green, `test_graphics` / the gates 0.

## 2026-11-12 — ADR-2026-11-10 step 3: textures move into the pin model

- **There are two declaration indices now**: `ContentDeclarations` borrows the buffers / textures tables at once (`declaresBuffer` /
  `declaresTexture`); in `ContentStore` both share one `Declarations` class (address → reference count: append adds, erase subtracts,
  zero deletes), and `GeometryTable` and the new `MaterialTable` each hold one. A material row declares / withdraws
  `slots[].texture` on append / reclaim.
- **`MaterialImages::beginFrame(declarations, imported)`**: the declaration = a material row slot **or the frame's pass-imported input**
  (the address of an `ImportedTexture` in `CompiledPass::inputs`; `ContentAssembly::beginFrame` collects them into a reused vector),
  and the other half is the entry's own `owner->revision()`. **No parking**: the entry holds only a lookup share, and the descriptor set holds the view / sampler itself.
  `releaseAbandoned()` is deleted and a cumulative `released()` count added; `VsgBackend::releasedTextures()` reads it.
- **`ContentSweep.{hpp,cpp}` and `ContentSweepTest.cpp` are deleted**: what was left was a wrapper over half of the store, and the name did not match the behaviour. <!-- drift-ok -->
  `VsgBackend::sweepAbandonedContent()` calls `store->releaseAbandoned(...)` directly and the documentation moved into the comments there;
  the default-material assertion moved to `ContentStoreTest.TheDefaultMaterialIsNeverAbandoned`.
- **Trap (a real defect the tests found)**: a frame with empty `passes` takes `commitFrameThrough`'s early-return branch, which did not run
  `recordContent` ⇒ the image sweep did not run ⇒ when "the host stops drawing anything" a texture was never released. The early-return branch now calls
  `assembly->beginFrame(...)` too.
- **Trap**: `std::span<const Texture*>` cannot be constructed from a `const std::vector<const Texture*>` (libstdc++'s range
  constraints), so a local vector must be writable.
- **Semantic change (the ADR's cost, T27)**: a texture is pinned only by live content — a texture the host holds but no material/import references leaves on the next frame.
  `VsgBackendTest.AWorkingSetLargerThanTheOldTextureBoundStaysResident` assumed exactly the old holding rule and was deleted;
  “no capacity cap” is still pinned by `MaterialImagesTest.EveryLiveTextureKeepsItsImages` (300 entries, no frames run).
- Mutation check: making `declared` constantly true ⇒ 3 new cases all red. `test_vsg` / full ctest 30/30, gates 0.

## 2026-11-12 — ADR-2026-11-10 step 4: material blocks move into the pin model

- **A pin is a material identity** (not `(identity, revision)`): the arena has one slot per identity, and the “still alive” half is the material table's
  per-identity row count — so `ContentDeclarations` borrows `Table::rows_of_` directly
  (`declaresMaterial(id)` = `find(bits)`), with **no second bookkeeping**. As long as a row still answers (including a superseded revision), the slot stays pinned.
- **builder / destroyer**: writes are still `MaterialArena::note` (the same memo as before while recording: 0 writes in steady state, one block per edit, a second write in the same frame is
  `Unchanged`); releasing a slot became `MaterialArena::retireUnpinned(is_live)`, asked once per frame by `BlockStorage::retireUnpinnedMaterials(
  declarations)` in `ContentAssembly::beginFrame` (after the tables are published): O(slots), not following the content.
- **No parking, release at once**: the arena's copy rotation is that window — “dropping the right to reuse the slot”, not bytes an in-flight frame reads.
  Parking would even have a hole: if content came back and rebuilt the entry inside the window, the parked slot would still be returned on expiry, and the entry's offset would then be written dirty by another material.
- **`ContentStore`'s three release loops → one `retireOwner`**: marking (`Live::isAbandoned()`: geometry asks the object or the
  shape, program/material ask the object) and dropping (`released && rowsOf == 0`) happen in one pass; `dropReleased()` is just
  `retire(false)`. `material_slots_pending`, the exit of `releaseAbandoned(released_materials)`,
  `BlockStorage::releaseMaterial` (the last production call site) and `VsgBackend::abandoned_materials` are all deleted.
- **Header trap**: `BlockStorage.hpp` cannot include `ContentFacts.hpp` — `ContentFacts.hpp` includes
  `ContentPipeline.hpp`, which requires `BlockDescriptors` to be included first, and `BlockDescriptors.hpp` is exactly what includes
  this file ⇒ a cycle. Use the forward declaration `class ContentDeclarations;` (references only) and put the implementation in the `.cpp`.
- **Counters**: `ResidencyKind::MaterialBlocks`'s `retired` went from “absent” to the arena's `retired()` (the residue of a3's old line
  “no release path - eviction only” is now measured at 0); `BlockStorage::materialResidency()` is filled in too.
- Cases: `CoreMaterialArenaTest.ThePinSweepReturnsTheSlotsNoLiveContentNames`,
  `BlockStorageTest.AMaterialSlotIsReturnedOnceNoLiveRowDeclaresIt` (real device),
  `ContentStoreTest.AMaterialDeclaresItsIdentityForAsLongAsARowAnswersIt`; a3's residual assertion became
  `liveMaterials() <= base_slots` (measured back to 0 every cycle). `test_vsg` all green.

## 2026-11-12 — graphics-next step 1: the plan is a value (`Plan` + diff by name + stable codes)

- **New public values**: `Plan` (an ordered `PassDesc` list; a pass's **name** is its identity), `PlanProblem { code, pass, detail }`,
  `PlanProblemCode` (`EmptyPassName` / `DuplicatePassName` / `UnknownInput` / `DependencyCycle` /
  `TooManyPasses` / `SessionBusy`), `kMaxPlanPasses = 64`, the pure function `validatePlan(plan, external_outputs)` and
  `planCodeText(code)`. Header `Plan.hpp`, implementation `src/Plan.cpp`.
- **Engine entry**: `RenderEngine::applyPlan(plan) -> vector<PlanProblem>` (empty = applied) and `RenderEngine::plan()`.
  Diff by name: a new name `makePassNode` + `insertSlot`; same name and same kind ⇒ `setDesc` **in place** (the node and backend state survive,
  P7); same name but the kind changed ⇒ delete and rebuild (keeping owner/role); a pass the plan does not name ⇒ `removePass`. The whole plan must validate
  before anything is applied, **all or nothing**.
- **`plan()` writes the effective order into `desc.order`**, so `applyPlan(engine.plan())` is idempotent (a pass that only declares a stage
  is not reordered by reading it back).
- **Naming**: `RenderEngine`'s old private `PlanProblem` (sentence identity) was renamed `CompileProblem` — the public `PlanProblem`
  is “a shape defect + a stable code”, and the two are not the same thing; `FramePlan` is still the **compiled** view and `Plan` is the declaration.
- **Trap**: `validatePlan` must use **two** maps — pass names (the diff identity, which must be unique) and `output.name` (the producer a consumer
  resolves). The first version merged them into one map and the cycle and feedback cases went red on the spot.
- **Tests**: `PlanTest` (12) — T7 (a pass exists and really runs with no add call), read-back idempotence, rename, in-place update, the four code classes +
  feedback is not a cycle, shape unchanged after a refusal, code spelling, `describe`. `test_graphics` 571 / `test_vsg` 540 (one
  existing window case red on the first run, green alone: the host window reports the known occasional `4294967295x4294967295`) / full ctest 30/30.
- **Docs**: `graphics-frame-pipeline.md` §3.1/§3.6/§8/§9.1/§9.3/§10 updated; `K1–K11` land in §8.1 as the in-repository
  authoritative copy (`K4`'s denominator became “the case count when that step started”); §9.3 corrected to “the `addPass` family is deleted in step 5” (the old table contradicted `C11`).
  `-audit` was **not deleted**: it is the source of `R31–R39` / `K7–K11` and is cited as evidence by two ADRs.

## 2026-11-12 — graphics-next step 2: `publish` returns four states (internal half)

- **Why**: `ContentMirror::publish` / `FrameWorker::publish` returned `bool`, and the same `false` meant three different things
  — “nothing changed”, “frozen inside a batch”, “the channel is full, retry later”. A caller could not decide between retrying and reporting, and `T31`'s “explicitly unpublished inside a batch” had nowhere to be expressed.
- **New values**: `src/viz/graphics/src/PublishOutcome.hpp` (deleted in 5b-2b)<!-- drift-ok --> — `PublishStatus{Posted, Unchanged, InsideBatch, Refused}`
  + `PublishOutcome{status, held}` (`held` = the version the mirror holds after the call, empty before the first publish) + `explicit operator bool`
  (keeping the old “was it published” reading, so not one existing `EXPECT_TRUE(publish(...))` changed) + `posted()`.
- **How “inside a batch” is told apart**: `SceneRows::editing(scene)` (a friend, reads `edit_depth_ > 0`). **Ask it first**, then compare versions: a batch's version is
  deliberately frozen and cannot be compared; asking first also stops a long batch from paying one row build every frame.
- **Not done**: a public `Scene::publish()`. No public snapshot type carries “the previous snapshot” today, so publishing it now could only answer a version number nobody can use
  and would create two truths with the mirror's bookkeeping. It belongs with “implicit publishing inside a frame disappears” in step 5 (§9.3 row 2 was rewritten).
- **Assertions**: three existing cases were upgraded from `EXPECT_FALSE/TRUE` to state assertions (`Unchanged` / `InsideBatch` / `Refused` / `Posted`),
  with no duplicated new cases. `test_graphics` 571.

## 2026-11-12 — graphics-next step 3, settling the seam first: warm-up cannot be “run a frame without submitting”

- **The obvious implementation the code ruled out**: `VsgBackend::executeFrame`'s loop
  (`openFrameBooks` → each pass → `closeFrameBooks` → submit) looks like it could “run to the end without submitting”, but
  `Session::beginFrame()` calls `viewer->advanceToNextFrame()`, which **acquires** a swap-chain image:
  no submit ⇒ the next frame has no image to acquire (starvation); submit ⇒ it is no longer warm-up. And a pass that draws to the window can only draw into that image,
  so that approach cannot even cover it. **Recorded in §7 rejected option D11 / §6.11 P11.**
- **The chosen seam**: warm-up only does the **value** side — halves / descriptor sets / uploads. The key is computed by
  `drawNeeds(compiled pass, draw)`, built by `prepare` and read by `ContentPass::record` (one builder, two consumers,
  `G1`). The window target's format is answered by `WindowTarget::facts()` (const, no acquire).
- **Entry**: `RenderEngine::prepare()` compiles to a `FrameDescription` first, and the backend receives **the same value** as `executeFrame()`
  (one line, two verbs, no new noun); the snapshot entering that input is step 5.
- **§9.3 step 3 therefore splits into 3a / 3b**: 3a = the answer (`T32`: the reported key set = the key set this frame actually builds),
  3b = pre-building (`T7`/`T10`: `builds` delta 0 after warm-up). One revertible commit per row.
- **Next**: 3a — locate the key-construction point in `ContentPass::record` (around line 1235, with the comments on `PipelineKey::variant`
  and topology), extract it into a pure function, and let `prepare` go through the same function.

## 2026-11-12 — graphics-next step 3a landed: `drawNeeds` is the only key builder

- **New files**: `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/DrawNeeds.hpp` + `src/internal/DrawNeeds.cpp`.
  The `DrawNeeds` value + `Miss{None,Geometry,Material,Program}` + `keyFor(compatibility)`; two `drawNeeds` overloads:
  `(pass, CompiledCommand, facts)` = a content draw, `(pass, CompiledDraw, inputs)` = a full-screen call (**no facts needed**:
  its identity is all in the plan + the caller's offer). `sampledColorCount` / `sampledDepthCount` moved from `ContentPass.cpp`'s
  anonymous namespace into the header (`record`'s set construction and `serveHalf` use the same spelling).
- **Two pairs of counts, one rule**: `set_colors/set_depths` = how many the frame's input set binds (a pass fact); `key_colors/key_depths`
  = how many the pipeline key writes (a program that does not sample set 1 ⇒ 0, with `programSamplesInputSet` left in the .cpp anonymous namespace). Both pairs
  are written in one place, so they cannot diverge.
- **Duplication deleted**: the variant/layout/program/Key construction in `ContentHalves::halvesFor`; `recordCommand`'s
  four-way geometry/material/variant/findProgram/half match (→ `halfFor(needs)`); `recordScreenDraw`'s source
  search and six hand-filled key fields; the third lookup in `describeMissingContentHalf`. `recordCommand` lost two parameters
  (`sampled_color_count/depth_count`) and reads `needs.set_*` instead.
- **T32 (device-independent)**: `ContentHalvesTest.TheReportedNeedsAreWhatTheFrameBuilds` — the identity is **hand-written** by the case
  (expectations are not fed from `drawNeeds`), then compares (a) the answer, (b) `halvesFor`'s half, (c) the pool's key set after recording (`keyOf(1..N)`,
  ids monotonic from 1 and never reused). `ContentPassTest.AMultiLayoutScopeServesEveryHalfItWasBuiltFor` got the same
  key-set assertion (on the recording side).
- **Mutations 3/3 red**: `keyFor` dropping topology / dropping variant, and `recordCommand` overwriting
  `record.key.compatibility` again.
- **Trap**: do **not** clang-format large files like `ContentPassTest.cpp` / `ContentHalvesTest.cpp` wholesale (it reorders existing <!-- drift-ok -->
  `Scope::Entry{...}` and `using` order); format only the new files and `ContentPass.*` / `ContentHalves.cpp`. <!-- drift-ok -->
- `test_vsg` = 541 cases / 78 suites all green. Next = 3b: the `RenderEngine::prepare()` entry +
  pre-building from the answer (criterion: `builds` delta 0 after warm-up).

## 2026-11-12 — graphics-next 5a: the shadow snapshot seam (`T34`)

- **New files**: `SnapshotSource` (`SceneSnapshot` → `DrawListSource`, memoised by `SceneSnapshot::revision`),
  `DrawListDiff` (field-level comparison).
- **Switch**: `ContentMirror::setSnapshotShadow` / `FrameWorker::setSnapshotShadow`, off by default; the shadow only compares, never replaces.
  `publish` takes the shadow with `SceneRows::snapshot(scene)` **after a successful post**; the frame protocol guarantees the engine half starts only after the content half
  completes, so “new rows, old shadow” cannot happen.
- **Trap**: do not build a snapshot from the copy in `SceneRows::publish` — it clears `nodes` / `node_epochs`, so `makeSceneSnapshot` loses
  chunk sharing; use the live `SceneBake`'s `snapshot_cache`.
- **What the diff compares**: `DrawListDiff` compares row identity / geometry + description / material + program / world / opacity / order /
  authored_state / instance slice; the order is `sortCollected`'s emission order (commands do not store a sort key).
- **Tests**: `SnapshotShadowTest` 600 frames (transform / opacity / material / placement edits) + an old picture inside a batch + an old snapshot's
  material/geometry/program + the empty-snapshot mutation; `FrameWorkerTest.TheSnapshotShadow...` for the frame-level seam. `test_graphics` 577 / `test_vsg` 542 /
  ctest 30/30 / TSan 0.

## 2026-11-12 — graphics-next 5b-1: Published mode binds the snapshot

- **Source flipped**: `FrameWorker::runPass(Published)` now binds every draw to `ContentMirror::snapshotSource()`; the old
  `RenderWorldSource` is only reachable through `legacySource()` as a shadow, and `setLegacyShadow` is off by default. `FrameMode::Mirrored` was renamed
  `Published`.
- **Publication**: `ContentMirror::publish` always takes `SceneRows::snapshot`; it calls `SceneRows::publish` + post only when LEGACY is on.
  The snapshot is built first and legacy posts after; when legacy is refused neither advances (the old tests' Refused semantics).
- **Trap (a test caught it)**: `SnapshotSource::beginFrame` must call `DrawListMemo::beginFrame` with the **previous frame's** token
  like `RenderWorldSource`/`Scene` do, or every frame deletes the previous frame's memo entirely; `PassesSharingSceneAndCameraCollectOnce...` went red on the spot.
- **Tests**: `test_graphics` 577 / `test_vsg` 542 / ctest 30/30 / TSan 0.

## 2026-11-12 — graphics-next 5b-2a: public `Scene::publish()`, `FrameWorker` holds `SnapshotSource` directly

- **Public entry**: `ScenePublishStatus` / `ScenePublishResult` (`Posted` / `Unchanged` / `InsideBatch`, no
  `Refused` any more); `Scene::publish()` uses `SceneRows::snapshot` + `published_snapshot_revision_` to decide `Unchanged`, and does not advance inside a batch.
  `ScenePublishResult` has `posted()` and an explicit `operator bool`.
- **Source ownership settled**: `FrameWorker` no longer goes through `ContentMirror`; each content has a `{content, unique_ptr<SnapshotSource>}`,
  `publish(Scene&)` calls `scene.publish()` and then hands the snapshot to the source; `drain`/`applied`/the LEGACY shadow are all deleted; `run` became void.
- **Trap**: a second `FrameWorker` calling `publish` on the same scene gets `Unchanged` (not `Posted`), so the old tests'
  `ASSERT_TRUE(publish(...))` had to accept `Posted|Unchanged`.
- **Tests**: `FrameWorkerTest.PublishAnswersPostedUnchangedAndInsideBatch`; `test_graphics` 577 / `test_vsg` 542 /
  ctest 30/30 / TSan 0.

## 2026-11-12 — graphics-next 5b-2b: delete the old mirror / delta / version

- **Deleted**: `ContentMirror` / `DeltaChannel` / `RenderWorld` / `RenderWorldSource` / `PublishOutcome` are all gone;
  `SceneRows` keeps only `snapshot` (`version` / `editing` / `publish` deleted with them). `DrawListMemo` stays as `SnapshotSource`'s
  pure per-view cache; `Scene::refreshBoxes/refreshMaterials` stay (snapshot inputs).
- **Tests**: `ContentMirrorTest` 11 + `RenderWorldTest` 7 + `DeltaChannelTest` 5 = 23 LEGACY cases deleted;
  `RenderWorldTest.cpp` keeps only `SnapshotTest` and `DrawListBakeTest`; `SnapshotShadowTest.cpp` became
  `SnapshotSourceTest` (published source vs Scene collecting directly, 600 frames + inside a batch + T17 + the empty-snapshot mutation).
- **Counts**: `test_graphics` 554 / `test_vsg` 542 / ctest 30/30 / TSan 0. 5b-2c (the `addPass` family /
  `PassRegistry` runtime refusal) was not done.
- **Trap**: `RenderEngine`'s contentCollectCount/ReuseCount read `FrameWorker::memoStores/Hits`; with LEGACY deleted
  only `SnapshotSource`'s memo is left, and `PassesSharingSceneAndCameraCollectOnce...` should still be green (`beginFrame`'s previous-frame
  grace is the premise).

## 2026-11-12 — GPU residency pin migration step 5: halves / descriptor sets

- **One table**: `ContentHalves` and `ContentSets` moved onto `core::ResourceTable`; `answerable` and `ContentSets`'
  sweep were deleted and replaced by one `retireUnpinned` predicate. Lookup went from a linear scan to a hash; the sweep went from O(entries × geometries)
  to O(entries).
- **Half pin**: `(program identity, revision, variant, kind)`. Layout does not take part — asking it would scan every geometry fact,
  exactly the cost this step deletes; the price is that layout / variant churn accumulates tens of KB of pipelines, watched by backlog `T28`, with misses not added back.
- **Descriptor-set pin**: program@rev + `declaresTexture` for every non-empty slot in the key + its own `texture.revision()`
  comparison. An empty source (all pass-level samplers) is pinned by the program alone.
- **Refused builds**: no GPU object, so they do not enter the table; a separate `unordered_set<Key>` records “do not retry”, forgotten with the program revision.
  So `halves()` / `sets()` counts match the old semantics (refused does not count as live).
- **`ResourceTable::forEach`**: `ContentSets::repoint` needs to walk every resident value; a visitor must not acquire/retire/clear
  (that moves `entries_`) but may change a value's body.
- **Trap**: a set's texture pin must call `declaresTexture` before dereferencing — the address may have been reclaimed, and only “a live row still declares it”
  guarantees that pointer is a `Texture` now; the reverse order is a use-after-free.
- **Tests**: `ContentHalvesTest.AHalfStaysPinnedByItsProgramWhenNoGeometryFeedsItsLayout`,
  the pin assertion at the end of `ContentPassTest.TheProducedSetsGiveEachTexturedDrawableItsOwnMap`,
  `ResourceTableTest.TheVisitSeesEveryResidentValueAndCanChangeItsBody`. `test_vsg` 544 / `test_graphics` 554.

## 2026-11-12 — same-day close-out: the half pin gains a layout declaration index (replacing the layout half above)

- **A fourth declaration index**: `ContentDeclarations` gained `layouts_` (`declaresLayout` / `hasLayoutIndex`);
  `GeometryTable` maintains `layout → live row count` in the same methods as append / eraseIf / reclaimBefore / clear
  (`Declarations<Key, Hash>` is now generic over the key; buffers / layouts / textures / materials share one rule).
- **Half pin**: `program@rev ∧ declaresLayout(layout)`; a screen half is pinned by the program alone. That makes “the last geometry row using this layout
  leaves the tree ⇒ the half is parked” O(1): no layout accumulation and no per-frame scan of geometry facts.
- **Optional index**: when a hand-built `ContentFacts` publishes no layout index, `ContentHalves` falls back to scanning its `geometries`
  (the same rule as scanning when `program_order` is empty); the production `ContentStore` always publishes, so the frame path never takes the fallback.
- **Public header**: `VertexLayoutKeyHash` (`backend/Keys.hpp`, the same fields as `operator==`); the `ContentDeclarations`
  constructor puts layouts last, so old 2/3-argument calls are unchanged.
- **Tests**: `ContentStoreTest.TheDeclarationsFollowTheRowsAndOutliveThemForTheParkingWindow` gained the layout's
  refcount / last-row-leaves assertions; `ContentHalvesTest.AHalfIsPinnedByTheLayoutDeclarationAndLeavesWithIt`
  pins “the index says it is there ⇒ keep it, the index says not ⇒ park it”. Backlog `T28` was deleted (a close-out item, no longer a trigger).
- **Counts**: `test_vsg` 544 / `test_graphics` 554 / ctest 30/30 / TSan 554 cases with 0 warnings.

## 2026-11-15 — F5 ruling: one frame in flight (and correcting the “CPU is the bottleneck” misreading)

- **Key lesson (easy to repeat)**: **“a frame's wall clock” ≠ “the host's work”**. `executeFrame` calls `vkWaitForFences` internally, so the benchmark's
  wall clock (median 8.63–8.94 ms) **includes device execution**; host-side work = wall clock − `FrameReport::timing` (7.95 ms) ≈ **0.9 ms**.
  The earlier reading treated 8.2–9.0 as CPU time ⇒ the opposite conclusion “the CPU is the bottleneck”, corrected in `design/vk-backend.md` §2.3 and audit F5.
  **Rule**: any number quoted as “CPU/GPU time” must first say whether it is a wall clock, host work or a device timestamp.
- **Ruling**: keep one frame in flight (`../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`). The upper bound on the gain from more frames in flight is
  `max(host, device) ≈ 7.95 ms` (about 10%), at the cost of changing the “draw then read” contract; the trigger condition = device time < half the wall clock (the host becomes the bottleneck),
  a product shape that needs overlap with the GPU, or a second backend wanting the same semantics.
- **How to measure** (reusable): run the demo with `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=N`; the device time appears only on **sampled frames** (every 30 frames),
  so pick N so the last frame lands on a sample (on this machine **N=29** hits; N=28/30/58 all answer `gpu not measured`).

## 2026-11-15 — locating the `test_vsg` Release heap corruption (vsg backend resource lifetime) + the memory-checker recipe

- **Symptom**: the full Release `test_vsg` crashed about 50% of the time (`malloc(): unaligned tcache chunk` / SIGSEGV, with a drifting landing point:
  `FrameUploadsTest`, `PrepareUploadTest`); Debug rarely; **the old commit did the same** (A/B checked).
- **Weapon order (important)**: ASan (Debug tree) ran 3 times all green ⇒ not an ordinary out-of-bounds; `valgrind --tool=memcheck` on the **Release binary that crashes**
  caught 5 `Invalid read`s in one run: every reader was a `libvulkan_lvp.so` worker thread, every free stack was in this process ⇒ **the host freed driver memory early**.
  Validation cannot see this (the Vulkan objects' relationship is legal; what got freed was the driver's bookkeeping block).
- **Two root causes (fixed)**: ① `OffscreenSession/Session::assignFrameGraphs()` replaced `task->commandGraphs` outright, so the previous frame's
  `vsg::CommandGraph` lost its last reference → `~Buffer` freed driver memory; ② `VsgBackend::shutdown()` called
  `releaseContentWorld()` directly while the session was still pipelined (`FrameTimeline` + slot ring), so `MaterialImages`' images were destroyed while the driver thread was still reading them.
- **Fixes**: ① teardown calls `waitDeviceIdle()` on **every session** before anything is destroyed (counted in `RetirementQueue::noteDeviceWait()`);
  ② new `detail::GraphLifetime` (`GraphLifetime.hpp`): a replaced frame graph goes into a keep-alive list and is dropped frame by frame by
  **`FrameTimeline::completedFrame()`** (the completion evidence the slot reclaim also uses) — steady state does not sacrifice the pipeline.
- **A third one (fixed the same day)**: `OffscreenSession::waitDeviceIdle()` used **`timeline().submittedFrame() == 0` as an early return**,
  treating “no frame recorded” as “not running” — yet prepare builds pipelines and submits upload batches. The criterion became **“this session has ever created work”**
  (`beginFrame` sets it + the backend calls `notePreparedWork()` during prepare), which fixes prepare-only teardown and keeps
  “a session that never created work does not pretend to have waited” honest (`OffscreenSessionTest.TheDestructiveWaitIsCountedOnce`). Result:
  `PrepareUploadTest.*`'s valgrind went from **4 `Invalid read`s → 0**;
  `*FrameUploads*:*PrepareUploadTest*:*BlockStorageTest*:*ContentPassTest*` 44 cases with 0 errors.
- **Measure**: full Release **4/4 green** (before the fix it crashed about once every two runs).
- **Recipe**: for a memory/lifetime defect run ASan three times first (to rule out out-of-bounds), then valgrind on the Release binary (`--gtest_filter`
  narrows it), and only `--num-callers=16` shows the “Block was free'd at” free stack.

## 2026-11-15 — one definition for `PrepareReport.passes` (T41 closed) + a real VUID: “dependencies are part of compatibility too” between variants

- **The definition is settled (one place in the SDK)**: `FrameShape` / `frameShapeOf` / `isDrawingCall` / `passHasEffect` went into
  `FrameDescription.hpp` (inline, no new exported symbol). The rule = **a pass with an effect**: at least one drawing call, **or** a declared
  clear (a “clear only” pass is a real pass); an entry with `draw == nullopt` does not count (it does not even declare a target). The two backends used to compute it differently:
  vk counted “entries with draws” (treating a call with no camera as a draw, +1), vsg counted “passes whose compiled draws are non-empty” (missing clear-only passes,
  −1), and `FrameCounters` was a third. Both now call `frameShapeOf(frame)`, matching the recording count by construction.
- **A real VUID (caught by the new cases, fixed)**: among `TargetImages`' three render-pass variants, the one with `depth_load == LOAD` widened
  `srcStage/srcAccess` (to cover “reading depth written by the previous pass”) while the CLEAR variant did not ⇒ **different dependency ⇒ the variants are incompatible**; the same
  framebuffer was compatible only with the CLEAR variant, so “two passes in one frame writing the same target with depth” reported
  `VUID-VkRenderPassBeginInfo-renderPass-00904` at `vkCmdBeginRenderPass`. Fix: dependencies **do not change with the load op** (the CLEAR variant covering the same source scope costs
  nothing). Every existing cross-backend case used “one target per pass”, so it had never shown up.
- **Cases**: `BackendParityTest.APrepareReportsTheShapeBothBackendsThenRecord` — one frame with three passes (clear only / a call with no camera /
  a real draw), asserting both backends' `PrepareReport` is `passes=2, draws=1`, the recording count agrees, and **asserting vk has zero
  warning/error** while the frame runs. Mutation: widening the dependency back to “LOAD only” makes the case red at once (“the direct backend reported a problem”).
- **Counts**: `test_vk` 89; `BackendParityTest` **13** (12 + 1); full Debug ctest 32/32; Release `test_vk` 89/89.

## 2026-11-15 —— `gfx_backend_vk` cross-session pipeline cache (F4/T44 landed): the location belongs to the host, the key to the device, the witness must be honest

- **Concept**: `DeviceDesc::cache_directory` (SDK, a creation input) = **the host says "where files may go"**, the backend says "what to store". When the host does not write it,
  the plugin uses `PluginLoadContext::ensureDataDirectory()` as the default inside `Plugin::load` (the framework's own rule: plugin files live in the plugin
  data directory); neither present = no persistence. `PipelineCacheFile` owns only the file format: `[magic][format][key][size][blob]`,
  key = vendor/device/driver/api/`pipelineCacheUUID`; the read side is strict (length, magic, version, key, **the trailing bytes must be used exactly**),
  the write side uses a temporary file + rename.
- **Trap 1: `pipelineCacheReport` used to fall back to 0 after shutdown**. The `preload_handed/refused` computed in `Device::initialize`
  lived in `ContentDraw` fields, and `shutdown()` calls `content.destroy()` before reading them ⇒ all reset. The fix: right after `create()`,
  **snapshot into `Impl::CacheBooks`** (the same home as saved_bytes); only `cache_hits` is read live while the session is alive, because it is a cumulative count.
  This is the third time for the same trap ("a report field living in an object that gets destroyed").
- **Trap 2: lavapipe both **advertises** the creation feedback extension and **never** reports a hit, and **does not validate** `pInitialData`** (change the blob's
  vendor and it is still accepted). So "the cache hit" is **not observable** on a software rasteriser and a test must not demand it; what the backend can honestly state is the
  **hand-over** (the blob really went to `vkCreatePipelineCache`, `preload_handed`); the hit count is only reported
  (`hits_measured` + `cache_hits`). The rule: **a witness must belong to the half you can prove**, do not treat driver silence as failure.
- **One incidentally useful corner**: when `vkCreatePipelineCache` refuses the preload, **retry once without data** (the session comes up as usual) and record
  `preload_refused` — on a real driver this is the visible answer to "the cache is stale".
- **Mutations (4/4 load-bearing)**: drop the key comparison → `AnotherDevicesFileIsRefused` red; drop the length/trailing-byte strictness → `ATruncated…` red;
  drop the version check → `AnotherFormatVersion…` red; erase `pInitialData` → `ASessionWritesWhatTheNextOneLoads` red
  (via `preload_handed`, not via the hit count).
- **Counts**: `test_vk` 89 (new `VkPipelineCacheFileTest` 5 + `VkPipelineCacheSessionTest` 4); full Debug ctest 32/32;
  Release `test_vk` 89/89.

## 2026-11-14 —— `gfx_backend_vk` device memory pool (F7/T45 landed): two rules that are easy to get wrong + mutation results

- **Concept**: `MemoryAllocator` (`internal/MemoryAllocator.hpp`) = a session-level **device memory pool**: it creates
  `VkDeviceMemory` blocks per memory type, a resource holds only `MemorySlice{memory, offset, size, block}` and **binds by offset**.
  `MemoryUsage` (`DeviceLocal` / `HostVisible`, the latter forces `HOST_COHERENT`) is translated to a memory type by the pool; `MemoryKind`
  (`Linear` / `Optimal`) exists for exactly one thing: `bufferImageGranularity` forbids a buffer and an optimal image sharing one
  granularity page — **only effective against a neighbour of the other kind**, same-kind neighbours get no padding. Ranges inside a block are **sorted and alternate live/free** ⇒ a release only merges with two
  neighbours, an allocation = first fit with alignment/granularity. A block that becomes empty while another block of the same type exists ⇒ destroyed (one big readback does not pin
  device memory until the session ends); the last block of a type stays warm. `Device::memoryStats()` is the externally readable witness.
- **Trap 1 (measured, worth remembering): whether `map` works is a property of the MEMORY TYPE, not of "why this block was created"**. I first gave `Block`
  a `usage` field and compared it in `map()` ⇒ on lavapipe (device-local and host-visible are the **same** type) the ring legally
  landed in the depth image's block, yet `map()` refused ⇒ `ensureRing` failed ⇒ the whole device would not come up (the window case only reported "no Vulkan device").
  The fix: `map()` asks `memoryTypes[type].propertyFlags & HOST_VISIBLE`.
- **Trap 2: establishment order**. The pool must open **before the first thing that consumes memory**: the window swapchain's depth image is created during `attach`,
  before `content.create`. `initialize()` now opens it right after `vkGetPhysicalDeviceMemoryProperties/Properties`;
  shutdown closes it after the last resource is released and before `vkDestroyDevice`.
- **Mutations (4/4 load-bearing)**: disable merging → only `FreeNeighboursMergeSoThePoolIsReused` red; disable granularity → only
  `LinearAndOptimalDoNotShareAGranularityPage` red; disable suballocation (one block per slice) → only
  `ManyTargetsShareThePoolBlocks` red; change release to keep live → only `AReadbackGivesItsStagingBytesBack` red.
- **The flaky `test_vsg` Release on this machine is pre-existing, not from this change**: the full suite occasionally reports
  `malloc(): unaligned tcache chunk detected` / SIGSEGV (the landing spot drifts: `FrameUploadsTest`, `PrepareUploadTest`),
  and `WindowCompositionTest` has also failed an X11 pixel read. **The A/B method**: `cp` the old and the new `build-release/bin/test_vsg` each
  to `/tmp`, set `LD_LIBRARY_PATH=<build>/lib`, run each 5 times alternately — **the old build segv/aborts the same way**. Two traps when reading the log:
  (1) after copying to `/tmp`, `$ORIGIN/../plugins` breaks, so `VsgBackendPluginTest.*` **falsely reds**; (2) with stdout redirected to a file
  gtest is **fully buffered**, so "the last `[ RUN ]` line" is not the crash point (use `stdbuf -oL`). `test_vk` Release run 5 times
  is stable at 80/80; full Debug ctest is stable at 32/32.
- **Counts**: `test_vk` 80 (new `VkMemoryPoolTest` 3 + `VkMemoryAllocatorTest` 2).

## 2026-11-14 —— `gfx_backend_vk` performance baseline (real host `Vine --open demo`, lavapipe)

- **Measurement method (new dev switch, demo plugin)**: `VN_BENCH_FRAMES=N` fires N back-to-back `RenderControl::renderFrame()` calls with a zero-interval `QTimer`
  after the assets are loaded (the host's own call: publish + engine half + backend record/submit/present), recording wall clock per call;
  `VN_BENCH_WARMUP=1` first calls `SceneView::publishContent()` and then `RenderEngine::prepare()` twice. Three log lines
  (`bench warm-up` / `bench first frames` / `bench`) + a last-frame count line `bench last frame`.
  **Measurement points**: a frame is **handed over asynchronously** (`frame()` waits for the previous frame's engine half) ⇒ the first reading is ≈ 0, the first-frame cost lands in
  the second number of `first frames`; steady state uses median/p95 (the mean is skewed by the upload frame). The control = move `gfx_backend_vk.so` out of
  `plugins/vine` and rerun the same command (**remember to move it back**; this tool's `trap EXIT` does not fire, `mv` it back by hand).
- **Release ×3 (this machine WSL2 + lavapipe, `VK_ICD_FILENAMES=lvp_icd.json`, Qt xcb, 60 frames)**:

  | Item | vk | vsg |
  |---|---|---|
  | Session up / first picture | 40 / 219 ms | 42 / 229 ms |
  | Longest application-thread occupancy during startup (`VN_BOOT_TIMING`) | 316 ms | 336 ms |
  | Warm-up batch (ms / passes / draws / halves / sets / uploads / pipelines) | **16.1–22.3** / 8 / 6 / 2 / 0 / 5 / 2 | **9.7–11.3** / 6 / 6 / 2 / 4 / 5 / 2 |
  | Steady-state frame median (three runs) / p95 | **7.06, 7.44, 6.93** / 7.95, 8.90, 7.94 ms | **5.63, 5.60, 5.63** / 6.52, 6.64, 6.26 ms |

  ⇒ vk steady state ≈ **+25–32%**, warm-up batch ≈ 1.8×, the two startup phases are nearly identical (vk slightly faster).
- **What the warm-up batch buys (vk, with/without warm-up alternating ×3, 40 frames)**: first frame 12.2–17.3 ms vs **30.2–35.8 ms** (those 5 uploads:
  2 streams + 3 textures) ⇒ first frame about −18 ms; the cost is 16–23 ms during load; steady-state medians are indistinguishable within noise (both land in 7.2–9.4).
  The second warm-up is 0.08–0.41 ms and all zeros = the contract "a warmed frame builds nothing".
- **Two measured findings (registered in the backlog)**: (1) `PrepareReport.passes` is vk 8 / vsg 6 / recorded count 7 on the same frame — three different accounts;
  vk counts every `FrameDescription` entry with draws (including the ones the recorder drops), vsg counts the compiled plan (T41);
  (2) vk allocates content/draw/screen descriptor sets **per draw per frame** (no cross-frame declared-set cache like vsg) ⇒ warm-up `sets` is always 0, the
  first frame still builds sets, possibly part of the steady-state +25% (not attributed, T42).
- **Boundary**: lavapipe is a software rasteriser, absolute values serve **same-machine comparison** and regression only, not GPU conclusions; Win32 and a real GPU are untested.
  The 60-frame window mixes the demo's PiP/overlay timers and one asset-upload frame.

## 2026-11-14 —— vk's per-frame cost model lands (T43+T42 from the same source; ring + dynamic offset + declared-set cache)

- **Shape**: `ContentDraw::Impl::UniformRing`, one persistently mapped buffer, `sliceSize()`-aligned slices; the four blocks are written into a slice, three set layouts
  (**after 2026-11-15 S3**: the ring and the set table live in `FrameScratch::Impl`, layouts and `buildPipeline` in `ContentCaches::Impl`;
  the `ContentDraw::*` spellings below name the same mechanism)
  are all `UNIFORM_BUFFER_DYNAMIC`, binding passes 4 dynamic offsets (content set 0/6/8 + draw set 0; the screen set is 6/8).
  A full frame **does not grow inside the frame** (that would destroy a buffer the command buffer already named): `reserveSlice` records the demand, skips that draw, and `beginFrame` grows by
  demand (the same rule as vsg's BlockStorage).
- **A set's key is "the resources it binds"** (5 texture views + the shadow view; the screen set has the same shape): the pool uses `FREE_DESCRIPTOR_SET`, releases per entry,
  **no longer resets every frame**; the draw set has one handle per ring generation. Eviction: `last_frame + 600 < frame_index`, scanned every 120 frames.
- **Witness**: `Device::contentCacheCounters()` (`ring_builds` / `set_builds` / `sets_live`) + two `VkUniformRingTest` cases;
  mutation (cache always misses) ⇒ both cases red. `test_vk` **74** cases.
- **Measured (Release + lavapipe, the same 11-pass scene, 3 runs each)**: steady-state median **9.03–9.23 → 8.16–8.30 ms (−10%)**;
  vk and vsg still **byte-identical**; warm-up unchanged (warm-up draws nothing).
- **Pipeline cache**: an in-session `VkPipelineCache` is added (passed to both `vkCreateGraphicsPipelines` call sites); persisting across sessions is still T44.
- **GPU timing (T46 closed)**: the SDK gains `FrameTiming{gpu_micros}` + `FrameReport::timing` (optional, absence ≠ 0); vk uses a pair of
  `VK_QUERY_TYPE_TIMESTAMP` queries (write TOP/BOTTOM, `vkGetQueryPoolResults` after the fence, absent when `timestampComputeAndGraphics` is false).
  **Sampling is mandatory**: measured on lavapipe, writing and reading a timestamp costs **~0.8 ms** per frame (off: median 8.30; on: 9.01–9.20, 3 runs each) ⇒ sample every 30 frames
  (`kTimestampSampleFrames`), an unsampled frame answers **absent** (not the previous frame's value). The demo's `bench last frame` prints gpu milliseconds when a sample exists
  (lavapipe measured **gpu 7.15–7.74 ms**, the same frame's **wall clock** median 8.2–9.0 — note that is wall clock including the fence wait,
  the host side is only ~0.9 ms, see the 2026-11-15 F5 ruling).

## 2026-11-14 —— `gfx_backend_vk` industry-standard review (conclusion + two small fixes)

- **Conclusion**: specification conformance/diagnosability is **industry grade** (validation 0 VUID, byte-identical cross-backend comparison, device-loss classification, barrier table,
  complete pipeline key); resource and performance architecture is **prototype grade** (per draw per frame UBO create/destroy, per draw per frame descriptor set allocation, per frame command buffer allocation,
  no `VkPipelineCache`, no suballocation, no GPU timing). A single frame in flight is the price of the **SDK contract** ("readable when the draw is done"), not a defect.
  The full text and F1–F13 are in `../research/graphics-2026-11-14-vk-industry-review.md`; backlog T43–T46.
- **Two fixes already in**: (1) physical devices are sorted by `VkPhysicalDeviceType` (discrete > integrated > virtual > cpu > other,
  ties keep the first seen) — it used to take the first serviceable device, silently using the iGPU on a laptop; (2) the `vkAcquireNextImageKHR` timeout changes from
  `UINT64_MAX` to **1 s** (a stuck surface turns from "hangs forever" into "skip that frame + report once per episode").
- **Measured numbers (do not forget)**: a demo frame is 22 content draws + 6 full-screen draws ⇒ **28 buffer+memory allocations / frame**
  (60 fps ≈ 1700 per second), all UBOs are `UNIFORM_BUFFER` (no dynamic offset); `vkCreatePipelineCache` and
  `vkCmdWriteTimestamp` appear 0 times in the whole repository.

## 2026-11-14 —— root causes of four "recorded, not drawn / no effect" cases (demo chain never adopted / vk full-screen inherits pass depth / vk full-screen binds only the shadow stand-in / demo previews share one name)

- **Host**: `PassChain::build()` **does not adopt itself**, `view->setChain(chain)` must be called (`Viewer` is the model). Without adoption,
  `SceneView::publishContent` does not publish the chain-owned transparent scene (one `never published`, that pass draws nothing) and
  `applySurfaceGeometry` does not resize the chain either (the demo used to resize in its own layout step, two sources of size). The demo is fixed, see
  `.ai/bugs/demo-chain-never-adopted.md`.
- **vk**: a full-screen call must **always** be `DepthMode::Disabled` (enforced by `ContentDraw::planScreen`, the reference backend's M4b rule).
  A full-screen triangle sits at z = 0.0 = the reverse-Z far plane; the window pass's D24 clears to 0 and deferred's composite **borrows** the G-buffer
  depth ⇒ inheriting `TestAndWrite` discards the whole block, while **zero VUIDs and every `recorded=1` in the recording layer**. The case is
  `AScreenCopyIsDrawnWhateverThePassDepthPolicySays`; the old screen fixture explicitly set `Disabled`, which is exactly why it did not catch it.
  See `.ai/bugs/vk-screen-draw-inherits-pass-depth.md`.
- **vk's screen set must bind the map the pass resolved**: bindings 7/8 existing ≠ binding the right image. `recordScreen` now takes
  `ShadowSource` (`Device::executeFrame` already called `resolveShadow` for every pass): with a map, bind the real depth + the per-call packed
  `VineShadowBlock`; without one, the white stand-in + `params.x = 0`. Symptom = "forward has shadows, deferred does not, zero diagnostics"; the case
  `VkShadowTest.AScreenPassShadesWithTheMapItResolved` (samples binding 7 and writes depth, a white stand-in reads 255).
  See `.ai/bugs/vk-screen-pass-shaded-without-the-map.md`.
- **The plan identifies rows by `(owner, name)`, a name is not a label**: the demo's four G-buffer previews were all called `gbuffer_preview` ⇒ four declarations
  landed on the same row, only the last survived ("only block 4 shows"). The other half: the layout step only changed the demo's own `ScreenPass`
  (`setViewport`), while the nodes in the frame are **built from declarations** ⇒ the reorder never took effect; after changing the object it must be **declared again** (`addDemoPass`
  applied again, in-place update for the same owner and name). The frame table can be read directly: `VN_FRAME_PLAN=1` should show 4 `gbuffer_preview_*` rows.
  See `.ai/bugs/demo-preview-passes-shared-one-name.md`.
- **Diagnostic recipe (reusable)**: (1) a recording-layer probe (per pass commands/recorded/why) separates "not in the commands" from "in, but not drawn";
  (2) **temporarily** change the 16F target under observation to RGBA8 (`ChainPresets.cpp`) to read the centre pixel back with `RenderEngine::device()->readColorBuffer`;
  (3) copy the GBuffer straight into a readable RGBA8 composite and read that, separating "source empty" from "copy broken". Remove the probe when done.
- **Two static-review findings (fixed 2026-11-14)**: (1) `RenderEngine::frameReport()` is **not** cross-thread safe (only
  `PresentRecord` explicitly says "readable from any thread") ⇒ a benchmark/HUD must `waitForFrame()` before reading it (the docs list "a benchmark" as
  a waitForFrame caller); (2) re-declaring a pass from the **layout step** (changing the viewport) is refused by `applyPlan` with `Busy`
  (the frame is recording) ⇒ check `PlanResult::applied()`, restate after `waitForFrame(250ms)`, otherwise the rectangle stays at the old value with only a
  one-shot warning.
- **clang-tidy baseline (`clang-tidy-22 -p build`, `bugprone-*` + `clang-analyzer-*`, 2026-11-14)**:
  `ContentDraw.cpp` 4 entries = 2 zero-initialised Vulkan structs (`stages[2]{}` / `multisample{}` whose enum fields are written afterwards,
  a convention, not a defect) + 1 **false positive** use-after-move on `build_why` (every `ensure*` takes `vn::String&`, nothing is moved on that path)
  + the `std::hash` specialisation (`ContentId.hpp`, intentional); `Device.cpp` 1 "Called C++ object pointer is null"
  (`window` is associated via `Runnable.window`, the collection site guarantees non-null — the analyser lost that association, a false positive); `DemoScene.cpp` 1
  `atoi` (a dev switch, consistent with the other `VN_*` switches). ⇒ static analysis of these three TUs is **clean**; new entries should first be ruled out against these categories.

## 2026-11-13 —— `gfx_backend_vk` M9: shadow map

- **Shape**: `TargetImages` accepts **depth-only** targets — `built()` no longer requires a colour view, `createRenderPass` with 0 colour attachments uses
  `colorAttachmentCount = 0` / `pColorAttachments = nullptr`, `buildPipeline` likewise (`blend.pAttachments`); `Device`'s
  per-attachment clear writes from `clear_values[0]`, and a depth-only target has no colour slot, so it must be skipped.
- **Which image is the shadow**: only the **target's own declaration** counts (`RenderTarget::shadowOf` + `producerViewProjection`), taking the first
  from the pass's `FrameDescription::Pass::inputs` (`Impl::resolveShadow`). **Do not** use "the first input with sampleable depth" —
  the G-buffer has depth too (`graphics-rendering.md` 6.3 records the defect the reference backend hit).
- **Layout**: the producing pass's render pass leaves depth in the attachment layout; **before** the consuming pass (outside the render pass) it moves to
  `SHADER_READ_ONLY`, `TargetImages::depth_sampled` books it, and it moves back before the next write to that target's depth — the same rule set as `color_sampled`.
- **Block**: `buildShadowBlock` (`ContentDraw.cpp`) has four switches: a map exists / the matrix is published / the light is in this call's `light_values` with
  `cast_shadow` and is directional or spot / the light has a slot in the light block. Matrix = `producer VP * inverse(view)`, **not folded** (the SDK clipping convention,
  the conversion happens in the shader); `params = {1, the light's own bias, 1, slot}`.
- **One predicate, two users**: `takesSlot()` in `LightBlock.cpp` drives both `packLightBlock` and `packedSlotOf`; deriving the slot separately would let the <!-- drift-ok -->
  shadow de-scale **another light**.
- **Sampler**: `Nearest` + `ClampToEdge` (`kShadowSampler`), the same values as the reference side's `ContentPipeline::depthSampler()` — filtering would average
  the caster's depth with the surface behind it and the silhouette would move.
- **Trap (Windows build)**: `test_vk` / `test_vsg` compile the plugin sources directly; when the target has no `VN_BACKEND_VK_LIB`,
  `VN_BACKEND_VK_API` expands to `dllimport` and 6 symbols stay unresolved at link time (on Linux it is only a visibility attribute, invisible).
- **Verification**: `test_vk` 51 cases (new `VkShadowTest` 3 cases); `BackendParityTest` case 10 (on Windows `test_vsg` does not compile because
  `VsgBackendTest.cpp`'s XCB case lacks a platform guard — pre-existing, Linux is its build environment).

## 2026-11-14 —— `gfx_backend_vk` M10: deferred path

- **Shape**: the engine's deferred frame = one scene pass writing a four-attachment RGBA16F + D24 G-buffer (`defaultGbufferGeometryProgram()`),
  followed by one SCREEN pass (`defaultDeferredLightProgram()`) sampling the four attachments by the pass's declared inputs (`Pass::inputs` +
  `DrawDescription::source`), with the light block at set 0 binding 6. **The backend needed no change**: M3's MRT / per-attachment clear
  (`extra_colors` defaults to transparent black), RGBA16F images, M4's multi-source screen draw and the light block were already enough. This is evidence that "the seam is enough",
  not a new capability.
- **Picture facts** (this machine): composite centre 204/204/204 (0.8 linear albedo × the sRGB encoding of the ambient term); the four attachments copied to RGBA8 read back
  204/204/204, 0/0/127 (octahedral encoding of normal (0,0,1) + roughness 0.5), 255 (alpha = occlusion), 4/0/127.
- **Tests**: `VkDeferredTest` 2 cases (the composite centre is the albedo under ambient light; each of the four attachments is copied and read back, the index really changes what is read);
  `BackendParityTest` case 11 (the same deferred frame 0/4096 differences, passes=2 draws=2). `test_vk` 53 cases (52 pass 1 skipped),
  `test_vsg` 519 cases (516 pass 3 skipped).
- **Trap**: feeding geometry with texcoords to vk's G-buffer pass triggers the validation warning
  "Vertex attribute at location 8 not consumed by vertex shader" — vk builds the vertex input from the geometry channel list,
  it does not filter by what the shader consumes. In the test, declare only the two channels the program reads (position + normal); the pipeline itself is legal.
- **Windows**: `VsgBackendTest.PrepareBuildsEveryIdentityAndTheNextFrameThenBuildsNothing` uses an XCB window without
  `#if !defined(_WIN32)` (pre-existing, invisible on Linux); the same guard as the top of the file is added, otherwise `test_vsg`
  does not compile on Windows.
- **T26's old assertion**: `OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands` still asserted "all given back",
  contradicting ADR-2026-11-13 (blocks ≤ `minimumStagingBufferSize` are kept); changed to "the 4 KiB upload's block is kept (release returns 0) +
  the one-off 32 MiB block is given back".

## 2026-11-14 —— T39 (`vsg` warm-up really submits textures) + T40 (`vk` depth resolve)

- **T39 `vsg`**: `MaterialImages::queueUploads(Device&, TransferTask&)` hands the images that still carry staging bytes to the session's transfer task
  (compiling the VkImage with the device pool when needed — warm-up does not run the compile traversal); `detail::flushUploads(task)` submits with
  `TransferTask::transferData(TRANSFER_ALL)` (the frame's own copy path), then adds a "wait only" queue submission to consume the completion semaphore
  and waits on the fence; on success `MaterialImages::releaseUploadedStaging()` drops the host bytes (otherwise a later compiled descriptor's
  ImageInfo would copy again on the grounds of "not copied yet"). `PrepareReport::uploads` now counts **streams + textures**; `VsgBackend::prepareFrame`
  calls `ensureOffscreenSession()` first for a windowless frame (creates the viewer/transfer task, acquires no image, submits no frame). Tests `PrepareUploadTest` 2 cases.
- **T40 `vk`**: `TargetImages` creates `depth_resolve` images/views for "multisampled + has depth" (`DEPTH_STENCIL_ATTACHMENT |
  SAMPLED`), the render pass carries `VkSubpassDescriptionDepthStencilResolve` (`sample0`; D24 has stencil, stencil is
  `sample0` too) — it can only hang off `VkSubpassDescription2`, so such targets use `vkCreateRenderPass2` (Vulkan 1.2), other targets still use 1.0's
  `vkCreateRenderPass`. Attachment order = [sample buffers…, colour resolve…, depth, depth resolve]; `depthPicture()` / `depthPictureView()`
  are the read side's only index (`resolveShadow` samples it, `depth_sampled` transitions act on it too); `ensureTarget`'s refusal changes from
  "multisampled + sampleable depth" to "device below 1.2".
- **Tests**: `VkMsaaTest.ASampleableDepthResolvesWithTheColour`; `VkShadowTest.AMultisampledMapCastsTheSameShadow`
  (a multisampled shadow map is byte-identical to the single-sampled one = the resolve really ran and samples the picture); `test_vk` 62 cases, `test_vsg` 562 cases,
  `ctest` 32/32.
- **Trap**: `vsg::Device::deviceMemoryBufferPools` is an observer — the pool is gone after the compile pass, so warm-up must hold its own
  `MemoryBufferPools`, otherwise what it gets is always null. `VkAttachmentReference` **cannot** be `reinterpret_cast` to
  `VkAttachmentReference2` (the latter appends sType/pNext/viewMask), build it field by field.

## 2026-11-14 —— sample-count contract closed: `max_samples` is a count, unbuildable requests are refused (framework first)

- **Decision**: `../decisions/ADR-2026-11-14-sample-count-refused-not-lowered.md`. `Capabilities::max_samples`
  is a **count**, legal counts are powers of two; a request the device cannot build (above the limit or not a power of two) is **refused once**, not lowered — so
  `RenderTarget::samples()` is always the number that was drawn.
- **Before the change there were three answers**: the docs said "refuse, never silently lower"; `vsg` behaved correctly but `max_samples` / `setMaxSamples` carried a
  **bit mask** (this machine answers 15, a host `setSamples(15)` walks all the way to `samplesToVk` rounding to 2 while `shape().samples` stays 15),
  and did not check for "not a count" requests; `vk` (M11) **lowered** over-limit requests, and lowered "multisampled + sampleable depth" to single sampling.
- **`vsg`**: new `largestSampleCount(flags)` (`StateCommands`, next to `samplesToVk`); `capabilities()` and
  `setMaxSamples` both carry counts (this machine 8); `HostTargets::build` additionally refuses non-powers-of-two; `reportUnbuildableTarget` separates
  "above the limit / not a count / borrowed depth" into three sentences.
- **`vk`**: `ensureTarget` decides with `ensureBuildableSamples(...)` (power of two ∧ in the supported set), otherwise reports once and
  `return false` (target not built, pass skipped, `readColorBuffer` answers `NotReady`); "multisampled + `isDepthPromoted`"
  is refused the same way (backlog **T40**); `clampSampleCount` is deleted.
- **Frame counters corrected along the way**: `executeFrame`'s `FrameCounters` count only **actually recorded** passes / draws (the semantics
  `FrameCounters` documents, and consistent with vsg's packets), a refused target no longer counts a draw; a refusal no longer adds an `Info`
  "no pass this backend can run" either (the refusal already said why).
- **Tests**: `VkMsaaTest` 5 cases — the count limit; a 4-sample picture (**now really multisampled**: that target used to promote depth by default,
  was silently lowered to single sampling, i.e. resolve was never tested); over-limit refused + reported once; 3 refused; multisampled + sampleable depth refused.
  `MsaaTargetTest` gains a 3 assertion; new `BackendParityTest.TheSampleLimitsAgreeBetweenTheBackends`
  (the same machine, both backends, the same number: this machine vsg=8 / vk=8).
- **Verification**: `test_vk` 61 cases (60 pass 1 skipped, validation silent), `test_vsg` 520 cases (517 pass 3 skipped), all eight gates clear.

## 2026-11-14 —— `gfx_backend_vk` M13: warm-up submits its own "transfers only" batch (framework first)

- **Decision written first**: `../decisions/ADR-2026-11-14-prepare-may-submit-transfers.md`. `prepareFrame` still **submits no frame**
  (no acquire, no present, no touch of `FrameProgress::submitted` / `presented`), but may submit **one command buffer containing only transfers**:
  copies + layout transitions, no render pass, no draw.
- **Changed the contract text (not only the backend)**: `RenderFrameExecutor::prepareFrame`'s "must not submit" splits into
  "submits no frame / may submit one transfers-only batch"; `PrepareReport::Outcome::Prepared` and the `uploads` comments change with it
  (`uploads` now counts **streams + textures**, same meaning as `FrameUploads`); design doc §3.7 / §6.11 `P11` / §7 `D13` / §8 `T35`;
  `.ai/README.md` index +1.
- **vk implementation** (`Device::prepareFrame`): lazily allocate one command buffer (only at the first serviceable pass), each pass calls
  `ContentDraw::preparePass(batch, draw.draws)` after `content.prepare(...)` (1×1 stand-in + material textures),
  after the loop `vkQueueSubmit` + `vkWaitForFences` only when `hasPendingUploads()` is true (reusing `impl->completion`),
  success then `ContentImages::releaseUploadStaging()`. **Zero work, zero submission**: a second warm-up of the same frame submits nothing.
- **Two small mouths on `ContentImages`**: `releaseUploadStaging()` (drops staging only, `releaseFrame()` now reuses it) and
  `hasPendingUploads()` (non-empty staging == a copy batch is recorded but never waited on — that is the answer to "submit or not").
- **Honest failure path**: a submit / fence failure is a device-level failure (OOM / lost), the entry is already resident but the image was never written, so it reports
  `SubmissionFailed` only, and **does not** promise "retransmit next frame" (an unchanged revision never retransmits).
- **Tests**: `VkPrepareUploadTest` 2 cases (warm-up reports 3 uploads = 2 streams + 1 texture, the following frame's `stream_uploads` /
  `texture_uploads` are all 0 and the pixels are the texture colour; the second warm-up's `uploads` / `halves` / `pipelines` are all 0 and the frame watermark does not move);
  `VkResidencyTest.PrepareBuildsWhatTheFrameNeedsAndSubmitsNothing` passes as-is (it now also pins "a transfer batch is not a frame");
  `test_vk` 59 cases (58 pass 1 skipped, validation silent), `test_vsg` 519 cases (516 pass 3 skipped).
- **`vsg` still leaves the upload to the first frame** (its copies live in the frame's transfer task, not the frame's command buffer) ⇒ backlog **T39**.

## 2026-11-14 —— `gfx_backend_vk` M12: the escape hatch at the exposure boundary and its gate

- **Shape** (`../research/graphics-2026-11-13-vk-exposure-boundary.md` §4.5): `support/Handles.hpp` declares only
  `Handles vkHandles(const RenderDevice& noexcept)`; the value `Handles` lives in `internal/Handles.hpp`. **The dependency direction can only be
  support → internal**: `internal/Device.hpp` never includes `support/` (otherwise the gate becomes the plugin's own dependency, and E1 becomes
  an empty check).
- **`Handles`**: instance / physical / device / graphics / graphics_family + `validation` (M0's evidence line relies on it).
  Empty before `initialize()` and after `shutdown()`; a non-vk device makes `dynamic_cast` fail and answers `{}`. `Device::handles()` is an internal
  accessor; `src/support/Handles.cpp` is the one place that `dynamic_cast` happens.
- **Gate**: `scripts/check_vk_exposure.py` (CTest `vk_exposure`) — the V2 SDK backend surface includes no vulkan/volk/vsg;
  V4 the plugin `shell/` has zero `Vk*` (comments count); E1 neither internal half includes `support/Handles.hpp`.
  `internal/` and `support/` deliberately have no gate (§6.2: a gate would only force out shadow types).
- **Tests**: `VkHandlesTest` 1 case (not started → running → after shutdown lifecycle); `test_vk` 57 cases (56 pass 1 skipped).
- **New finding on this machine**: `test_graphics` does not link on Windows (W3's `detail::expandFrame` / `FrameRunner` lack
  `VN_GRAPHICS_API`), recorded as backlog **T38**; so "whole-tree ALL_BUILD" is not a valid criterion on this machine, the evidence is
  `test_vk` / `test_vsg` / plugin targets + the gates.

## 2026-11-14 —— `gfx_backend_vk` M11: multisampled targets

- **The limit is a count**: `caps.max_samples = largestSampleCount(framebufferColorSampleCounts & framebufferDepthSampleCounts)`
  (this machine 8). `ensureTarget` clamps a request to "the largest supported count ≤ the request" with the same set, reports once above the limit and builds with the closest count;
  unsupported odd counts (e.g. 3) are silently lowered. **vsg still answers bit mask 15** (backlog T37).
- **Shape**: `TargetImages` creates **sample buffers** (`COLOR_ATTACHMENT` usage only) + a **single-sampled resolve image** when multisampled;
  render pass attachment order = [sample buffers…, resolve…, depth], `pResolveAttachments` points at the resolve, the framebuffer is in the same order.
  **`colors` / `color_views` are the picture (the resolve image) from now on**: screen draws sample it, `readColorBuffer` reads it,
  `color_sampled` transitions act on it too; the sample buffers are valid only inside the render pass.
- **depth does not resolve**: created with the same count, but when `target.isDepthPromoted()` (depth may be sampled) it is forced to single sampling and reported once —
  otherwise the shader / readback gets samples instead of the picture.
- **Clear values by attachment index**: a multisampled target's depth slot = `colorCount()*2` (`depthAttachmentIndex()`), not `colorCount()`.
- **pipeline**: `rasterizationSamples = StateMapping::sampleCount(key.compatibility.samples)`;
  `RenderPassCompatibility` already includes samples in the key, so single and multisampled never share a pipeline.
- **Tests**: `VkMsaaTest` 3 cases (limit=8; a 4-sample target's full-screen monochrome centre/corner = 51/102/153, and the centre equals the single-sampled one;
  an over-limit request is served and reported once); `test_vk` 56 cases (55 pass 1 skipped); `test_vsg`'s parity / offscreen / sdk-pixel 20 cases all pass.

## 2026-11-13 —— third-round audit W3: `FrameWorker` split into `PublicationStore` + `FrameExpander`

- **Object-level back edge removed** (`G2` / `G12`): `detail::PublicationStore` (`src/viz/graphics/src/PublicationStore.hpp`)
  owns publishing and binding (`publish` / `bindSnapshot` / `hasExplicitSnapshot` / `beginFrame` / `sourceOf` + the table lock);
  `detail::FrameExpander` (`src/viz/graphics/src/FrameExpander.hpp`, deleted in W9a) only reads it (`runPass` / `run` / `unbound`).<!-- drift-ok -->
  The content half no longer needs the engine half's objects; the two old `FrameWorker` files are deleted, `SceneSnapshotRef`'s friend changes to
  `detail::PublicationStore`.
- **Publication granularity**: `buildFrameValue` first runs every `before_draw` of this frame (recording each hook's cost), then publishes "once per frame per scene",
  and finally describes. The old shape published once per pass (the following N-1 `Unchanged` still paid the lock + epoch comparison); publishing first and then running later hooks would leak
  what a hook wrote out of this frame (`RenderEngineTest.AFrameAnswersOneStateSoEveryPassSeesWhatAnyHookWrote` pins this).
- **`record_micros`**: the hook phase records hook costs first, the describe phase adds publishing (the first pass that names the scene) + resolve + describe,
  the total account stays "describe + collect".
- **Tests**: the old `FrameWorkerTest` (deleted) splits into `PublicationStoreTest.cpp` and `FrameExpanderTest.cpp` (renamed `FrameExpandTest.cpp` in W9a);<!-- drift-ok -->
  `FrameRunnerTest` / `ValueChannelTest` use the two objects. `T3` / `T31` are pinned by
  `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch`; `T34` by
  `FrameExpanderTest.AFrameExpandsTheSamePictureTheSceneWould`, `T35` by
  `FrameExpanderTest.ABoundPublicationIsThePictureTheFrameDraws`.
- **Counts**: `test_graphics` 556 / `test_vsg` 542 / TSan 556 cases 0 warnings. Full parallel ctest on this machine
  occasionally reports `test_vsg`'s `backend->initialize()` false (4 cases) and a moving-camera measurement over budget; a single-suite rerun is green, rerun before suspecting a regression.

## 2026-11-13 —— third-round audit W4: the plan expresses only the shape of a `PassKind`

- **Rule**: a plan can only say `Scene` / `Screen`; a subclass pass is `describe()`'s escape hatch, outside the plan contract. An in-place update of a declaration with the same name and kind
  keeps it (the node and `describe()` both stay alive); a kind change would rebuild it as the built-in base class, so `applyPlan` refuses it.
- **New code**: `PlanProblemCode::SubclassPassWouldBeRebuilt` (`subclass-pass-would-be-rebuilt`), decided by
  `RenderEngine::collectRebuildProblems` after `validatePlan` — a pure plan check cannot see "which object is registered under this name".
  The decision uses the C++ dynamic type (`typeid`), not the metatype: a quick subclass without `VN_OBJECT_META_DECL` must be caught too.
- **Tests**: `PlanTest.AKindChangeOnACustomPassIsRefusedWithItsCode` (in-place update keeps the node → kind change refused →
  a built-in pass can still change kind); `TheCodeSpellingsAreStable` gains the new token. `test_graphics` 557.

## 2026-11-13 —— third-round audit W6a: `Scene`'s derived side moves into `detail::ScenePublisher`

- **What moved**: `SceneBake`, the per-view `DrawListMemo`, `content_revision_` / `memo_epoch_` / `content_frame_`,
  the edit batch `edit_depth_`, the published version pair and `published_snapshot_`, and three counters all land in `src/ScenePublisher.{hpp,cpp}`.
  `Scene` keeps only the author tree (root / lights / visible / opacity), queries (boundingBox / findNode) and delegation
  (collect / publish / beginEdit / endEdit / setContentFrame / invalidateContent / counters). The dependency direction is only
  Scene → publisher (the publisher reads the author's tree via `Scene&`; `Scene` is its friend).
- **The gate did not change**: `detail::SceneRows::snapshot(scene)` is still a one-line gate, forwarding to `ScenePublisher::snapshot()`; `SnapshotShadowTest` /
  `RenderWorldTest` need no change to their references. The empty shared command table is unified by one `detail::noCommands()` (two TUs each holding a copy would break
  `TheSharedCollectionIsOneListAndTheOwningSpellingCopiesIt`'s "the empty list is the same" assertion).
- **New files need a reconfigure**: library sources are `file(GLOB_RECURSE)` (no `CONFIGURE_DEPENDS`), after adding a .cpp you must
  `cmake -S . -B build` (`build-tsan` likewise) before building, otherwise the link only sees undefined symbols.
- **Tests**: `test_graphics` 557 / `test_vsg` (gtest_list 546 cases) all green / TSan 557 cases 0 warnings; public API and test semantics untouched.

## 2026-11-13 —— third-round audit W2: state marks on the migration APIs

- The four entries `applyPlan` / `plan` / `bindSnapshot` / `prepare` are marked "migration API, not yet a host contract": the design docs
  (`graphics-frame-pipeline.md` §4, `graphics-threading.md` §4.2, `graphics-architecture.md` §1.3 / §2.10) and the
  public headers' `@note` say so together; no new public entry before 5b-2c.
- **Decision**: `FramePlan` is renamed `FrameTable` in 5b-2c (it reads the compiled frame table, not the plan); `SessionBusy` moves out of
  `PlanProblemCode` with `W7` (it describes a runtime precondition, not a shape defect). Both change together in the W9 window, public signatures stay untouched until then (`C11`).
- Counts: 0 code changes; the production path is still the `addPass` family + the implicit publish at frame start.




## 2026-11-13 —— W9a (W6b): the frame value holds only publications, `Scene` is no longer a `DrawListSource`

- `Scene` drops the `DrawListSource` base class: `collectRenderCommands` is still the content thread's own query (no longer an override),
  `ScenePublisher` no longer implements that interface either. The frame's binding and expansion are centralised in `detail::FrameExpand`: `bindSceneDraws` points unbound
  scene draws at their publication source, `expandPass` / `expandFrame` only read values.
- `RenderPass::describe` writes only VIEW (camera / lights values), not content; `buildFrameValue` binds after describe.
  `FrameExpander` (bind + `unbound()`) is deleted entirely — binding was its only job; `PublicationStore::sourceOf` now looks up by
  `Scene`, lookup only, no creation (not published ⇒ null ⇒ draws nothing, `FrameExpandTest.ADrawNoPublicationAnswersDrawsNothing`).
- `RenderPass::execute` (the host driving one pass itself) publishes + binds itself: `PublicationStore` must outlive `expandPass`; putting it in the
  `if` scope is a use-after-free (hit locally, `RenderPassTest.Execute*` segfaults).
- Tests: `FrameExpanderTest` → `FrameExpandTest` (6 cases; file and suite both renamed), `FrameRunnerTest` / `ValueChannelTest`
  changed to pass the publication source, `PassDrawTest` goes through binding too. `test_graphics` 558 / `test_vsg` 546 all green.

## 2026-11-13 —— W9b: the frame no longer publishes, publishing is the content writer's verb

- **Deleted**: `RenderEngine::buildFrameValue`'s "implicit publish once per frame per scene"; `PublicationStore::publish` /
  `hasExplicitSnapshot`. `bindSnapshot` drops from "take over" to **override** (the default is the scene's own latest publication).
- **Added**: `Scene::publishedSnapshot()` (the one the frame build takes); `PublicationStore` holds one `SnapshotSource` per `(content, revision)`,
  `beginFrame` drops sources nobody draws any more (there is only one frame in flight ⇒ at most two per content); `SceneView::publishContent()`
  / `PassChain::publishContent()` (the chain-owned transparent scene); `AxisGizmo::update` / `FpsOverlay::update` publish themselves in their hooks
  (a hook that writes content must publish, otherwise this frame does not see it).
- **Host**: `SurfaceWindow::renderFrame` calls `view()->publishContent()` then `engine->frame()` every frame. No wait, no bind —
  the frame in flight pins the value taken at build time.
- **Failure shape**: an unpublished scene ⇒ the draw names no source ⇒ nothing is drawn + one `ContentSkipped` ("pass 'x' draws a scene that was
  never published"), instead of silently reading the live tree.
- **Tests**: `PublicationStoreTest` rewritten as four cases (three states / one publication one source / a source dropped / binding overrides); `test_graphics` 561 /
  `test_vsg` 546 all green. Engine/pixel cases move to "host verbs": `scene->publish()` or `view->publishContent()`, hook cases publish
  inside the hook.

## 2026-11-13 —— W9c: the light live-pointer channel is deleted

- `DrawDescription::lights` (a `raw_ptr<const Light>` array) and `FrameRecorder::setLights(std::span<const Light* const>)`
  / `snapshotLights(Light*...)` are deleted; `light_values` is the only channel, `Light::value()` is called on the content side only.
- The old `FrameRecorder::render(..., const ShaderProgram*)` overload that read a live program is deleted along the way (only the description version remains).
- Tests: `MockBackend` reads `light_values` (`last_light` becomes `std::optional<LightValue>`), `OwnedFrame` copies
  `light_values`; LightBlock/ShadowBlock/ContentPass tests convert at the call site with the new `tests/test_vsg/LightValues.hpp`.
  `test_graphics` 561 / `test_vsg` 546 all green.
- `RenderCommand::geometry/material/program` stay: they only carry identity and lifetime (stream binding, residency, `releaseContent`'s "who still holds it"),
  the recording side no longer reads revision / transparency / light values from them; deleting them needs a content registration channel (triggered by T30/T22).

## 2026-11-13 —— W9d: the `addPass` family is deleted, the plan is the only shape

- **Deleted**: `RenderEngine::addPass` ×3 / `removePass` / `clearPasses` (`removePass` becomes private, for the plan's own diff and
  scope teardown); `PassRegistry::add` ×3 / `remove` / `clear` / `addOwnedBy`; `PassProblemCode::SubclassPassWouldBeRebuilt`
  (a plan expresses only `PassKind`, subclass passes no longer exist).
- **Added**: `applyPlan(plan, const void* owner)` (scope form: **upsert only, no deletion**, identity is `(owner, name)`),
  `planFor(owner)`, `pass(name)` / `passFor(name, owner)`; `PassRegistry::apply` / `find` / `findFor` / `clearOwnedBy`.
- **Chains**: `PassChain` only collects declarations (`addPass` no longer touches the engine), `apply()` submits the whole plan at once (so "one pass reads another's produced
  name" validates as a whole); `PassChain::build(engine, desc, owner)` lets the host name the scope, **a rebuild restates the same scope** (nodes update in place,
  backend state untouched); `SceneView` is its chain's scope and `clearOwnedBy(this)` on destruction; `Viewer` / `buildFor` pass the view.
- **Other contributors**: `DemoScene` uses its own `RenderControl` as scope; the `addOffscreenToScreen` preset uses the engine as scope, two
  declarations in one plan; tests use `tests/PlanTestSupport.hpp` (`applyPassForTest` / `registerPassForTest`, scope = the engine).
- **Trap**: `apply()`'s "refresh declarations from nodes" can only work on **already submitted** rows (`applied_`), otherwise a rebuilt new declaration would be overwritten by the old node.
- **Tests**: `test_graphics` 560 / `test_vsg` 546 all green (one case testing only the subclass kind change deleted; `AddSamePassTwice...` changed to
  "same name twice = one row, the last declaration wins").

## 2026-11-13 —— W9e: result shapes closed, `FramePlan` renamed `FrameTable`

- **`FramePlan` → `FrameTable`** (file / type / accessor): `RenderEngine::framePlan()` → `frameTable()`, the row type
  `PassPlan` → `PassRow`, the dump header becomes `frame table:`; the demo's `logFramePlan` becomes `logFrameTable`. Reason: it reads the
  compiled frame table, not the plan (`Plan` is the declaration).
- **`frame()` two states**: `FrameOutcome` changes from an enum to a struct — `Status::{Ran, Submitted, NotReady}` + `reason`
  (`EngineNotReady::{NoDevice, NotInitialized}`), predicates `ran()` / `submitted()` / `driven()`. `NotReady` is a precondition,
  not a frame ending; `prepare()` reuses the same `EngineNotReady` (`Outcome::NotReady` + `reason`).
- **`applyPlan()` three states**: returns `PlanResult` — `Applied` / `Invalid` + `problems` / `Refused` + `PlanRefusal`
  (`Busy` / `EngineGone`); `PassRegistry::apply` has the same shape. `SessionBusy` is deleted from `PlanProblemCode` (`planCodeText` loses one entry),
  busy -> `Refused{Busy}` (answered before validation, diagnostics still come from `sessionChangeAllowed` once per episode).
- **Migration window closed**: the "MIGRATION ENTRY" notes on `applyPlan` / `plan` / `bindSnapshot` / `prepare` are deleted, the design docs and
  `docs/guides/graphics.md` follow; the guide's old `addPass` example gains W9d's registration spelling note (the example fields are still `RenderPass`
  setters, registration translates through `PassDesc` + `applyPlan`).
- **Tests**: `test_graphics` 561 (new `PlanTest.ABusyEngineRefusesThePlanInsteadOfBlamingItsShape`; the `SessionBusy`
  spelling assertion deleted; `frame()` assertions use predicates) / `test_vsg` 546 all green.

## 2026-11-13 —— F11 + F12: a total-order sort key and `FrameResult`

- **F11**: `detail::SortKey` (`DrawListBake.hpp`) replaces `sortCollected(commands, eye)`'s lambda — drawOrder → layer
  (`std::countr_zero(layers)`) → opaque/transparent → quantised camera-space depth (`view * center`'s -z, 1/4096 units, clamped to ±1e9) →
  `(row_index << 32) | primitive_index`. The representative point is the row's `world_box` centre (a view-resolved row uses `local_box.transformed(world)`),
  a box-less row falls back to the matrix origin. The key is produced by `selectRows` (row data lives only there), `sortCollected(commands, keys)` uses `std::sort`
  (unique tie ⇒ no equal keys, no reliance on stability). Both producers (`ScenePublisher` / `selectSnapshot`) pass keys now.
- **F12**: new `sdk/vine/graphics/FrameResult.hpp` — `FrameQuery{snapshot, view}` + `FrameResult{frame, present, query,
  passes, draws, diagnostics}`; `RenderEngine::frameResult()` is written once in `runEngineHalf` (`frame_result_mutex_`),
  `presentedCamera()` reads its `query.view` (`presented_camera_` deleted). `EngineFrame` gains `presented_snapshot`
  (`buildContentHalf` takes `publicationFor(presenting->content())`, at the same moment as the camera) and `diagnostics_before` (`diagnosticCount()`'s initial value).
- **Tests**: `SelectTest` (4 cases: total-order permutation, field order, box centre vs node origin, box-less fallback); `RenderEngineTest.
  TheFrameResultIsThePictureThatWasAnswered` (empty value → snapshot/camera/counts/diagnostics → an inter-frame publication does not change the answered frame). `test_graphics` 566.

## 2026-11-13 —— F8 rejected: one engine drives one surface

- **Decision**: `ADR-2026-11-13-one-engine-one-surface.md` — keep one engine per area; multiple windows = one device with multiple engines (one engine / one area / one `Viewer` per window).
- **Deleted / rewritten**: graphics-next's F8, `R37`, `T29`, `K10`, §9.3 step 4 deleted; `R21` / `R27` changed to one engine one surface; `SurfaceId` leaves the id table;
  §3.8 becomes "multiple windows: one engine drives one surface"; §7 gains `D12`.
- **Document contradiction fixed**: `graphics-architecture.md` F19 used to say "one engine can serve multiple windows" (contradicting the code's `createArea()` refusing a second area and
  the same document's §7 D2), now fixed; D2 gains the ADR link; §2.8's "the `Session` type lands with F8" becomes "none needed and none exists".
- **Reasons** (evaluation conclusion): no real consumer (`G8`); `FrameResult` / `PresentRecord` just landed as single values in F12, making them per-surface is rework (`G10`);
  appfw ownership (a window owns an engine) would need a rewrite; estimated ~900–1400 lines / 14–20 files.
- **Triggers**: a real host needs "the same plan in two windows with different viewports"; or a second backend makes "device = Session" necessary.

## 2026-11-13 —— T8: `select` is a pure function, concurrent runs agree

- **Test**: `SelectTest.SameInputSameList` — a 40-node scene (drawOrder / layer / transparency / depth all present) snapshot: repeating with the same view,
  interleaving another view, and after `scene.setRoot(nullptr)`, all match the serial reference `diffDrawLists` item by item; 8 threads × 32 rounds share one snapshot,
  each producing output, item by item equal to the serial answer. Assertions on the main thread only, workers count with `std::atomic` and record the first failing thread.
- **Why concurrency is legal (evidence)**: `intrusive_ptr` counters are atomic (`RefCounted`); snapshot chunks are read-only; `AutoTransformPlacement`
  holds numbers only; `selectRows`/`Frustum`/`SortKey` have no static state. So a red is a real bug.
- **Boundary**: the test calls `detail::selectSnapshot` directly, **not** through `SnapshotSource` (the memo belongs to the engine thread alone); `R15` (fold once)
  reuses `GraphicsTest`'s `contentCollect*` cases, no new counter.
- **Result**: `test_graphics` 567 all green; `scripts/tsan_check.sh` 567 cases 0 warnings (the parallel case 31 ms).

## 2026-11-13 —— backlog §2.2 review + T25/T26/T29 landed

- **Review conclusion (written back to the backlog)**: T9 (V2 `forget`) deleted (`forget(` exists neither in src nor in vsg 1.1.16); T24 rewritten
  (the old 1597 µs measured `Scene::collectRenderCommands`'s direct path, the frame goes through `SnapshotSource::selectSnapshot`,
  re-measure with the select recipe first); T31 upgraded to §1's **known potential race** (`StreamUploads.cpp`'s engine thread reads `Buffer::revision()`,
  that field is not atomic; today no host changes a buffer every frame); 2.4's duplicate T22 → T32; T8 notes the evidence rig is in place;
  the "content out of the library" entry is deleted (ADR-2026-11-13-content-seam-stays-in-the-plugin).
- **T25 + T29**: new `FrameUploads` (SDK `FrameStats.hpp`) hangs off `FrameReport::uploads` — this frame's stream/texture
  upload count, bytes and release count. `StreamUploads` gains `uploadedBytes()`, `MaterialImages` gains `built()` / `uploadedBytes()`;
  `VsgBackend` takes a baseline at `executeFrame` frame open and the difference **after recording** (uploads happen in `recordContent`, not `collectFrame` —
  the first version took the difference after collect and got all 0), the failure path zeroes first.
- **T26**: `StagingTask::releaseStaging()` gives back only blocks with `staging->size > minimumStagingBufferSize`, blocks at the limit are kept
  (a steady-state host uploading every frame allocates nothing); a new ADR supersedes ADR-2026-11-10's "keep nothing".
- **Tests**: new `FrameUploadsTest` (increments, steady state 0, buffer revision retransmit, content change reports releases); `UploadScratchTest` changed to
  "every live block is a multiple of the floor + one-off big blocks are given back" (`makeBigTriangle` 17 MiB). `UploadScratchTest`'s floor is no longer fixed at
  16 MiB: each used frame slot keeps one block (at most 3 blocks).
- **Shared fixtures**: `tests/test_vsg/TestTriangleContent.hpp` (TriangleFixture / makeTriangle / makeBigTriangle / makeCamera).

## 2026-11-13 —— T24: culling per row, not per chunk

- **Test**: `SelectTest.OneChunkCullsEachRowOnItsOwnBox` — one snapshot chunk (`row_count < kRowsPerChunk`) with two
  sibling segments each carrying its own parent transform (x = 1000 / x = 0), the walk order puts the **off-screen** one first; with the camera looking at the second segment, select answers exactly it
  (a chunk-level skip by the first row's box would answer nothing), and after changing the camera it answers exactly the other one (culling was not disabled). Assertions: `row.identity` finds the node,
  `parent_world.element(0,3)` proves the parent transforms differ, `Frustum::isOutside(row.world_box)` proves one box in and one out.
- **Account**: a chunk is a sharing unit, not a culling unit; if "chunk-level early culling" is ever done, the chunk box must be the union of all rows in the chunk (conservative),
  never `chain[0]`.
- **Docs**: `graphics-frame-pipeline.md` §3.3 F2 acceptance gains `T24`; §8's T24 criterion becomes "culling per row, not per chunk", implemented.

## 2026-11-13 —— backlog T24: select's Release recipe and re-measurement

- **Recipe**: `SelectTest.MeasureWhatOneSelectCostsWithAMovingCamera` — 2000 / 10000 drawable grids (all in view,
  depth layered), the snapshot published once, 10 views swept past the eye (pre-built, timing excludes camera construction); prints pure `detail::selectSnapshot` and
  the production miss (`SnapshotSource::collectRenderCommands`, including the memo lookup and the command vector's shared allocation), asserts the answer count and
  "every moving view is a miss".
- **Numbers (RelWithDebInfo, this machine ×3)**: 2000 → select 337–340 µs / source 435–443 µs; 10000 → select 2348–3067 µs /
  source 3388–3626 µs. Debug for reference only: 1815 µs / 10675 µs.
- **Conclusion**: 10k < 4 ms gate, 2000 < 2 ms line ⇒ no optimisation, T24 stays trigger-gated; but 10k already crosses 2 ms, the production path costs ~0.9 ms more than pure
  select (lookup + allocation), so when 10k drawables become everyday, look at that half first.
- **Docs**: backlog T24 replaces "add the recipe first"; `graphics-threading.md` §1.1's 1597 µs/frame (the old direct path) becomes the frame path's
  select / source numbers.

## 2026-11-13 —— backlog T31: race reproduced + TSan evidence

- **Case**: `StreamUploadsTest.AnEditFromTheHostThreadRetiresTheSameUploadASerialOne` — one thread loops
  `StreamUploads::beginFrame`, the main thread changes bytes inside a `Buffer::edit()` scope; the normal build asserts "a concurrent edit and a serial edit give the same answer"
  (the old upload retires, the next acquire retransmits).
- **TSan (clang-23, hand-relinked `test_vsg`)**: 1 report — write `Buffer<float>::bumpRevision()`
  (`Buffer.hpp:363`, from `Edit::~Edit`, main) vs read `Buffer<float>::revision()` (`Buffer.hpp:181`, from
  `StreamUploads::beginFrame`'s lambda, `StreamUploads.cpp:228`, T1).
- **Link trap**: `test_vsg` cannot be linked with `-fsanitize=thread` directly — `tests/test_vsg/AllocationCounter.cpp` replaces global
  new/delete, conflicting with `libclang_rt.tsan_cxx`'s strong definitions (multiple definition). Evidence recipe: `ninja -t commands test_vsg`
  to take the link command, drop `AllocationCounter.cpp.o` and relink. Formal coverage recorded as backlog T34.
- **Same-kind live reads (fix them together)**: `ChannelFacts::streamKey()` (`ContentFacts.hpp:233`), the instance buffer
  (`ContentPass.cpp:1123`), textures (`ContentSets.cpp:268`, `MaterialImages.cpp:516`).
- **Docs**: `graphics-threading.md` §2.4 gains the case pointer; backlog T31 records the evidence and the fix scope.

## 2026-11-13 —— T31 fix: revision counters made atomic

- **Change**: `Buffer<T>::revision_` and `Texture::revision_` become `std::atomic<std::uint64_t>`; writers (`Edit` scope
  end / `setRevision` / `setLayer`) use release, `revision()` uses acquire. `Geometry` / `ShaderProgram` / `Material`
  untouched — the frame side reads description values, not live objects.
- **Why it is enough**: observing a new revision ⇒ the bytes written before are visible; polling therefore has a defined order. Still not a value channel: writing bytes after the announcement without a new
  revision remains outside the contract.
- **Verification**: `StreamUploadsTest.AnEditFromTheHostThreadRetiresTheSameUploadASerialOne` is green in the normal build; the same TSan
  binary (relinked without `AllocationCounter.cpp.o`, see backlog T34) goes from "1 warning" to 0 warnings.
- **Docs**: `graphics-threading.md` §2.4 / F7 and `graphics-geometry.md` F18 become "counters atomic, bytes still not on the value channel";
  backlog §1 is emptied, T34's trigger recorded as hit.

## 2026-11-13 —— T30 evaluation: GPU resource abstraction

- **Conclusion**: stays trigger-gated (a second backend); no `GpuResource` base class. The rule layer (pin / retirement / residency books / frame values / capabilities /
  target decisions) is **complete**; the type layer is **incomplete and should not be completed now** (the host has no verb that creates a GPU resource, 0 of `RenderDevice`'s 18 virtual verbs).
- **Facts**: `backend/` 24 headers 3,897 lines (`BackendHalfGuard` compile-time isolation); the vsg backend 28,354 lines / 85 internal files —
  the second backend's cost is in the materialisation layer, not in "resource types". The loading path exists: `RenderBackendRegistry` / `RenderBackendFactory` /
  `RenderApi` / `vn_add_plugin`.
- **Preconditions**: T15 (`device_color_formats` / `subpass` and other Vulkan policies → opaque device format codes) and T32 (`RenderBackend` aggregate split
  into device / executor exits); S4/S5 separately.
- **Docs**: `../research/graphics-2026-11-13-gpu-resource-abstraction.md` (dated, non-normative), backlog T30 linked.

## 2026-11-13 —— vk's generalisation boundary (design argument)

- **Decision**: do Vulkan only; write new code and easily changed code in the general shape; shaders get no implementation-level generalisation, concepts only.
- **Principle**: generalise in "values / rules / mapping tables", not in "API objects". Criteria J1 (input vs output), J2 (is there a second consumer today),
  J3 (does it touch the public surface).
- **General from now (cost ≈ 0, by "not writing a second copy")**: `RenderBackendInfo`, `Capabilities`, `planTarget`, `PipelineKey`/
  `VariantPool`, `ResourceTable`/`RetirementQueue`/`FrameTimeline`, `ResidencyLedger`, `ReadbackResult`,
  `DiagnosticsRoute`, `FrameDescription`. Shadow types forbidden (`vn::backend_vk::PipelineKey` and the like).
- **The only new general thing**: the `BarrierPlan` mapping table (`ImageLayout` as purpose + `LoadOp/StoreOp` + `AccessKind` → Vk
  barrier), 150–250 lines + device-free table-driven tests; the counter-example is a "general barrier type".
- **Shaders keep concepts only**: what is missing is the **source language** (no fields, only the future shape `ShaderSourceLanguage` + a per-backend compile entry),
  not the block layout (`ShaderAbi.hpp:107` already states that both std140 / cbuffer packing hold); the toolchain (DXC / SPIRV-Cross) goes through an ADR.
- **Explicitly not done**: allocator abstraction, a second recorder, descriptor model abstraction, resource base class, swapchain abstraction, a general barrier type.
- **Discipline gates**: D1 `backend/` public surface zero diff; D2 `Vk*` only in `src/internal/`; D3 residency books complete; D4 same values same picture;
  D5 resize / state changes do not rebuild pipelines.
- **Docs**: evaluation §9 (J1–J3, the list, D1–D5), backlog T35 linked and `BarrierPlan` scheduled into M1.

## 2026-11-13 —— Vulkan exposure boundary (design argument)

- **Conclusion**: the plugin's `internal/` + `support/` **may** expose Vk freely (18/57 headers contain `Vk*` today); the SDK host surface and the SDK backend surface carry **zero Vk types**
  (the measured hits are all comment text); the plugin `shell/` is 0, but **shape only, not a gate** (plugin headers are not installed, no second consumer, `G8`).
- **The Vk access a learning backend needs**: a **free function** in `support/Handles.hpp` (not in `RenderDevice`'s verb table, named after the backend,
  not installed, not included by the production path) — the same shape as Diligent's `ITextureVk` / QRhi's `nativeTexture()` / Godot's `get_driver_resource()`.
- **What is checkable**: V1 host surface zero Vk types (today distinguished by hand/comments), V2 backend surface zero `<vulkan>`/`<vsg>` includes (already 0),
  V3 the install component contains no Vulkan headers, V4 the plugin `shell/` zero Vk (already 0). Upgrade triggers: plugin headers start being installed / a second module includes `shell/` /
  `support/` is included by the production path.
- **Downgrade**: the previous round's "D2 only allows `src/internal/`" is withdrawn (inconsistent with vsg's `include/vine/backend_vsg/internal/` layering, and with no victims),
  replaced by V4 + upgrade conditions.
- **Docs**: `../research/graphics-2026-11-13-vk-exposure-boundary.md` (dated, non-normative).

## 2026-11-13 —— `gfx_backend_vk` M0 lands (two rounds of naming: `vkraw` → `vk` → `vn::backend_vk`)

- **Naming**: plugin `gfx_backend_vk`, registration name `"vk"`, namespace `vn::backend_vk`, include path `vine/backend_vk/`,
  classes `vn::backend_vk::{Device, BackendFactory, Plugin}`, macros `VN_BACKEND_VK_{LIB,API,NS_BEGIN,NS_END}`,
  tests `tests/test_vk` (`VkBackendTest`). **The two namespace segments each say one thing**: `backend` = this is an **implementation** of the render seam
  (`vine/graphics/` is the seam itself), `vk` = the **direct Vulkan** implementation path (`vsg` goes through VulkanSceneGraph).
  The previous version was `vn::vk`, rejected for two reasons: it reads like "Vine's Vulkan wrapper library", and `vn::vsg` collides with VulkanSceneGraph's own
  `::vsg` namespace. `"vk"` as a **registration name** is unchanged (the host creates by name); `"vsg"` is `RenderApi::Vulkan` like it, the difference is the
  implementation path, not the API family.
- **Hard constraint**: the `Vk*` spelling belongs to Vulkan's own types inside the module (volk brings in `vulkan.h`), so our own classes **cannot** be called `VkDevice` /
  `VkInstance` and the like; `vn::backend_vk::Device` is the safe name.
- **M0 content**: volk creates the instance (validation layer and debug-utils enabled by availability) / picks a physical device with a graphics queue / creates the logical device;
  `capabilities()` honestly reports `render_targets=false`, `presentation=false`; `executeFrame` and `setRenderTarget` each report
  "M1/M2 not implemented" once; `shutdown()` is idempotent and reentrant.
- **One trap**: `tp::volk` is compiled with `VOLK_NO_DEVICE_PROTOTYPES`, device-level functions have **no** free-function declarations; the correct usage is
  `volkLoadDeviceTable(&table, device)` then `table.vkDestroyDevice(...)`. Including `<vulkan/vulkan.h>` + `<volk.h>` directly
  makes `vkEnumeratePhysicalDevices` ambiguous between the global and `volk::`.
- **Verification**: `test_vk` 3 cases all green; this machine `validation=on`, `InitFailed` diagnostic count 0; `plugins/vine/gfx_backend_vkd.so` builds and deploys.

## 2026-11-13 —— the vsg backend aligns its naming: `vn::backend_vsg`

- **Scope**: `vine/vsg/` → `vine/backend_vsg/`, `vsg_global.hpp` → `backend_vsg_global.hpp` (the old name is recorded only here `<!-- drift-ok -->`), `vn::vsg::` → `vn::backend_vsg::`,
  `VN_VSG_{LIB,API,NS_BEGIN,NS_END}` → `VN_BACKEND_VSG_*`. 176 files in total (src/tests/cmake/scripts/docs/.ai).
- **Untouched**: VulkanSceneGraph's own `::vsg` (2134 `::vsg::` occurrences and the global `namespace vsg {` forward declarations) — inside the module the spelling always carries a leading
  `::`, which is also why the batch replacement was safe.
- **Class names unchanged for now**: `VsgBackend` / `VsgExecutor` / `VsgHostWindow` … are redundant under `vn::backend_vsg`, but that is ~1000 references with no
  behavioural gain; left as a separate step (if done, follow the vk module's `Device` / `BackendFactory` / `Plugin`).
- **One real trap (a gate failing silently)**: `scripts/check_doc_symbols.py`'s `MAPPED_DIRS` said
  `PLUGIN / "include" / "vine" / "vsg"` (segmented strings), the text replacement could not match, so it **still reported OK**, but the covered units fell from 104 to 47.
  Fixed to `"backend_vsg"`, restored to 104. Lesson: after a rename, look at the gate's **counts**, not only its exit code.
- **Rest**: the old path in `scripts/vn_shader_check.sh`'s comments is updated; `content_seam` ctest green.
- **Verification**: `test_vsg` 548 all green (1 of 5 runs had a flaky red `EachAttachmentClearsToItsOwnColour`, recorded as backlog T36 with diagnostics),
  `test_vk` 3/3, `gfx_backend_vsg` / `gfx_backend_vk` both build and deploy, the four doc/header gates + the symbol gate all clear.

## 2026-11-13 —— `gfx_backend_vk` M1: offscreen clear + readback

- **`BarrierPlan`** (§9.3's general table): `ImageLayout` (as "purpose") + `LoadOp`/`StoreOp` → `VkImageLayout`/stage/
  access; plus `transferReadState()` (a readback is not a pass, so this end has no SDK spelling, it stays in the plugin) + `barrierBetween`.
  5 device-free table-driven tests: one distinct layout per purpose, no barrier needed for itself, fields on both sides of a barrier, Undefined/Present ends, load/store mapping.
- **`TargetImages`**: one set of VkImage/memory/view/render pass/framebuffer per target; the render pass is fixed at `CLEAR`/`STORE`,
  initialLayout `UNDEFINED` (M1's contract: a pass drawing into a target must declare clear, otherwise it reports once and skips). resize in M1 is a rebuild
  (M3 replaces it with `planTarget`'s `ResizeInPlace`).
- **Device**: command pool + fence (created in init, destroyed in shutdown), `executeFrame` one command buffer per frame, waits on the fence after submit;
  `readColorBuffer` goes through a staging buffer (HOST_VISIBLE|COHERENT), barrier → `vkCmdCopyImageToBuffer` → barrier back to
  `COLOR_ATTACHMENT_OPTIMAL`, supports RGBA8 only (other formats answer `Unsupported`).
- **capabilities**: `render_targets` becomes true; `presentation` stays false. M0's test assertions change with it.
- **Verification**: `test_vk` 10 cases all green (3 backend + 5 barrier + 2 offscreen), this machine `validation=on` and the new path reports nothing;
  `plugins/vine/gfx_backend_vkd.so` builds and deploys.

## 2026-11-13 —— the second backend is set to hand-rolled Vulkan (`vk`, learning/research)

- **Trigger correction**: it is a second backend of the **same API family** ⇒ neither T15 (device independence) nor T30 (unified types) **triggers**; their trigger narrows to
  "a second **API family** backend"; T32 does not block (implement the aggregate directly).
- **What it consumes is the seam**: `RenderDevice` (18 verbs) + `RenderFrameExecutor` (2 verbs) + `WindowPresentTarget`; lifetime rules reuse
  `backend/`'s `ResourceTable` / `RetirementQueue` / `FrameTimeline` / `ResidencyLedger`, L1 blocks follow `ShaderAbi`.
- **Shape**: plugin `gfx_backend_vk` registers factory `"vk"` (`RenderApi::Vulkan`); not the default backend; tests separate
  (new `test_vk`, compiling the plugin sources directly like `tests/test_vsg`).
- **Milestones**: M0 plugin+device; M1 offscreen clear+readback; M2 one content draw (glslang→pipeline, channels→upload, View/Draw blocks);
  M3 targets/state/MRT; M4 material/texture/light/screen draw; M5 swapchain; M6 residency/prepare; M7 comparison with vsg.
  M0–M2 about 2–4k lines, M3–M6 about 10–20k lines. Details are in backlog T30.
- **Docs**: backlog §1 new T35 (the only "doable now" item).

## 2026-11-14 —— the light block moves up into the SDK (`LightAbi.hpp`): one shape, one home

- **Motivation**: when `vk` landed it copied `VineLightsBlock` and `packLightBlock` from vsg (the M4/M9 notes are the evidence) —
  the same ABI rule written by hand twice; the comparison then proved such copies drift (M7's depth fold is one example). The trigger "a second backend needs
  the same light layout" has hit, so backlog `S5` closes.
- **The home is not `ShaderAbi.hpp`**: the light block's capacity `kMaxLights`, unused-slot label `kLightKindEmpty`, shape `VineLightsBlock`
  and packing `packLightBlock` / `packedSlotOf` all live in `sdk/vine/graphics/LightAbi.hpp` (implementation
  `src/viz/graphics/src/LightAbi.cpp`). Reason: packing needs `LightValue` / `CameraSnapshot`, pulling them into
  `ShaderAbi.hpp` would make the ABI leaf depend on the scene model (back edge + rebuild fan-out); the dependency direction is fixed as
  `ShaderAbi.hpp` <- `LightAbi.hpp` <- backends.
- **Deleted copies**: each backend's `LightBlock.hpp` + `LightBlock.cpp` (`ContentPass` / <!-- drift-ok -->
  `ShadowBlock` / `ContentPipeline` / `ContentDraw` now include `LightAbi.hpp`); `vk`'s `ContentDraw.cpp`
  private `struct VineShadowBlock` (80B, same shape as the SDK) is deleted too, replaced by `vn::graphics::VineShadowBlock`.
- **Tests moved**: the pure packing cases move from `tests/test_vsg/LightBlockTest.cpp` to `tests/test_graphics/LightBlockTest.cpp`
  (10 cases: shape/view space/rotation/disabled and unavailable lights/empty announcement/point light/spot light/8 slots/no camera/slot rule); `test_vsg` keeps only three
  device picture cases (content four-band, two paths same binding, full-screen three-band).
- **New trap**: `LightValue::identity` defaults to null — taking the **address** of a stack `LightValue` as identity in a unit test always answers
  `kMaxLights`; a light that must be named needs an explicit `identity = &stable_id`.
- **Verification**: `test_graphics` / `test_vk` / `test_vsg` all green; gates in the same commit.

## 2026-11-14 —— material / draw block packing moves up into the SDK: one block packed in one place (C8)

- **Motivation**: the continuation of the light block's ruler — the material and draw field mappings were written once per backend (vsg `blockOfMaterial` /
  `packDrawBlock`, vk `buildMaterialBlock` / `buildDrawBlock`). Adding one factor meant editing two places, and missing one left a backend
  "old" with nothing in the log.
- **Homes**: `MaterialAbi.hpp`'s `packMaterialBlock(values, present_slots, out)`, `DrawAbi.hpp`'s
  `packDrawBlock(model, opacity, point_size, out)`, one `.cpp` implementation each; `ShaderAbi.hpp` stays the zero-dependency leaf.
  This "shape vs fill surface" layering and "one block packed in one place" are written into `graphics-shader-params.md` §5.1's C7 / C8.
- **Details**: the material block's slot mask is the **caller's** declaration (vsg = slots with declared textures; vk = slots that really uploaded, a failure leaves 0 so the
  shader falls back to constants); the default material does not call packing (`VineMaterialBlock{}`'s member defaults are the default material's values). The draw block's
  reserved words `params.z/w` are always written 0, not merely by `{}`.
- **Deleted vsg units**: `DrawBlock.hpp` / `DrawBlock.cpp` (the public name disappears with them, `packDrawBlock` is now <!-- drift-ok -->
  `vn::graphics::`); `ContentPass` / `ContentSources` call the SDK; vk's `buildMaterialBlock` /
  `buildDrawBlock` are deleted, call sites hand the values to the SDK themselves.
- **Tests**: the arithmetic cases move to `tests/test_graphics/DrawBlockTest.cpp` (3 cases), new `MaterialBlockTest.cpp`
  (4 cases: field row / no clamping / default material == ABI default / mask passthrough); vsg's `DrawBlockTest` keeps only the "value path"
  case (recorder → compiler → bytes).
- **The shadow block's exception** (written into §3.3): the input `ShadowFacts` is in the backend half (`backend/FrameCompiler.hpp`, D1 not open),
  each backend packs its own; but slot derivation must call `packedSlotOf`.

## 2026-11-16 —— vsg S8c-1: the frame body shares the device, device content goes behind one content lock (T48)

- **Which gate changed**: `graphics::FrameGate` (one frame at a time) in vsg's place becomes `internal/DeviceGate` (new unit):
  the frame body takes **SHARED**, host verbs take **EXCLUSIVE**, thread-local depth turns "a callback from inside the frame body" into an **empty lease + refusal**
  (readback `InsideFrame` → `ReadbackResult::Failed`; content verbs false/no-op + `vsg.device.reentry`), never waits on itself.
  The diagnostic route and the episode lock must be **recursive** (the sink calls back).
- **Frame state belongs to the driver**: `VsgExecutor::FrameDrive` (scratch / driving / uploads_before / last_uploads) replaces
  `Data::{driving, scratch, driving_executor, uploads_before}`; `presentWindow` takes the driver back through the **thread-local frame in flight**
  (`FrameInFlight`) — the window's `present()` has no frame parameter. `driving` is **not cleared** at frame end (the test seam
  `BackendContentAccess::windowTarget` and diagnostics read "the last frame's window"), only cleared at shutdown.
- **Frame-head snapshot**: `openFrameHead` takes the content lock once: `passes.adopt`→`FrameScratch::pass_ids`, `adoptHostTarget`,
  `targets.observe`, `store.track` (moving the tracking out of `collectContentDraw/collectScreenDraw`),
  `collectTargetFacts`→`scratch.facts`, staging sweep, `growBlockStorageIfNeeded`, upload baseline.
  Collect/compile (`collectFrame`/`closeFrameBooks`) read scratch only ⇒ the plan compiler's report reaches the sink **without holding the lock**
  (that is where the overlap evidence is caught). **Content phase**: `commitFrameThrough` takes the content lock once for applyTargetPlans/material arena/
  `recordContent`/`executor.record`/sweep/commit, and writes `last_uploads` + `last_counters` at every exit.
- **Per-session frame claim**: `WindowSlot::frame` / `Data::offscreen_frame` two mutexes, the frame body holds one until the end. Two frames of the same window / the same offscreen session
  **wait** (two offscreen sessions producing pictures concurrently is a supported shape, it cannot refuse); different windows do not wait on each other.
- **Counters belong to the session**: `SessionOptions.observe` no longer borrows `d->observe` (the recorder resets counters at frame open, sharing would overwrite them);
  `frameReport().counters = Data::last_counters` (a copy of the last finished frame), retention stays device-level.
- **The real race hit**: two drivers running an offscreen frame for the **first** time concurrently — `ensureOffscreenSession`'s lock-free lazy creation lets the loser overwrite a half-built viewer
  (segfault, appearing irregularly in the test suite) — creation must happen under the content lock.
- **Still open for S8c-2**: the recording path still reads the device tables (halves/sets/uploads/images/material slots) ⇒ `C3` does not hold for vsg yet; the shape = at frame head
  resolve per-draw needs into values (the same shape as vk's `CommandPlan`).
- **Evidence**: `test_vsg` 561/561, parity 13/13; `ACompilingFrameDoesNotWaitForAnotherDriversFrame` (mutation: the gate back to exclusive
  ⇒ 5 s timeout red), `TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`, `AReadbackFromInsideAFrameBodyAnswersInsideFrame`;
  TSan (vsg subset) 0 reports (521 pass / 6 expected skips, suppression only lavapipe's 290); ASan new path PASS.

## 2026-11-16 —— vsg S8c-2: the recording path is lock-free, content split into resolve/record/commit phases (T48)

- **New unit**: `internal/ContentPlan` (`ContentPlan.hpp/.cpp`) — the **values** resolved for one frame: one `Call` per draw call
  (`ContentDraw*` recorder + the resolved `Draw`/`ScreenDraw` + fixed arrays for block bindings/push blocks/vertex bindings), one slot per pass
  (with its own `core::StateRegistry`); `Pass::pass/registry` are borrowed, `halves_` is a **pin** on `shared_ptr<const void>`
  (`ContentHalves::pin()`), so another driver's frame-head sweep does not release a recorder this frame still needs (retirement parking does not care about
  "a frame not yet submitted"). A call **stores no spans** (`Call::value()` assembles from the call's own arrays) — vector reallocation would invalidate a span pointing into itself.
- **`commitFrameThrough` three parts**: **resolve** (content lock: `applyTargetPlans`, material arena growth, `resolveContent` →
  `ContentAssembly::resolve` → `ContentPass::resolve`, refusals/reports happen here), **record** (lock-free: `ContentPlan::record`
  builds vsg nodes), **commit** (lock again: `executor.record`, sweep, session submit, `finish`). The frame head writes the view/lights/shadow/draw/
  material blocks and hands only offsets to recording — another frame head rotating a ring/sweeping/growing in between does not affect this frame.
- **Resolve half / record half separated**: `ContentDraw::plan(key)` (takes the layer + variant pool, returns `Identity{pipeline, variant}`) and
  `record`/`recordScreen` (bind only the resolved pipeline, **no acquire, no pool touch**); `core::StateRegistry` gains a
  `resolve(std::uint64_t variant, …)` overload — the pool is only asked at frame head, recording carries values. `ContentDraw`'s counters become `std::atomic`
  (the same half may be recorded by two drivers at once). A command with zero geometry is refused in the **resolve** phase (before identity resolution, "a refused draw is not compiled" still holds).
- **`ContentHalves::Scope::Entry` gains a trailing field `keep_alive`** (`ContentPlan::Pin`); `serve()` now takes `shared_ptr<Half>`.
  `ContentAssembly::record` (the old convenience entry) is kept = `resolve` + `ContentPlan::record`; `prepareContent` is warm-up going through the same
  offer loop (`offerInputs` extracted) but only `prepare`.
- **Test seam**: `BackendContentAccess::setRecordSeam` (at recording start, no content lock held); new case
  `VsgBackendTest.ARecordingFrameDoesNotStopAnotherDriversContent` (an offscreen frame is stopped inside recording, a window frame completes fully, and the stopped frame's
  read-back pixels are still its own red); **mutation: three phases back to S8c-1's single lock ⇒ 5 s timeout red**. Test side: every case that called `ContentDraw::record`
  directly now calls `plan()` first (ContentDrawTest's "refused before memoised" becomes a hand-built identity-less Draw).
- **Evidence**: `test_vsg` **562/562**, parity 13/13, `test_graphics` 587, `test_vk` 133; TSan vsg subset 0 reports (three reruns
  all green; the occasional `initialize()` false is the existing `VSG_MAX_DEVICES` flakiness, see backlog T36/T33, not this step); ASan
  new path PASS; nine doc/code gates 0 findings (`check_doc_symbols` requires `docs/guides/vsg-backend.md` §5 to name <!-- drift-ok -->
  `ContentPlan.hpp`). `C3` holds for vsg from now on; `T48`'s migration table is fully landed (the backlog entry deleted per the rules). <!-- drift-ok -->
- **Test-infrastructure note**: the plugin `.so` and test binaries in `build/` must be **rebuilt from the same source** — mixing across `git stash` produces
  `undefined symbol` and inexplicable device-limit failures (this investigation took time; lesson: after a stash, build `ninja test_vsg gfx_backend_vsg` together).

## 2026-11-16 —— vk F4: the three-state residency wired into the device (`ensureGeometry` asks the table, `FrameBuilds` publishes at frame completion)

- **Wiring shape**: `ContentCaches::Impl` holds `ResidenceTable residence`; `ensureGeometry(description, builds, why)`
  first checks this frame's resolutions (`builds.entryOf`), then asks `residence.admit(&description, session)`: `Use` takes the cache entry (**and calls
  `fillStreams` to fill the holes `releaseStreams` punched** — this was once missed and `VkResidencyTest.ReleasingOneBuffer…` went red immediately),
  `Build` fills the cache entry and hands the `Claim` to `FrameBuilds`, `Copy` creates a **detached** entry (`builds.noteCopy`). When the table and the cache disagree
  (`Use` but no entry), `forget` and ask again once — the cache is the truth.
- **`ContentCaches::FrameBuilds` (nested class, frame-scope value)**: `resolved_` (this frame's Use/Build/Copy answers),
  `claimed_` (`Claim` + entry pointer, used to skip failed fills), `copies_` (`unique_ptr<GeometryEntry>`).
  `publishAll()` is called after the frame fence wait; a failed fill **never registers a claim**, so `FrameBuilds` holds claims only, not cache entry
  pointers (a host `releaseContent` while the frame is in flight would move the entry away and the pointer would dangle); the destructor withdraws unpublished claims and calls
  `owner_->releaseTemporary(copies_)` (**immediate destruction**: a copy never enters the table, only this frame can name it; not inside the frame-head lock, it takes the lock itself).
  A copy's upload counts `stream_builds`, its destruction **does not count** `stream_releases` (not a release announcement).
- **Publish points**: `executeFrame` calls `publishAll()` after `completed` (submit failed / command buffer not closed ⇒ no publish, the guard withdraws);
  `prepareFrame` uses the `transfers_completed` flag (batch not closed or not run ⇒ no publish; zero-work batch = host memcpy already done ⇒ publish).
  `FrameBuilds` is declared outside the frame head's `{ }`, so its destructor takes the cache lock without holding it.
- **Release/teardown**: `releaseGeometry` calls `residence.forget(description)` first (even when the entry does not exist); `destroyAll` clears the table.
  Lock order is always cache lock → table lock; `publishAll` takes the table lock only (so `VkFrameHeadTest`'s lock counts are unchanged).
- **Tests**: `tests/test_vk/ResidenceWireTest.cpp` 4 cases — (1) under a fake lease (`device->residence().admit(desc, 9999)`, the same gate as
  `TargetClaimFrameTest`), one frame copies and the pixels are correct, exactly 2 streams, `used_bytes` back to baseline; after revoking the lease the next frame Builds+publishes,
  the frame after that allocates nothing; (2) two real sessions on two threads first-drawing the same geometry: claims 1 / published 1 / copies ≤ 1 / both pictures red / then zero uploads per session;
  (3) `setMemoryBlockCreator` refuses the block + a 20 MiB vertex stream (>16 MiB host-visible first block) ⇒ the failed fill is withdrawn
  (`uploading 0`, `stateOf Absent`), a rebuild after release succeeds; (4) after release `stateOf Absent`, the next frame claims/publishes again.
  **Mutations 3/3 red**: do not ask the table (4 cases red), publish a claim for a failed fill (case 3 red), release without forget (case 4 red).
- **Evidence**: `test_vk` **137/137**, `test_vsg` (including parity) all green, `test_graphics` 587; TSan `test_vk` 0 reports
  (132/132, windows/probes/plugin excluded by the existing rules, suppression only lavapipe); ASan focused
  (`VkResidenceWireTest`+`VkResidencyTest`+`VkMultiSessionTest`+`VkFrameHeadTest`) PASS.
- **Build note**: `tests/test_vsg` compiles the vk backend sources too, the new `ResidenceTable.cpp` must go into <!-- drift-ok -->
  `tests/test_vk/CMakeLists.txt` **and** `tests/test_vsg/CMakeLists.txt`, otherwise `all` reports a run of
  `ResidenceTable::Claim` undefined at the test_vsg link.
- **Still a boundary**: the two drivers' frame-head resolution still sits on one cache lock (`C3` governs the recording path); a fill is a host memcpy, so `fillStreams`
  returning proves completion — after M6 moves uploads to device-local, the publish point is still `publishAll`, the evidence becomes the fence.

## 2026-11-16 S2 batch 2a: one frame gate, two sides, two backends (B2a)

- **The same rule had three implementations**: vk's per-driver `graphics::FrameGate` (`enter()` exclusive + same-thread reentry refusal), the vk device's
  `std::shared_mutex body_gate` (16 unique/shared_lock sites in `Device.cpp`), vsg's private `internal::DeviceGate`. Now
  **one**: `graphics::FrameGate` has `enterShared()` / `enterExclusive()` + `Shared` / `Exclusive` leases +
  `heldByThisThread()` / `inSharedEntry()` + `Counters{acquisitions, waits}`; nesting is recorded **per gate** on a per-thread stack
  (vk's driver gate EXCLUSIVE wrapping the device gate SHARED is normal nesting, a single depth would treat the inner one as reentry). `internal/DeviceGate.hpp` is deleted.
- **The empty lease has a refusal value at every vk host-verb site**: `shutdown` / `setMemoryBlockCreator` / `setDefaultContentProgram` /
  `createSession` (null) / `releaseSession` / `setRenderTarget` / `createSurfaceTarget` (`UnsupportedSurface` + why) /
  both readbacks (`Failed`) / `contentPipelines` (0) / `contentCacheCounters` / `memoryStats` / `retirementNumbers` /
  `pipelineCacheReport` (empty). These used to be **deadlocks**. One `vk.device.reentry` episode (a new diagnostic code) names that verb;
  `Impl::report_mutex` becomes `std::recursive_mutex` — the sink calling back is exactly this path.
- **`s_frame_depth` / `FrameBodyDepth` are deleted**: the readback precondition asks `impl->body_gate.heldByThisThread()` (a window frame's
  exclusive body covers it too). `executeFrame` / `prepareFrame`'s body lease can also get an empty lease (another driver, same thread), answering `Refused`.
- **`FrameReport::locking` (new `FrameLocking{acquisitions, waits}`)**: the frame's own content-lock spending. vk's delta comes from
  `ContentCaches::lock()` (new `waits`: a failed try counts, then blocks; `lockWaits()` atomic read) and `FrameBooks::locking`;
  vsg uses `Data::lockContentForPhase()` (counted at frame head / commit / warm-up only, a host verb using `content` directly is not counted) and
  `FrameDrive::locking_before` + `Data::last_locking`. Steady state = a constant + `waits == 0`.
- **The tests moved once**: `tests/BackendSeamTest.cpp` = the seam's 14 `Core*` (TargetPlan / RetirementQueue / DrawState /
  Readback / DeviceClock) + 6 `FrameGateTest`, listed once in **three CMake files** (`test_graphics` reads
  `vine/graphics/` directly). The vsg-only Cores (Timeline / Arena / Observe /
  PhaseTable / DepthProbe / DeviceRequirements) stay in `tests/test_vsg/BackendCoreTest.cpp`; the FrameGate case file in the vk suite
  is deleted with it (the cases are merged into the shared source).
- **New cases**: `FrameGateTest` 6 (shared side, empty host lease, cross-gate nesting, waits counting); `VkFrameHeadTest.AFrameReportsWhatItSpentOnTheCacheLock`;
  `FrameLockingTest.AFrameReportsWhatItSpentOnTheContentLock` (vsg); `VkConcurrentFrameTest.AHostVerbCalledFromInsideAFrameIsRefusedInsteadOfHanging`
  (`contentPipelines()` in the sink answers 0 + one reentry, the frame still ends).
- **Counts**: `test_graphics` 607 / `test_vk` 158 / `test_vsg` 571 / `BackendParityTest` 13.

## 2026-11-16 S2 batch 2b: the `FrameHead` vocabulary (same name, same shape in both backends, no shared type)

- **One concept, two shapes → one name and shape**: the values resolved at frame head were `CommandPlan` + `ScreenPlan` + `ResolvedTextures`
  in vk and `ContentPlan::Call` in vsg; now both are called **`FrameHead`** + **`FrameHead::DrawPlan`**.
  **No shared header**: the ADR rejected "a shared `ResolvedDraw` across the backends" (only type erasure, the reverse of `G7`) — what is unified is the name and shape, not the type.
- **vsg**: `internal/ContentPlan.{hpp,cpp}` → `FrameHead.{hpp,cpp}`; class `ContentPlan` → `FrameHead`, nested `Call` → `DrawPlan`
  (`Pass` / `Pin` / `kMax*` follow the class prefix). `FrameScratch::content`'s type follows; `tests/test_vsg/CMakeLists.txt`'s hand-written list follows;
  `docs/guides/vsg-backend.md` §5 names `FrameHead.hpp` (`check_doc_symbols` requires every unit to be named). <!-- drift-ok -->
- **vk**: new header `internal/FrameHead.hpp` (header only, no new `.cpp`): `FrameHead` holds per-pass `Pass{std::vector<Call> draws}`,
  `Call{std::vector<DrawPlan> plans; ContentRefusal refusal}`, and **frame-level** `fallback_view` / `fallback_sampler` / `shadow_sampler`;
  `DrawPlan` = `kind` (Content/Screen) + pipeline/resolved + content payload (material/variant/streams/…) + this call's `slots`.
  `ContentCaches::planCommand` / `planScreen` **merge into one verb, two overloads** `planDraw(RenderCommand,…)` / `planDraw(DrawDescription,…)`;
  `resolveTextures(material, commands, FrameHead&, FrameHead::DrawPlan&, refusal, lock, batch)` writes frame-level facts into head, slots into plan.
  `ContentDraw::record(…, span<const FrameHead::DrawPlan> plans, const FrameHead& head, …)`, `recordScreen(…, const DrawPlan&, const FrameHead&, …)`.
  The old `FrameDriver::DrawContent` / `PassContent` are deleted, replaced by one `FrameHead frame_head`.
- **One visible difference (honest)**: `record`'s "1x1 stand-in missing" check moves from "this call's first plan's fallback" to "the frame's fallback".
  Today a call whose first command is refused would therefore report one extra `vk.content.stand_in_missing` (the stand-in actually exists); after the change only a real head refusal is reported.
  No case pins that wording; the stand-in is only missing on OOM, and the new check still refuses when it is really missing.
- **Zero behaviour, zero test renames**: `test_graphics` **607** / `test_vk` **159** / `test_vsg` **571** / `BackendParityTest` 13;
  nine gates + `content_seam` / `vk_exposure` all green; this batch does not touch threading (no TSan needed).
- **Trap**: the vsg plugin has a GLOB source list → after renaming a `.cpp` file you **must reconfigure `build/`** (`cmake -S . -B build`), otherwise the old path is still on the target.

## 2026-11-16 S4: target tables unified (B5) + one shared light value (B11)

- **B5a `0e9a9b84` (vk `TargetTable`)**: map, shape/build/rebuild, claim books, release/park, `drain` in one object;
  `TargetClaims.{hpp,cpp}` deleted, `TargetTableTest` 8 cases (+1: drain clears records and leases in one go), `TargetClaimFrameTest` 2 cases;
  `Device::targetTable()` replaces `targetClaims()`; a readback holds a `shared_ptr` (a concurrent release parks instead of the map keeping it alive);
  `ReleaseRenderTarget` becomes one `table.release()` sentence. `test_vk` 165 → 166.
- **B5b `81e0ba0d` (vocabulary + one vsg table)**: vk `TargetImages`→`TargetObjects`; vsg `OffscreenTarget`→`TargetObjects`,
  `HostTargets`→`TargetTable`, with `VsgTargetRegistry` **merged in**: `Entry` holds the owner label (`std::string`, fixing the old
  `const char*` dangling read into a temporary `std::string::c_str()`) and the executor's `unapplied` episode; `resolve` / `adopt` are table verbs,
  the `clearTargets` / `addTarget(nullptr)` paths disappear. The three test sets keep their names except the `TargetObjectsTest` suite name following its type.
- **B11 `1d57dc77` (lights no longer allocated per pass)**: the SDK gains `LightSet` (`std::shared_ptr<const LightSet>`, immutable
  `vector<LightValue>`); `Light::revision()` is incremented by every setter that feeds `value()`; `Scene::lightSet()` caches its values by
  (identity, revision); `DrawDescription::light_values` becomes `LightSetPtr` (null = no announcement, an empty set = no lights),
  both backends read `->values()`, vsg's per-pass `light_scratch` intermediate copy is deleted (the recorder snapshots into the frame arena itself).
  Engine witness: `ValueChannelTest.EveryPassOfOneFrameHoldsTheSameLightSetUntilALightMoves` (two passes share the pointer, steady state shares the pointer,
  changing a light makes a new set). vk witness: `FrameHead::storageGrows()` + `Device::frameHeadStorageGrows()` (**not** a `FrameReport` field),
  `VkFrameHeadTest.ASteadyFrameDoesNotGrowTheHeadsStorage` (5 frames unchanged after a 3-frame warm-up). Fixed along the way: `planDraw`'s
  `plan = FrameHead::DrawPlan{}` (releasing and reusing the vector per command) → `plan.clear()`.
- **Honest**: **the whole frame still makes about 97 allocations/frame** (the pipeline key's `channels`/`bindings`/`attributes`, `buildPipeline`'s by-value key
  copy, content cache map inserts) — B11 only proves the head pass/call storage is reused, it does not claim global zero allocation; see the investigation record in the bundle.
- **Trap (rename tooling)**: combining `vscode_renameSymbol` with `mv` writes **the old path's file back** (with the new name inside), resulting in two files with
  the same symbols → multiple definition at link time. Rename order: change the symbol first, then `mv`, and finally look at `git status` for "new files under old names" and delete them.
- **Counts**: `test_graphics` **614** / `test_vk` **167** / `test_vsg` **571** / `BackendParityTest` 13; TSan three suites 0 reports;
  `Vine --open demo` 0 errors.

## 2026-11-17 B8: the seam drops Vulkan spellings (`DeviceFormat` + `RenderPassKey`)

- **One type, two responsibilities**: the device format code used to be a bare `std::uint32_t` (the comment said "this is a `VkFormat` value"), subpass was another
  Vulkan noun. Now **`DeviceFormat`** (a strong type over `std::uint32_t`, `kUndefined` = 0) carries the device's own format codes,
  **`RenderPassKey`** (u64, `kNotStated` = 0) carries the pass structure — computed by the backend that builds the render pass, the SDK only hashes/compares.
  `TargetShape` / `RenderPassCompatibility`'s fields become `std::vector<DeviceFormat>` / `DeviceFormat` / `RenderPassKey`;
  `subpass` disappears from both structures.
- **The "not stated" rule stays**: `kNotStated`/`kUndefined` **do not take part in comparison** in vsg's `statedShapeAgrees` (do not argue about what the plan has not learned),
  the same rule as the old device format's "unknown ≠ absent"; vk's `TargetTable::ensure` fills both halves every time, so `planTarget` agrees on both sides.
- **Mapping points** (one per backend): vk `TargetTable::ensure` / `WindowTarget::compatibility`, vsg `TargetObjects::shapeOf` /
  `WindowTarget::sampledShape`; each emits `kSingleSubpassRenderPass` ("every render pass of this backend is single-subpass"),
  the constant lives in each internal's `TargetObjects.hpp` (the render-pass builder).
- **One manifestation of type safety**: `ContentCaches::PipelineKey::operator<`'s `lexicographical_compare` eats `DeviceFormat`'s
  `operator<` directly; places inside vk that take `VkFormat` back write `.code` uniformly.
- **Evidence**: `test_graphics` **616** / `test_vk` **167** / `test_vsg` **572** all green; `content_seam`, `vk_exposure` all green;
  nine doc/code gates all green. Zero behaviour change: the pixel comparison and existing cases (including the PlanCarries/StatedShape two) pass directly after the type change,
  and an assertion is added that "the render pass key in the plan is its own copy".

## 2026-11-17 B16: one pass author shape (E-04 / E-05 / E-06, BC-08)

- **B16a (`tag` deleted)**: `PassDesc::tag` / `PassRow::tag` deleted — 12 writes, 0 reads (E-06). The frame table uses `name`.
- **B16b (callbacks belong to the object)**: the SDK gains `FrameUpdate` (`update()`, implemented by AxisGizmo / FpsOverlay);
  `PassDesc::before_draw` is deleted, `buildFrameValue` no longer runs hooks (`record_micros` is only describe + expand).
  `PassChain::update()` calls the overlays' `update()` in registration order; `SceneView::update()` forwards;
  `SurfaceWindow::renderFrame` updates → publishContent → frame every frame. `OverlayLayout` becomes **returning** `Viewport`
  (`std::function<Viewport(int,int,double)>`), `resize()` restates the rectangle from the chain — the callback no longer receives a mutable pass.
  Witness `PassChainTest.AnOverlaysUpdateRunsWhenTheChainUpdatesAndNeverInsideAFrame`. BC-08: a host driving the engine itself
  must call `SceneView::update()` / `PassChain::update()` itself.
- **B16c (PassDesc is the only author shape)**: `RenderPass`'s public surface is read-only (`declaration()`/`name()` + the readers);
  the 35 setters and `ScreenPass::setProgram` are deleted. The engine's write gate is `detail::PassNode`
  (`adopt` / `resolveTarget` / `setDrawSize`, friend). `PassChain::restate(desc)` is the chain's "one explicit diff"
  (the name deviates from the research doc's `chain->update(d)`: `update()` is already a per-frame verb, avoid one name two meanings). `PassChain::apply()` no longer
  refreshes from nodes — `pending_` is the current shape. `RenderEngine::bindPassContent` becomes adopting a declaration with changed content.
  One **semantics-preserving point**: in `adoptDesc`, "an engaged invalid `DepthView`" = explicitly clear the borrowed depth; `nullopt` = say nothing.
  An empty input name is skipped in plan validation and resolution (the old setter's "empty name ignored").
- **Migration surface**: demo (`addDemoPass` takes `PassDesc`, PiP re-anchors through `applyPlan`, preview layout holds declaration values),
  `tests/PlanTestSupport.hpp` (`applyPassForTest(desc,order)`, `updatePassForTest`, `restatePassForTest`,
  `asScreenPassForTest`), all of test_graphics / test_vsg / test_vk / test_gui.
- **Counts**: `test_graphics` **617** / `test_vk` **167** / `test_vsg` **572** all green; nine gates all green;
  `Vine --open demo` 0 errors (session up + Pending -> Presenting).
- **Trap**: when batch-editing tests, check every oldString of `multi_replace` character by character (several "successes" actually hit another site);
  a new `.cpp` (`FrameUpdate.cpp`) needs a `build/` reconfigure. `clang++-23` occasionally had a parser bus error, a rerun passes.

## 2026-11-18 B7: the application names its render backend

- **Defect**: `SurfaceWindow::useDefaultBackend()` took `RenderBackendRegistry::entries().front()`. That vector is
  ordered by NAME, so the renderer was picked by the alphabet (`vk` < `vsg`); a plugin named `aa` would have switched
  every window silently. The backend guide claimed "按名创建可显式选 `vsg`" while no host named one - the demo could
  not run on vsg from a normal build at all.
- **The rule (new private header)**: `src/fw/appfw/src/gui/BackendChoice.hpp`, header-only `chooseBackend(entries,
  named) -> BackendChoice{name, refusal}`: stated name that is registered → it; stated but unknown → refusal naming it
  and what is registered (a typo must not pick another renderer); no name + exactly one → it; none → "no render backend
  is registered"; no name + several → refusal listing them and `RenderControl::setDefaultBackendName()`.
- **The application's one statement**: `RenderControl::defaultBackendName()` / `setDefaultBackendName(name)` - **static
  members** of `RenderControl`, not free functions: the repo's process-wide settings live on their owning class
  (`Application::current()`, `MainWindow::current()`, `PluginManager::setSkipList`), and the only reader here is the
  attach of a control (state = `s_default_backend_name` in `RenderControl.cpp`).
  `SurfaceWindow::useDefaultBackend()` now returns the refusal, and `init()` hands it to `failAttach`.
- **The host**: `app_shell` calls `chooseRenderBackend()` at the top of `AppShellPlugin::load()` - `vk`, overridden by
  `VN_RENDER_BACKEND`. `VN_RENDER_BACKEND=vsg ./Vine --open demo` is how the reference backend is exercised now.
- **Evidence**: `test_gui` **332** green (`RenderControlTest` 26, `BackendChoiceTest` 5); `test_appfw`, `test_graphics`
  green. Demo on vk: session up 86 ms, `Pending -> Presenting` 300 ms, 0 `[error]`, 3 `[warning]` (B8's). Same build with
  `VN_RENDER_BACKEND=vsg`: 68 ms / 276 ms, 0 `[error]`. With `VN_RENDER_BACKEND=no_such_backend`:
  `Failed after 3 ms: the render backend "no_such_backend" is not registered (registered: vk, vsg)` - 4 attempts then it
  stops (the startup path retries a bounded number of times).
- **Trap**: do not test the rule by registering stub factories in `test_gui` - the registry has no `unregisterFactory`,
  so the stubs would be permanent and `ReportsFailedWhenNoRenderBackendIsRegistered` would skip forever. The rule is a
  pure function for exactly that reason; the end-to-end case names an UNREGISTERED backend instead.
- **Docs**: `docs/guides/graphics-backends.md` §1 (which rule decides), `docs/guides/render-views.md` §2 comment,
  `.ai/design/appfw-render-surface.md` F6 / C6 / R6 / T6 / D7-D8, backlog T39 retired (B3 had already landed it).

## 2026-11-18 B8: the demo's validation log is empty

- **Two warnings, two different causes** (both vk-only, both were "known and owned by B8"):
  1. *`fragment writes Location 0 ... no pColorAttachments[0]`* (twice): the shadow pass has no program, so every
     drawable drew with its own - the content program, whose fragment writes colour - into a depth-only target.
  2. *`Vertex attribute at location 1 not consumed by vertex shader`*: the sky box is a `makeDirectionBox` (position +
     normal + direction) drawn by `skyboxProgram()`, whose vertex stage reads only position and the direction.
- **Fix 1 (engine)**: `BuiltinShaders::depthOnlyProgram()` = the CONTENT vertex stage (`builtin_forward.vert`) + a new
  `builtin_depth.frag` whose `main` is empty; `ChainBuilder::buildShadowPass` sets `desc.program_override` to it. Not a
  position-only vertex stage on purpose: that would declare inputs the geometry need not carry (an *error*, not a
  warning, for a normal-less geometry), and it would duplicate the instancing/push-constant math. Variants still apply.
  Bonus: the shadow pass no longer shades per fragment (demo `session up` 86 -> 42 ms, `Pending -> Presenting` 300 -> 190 ms).
- **Fix 2 (vk)**: a content pipeline declares only the vertex-input locations its **vertex stage reads**, read back from
  the compiled module (`SpirvModule::input_locations`, `describeInputLocations` in `ShaderCompiler.cpp`: `OpVariable`
  in the Input storage class + its `Location` decoration + the TYPE's width, so a matrix input owns one location per
  column and the instance stream keeps 12-15). `ProgramEntry::vertexStageReads`; the binding NUMBER stays the geometry
  channel's index, so `ContentDraw`'s `vkCmdBindVertexBuffers` is untouched. Unknown (unreadable module) means
  "declares everything", so narrowing never happens on a guess.
- **Deliberately not done**: removing the sky box's normal channel (a demo-content fix that would break again under a
  pass program override) and a vsg equivalent (vsg's `ContentPipeline` builds from the geometry and has no reflection
  path; the difference is only in the declaration, never in what is fetched - recorded in the backend guide's §3 table).
- **Evidence**: demo 0 `[error]` / **0 `[warning]`**; `test_vk` **174** (169 + `VkVertexInputTest`'s 5), `test_vsg` 572,
  `test_graphics` green; `scripts/vn_shader_check.sh` PASS (10 stages, `builtin_depth.frag` embedded byte-for-byte).
- **Trap**: the SPIR-V width matters - `layout(location = 12) in mat4` produces ONE `Location` decoration, so a reader
  that keys on the decoration alone drops columns 13-15 and instanced draws read the wrong rows. `VertexInputTest` pins it.

## 2026-11-18 B8b: a resize is a session change too (found after B8)

- **Symptom** (user's run): `applyPlan was refused` (a frame being recorded) right after `open_demo`; my repeated runs
  also showed four `plan refused [unknown-input]` for the G-buffer tiles, at shutdown.
- **Cause 1 (SDK)**: `SceneView::applySurfaceGeometry` hands the surface size to the adopted chain, and
  `PassChain::resize` RESTATES the passes it re-lays out (gizmo + FPS HUD) - i.e. it applies a plan - without waiting for
  the frame, which `RenderEngine::applyPlan` requires. Every host's resize could therefore be refused mid-frame.
- **Cause 2 (demo)**: `install()` states rows (and `PassChain::build` applies the whole chain) at whatever moment it
  runs - measured: a 208/358 ms start puts it *after* the first attach - with no wait.
- **Cause 3 (demo)**: the G-buffer tile step re-states four rows after the chain that publishes `GBuffer` is gone (a view
  runs its layout steps when it DROPS a chain, see `SceneView::setChain` -> `applySurfaceGeometry`).
- **Fix**: `applySurfaceGeometry` waits (bounded 250 ms) before touching the chain; the demo's `addDemoPass` and the
  `PassChain::build` call wait the same way; the tile step returns when `view->chain() == nullptr`.
- **Evidence**: 18 demo runs (incl. kills at 2 s / 5 s) 0 `[warning]`; `test_graphics` **618** (new
  `RenderEngineTest.AViewResizeWaitsForTheFrameBeforeItRestatesItsChain`, mutation-checked: removing the wait reddens it),
  `test_vk` 174, `test_vsg` 572, `test_gui` 332 green.
- **Trap**: `waitForFrame(timeout)` returns FALSE immediately when no frame was ever handed out (`number == 0`), so a
  "wait then apply" that ignores the answer is a no-op on a fresh engine - harmless there, but do not read the return as
  "a frame is in flight".

## 2026-11-18 B9: a frame's host allocations, counted once

- **What was wrong with the old number**: "the whole frame still makes about 97 allocations/frame" (2026-11-16, above)
  describes the **backend's recording window**, not the frame. Quoted as "the frame" it is off by a factor of ~20.
- **Measurement 1 (backend window, exact window, stable)**: vk's `executeFrame`, 64x64 RGBA8+D24, `unlitForwardProgram`,
  16 steady frames after 8 warm-ups: **85 allocations / 10,612 B** for ONE drawing call, **133 / 12,424 B** for FOUR -
  the same number on every frame, so churn, not growth. That is ~16 per further drawing call and 69 fixed; bands 66/10/8/1
  (<64 B / <256 B / <1K / <4K). Recipe: temporarily add a counting `operator new` to a `test_vk` TU and count around
  `executeFrame` (the probe is NOT committed: an `operator new` replacement in `test_vk` would break its TSan build, the
  trap test_vsg already carries).
- **Measurement 2 (whole chain frame)**: the demo's deferred path (10 passes / 10 draws, shadows + sky + HUD), counting
  every C allocation of the process through a throwaway `LD_PRELOAD` counter and dividing the difference between
  `VN_BENCH_FRAMES=8` and `=120` by 112 frames: **≈1966 allocations and ≈2.6 MB per frame** (one pair: 315,712 ->
  535,872 calls, 711.7 MB -> 1,007.1 MB). Two 8-vs-40 pairs gave 2,127 and 1,911, so treat the figure as ~1.9-2.1 k.
- **Consequence**: the design pages now state both numbers with their scopes (`vk-backend.md` 9.1 says they are not
  interchangeable; `graphics-threading.md` 1.1 names which backend its 33-per-triangle baseline belongs to). No allocator
  was written, and none is owed: the number is churn in short-lived vectors, and the frame still measures 8.2-8.6 ms
  median on lavapipe. If it ever becomes the reason a frame is slow, the measurement above is the baseline to beat.

## 2026-11-18 B11: the concept tables (docs only)

- **Delivered**: `docs/guides/graphics.md` gained chapter §5 (`延伸阅读` is §6 now) - 5.1 translation against
  Filament / O3DE / UE, 5.2 false friends, 5.3 the five `Frame*` values, 5.4 the six target roles, 5.5 "the engine is
  a frame compiler, not a render graph", 5.6 `SceneView` as the per-window controller. §3.12 points at 5.3.
- **Normative halves** (one fact one home - the guide translates, the design page states): `graphics-frame-pipeline.md`
  2.11 (five values, one question each; `FrameQuery` is `FrameResult::query`'s TYPE, not a sixth answer, and
  `FrameOutcome`'s `NotReady` is a precondition) and 2.12 (no render graph; `Transient` borrows the WORD, not the
  read/write edges; `G8`); `graphics-architecture.md` 2.13 (`SceneView`; the `View` VALUE is camera snapshot +
  device-pixel area + layer mask, one derivation long).
- **Wording fix** (C-A3-05): `vk-backend.md` 2.6 called the ANSWER "the SDK's `TargetPlan`"; the answer is
  `TargetDecision`, `TargetPlan.hpp` is the unit and `planTarget` is the verb.
- **No duplication**: `.ai/memory/glossary.md` §3 keeps the code-spelling ↔ Chinese table and now points at the guide
  chapter instead of copying it; §1 gained the five frame values (帧描述 / 帧表 / 帧结果 / 调用结果 / 帧所见).
- **Trap for the next editor**: the guide is under `check_doc_symbols.py` - a backticked unit file name has to exist in
  the tree, so a concept table may name types freely but must not invent a unit name (the gate said so while this entry
  was written).
- **Gates**: `check_ai_docs` 194 notes, `check_design_docs` 56, `check_doc_symbols` 2 docs / 138 units,
  `check_public_docs` 974 baseline / 0 new / 0 stale - all green. Docs-only batch: no build, no test run.

## 2026-11-18 B13: three plug-in-internal names, one role each

- **Why**: three near-names (`Residency*` report / `ResourceTable` / `ResidenceTable`) plus one opaque one
  (`ProgramHalves`, "half of what?"). Now `PinnedResources` (vsg, the pin-driven template), `ResidencyClaims` (vk,
  who may build a key), `CompiledPrograms` (vsg, one pipeline layer + recorder per key).
- **Method**: `git mv` eight files (3 headers, 2 `.cpp`, 3 test units), then ONE sed over `src tests docs .ai/design
  .ai/research`, explicitly excluding `.ai/memory` (dated history) and `.ai/decisions` (records).
- **The guard bit**: `ctest -R content_seam` FAILED first with "ProgramHalves.hpp is gone - update this list with it
  (a rename must not silence the guard)" - which is exactly what `scripts/check_vsg_content_seam.cmake` is for. Update
  that list in the same batch, plus `tests/test_vk/CMakeLists.txt` AND `tests/test_vsg/CMakeLists.txt` (the latter
  compiles vk's ResidencyClaims.cpp too - the trap already recorded above for the old vk table's `.cpp`).
- **What stayed, and why**: the member spelling (`halvesFor` / `halves()`) and the word *half* in prose and test names.
  Renaming them item by item is churn and the case names quote the word (`G8`); instead each class doc says once that
  one *half* IS one compiled program, so the type name answers the question at the call site.
- **Records**: the three ADRs that named the old classes got a dated
  `- **Correction (2026-11-18, audit-4 B13)**` line; two lines of this page and one ADR heading that name the file as
  it was keep `<!-- drift-ok -->` - history is not rewritten, and `check_ai_docs --strict` is what asks for the marker.
- **Evidence**: `test_vsg` 572, `test_vk` 174, `BackendParityTest` 13, `content_seam` + `vk_exposure` green; demo on
  vk 79 ms / 286 ms and on vsg 113 ms / 406 ms with 0 `[error]` / 0 `[warning]`; nine gates green.
  One WSLg flake on the first run: `VsgBackendTest.ARefilledImportedTextureIsReuploadedForTheNextFrame` failed with
  `initialize()` false after 10.4 s (window reported 4294967295x4294967295); it passed alone and on the suite rerun -
  the backlog T33 family, not the rename.

## 2026-11-18 B14: the seam is a property, not a namespace

- **Question** (third round `C-A3-10` / audit-4 `T-14`): the 8 mechanisms both backends use live in `vn::graphics`
  beside the host's model types, so no spelling says "this is the seam". The two options were a namespace alias group
  `vn::graphics::backend`, or the file-set gate (B3) alone.
- **Decided: no alias group** (`ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md`), for two measured
  reasons: the shared surface is 25 SDK headers both backends include directly (`SEAM_SHARED_HEADERS`), so an alias
  over the 8 mechanisms would claim a boundary at 8 of 25; and an alias gives one class two names, with
  `graphics::backend::X` reading as a plugin namespace beside `vn::backend_vk` / `vn::backend_vsg`.
- **What the batch actually added** is the mechanical half that was missing: `check_include_hygiene.py` now requires
  each page that states the seam (`.ai/design/graphics-architecture.md` 2.9 and `docs/guides/graphics-backends.md` 5)
  to name EVERY mechanism `SEAM_HEADERS` holds (`SEAM_DOCS`), so the decided list and the prose cannot drift apart.
  Coverage only - staleness cannot be decided from prose, because both pages name mechanisms in other sections too.
- **Mutation evidence**: un-backticking `DiagnosticDraft` in the guide reddens the gate ("the seam list holds
  `DiagnosticDraft`, and this page no longer names it"); restoring it is green. Steady state: 0 findings / 1350 files.
- **Trap for the next editor**: the ADR index in `.ai/README.md` is itself checked (`check_ai_docs` R2) - a backticked
  `*.md` token there must be a note name, so an ADR bullet says "the backend guide 5", not the guide's file name.
- **Gates**: nine green (195 notes / 56 design / 138 units / 974 entries). No C++ changed, so the build and the suites
  were not re-run (B13's green build still describes the tree).

## 2026-11-18 B12: the public vocabulary, renamed whole (explicit go)

- **What changed** (five spellings, one batch, ~1,200 references): `RenderCommand` -> `DrawItem` (and
  `collectRenderCommands` -> `collectDrawItems`; its header and `.cpp` renamed with it), `DrawDescription` ->
  `DrawCall`, `MaterialBase` / `MaterialBasePtr` -> `MaterialValues` / `MaterialValuesPtr` (files renamed),  `EngineNotReady` -> `EngineNotReadyReason` (file renamed), `createSession` / `releaseSession` ->
  `createFrameDriver` / `releaseFrameDriver`. Rationale and rejected options: ADR-2026-11-18-public-vocabulary-unification;
  migration notes BC-17..BC-21 in `graphics-rendering.md` 2.10.
- **No aliases kept, on purpose**: B4's deprecated shims migrated a *shape* whose in-tree callers still existed; a migrated
  *word* has no callers left after the sweep, and an alias would restore the double spelling the batch removes.
- **Trap 1**: the `src/` glob is not CONFIGURE_DEPENDS, so renaming a `.cpp` leaves a stale ninja rule ("missing and no known
  rule to make it"). `cmake -B build` once, then build with the tool. (Same trap as adding a file.)
- **Trap 2**: `check_public_docs.py` keys its baseline by declaration name - a rename shows the new names as NEW and the old
  as stale; `python3 scripts/check_public_docs.py --update` moves them (974 entries before and after).
- **Trap 3**: `check_ai_docs --strict` flags a record that names a unit which no longer exists, so the ADR's rename table
  needs `<!-- drift-ok -->` on those rows - a record has to name the old file.
- **One flake**: `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` failed in a direct full-suite run, then passed
  alone (111 ms) and under ctest - the recorded load-sensitive measurement case.
- **Evidence**: `test_graphics` 618, `test_vsg` 572, `test_vk` 174, `BackendParityTest` 13, `content_seam` + `vk_exposure`
  green; demo on vk and on vsg 0 `[error]` / 0 `[warning]`; nine gates green (196 notes / 56 design / 138 units / 974
  public docs).

## 2026-11-18 review 5: the first example is code, references are gated

- **The finding**: the module guide's first example (`docs/guides/graphics.md` §3.1) **did not compile** -
  three public-API changes had passed it by (`make_intrusive` named unqualified, a `RenderArea` built
  directly although the constructor is private and `RenderEngine::createArea` is the only door, and
  `engine->frame(dt)` where `frame()` takes no time). Nothing compiled a snippet and no gate read it.
- **The fix that lasts**: the block is now a translation unit - `tests/test_graphics/GuideExample.cpp` -
  whose device-free half (`guideScene` / `guideCamera`) `GuideExampleTest` **executes** (asserts one collected
  draw item) and whose device half compiles. `check_doc_symbols.py`'s rule 3 requires every statement of a
  `// example: <path>` block to exist in that file (statements compared, comments ignored, so the page can
  keep Chinese prose and the file English).
- **New gate**: `scripts/check_cross_refs.py` resolves every `name.md §N.M` in `.ai/**` and `docs/guides/**`
  against the target page's numbered sections (headings plus `N.M **point**`). It found four dead references -
  in `async-design.md`, `base-crash.md`, `graphics-threading.md` and `graphics-architecture.md`, at sections
  4.4, 2.10, 3.1.6 and 8.2 respectively - all repaired. The numbers are written apart from the file names on
  purpose: the entry records the repair, and a reference shaped like the live ones would be read as one. A page
  that *must* name a dead reference writes the bare `<!-- drift-ok -->` marker on any line the reference spans.
- **New rule R6** in `check_design_docs.py`: numbered items ascend, in both forms (`### N.M` and
  `N.M **point**`). Found one instance (2.10 before 2.9 in `graphics-architecture.md`); fixed by moving the
  block, **ids unchanged** - renumbering would break every citation.
- **Design truth added**: `graphics-scene.md` 2.12 (and guide §5.2) - the three derivation verbs are chosen
  by INPUTS: `collect*` (a node), `resolve*` (a value + a context), `effective*` (a node, the one-step
  composition). `effectiveRenderState` == `applyStateDefaults(collectRenderState(node))` (the one-argument
  overload was renamed on 2026-11-18, see the entry after this one).
- **Registered, not done** (arguments in `../research/graphics-2026-11-18-review-5.md` §2): **T40** a
  `FrameContext` for the per-frame hook (the frame verb has no time; `FpsOverlay` is the only consumer and it
  self-clocks - `G8` says wait); **T41** `DiagnosticsRoute` should be named as the handle it is.
- **Measured, refuted**: "the engine is still a god object" - 26 public members, three delegated mechanisms.
- **Evidence**: `test_graphics` **619** (+`GuideExampleTest`), `test_vk` 174, `test_vsg` 572,
  `BackendParityTest` 13, `content_seam` + `vk_exposure` green; **ten** gates green (197 notes / 56 design /
  1351 include files / 974 public docs / 138 units). Both new rules mutation-tested (red on a reintroduced
  defect, green after restore). No host or backend code changed, so no demo run.

## 2026-11-18 review-5 follow-up: the other three complete examples are compiled

- **What**: `tests/test_graphics/GuideExamplePasses.cpp` now compiles the guide's §3.3 (two-pass
  offscreen composite), §3.4 (hand-built deferred pipeline) and §3.5 (the builder plus a HUD overlay).
  All three carry `// example:` and are therefore statement-checked by `check_doc_symbols.py`; the guide
  says so in each section. Compile-only, no executed half: each needs a device and a chain.
- **Seven defects found**, every one of the R5-01 family - a snippet drifts because nothing compiles it:
  three unqualified `make_intrusive` calls (§3.3 twice, §3.5 once); `chain->addOverlay` on a variable
  the block never declared (it is `pipeline`); `engine.setDefaultContentProgram` - wrong object AND
  wrong syntax, the setting lives on the device's frame half
  (`engine->device()->executor().setDefaultContentProgram`); `PassChain::build(&engine, ...)` where the
  parameter is already a `raw_ptr`; `addOffscreenToScreen`'s `[[nodiscard]]` ignored (now captured and
  checked, which also teaches that the answer says whether the PiP was built); and `intrusive_ptr<
  Camera>` / `<Scene>` spelled as if they were `vn::graphics` names - they are `vn`'s, and that only
  resolves inside the SDK's own namespace, never in a host translation unit.
- **Style unified**: §3.3/§3.4 dropped their `vn::graphics::` prefixes and open with
  `using namespace vn::graphics;`, the way §3.1 already did - one style in the chapter, and it is what
  the compiled copy uses.
- **Trap**: a python block-rewriter keyed on the first `// example:` fence hit §3.1 first and put the
  new note in the wrong section. Fix by keying on the block's TARGET path, not on its ordinal.
- **Evidence**: `test_graphics` 619 (no new case - these are compile-only), `test_graphics`/`test_vk`/
  `test_vsg` + `content_seam`/`vk_exposure` green; ten gates green (197 notes / 56 design / 1352
  include files / 974 public docs / 138 units). Mutation check: changing `gbuffer_pass.order = -3` to
  `-4` in the guide reddens `check_doc_symbols`, restoring greens it. No host or backend code changed.

## 2026-11-18 a public name says what you are handed (T41 + two neighbours)

- **What**: three spellings changed because each named a neighbouring thing rather than the thing in the
  caller's hand. `DiagnosticsRoute` -> **`DiagnosticsHandle`** (`Diagnostics` stays the route; a DEVICE is
  handed the route via `setDiagnosticsRoute`, the ENGINE hands out the lease-backed handle); the duplicate
  `MaterialPtr` alias deleted from `Geometry.hpp` (`Material.hpp` is its home); the one-argument
  `resolveRenderState(state)` -> **`applyStateDefaults(state)`** (it takes no context, so under the
  derivation rule of `graphics-scene.md` 2.12 it was never a `resolve*`). Notes `BC-22`/`BC-23`/`BC-24` in
  `graphics-architecture.md` 2.14; argument and rejected roads in
  `../decisions/ADR-2026-11-18-names-state-what-is-handed-over.md`. **T41 is closed** and deleted from the
  backlog. No deprecated aliases.
- **Trap 1 - a sweep must not rewrite history.** A blanket `\bDiagnosticsRoute\b` rewrite also hit
  `.ai/memory/graphics.md`, `../research/graphics-2026-11-18-review-5.md` and `.ai/README.md`, where those lines record what
  the code *was*. Restore them (`git checkout HEAD -- <the record>`), let the design pages carry the new
  name, and append a dated correction to the record instead. The backlog's T41 row became nonsense after
  the sweep ("`DiagnosticsHandle` does not say it is a handle") - closed rows are deleted, so it went.
- **Trap 2 - the renamed `.cpp` needs one `cmake -B build`** (glob without CONFIGURE_DEPENDS), then build
  with the tool. Same trap as every file add/rename.
- **Trap 3 - grep by the function NAME, not by an assumed argument name.** Three test call sites of the
  one-argument overload survived my first sweep because I searched for `resolveRenderState(state)`; the
  compiler found `resolveRenderState(RenderState())`, `(partial)` and `(content)`.
- **No baseline move needed**: `check_public_docs.py` reported 0 new / 0 stale after the rename (unlike the
  B12 vocabulary batch, which required `--update` because whole units moved).
- **Evidence**: `test_graphics` 619, `test_vk` 174, `test_vsg` 572, `BackendParityTest` 13; `content_seam`
  + `vk_exposure` green; ten gates green (198 notes / 79 ADRs / 56 design / 974 public docs / 138 units).
  SDK + both plugins rebuilt (a class rename in a header reaches the plugins).

## 2026-11-18 T40: the frame carries its context (explicit go, ahead of its trigger)

- **What**: new `FrameContext.hpp` (frame id, `elapsed_seconds`, area size in logical pixels, pixel ratio,
  `renderWidth`/`renderHeight`/`hasArea`); `RenderEngine::frameContext()` composes it; `FrameUpdate::update(const
  FrameContext&)` takes it; `PassChain::update(context)` / `SceneView::update(context)` forward ONE value per call;
  appfw's `SurfaceWindow` passes `engine->frameContext()`. `FpsOverlay` **stops reading `steady_clock`** - the system
  is back to one clock. Design: `graphics-frame-pipeline.md` 2.13; note BC-25; ADR-2026-11-18-the-frame-carries-its-
  context; T40 closed and deleted from the backlog.
- **Shape decided** (and why): the value is a PURE READ (facts live, time stored), so two reads of one frame agree; the
  clock is read once at the head of `frame()` **after** the readiness checks (a NotReady frame is not an interval), so
  `elapsed_seconds` is one frame behind. `update(double dt)` rejected (names one field, invites re-derivation); a
  host-supplied clock rejected for now (`G8`; the replay/determinism argument therefore stays OPEN and the docs say
  so); a `FrameClock` lease rejected (a third narrow boundary so hosts could keep a zero-arg call).
- **Wins beyond the contract**: the FPS readout's test lost its `sleep_for(80ms)` - it now states the interval and
  asserts the published figure, so a timing-dependent case became deterministic. `AxisGizmo` ignores the argument and
  says so (it mirrors the source camera, not the frame).
- **Trap**: `RenderContext` is in `vn`, not `vn::graphics` - but `FrameContext` is the SDK's, so a host TU reaches it
  through `<vine/graphics/FrameContext.hpp>`. Also: `SceneView.cpp`'s out-of-line definition must change with the
  header, or clang reports "out-of-line definition does not match any declaration" (the body edit alone is not enough).
- **Flake seen**: `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` failed inside the parallel ctest run and
  passed alone (2850 µs against its 7000 µs budget) - the recorded load-sensitive measurement case, not a regression.
- **Evidence**: SDK + both plugins + appfw + `Vine` rebuilt; `test_graphics` **621** (+2: `PassChainTest.
  OneUpdateHandsEveryOverlayTheSameFrameContext`, `RenderAreaTest.TheFrameContextCarriesTheAreasSizeAndRatio`),
  `test_vk` 174, `test_vsg` 572, `test_gui` 332; `content_seam` + `vk_exposure` green; ten gates green (199 notes /
  80 ADRs / 1353 include-checked files / 974 public docs / 138 units); demo `Vine --open demo` on vk: 0
  `[error]` / 0 `[warning]`, session up 47 ms, `Pending -> Presenting` 225 ms.

## 2026-11-18 the hook's verb: `FrameUpdate::update()` -> `onFrame()`

- **What**: one method rename, no type renamed. `FrameUpdate::onFrame(const FrameContext&)`; `AxisGizmo`, `FpsOverlay`
  and the two test doubles follow; `PassChain::update` / `SceneView::update` keep their names. Rule now stated in
  `graphics-architecture.md` 2.14: a **driver** is `update` (its job is to walk and call), a **reaction** is
  `on<Event>` (`onSurfaceResized`, `onResize`, `onKeyDown`, `onScroll` - the repo already spoke this way). BC-25 covers
  both halves of the T40 migration. ADR-2026-11-18-the-frame-carries-its-context carries a dated correction rather
  than a rewrite.
- **Why it was worth a public-API batch**: "update" was doing three jobs at once (the interface, its method, and the
  two drivers) and `FrameUpdate::update` read as a stutter; the call site now says what happens
  (`overlay.update->onFrame(context)`).
- **Rejected**: `FrameUpdateCallback` - in this SDK "callback" means the `std::function` a host passes in (prose only:
  `DiagnosticSink`, `OverlayLayout`, `Step`, `StepWithRatio` are named by ROLE, never `XxxCallback`), while the hook is a
  facet of a content object discovered by cast. `FrameStep` would be right only if the work became a function object.
- **Doc drift fixed on the way**: backlog T6 said "`FrameContext` has no consumer" - it has two now; what has none is a
  walk of the SCENE graph. T6 restated accordingly.
- **Evidence**: `test_graphics` 621, `test_vk` 174, `test_vsg` 572, `test_gui` 332; `content_seam` + `vk_exposure`
  green; ten gates green. No behaviour changed and no case renamed.

## 2026-11-18 the residency ledger speaks the host's words (`BC-26`)

- **What**: `ResidencyKind` is now the host's content model - `Geometry` / `Textures` / `Materials` / `Programs` -
  replacing `Streams` / `Images` / `MaterialBlocks` / `Halves`; `Sets` is deleted (no producer: one reference in the
  tree, its own name table). No aliases. Both backends now note ALL four kinds: vk reports `Materials` at `live = 0`
  (material values go into each frame's own scratch), vsg reports `Programs` from
  `ContentAssembly::halves()` (`halves()` / `builds()` / `refused()`), and vsg notes a zero `Materials` row when it
  has no arena. `nameOf()` answers the new words.
- **Why**: the ledger is the one part of the old T30 question that IS public, and it was promising a taxonomy no
  backend implemented whole - `Halves` was vk-only, `MaterialBlocks` vsg-only, `Sets` nobody, so `rows()` was 3 rows
  with a backend-dependent composition and a host could not write one loop over it. The criterion that decided it is
  `graphics-architecture.md` 2.15 (public = the host must decide on it). T30 itself is now only the placement of
  `PinnedResources` / `ResidencyClaims`, each still with one backend consumer.
- **Rejected**: keeping `Streams` / `Images` (a host translates its own content types into backend plumbing);
  keeping `Sets` "for symmetry" (`G8`: zero producers, zero readers); filling vsg's `Programs` from
  `ContentStore::programEntries()` (those are table ROWS, a superset of compiled halves).
- **Evidence**: `test_graphics` 621, `test_vk` 174, `test_vsg` 572, `test_gui` 332; `content_seam` + `vk_exposure`
  green; eleven gates green (incl. `check_translations`). New assertions: `VkResidencyTest` fresh-device expects
  `rows().size() == kResidencyKindCount` and a zero `Materials` row; `VsgBackendTest`'s frame-report case expects the
  same count.

## 2026-11-18 dead research-page citations in source comments (trap)

- **Fact**: `.ai/research/` holds only the two current pages; a conversion deletes its source page, so a code comment
  citing `../research/*.md` is usually dead - 16 of them were, in 6 headers/sources and 8 test files, and none showed
  up in a gate because `check_cross_refs.py` scans `.ai/**` and `docs/guides/**` only.
- **Trap**: the gate's section rule cannot see that class either - a page that does not exist reads as PROSE
  (`candidates is None -> continue`), so "the citation resolves" and "the page was deleted" look the same.
- **Why the gate was NOT extended**: an existence rule would also flag the ADR pointers the archive policy deletes on
  purpose (a superseded ADR is removed, git keeps it), so it needs to tell a current claim from a dated record first.
  Registered as backlog T44 together with the 35 section-number drifts the same scan found in `src/` + `tests/`.
- **Where the claims went**: `vk-backend.md` 2.1 (the two halves / the escape door) / 2.4 (the cost model) / 2.7
  (barriers) / 2.10 (the test is half the contract) / 2.17 (residency's three states), plus
  `ADR-2026-11-16-no-backend-half.md` for the seam relocation. **Also corrected**: `BackendFactory.hpp` claimed
  "`vsg` remains the production backend ... nothing selects `vk` implicitly", while `AppShellUi.cpp`'s
  `chooseRenderBackend()` names `vk` and honours `VN_RENDER_BACKEND` - the header now states that split.

## 2026-11-18 residency bytes: three kinds measured, `Programs` cannot be

- **What landed**: vk's `Materials` row now reports `bytes = 0` (a real 0: material values live in each frame's own
  scratch, so nothing survives a frame); vsg's `Geometry` row reports `StreamUploads::residentBytes()`, a new
  on-demand sum over the entries (each entry keeps the size of the array its bind reads, set at build). Summed per
  call rather than kept as a counter, because the table retires inside `PinnedResources` and a second running total
  would be one more number to keep in step.
- **Pinned by tests**: `VsgBackendTest`'s frame-report case asserts the Geometry row's bytes > 0, and
  `ReleasingAStreamBufferDropsTheUploadButNotTheGeometry` asserts they FALL on a retire and return to the same value
  after the re-upload (a churn counter would not); `VkResidencyTest`'s fresh case asserts `Materials` has an engaged
  0.
- **The finding that ends T45 as a tally**: `Programs` cannot report bytes on EITHER backend - Vulkan has no query for
  the memory a compiled pipeline occupies and vsg exposes none. So `unknown` is 1 on both and `totals().bytes` stays
  disengaged permanently; a partial sum is exactly what `ResidencyTotals`' own rule rejects. T45 now records a
  DECISION (delete `bytes` + `unknown`, or keep them and accept the figure is unavailable) rather than work.

## 2026-11-18 `BC-27`: the ledger's byte total is deleted

- **What**: `ResidencyTotals` (`bytes` + `unknown`) is gone; `ResidencyLedger::live()` - the sum of every row's
  `live` - is the only figure the ledger adds. `totals()` went with it. No aliases.
- **Why**: `bytes` could never be engaged. No GPU API reports the memory a compiled pipeline occupies, so the sum
  would always have left `Programs` out while looking complete - exactly what the struct's own rule rejected
  ("known added to unknown is a number nobody can act on"). `unknown` existed only to explain that gap, so it left
  with it. Same rule that deleted `ResidencyKind::Sets`: a promise no backend can keep does not stay on the public
  surface.
- **What a host does instead**: reads the rows it needs (each row's `bytes` is an `std::optional`, documented
  "absent is not zero"), and for a device-wide VRAM figure waits for the capability + report part in backlog T46 -
  vk already measures it internally (`MemoryStats`), it just has no public door.
- **Tests rewritten, not deleted in spirit**: the two cases about a silent total are replaced by
  `ARowThatDoesNotCountBytesSaysSoRatherThanReportingZero` (the "absent is not zero" rule at row level, and why only
  the COUNT is summed) and `ANoteReplacesItsKindInsteadOfAccumulating` (a row is replaced, and its bytes must go
  down). 5 cases in the suite.
