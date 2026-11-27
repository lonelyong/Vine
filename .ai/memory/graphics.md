# graphics — current-state notes

> **Working notes** (§1–§6): mechanisms, criteria, traps and evidence, plus “why it was decided that way” — some of them measured against the retired backend, whose own record is
> `.ai/design/vsg-backend.md`; the dated sections below them are the build record. The design docs are in `.ai/design/`, and **the only to-do list** is `.ai/backlog/graphics.md`.
> The shipped backend's contract is `.ai/design/vk-backend.md`; the retired backend's record is `.ai/design/vsg-backend.md` with its trap catalogue in `vsg-pitfalls.md`.

## 1. Layering and one frame

- SDK: `src/viz/graphics/sdk/vine/graphics`; shipped backend: `src/plugins/gfx_backend_vk` (`include/vine/backend_vk/{internal,shell,support}` + `src/internal`).
- `RenderEngine`: lifetime / `frame()` / device borrowing / size / capabilities and reports; the three facades are `PassRegistry`, `OutputRegistry`, `DiagnosticsRoute`.
- **The host reads memory through two values (2026-11-23)**: `Features::MemoryUsage` says the device reports what its pool holds, and `FrameReport::memory`
  (`DeviceMemory`: `used_bytes` / `reserved_bytes` / `peak_used_bytes` / `free_bytes` / `largest_free_run`) answers it; it is the pool's own account - not the
  whole VRAM, not the frame's traffic (`FrameUploads`) - and the ledger's rows total no bytes (`ResidencyLedger::live`), so this is the public surface's only byte figure.
- `frame()` returns `FrameCallOutcome` (`Ran` / `Submitted`; `NotReady` + `EngineNotReady`), `applyPlan` returns the three-state `PlanResult`; a pass's content lives in `PassDesc::content`, and `bindPassContent` answers the three-state `ContentBindOutcome` (`Bound` / `NotRegistered` / `Refused` - a content bind is a shape change, gated like a plan). The output table (`publish` / `resolve` / `unpublish` / `transientTarget`) may be used from any thread, including during a frame (C23).
- **The sort key is `detail::SortKey`** (`DrawListBake.hpp`): drawOrder → layer (smallest layer number) → opaque/transparent → quantised camera-space depth (1/4096 units) → row/primitive index. The representative point is the **world box centre** (no box falls back to the matrix origin), and `sortCollected` uses `std::sort` (`tie` guarantees no equal keys); it is the last step of `selectRows`, so a caller receives a sorted `Selection`.
- **A row is three parts** (`DrawListBake.hpp`): `RowLinks` (parent / subtree end / child count in walk order), `RowOwn` (`Fold`: placement, opacity, layers, draw order, view placement; `Body`: state, material, program, own bounds, geometry, draws) and `ResolvedRow` (the fold's one value, with `resolved_serial` moved only by `storeResolved`). `refreshRow` classifies an edit from the groups: `Body` equal → `RefoldSubtree`, otherwise `RebuildTable`; a chunk-share test compares `RowLinks` + `resolved_serial`.
- **Chunk sharing and the publication cache are the publisher's**: `ScenePublisher::publication_cache_` holds the last snapshot; `SceneBake` is only the row table (+ `nodes` / `node_epochs` / `next_serial`).
- **A layout step is a creator callback** (`SceneView::addSurfaceLayout`): the view runs one only while it is alive and only from a geometry event (a resize, the adoption of a chain, presentation on a sized area). Dropping a chain applies nothing and `~SceneView` runs none - a view is destroyed inside its creator's teardown, so a step that named that creator would be called on a half-destroyed object (`.ai/bugs/graphics-layout-step-runs-in-view-destruction.md`).
- **The frame result is a value**: `RenderEngine::frameResult()` → `FrameResult{frame, present, query{snapshot, view}, passes, draws, diagnostics}`, written once when the frame is answered (`frame_result_mutex_`); `query.view` is a `View` value (camera snapshot + device pixel area + layer mask), and `presentedCamera()` is its camera half. The `poses` half waits for F10.
- Type tests on the `Object`-derived nodes / passes / geometry use `obj_cast<T>` (core's meta-type chain), never `dynamic_cast`; `CameraManipulator` and other non-`Object` types are the exceptions.
- `PassChain`: one picture's recipe + registration ownership. `PassChain::build(engine, ChainDesc)` builds the chain and registers the passes; the destructor hands its passes back to the engine; `SceneView::setChain` adopts it.
- **One mesh-append vocabulary (private)**: `detail::appendTriangulation` / `detail::appendBox` (`src/MeshAppend.hpp`) copy a `vn::geom::spatial::Triangulationf` into authoring arrays, re-basing the indices and applying a rotation plus offset; `FpsOverlay` (bars) and the axes marker (arrows) therefore read geom's one definition of each solid.
- **A box, quad or triangle needs no hand-written vertex**: `geometryFromShape()` takes `models::Box` / `QuadMesh` / `TriangleMesh`, tessellates, and states the indexed draw itself; the raw `packAttribute` path is for a custom channel (e.g. a vec3 cube-map texcoord) or an in-place rewrite only.
- **A coordinate frame is `makeAxesMarker(AxesStyle)`** (`AxesMarker.hpp`): ONE geometry, ONE primitive, arrows told apart by vertex colour (white diffuse material), and a bounded prototype cache keyed on the exact style - every marker of a style reads the same vertex/index/colour buffers, so N markers cost N draws and ONE upload. `axis_mask` is a build parameter; `AxisGizmo` is its first caller (one node, one draw) and adds the shading itself: an ambient floor (0.45) plus a camera-locked key light (0.75) in its own scene, because a HUD scene otherwise gets only the backends' 0.15 ambient fallback and the arrows read as flat silhouettes.
- **A HUD owns the depth inside its box (2026-11-20)**: a pass states its clear as TWO independent dimensions -
  `clear_enabled` (colour) and `clear_depth` (depth), carried in `ClearPolicy{color, depth, value, extra_values}` - and a
  pass that clears no colour clears depth only by stating it, so every pre-existing "no clear" declaration means the
  same thing. The chain declares the gizmo `clear_enabled = false, clear_depth = true, depth_policy = TestAndWrite`: the
  arrows occlude each other like the solids they are and the scene underneath is never touched (declaring only
  `depth_policy` would test them against the SCENE's depth, which has nothing to do with their own). What a target can
  honour: an off-screen target every combination (the attachment info states each attachment's load op); the WINDOW's first pass both, a LATER
  window pass depth only, and a later COLOUR clear is reported as `UnsupportedRequest` rather than dropped.
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
- The present outcome is a value: `PresentOutcome{Presented, Skipped, Lost, Refused, Failed}` + `RenderEngine::presentRecord()` (frame number + outcome, one atomic word).
  `Refused` means “the device did not record this frame at all”: the calling thread is already inside a frame (next bullet); it is neither `Skipped` (nothing to present) nor `Lost` (there is a surface but the picture did not arrive).
  `Failed` (2026-11-19) means the frame's engine half threw: the frame is still ANSWERED (`engine.frame.failed` at Error comes with it), and the host's move is the diagnostic, not a re-attach.
  `Lost` is repaired by the **engine** (forget the announced size → report it again next frame → the backend rebuilds the swap chain); the host only logs one line, does not tear down the session and does not hide the area
  (hiding the area would stop the repair frame). Device verbs (`createSurfaceTarget` / `setWindowHandle` / `initialize`) are **between-frame** operations; `RenderEngine::setDevice` waits for the frame in flight itself (2026-11-19).
- **One frame at a time is a “checked fact”, not a comment (2026-11-15, T47)**: `backend/FrameGate.hpp` — `executeFrame` /
  `prepareFrame` takes a **lease** on entry: another thread **waits**, the same thread re-entering is **refused** (`PresentOutcome::Refused` /
  `PrepareReport::Outcome::Refused`, one `*.session.frame_reentry` diagnostic). The SDK ships one implementation, so the answer cannot differ per backend. Why a lease rather than `enter/leave`: a frame has several early returns, and forgetting `leave` wedges the next frame,
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
- Instance stream: 16 floats per instance (column-major mat4); `instance_count` is a draw parameter and is not re-uploaded; `instanceCapacity()` is the slice's capacity; `VN_INSTANCED` decides binding 4 and the vertex layout.
- Every per-primitive mutator goes through the private `Geometry::updatePrimitive` (range guard that counts an ignored edit, copy, `setPrimitive`); `setInstanceTransforms` additionally announces its bounds because the same slice may hold new bytes.
- revision: **shape belongs to geometry** (`Geometry::setRevision` / `bumpRevision`), **bytes belong to the buffer** (`Buffer::setRevision`). The upload cache keys on the buffer revision only; the geometry revision only drives fact-row rebuilds.
- Identity: `vn::graphics::Handle` (gen24/domain8/index32) + `ContentRef{id, revision}`, public header `sdk/vine/graphics/ContentId.hpp`. `Node::id()` / `MaterialBase::id()` are allocated at construction and released at destruction; `Row::identity`, `RenderWorld::RowId`, delta, `RenderCommand::node`, `CompiledCommand::geometry`, `GeometryFacts::geometry` and the material table all key on it. **A table builds rows by `(id, revision)`**, and a row of an old revision keeps answering inside the parking window. An address is no longer an identity (`ADR-2026-11-11-content-identity-is-a-handle`); `Buffer`/stream upload still is an address + `ADR-2026-11-09`'s “holding is identity” (content objects, plans and compiled halves have all moved to `ContentRef`).
- Picking tree: `Geometry::prepareBvh(draw, builder)` defaults to `pickTreeBuilder()` (`kPickTreeLeafSize = 64`), and the built tree is **sealed** (`Bvh::seal()`) — a 14M-triangle tree with a self-chosen leaf capacity of 4 went from 1.36 GB down to 88 MB. A sealed tree's query callback gets the leaf box as its second argument, which picking never uses anyway.
- Collection: `Scene::collectRenderCommands` emits one command per primitive, snapshotting world / state / material / program / instance; sorted by `drawOrder`, opaque front-to-back and transparent back-to-front.
- Bounding boxes: the `Geometry::localBounds` cache key = positions buffer + its revision + segment + geometry revision; `subtreeBounds` in the node's own space; `Scene::boundingBox` counts only content that is visible and matches the layer mask.
- Box/line/plane types live in `vn::geometry`: `Aabb<T>` (`Aabbf/Aabbd/Aabbi`), `Obb<T>`, `Line<T>`, `Plane<T>`; `math` has numbers and transforms only — no `Rect3`, no `math::Aabb*`. See `.ai/design/geometry-primitives.md`.
- OBB fitting: `Obb::computePca` / `computeFixedOrientation`; the eigendecomposition is `math::symmetricEigen` (ascending spectrum, upper triangle only), and matrix → quaternion is `math::toQuaternion`. The minimum-volume OBB and `merge` are not done.

## 3. Targets, layers, colour, image sources

- `RenderTarget` is a **description**; the backend creates images from it. `RenderTarget::setSamples` is the only MSAA entry, and the sample count goes into `RenderPassCompatibility` ⇒ pipeline key ⇒ attachment/image. Colour and depth each resolve to one single-sample image (depth `sample0`, no averaging).
- **A pass's target and its publication are each a value** (B14): `PassDesc::target` = `Target{Kind::Window|Explicit|Transient, target, name, extent, shape}` (`Target.hpp`), `PassDesc::output` = `Output{name, whole_target, image}` (`Output.hpp`); the six `writes*` / `output_*` fields are deleted. `Target::isValid` / `Output::isValid` are checked by `validatePlan` (`InvalidTarget` / `InvalidOutput`); a transient resolved by compile goes into `RenderPass::setResolvedTarget` and **does not** rewrite the declaration.
- **The seam no longer spells Vulkan formats and subpasses** (B8, 2026-11-17): the attachment formats in `RenderPassCompatibility` are `DeviceFormat` (an opaque code, 0 = `kUndefined` = “no such attachment / not learned”), and the render-pass structure is a backend-computed `RenderPassKey` (0 = `kNotStated`; both backends emitted `kSingleSubpassRenderPass` then). Both ends only compare, never decode; `RenderPassKey` is the **structure half** and takes part in equality **beside** format/depth/sample count, not as their hash. `ImageLayout` was still the SDK's Vulkan semantics then (it left in `T15`, see the bullet below).
- **T15 closed (2026-11-23)**: `ImageLayout` and `LoadOpVariantKey` LEFT the SDK. Each backend owns its own
  (`gfx_backend_vk/internal/ImageLayout.hpp`; the load-op variant key left the tree with its only producer, the retired backend). The seam now carries the neutral vocabulary (`LoadOp` / `StoreOp`,
  `DrawKind`, ...) and no Vulkan SEMANTICS at all; `check_vk_exposure.py` V2 says so. The B8 sentence above is the
  state before this move, kept as history; `keyAuditTable()` has six rows (the variant key cannot enter `PipelineKey`
  because it is no longer an SDK type). Review: `.ai/research/graphics-2026-11-23-review-8.md` 4.7/4.8.
- Layers: `Node::setLayers` + `PassDesc::layer_mask`; the nearest declaration wins; `Node::subtreeLayerUnion` is a lazy cache driven by the layer epoch, and the three walks test `(union & mask) == 0` right after visibility.
- Colour: `Colorf` is linear light, `Color` is sRGB-encoded bytes; `toLinear()` / `toColor()` are the pair of conversions; clear colour and pass colour use linear values.
- Image sources: `ImageInput` / `ImageSource`; produced (`ImageRef`: target + attachment) and imported (`Texture`) lifetimes are not merged. `MaterialImages` uploads by key `(Texture*, revision)`, and samplers are cached by value.

## 4. Platform facts (hard constraints)

- Vulkan 1.3 floor with `synchronization2` + `dynamicRendering` + EDS1 / EDS3; a device without EDS3 is refused (`vk.init.dynamic_state_missing`).
- Pipelines are keyed by the attachment SHAPE (formats + sample count) and state no extent: a resize recompiles nothing.
- push constant 128 B; reverse-Z (far = 0); the SDK's picture-CCW front face is `VK_FRONT_FACE_COUNTER_CLOCKWISE`
  (`StateMapping::kFrontFace`; measured by `VkContentDrawTest.TheFragmentStageSeesTheSdksFrontFace`, and the old
  `CLOCKWISE` value was inverted - see the review-17 follow-up below).
- X11 / Win32 are measured on hardware; Wayland is implemented against the WSI contract only (backlog T13).
- One frame in flight per driver (`Impl::kFramesInFlight = 1`), and a driver registers one `DeviceClock` window with that slot count.
- Retirement window = `slots + 1`; `RetirementQueue::setSlots()` updates in place — do not assign a new queue (that zeroes the counters).

## 5. Traps (do not step in again)

> Entries that name the retired backend are its record (`../design/vsg-pitfalls.md` has the catalogue); a rule that still binds is written against the shipped backend.

- `vn::String` wraps `std::u8string`; write literals as `u8"..."`, and printf/gtest use `reinterpret_cast<const char*>(s.data()), s.size()`.
- `PassDesc::camera` is an **owning** `intrusive_ptr<Camera>`; passing a stack camera's address double-frees.
- Under dynamic rendering the implicit `initialLayout` a render pass used to give is the RECORDER's job: an attachment that is not transitioned into the layout
  its `VkRenderingAttachmentInfo` states fails `VUID-vkCmdBeginRendering-pRenderingInfo-09592` (seventeen `test_vk` cases cite it).
- A borrowed depth is always LOADed, never cleared (`TargetObjects::beginPass`): another target's image may not be wiped by its borrower.
- Device extensions must be truly enabled (EDS3's polygon mode / color blend); setting the feature bit alone is not enough, and only the validation layer sees it.
- Vulkan NDC z is the depth (there is no OpenGL `z*0.5+0.5`); reverse-Z far = 0.0, so a depth clear must not clear to 1.0.
- Dynamic `vkCmdSetPrimitiveTopology` may only stay inside the pipeline's topology class; `Topology` goes into `PipelineKey`.
- `POINT_LIST` requires the vertex stage to write `gl_PointSize` statically; the built-in programs write 1.0.
- **MSBuild's incremental build is not trustworthy after a large tree move**: after the 2026-11-23 fast-forward over 403
  commits, objects built against the old headers survived and this suite's binary mixed two vtables
  (`0xc0000005` at 0 ms in a case that passes alone). Delete `build/src`, `build/tests`, `build/tools` objects and rebuild,
  or a stale-object crash gets blamed on the new code.
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
- Transparency: magnitude = drawable opacity × material alpha × base-map alpha, meaning = the material's AlphaMode (`graphics-material.md` 2.7); blend is per-draw dynamic state (baked false, EDS3 enable), not a pipeline constant.
- Device readback pixels are in BGR byte order; assert a single channel with `isRed()` and friends, never by reading `pixel[0]`.
- Changing a public signature or a name requires a full build; an old plugin `.so` reports `undefined symbol`.
- **A hidden / culled upload is not dropped — leaving the tree or a release drops it**: residency is asked by the frame's identities (each draw's buffer + revision + segment), so a
  hidden or culled object is simply absent from the frame and asks nothing; an unchanged revision uploads nothing (`VkResidencyTest.TheSecondFrameOfTheSameContentBuildsNothing`).
  The upload leaves when the host releases the buffer / content (`releaseContent` forgets the key and the object parks) — no time window and no row scan.
  Each frame walks only the resident entries, never the content rows (walking rows makes a “nothing drawn” frame the most expensive one). See `.ai/design/graphics-content-lifecycle.md` §2.5 / §2.9.
- **Two build traps in non-Debug configurations (measured 2026-11-10)**:
  - vcpkg's glslang exports only the `DEBUG` / `RELEASE` configurations; RelWithDebInfo / MinSizeRel have no exact match, so CMake falls back to the **first** (DEBUG)
    ⇒ the plugin links the `/MDd` glslang targets into the `/MD` DLL: `LNK2038` (`_ITERATOR_DEBUG_LEVEL`, `RuntimeLibrary`) + unresolved `__imp__CrtDbgReport`.
    `cmake/VineGlslang.cmake` sets `MAP_IMPORTED_CONFIG_RELWITHDEBINFO|MINSIZEREL = RELEASE` on `glslang::glslang` / `::SPIRV` / `::glslang-default-resource-limits`;
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
  `test_vk` (the server does not report viewable for 10 s, `readError=8 BadMatch`) and **passes in 1 ms when run alone**; an A/B with `git stash` on the previous commit
  **fails the same way** ⇒ unrelated to this change, it is X-server timing/environment (the same class was recorded earlier in this file). Do not change code for it; run it alone + A/B first.
- **S6c-2a landed (2026-11-16, frame bodies truly parallel)**: the device's big lock became a `std::shared_mutex body_gate` — **frame bodies SHARED** (two drivers
  recording/submitting at once), **a frame leaving the window and host verbs EXCLUSIVE** (may wait for frames, does not compete), and `shutdown` EXCLUSIVE too. To make that work: `FrameTimeline`
  **opens frames multiply** (`begin()` mints a new number each time, `submitted`/`abandoned` close by token — the old single slot let the second driver take the first one's token,
  collapsing two frames into one in the watermark comparison); `DeviceClock`/`RetirementQueue` are serialised by the device's `clock_mutex` (**the lock lives on the device, not on the SDK value types**:
  a single-session device supplies none, `G8`; both must still state "the caller serialises"); the allocator / target map / report sink / per-session default program /
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
  and texture uploads are recorded into the frame command buffer at the frame head too; `record`/`recordFullscreen` only read those values. Observation point: `contentCacheCounters().cache_lock_acquisitions`
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
  `ContentDraw` (**recorder**: holds borrowed references to both, keeping only `prepare` / `prepareScreen` / `record` / `recordFullscreen`).
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
- **T48's implementation design is in `.ai/design/graphics-sessions.md` (landed; the shape and the lock table are §2–§4 there)**: a session = a **frame driver** (not a window); the frame-head snapshot makes the record path lock-free; residency is three-state “publish or withdraw”;
  a target claim conflict skips only that pass; retirement **reuses** `DeviceClock` + `RetirementQueue`.
  The correction that decided the division of work: the retired backend's `api::Session` was a **window session**, and its collection / compile scratch was a **device-level singleton**
  — so “a session is closer to a window” was only half right, and what moves into a session is the frame scratch, not the swap chain.
- **Diagnostics carry a stable code (2026-11-15, audit A3)**: `RenderDiagnostic`'s `code` (`vk.<class>.<cause>`) is what the host and the tests
  match on; `message` may be reworded, the code may not; `subject` says “which target this report is about”, and `limit` is the
  `requested` / `supported` pair. The vocabulary is registered in `scripts/diagnostic_codes.txt` (checked both ways: what is used must be registered,
  what is registered must be used), and `check_diagnostic_formats.py` uses `scripts/diagnostic_code_baseline.txt` to set a **cap** on call sites not yet converted
  — a newly written report without a code goes red. **Criterion**: **constructing** a diagnostic (≥3 arguments) must write a code; **forwarding** a `RenderDiagnostic` (`reportDiagnostic(diagnostic)`) does not count — the value already carries the code.
  The registry holds **125 codes** (2026-11-23): `vk.*` for the shipped backend and `engine.*` for the engine's own reports; the retired backend's `vsg.*` family left with it.
  **The reason is named at the layer that knows it**: the content refusal is `ContentRefusal` (category + code + sentence, first one wins) and the device only forwards it; a readback refusal
  is one `ReadbackResult`, mapped in the device where the wait's outcome is known.

- `test_graphics` (model-independent), `test_vk` (the backend, including real-device cases), `test_gui`, `test_core`.
- A backend run is clean when the validation layer reports 0 VUID / 0 SYNC-HAZARD (`VkMsaaTest` / `VkWindowPresentTest` assert the count) and the demo's picture percentage is unchanged.
- Static gates: `check_ai_docs.py`, `check_doc_symbols.py`, `check_include_hygiene.py`, `check_export_annotations.py`, `check_diagnostic_formats.py`, `check_public_docs.py`,
  `check_api_style.py` (API style: no setters/callbacks in values, no `getXxx` accessors, `bool` + out-parameter may not coexist, an `XxxBuilder` must have `build()`;
  4 existing findings are registered in the script's `BASELINE` and are deleted as they are fixed),
  `check_vk_exposure.py` (also CTest `vk_exposure`: V2 no render-API include, V4 shell types, V5 **the SDK's prose
  names no backend** - `\bvsg\b|\bVsg|\bvk\b` except a `vk.<category>.<cause>` code, V6 **the SDK names no
  plugin-internal unit** (`ResidencyClaims`, `TargetTable`) - the set is derived from every plugin `internal/` stem;
  `graphics-architecture.md` C4).
- `scripts/asan_check.sh` / `VN_ASAN_*`; only `skipped == 0` counts as having run the device battery.
- Measuring a backend's per-frame cost: `test_vk`'s measurement cases (`--gtest_filter='*Measure*'`) print µs/frame plus `builds` / `uploads` / `created`, and per-frame host allocations are counted in `../design/vk-backend.md` §9.1; the cost model is that page's §2.4 / §2.16.

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
- **What a shareable chunk means (2026-11-19, review 6)**: a row is unchanged only when BOTH its node content epoch and its fold serial
  (`Row::resolved_serial`, moved by the fold's one writer `storeResolved`) are unchanged. The epoch alone could not see an edit's effect on the
  subtree BELOW the edited node (an announcement climbs to the ancestors only), so a chunk of pure descendants was served from the previous
  publication. The fold now stores the inherited state / material / program on EVERY row, containers included, because that is the seed a re-fold
  below it reads back. A full rebuild reports every row as moved (a fresh row cannot claim the same fold answer): the edit path shares, a rebuild copies.
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
  **Superseded 2026-11-23**: the case now asserts the shared-chunk shape instead of a wall-clock limit - see the
  dated entry at the end of this file.

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
  `(pass, CompiledCommand, facts)` = a content draw, `(pass, CompiledDraw, inputs)` = a full-screen packet (**no facts needed**:
  its identity is all in the plan + the caller's offer). `sampledColorCount` / `sampledDepthCount` moved from `ContentPass.cpp`'s
  anonymous namespace into the header (`record`'s set construction and `serveHalf` use the same spelling).
- **Two pairs of counts, one rule**: `set_colors/set_depths` = how many the frame's input set binds (a pass fact); `key_colors/key_depths`
  = how many the pipeline key writes (a program that does not sample set 1 ⇒ 0, with `programSamplesInputSet` left in the .cpp anonymous namespace). Both pairs
  are written in one place, so they cannot diverge.
- **Duplication deleted**: the variant/layout/program/Key construction in `ContentHalves::halvesFor`; `recordCommand`'s
  four-way geometry/material/variant/findProgram/half match (→ `halfFor(needs)`); `recordFullscreenDraw`'s source
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
- **Trap**: do not build a snapshot from a COPY of the rows — it clears `nodes` / `node_epochs`, so `makeSceneSnapshot` loses chunk sharing; the publication cache
  lives on `ScenePublisher` (`publication_cache_`) and survives a table rebuild (moved out of `SceneBake` on 2026-11-19).
- **What the diff compares**: `DrawListDiff` compares row identity / geometry + description / material + program / world / opacity / order /
  authored_state / instance slice; the order is the select's draw order (commands do not store a sort key).
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
- **Public header**: `VertexLayoutKeyHash` (`vine/graphics/DrawState.hpp`; since 2026-11-21 the key is one
  `VertexLayoutSlot{location, components}` per declared attribute in binding order, and the hash covers both fields); the `ContentDeclarations`
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

- **The definition is settled (one place in the SDK)**: `FrameCounts` / `frameCountsOf` / `packetDraws` / `drawItemsOf` / `passHasEffect` went into
  `FrameDescription.hpp` (inline, no new exported symbol). The rule = **a pass with an effect**: at least one draw packet, **or** a declared
  clear (a “clear only” pass is a real pass); an entry with `draw == nullopt` does not count (it does not even declare a target). The two backends used to compute it differently:
  vk counted “entries with draws” (treating a call with no camera as a draw, +1), vsg counted “passes whose compiled draws are non-empty” (missing clear-only passes,
  −1), and `FrameCounters` was a third. Both now call `frameCountsOf(frame)`, matching the recording count by construction.
  (2026-11-20: the value is `FrameCounts` and carries `passes` / `draw_packets` / `draws` - the last is the `DrawItem` count a profiler calls draw calls.)
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
- **vk**: a full-screen packet must **always** be `DepthPolicy::Disabled` (enforced by `ContentDraw::planScreen`, the reference backend's M4b rule).
  A full-screen triangle sits at z = 0.0 = the reverse-Z far plane; the window pass's D24 clears to 0 and deferred's composite **borrows** the G-buffer
  depth ⇒ inheriting `TestAndWrite` discards the whole block, while **zero VUIDs and every `recorded=1` in the recording layer**. The case is
  `AScreenCopyIsDrawnWhateverThePassDepthPolicySays`; the old screen fixture explicitly set `Disabled`, which is exactly why it did not catch it.
  See `.ai/bugs/vk-screen-draw-inherits-pass-depth.md`.
- **vk's screen set must bind the map the pass resolved**: bindings 7/8 existing ≠ binding the right image. `recordFullscreen` now takes
  `ShadowSource` (`Device::executeFrame` already called `resolveShadow` for every pass): with a map, bind the real depth + the per-call packed
  `VineShadowBlock`; without one, the white stand-in + `params.x = 0`. Symptom = "forward has shadows, deferred does not, zero diagnostics"; the case
  `VkShadowTest.AFullscreenPassShadesWithTheMapItResolved` (samples binding 7 and writes depth, a white stand-in reads 255).
  See `.ai/bugs/vk-screen-pass-shaded-without-the-map.md`.
- **The plan identifies rows by `(owner, name)`, a name is not a label**: the demo's four G-buffer previews were all called `gbuffer_preview` ⇒ four declarations
  landed on the same row, only the last survived ("only block 4 shows"). The other half: the layout step only changed the demo's own `FullscreenPass`
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
  (`extra_colors` defaults to transparent black), RGBA16F images, M4's multi-source full-screen draw and the light block were already enough. This is evidence that "the seam is enough",
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
  **`colors` / `color_views` are the picture (the resolve image) from now on**: full-screen draws sample it, `readColorBuffer` reads it,
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

- **Rule**: a plan can only say `Scene` / `Fullscreen`; a subclass pass is `describe()`'s escape hatch, outside the plan contract. An in-place update of a declaration with the same name and kind
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
- **`frame()` two states**: `FrameCallOutcome` changes from an enum to a struct — `Status::{Ran, Submitted, NotReady}` + `reason`
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
- **T25 + T29**: new `FrameUploads` (SDK `FrameReport.hpp`) hangs off `FrameReport::uploads` — this frame's stream/texture
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
  M3 targets/state/MRT; M4 material/texture/light/full-screen draw; M5 swapchain; M6 residency/prepare; M7 comparison with vsg.
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
  `targets.observe`, `store.track` (moving the tracking out of `collectContentDraw/collectFullscreenDraw`),
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

- **New unit**: `internal/ContentPlan` (`ContentPlan.hpp/.cpp`) — the **values** resolved for one frame: one `Call` per draw packet
  (`ContentDraw*` recorder + the resolved `Draw`/`FullscreenDraw` + fixed arrays for block bindings/push blocks/vertex bindings), one slot per pass
  (with its own `core::StateRegistry`); `Pass::pass/registry` are borrowed, `halves_` is a **pin** on `shared_ptr<const void>`
  (`ContentHalves::pin()`), so another driver's frame-head sweep does not release a recorder this frame still needs (retirement parking does not care about
  "a frame not yet submitted"). A call **stores no spans** (`Call::value()` assembles from the call's own arrays) — vector reallocation would invalidate a span pointing into itself.
- **`commitFrameThrough` three parts**: **resolve** (content lock: `applyTargetPlans`, material arena growth, `resolveContent` →
  `ContentAssembly::resolve` → `ContentPass::resolve`, refusals/reports happen here), **record** (lock-free: `ContentPlan::record`
  builds vsg nodes), **commit** (lock again: `executor.record`, sweep, session submit, `finish`). The frame head writes the view/lights/shadow/draw/
  material blocks and hands only offsets to recording — another frame head rotating a ring/sweeping/growing in between does not affect this frame.
- **Resolve half / record half separated**: `ContentDraw::plan(key)` (takes the layer + variant pool, returns `Identity{pipeline, variant}`) and
  `record`/`recordFullscreen` (bind only the resolved pipeline, **no acquire, no pool touch**); `core::StateRegistry` gains a
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
  `DrawPlan` = `kind` (Content/Fullscreen) + pipeline/resolved + content payload (material/variant/streams/…) + this call's `slots`.
  `ContentCaches::planCommand` / `planScreen` **merge into one verb, two overloads** `planDraw(RenderCommand,…)` / `planDraw(DrawDescription,…)`;
  `resolveTextures(material, commands, FrameHead&, FrameHead::DrawPlan&, refusal, lock, batch)` writes frame-level facts into head, slots into plan.
  `ContentDraw::record(…, span<const FrameHead::DrawPlan> plans, const FrameHead& head, …)`, `recordFullscreen(…, const DrawPlan&, const FrameHead&, …)`.
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
  the 35 setters and `FullscreenPass::setProgram` are deleted. The engine's write gate is `detail::PassNode`
  (`adopt` / `resolveTarget` / `setDrawSize`, friend). `PassChain::restate(desc)` is the chain's "one explicit diff"
  (the name deviates from the research doc's `chain->update(d)`: `update()` is already a per-frame verb, avoid one name two meanings). `PassChain::apply()` no longer
  refreshes from nodes — `pending_` is the current shape. `RenderEngine::bindPassContent` becomes adopting a declaration with changed content.
  One **semantics-preserving point**: in `adoptDesc`, "an engaged invalid `DepthView`" = explicitly clear the borrowed depth; `nullopt` = say nothing.
  An empty input name is skipped in plan validation and resolution (the old setter's "empty name ignored").
- **Migration surface**: demo (`addDemoPass` takes `PassDesc`, PiP re-anchors through `applyPlan`, preview layout holds declaration values),
  `tests/PlanTestSupport.hpp` (`applyPassForTest(desc,order)`, `updatePassForTest`, `restatePassForTest`,
  `asFullscreenPassForTest`), all of test_graphics / test_vsg / test_vk / test_gui.
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
  16 steady frames after 8 warm-ups: **85 allocations / 10,612 B** for ONE draw packet, **133 / 12,424 B** for FOUR -
  the same number on every frame, so churn, not growth. That is ~16 per further draw packet and 69 fixed; bands 66/10/8/1
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
  `FrameCallOutcome`'s `NotReady` is a precondition) and 2.12 (no render graph; `Transient` borrows the WORD, not the
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
  `DrawPacket`, `MaterialBase` / `MaterialBasePtr` -> `MaterialValues` / `MaterialValuesPtr` (files renamed),  `EngineNotReady` -> `EngineNotReadyReason` (file renamed), `createSession` / `releaseSession` ->
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

## 2026-11-23 — a measurement states the shape, the time is the record (N2 / K1)

- **What changed**: `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` no longer asserts `publish ≤ 250 µs`.
  It asserts what N2 actually means - an edit is a NEW publication that shares every 32-row chunk but the one it
  touched (`shared == chunks - 1`) - and still prints `publish` / `select` / `shared`. `N2` and `K1` in
  `graphics-frame-pipeline.md` are restated in the same terms.
- **Why**: an absolute microsecond figure is a property of the machine. On this box (Windows, MSVC 19.52,
  RelWithDebInfo) the case reads **publish 321-346 µs, select 560-600 µs, shared 62/63** over ten runs - stable,
  not noise - while `SelectTest`'s 2026-11-13 reading of a comparable workload was 337-340 µs for select. A gate
  calibrated on a faster box therefore fails here while the code does exactly what it should, and a red test that
  cannot tell "this machine is slower" from "the copy-on-write broke" teaches nothing. `SelectTest` already states
  the rule ("the times are the record, not a CI gate"); this case now follows it.
- **What still catches the defect K1 names**: the sharing count. A whole-table copy makes `shared` 0 while
  `makeSceneSnapshot` otherwise looks perfectly correct, and the assertion fails on any machine.
- **If the time is ever revisited, measure the split first**: on an epoch change `ensureRows` walks the row table
  three times (`rowsStillMatchTheGraph`, `refreshBoxes`, `refreshValues`) before `makeSceneSnapshot` copies the one
  dirty chunk. The chunk copy is NOT the cost (`shared == 62/63`); the three walks are the candidates, and the
  250 µs figure predates the four-named-steps refactor (`17ab42eb`) that made them explicit.

## 2026-11-23 — vk on Windows: the platform branch runs, and the pixel instrument landed the same day

- **Measured**: `build\bin\RelWithDebInfo\test_vk.exe --gtest_brief=1` on this Windows box (MSVC 19.52, hardware
  Vulkan - `vulkaninfo --summary` lists an RTX 3050 and an Intel UHD 770; the run reports `max_samples = 8`,
  validation on): **174 tests from 42 suites, 174 passed, 0 skipped, ~11 s**. The plugin module builds here too
  (`build/bin/RelWithDebInfo/plugins/vine/gfx_backend_vk.dll`).
- **What that proves**: `WindowSystem.cpp`'s `#if defined(_WIN32)` half RUNS, not merely compiles -
  `VkProbeTest.AProbeDeviceBringsUpItsOwnWindowAndPresents` creates a window with `CreateWindowExW`, creates the
  surface through `vkCreateWin32SurfaceKHR`, presents into it, and destroys window and surface with the session.
- **The pixel half landed the same day** (this closed the gate's last platform item): `WindowPresentTest` no longer
  has its own XCB window; both cases take `tests/test_vsg/TestHostWindow.hpp` - T12's fixture, one instrument for
  XGetImage (X11) and `PrintWindow` + `PW_RENDERFULLCONTENT` after `DwmFlush` (Win32) - and assert the presented
  picture on both platforms. The suite read **172 passed + 2 skipped** before the port (the two window cases had no
  instrument on Windows) and **174/174 with no skips** after it.
- **The mutation is the evidence**: with the window pass' clear changed to black, the corner assertion fails with
  `rgb = 0, 0, 0`; with it magenta it passes. So the case reads the composited picture, not the window's own
  background - a pixel case that passes for both colours would have been an instrument check in disguise.
- **Trap (write assertions against a CHANNEL, not an order)**: a Win32 read hands back the surface's own order
  (blue-green-red for a GDI window, the swapchain's for a Vulkan one) and X11 hands back the server's. Pure colours
  whose red and blue bytes are EQUAL survive both: green and magenta for the picture, white for the instrument
  paint. That is also why the frame's clear moved from red to magenta.
- **Consequence for T35's gate**: the platform item is closed; open are a measured Wayland fallback (T13) and a
  performance reading the gate accepts.
- **Three homes held one stale fact**: the user guide, `vk-backend.md` 1.3 / 9.2 / 9.3 and this plugin's
  `CMakeLists.txt` all still said "Win32 待实现 / only Linux/X11 ships". A platform claim is a MEASURED fact, so it
  moves only with a run to quote - this one came with the numbers above. Deleting `vsg` is a separate criterion
  (backlog T60), not part of the switch gate.

## 2026-11-24 — the layout record is per IMAGE, and that is what made two fixes stick

- **What changed**: `TargetObjects` dropped `color_sampled` / `depth_sampled` for one stored `ImageLayout` per image
  (`color_layouts`, `sample_layouts`, `depth_layout`, `depth_picture_layout`); `beginPass` now TRANSCRIBES those records
  into `PassAttachments` instead of deriving four layouts from three flags, and every mover writes the record of the image
  it moved (`beginPass`, `resolveShadow`, the full-screen source transition, both readbacks). `PassAttachments::color_from` /
  `picture_from` are spans, one entry per image. `TargetObjects::fresh` stays - it is the one SET-level, atomic hint a
  reader outside the frame gate takes.
- **Why per image and not per role**: the two bugs of 2026-11-23/24 both came from one flag standing for two images (an
  MSAA depth attachment + its resolve picture) or for one image in two roles (a single-sampled depth that IS the picture).
  With a record per image neither shape has an expression left.
- **The borrow edge now OWNS its record**: `TargetObjects::depth_lender` is a `shared_ptr<TargetObjects>`, so the borrower
  states the lender's real layout and writes it back, and the lender's images cannot be destroyed while a borrower still
  attaches them (a `borrowed_depth_image` handle could dangle before).
- **Measured**: `test_vk` **188 cases / 44 suites**, `test_graphics` **642 / 82**, ten gates green.
  `VkDeferredTest.TheDeferredFrameIsDrawnAgainAfterItsGbufferWasSampled` is the colour-half witness (one sample and the
  device's maximum): forcing `color_from` to `ColorAttachment` turns it red with
  `VUID-vkCmdBeginRendering-pRenderingInfo-09592` + `vkCmdDraw-None-09600`, the mirror of the depth half's 09588.
- **Lesson (the one worth keeping)**: a *derived* record pair invites the same bug twice - first in the depth direction,
  then in the colour one. Storing the fact next to the image it describes costs one enum per image and removes the class.

## 2026-11-24 — review of the same change: what the records had wrong, and one correction

- **Correction to the entry above**: `depth_lender` does NOT keep the lender's images alive while a borrower attaches them -
  the device's park still destroys them once the completion watermark passes. What refuses such a frame is
  `TargetTable::ensure`, which needs the lender's map entry and answers `vk.target.borrow_source_missing` when it is gone.
  The shared pointer buys the RECORD's lifetime (and the layout, which cannot be read any other way without the table lock).
- **One image playing two roles had one record but two write paths**: `beginPass` still special-cased `depth_borrowed`
  while the image/view/layout accessors delegated. The indirection now lives in all four accessors
  (`setDepthAttachmentLayout` added), so `beginPass` has no borrowing branch at all.
- **A colour-only target was given a depth record**: `depth_layout = DepthAttachment` ran unconditionally, describing an
  image the set does not have. The depth roles are now stated only `if (hasDepth())`, and no depth record is written then.
- **Readbacks stopped being movers**: each reads ONE image from its own record and puts it back there (transfer layout
  lives inside the command buffer), so a device verb changes no record - and a readback of one attachment no longer drags
  the whole set out of `SHADER_READ_ONLY`.
- **The pass contract is four ROLES**: `PassAttachments` = `color` / `picture` (`PassRole`: images + views + records, three
  parallel spans because the bind path needs one contiguous view span) + `depth` / `depth_picture` (`PassImage` values) +
  the aspect. 13 fields -> 5, and the silent `Undefined` fallback for a short layout span is gone with them.
- **Renamed**: `sample_layouts` -> `multisample_color_layouts` (`sample` in this domain reads as "sampled", and the sibling
  vectors say `multisample_*`).
- **Measured again**: `test_vk` 188 / 44, `test_graphics` 642 / 82; forcing the colour role's and the depth role's records
  to a constant layout turns all three `*DrawnAgain*` cases red (09588 for depth, 09592 + 09600 for colour), so the two
  witnesses still pin the plumbing, not the old field names.

## 2026-11-24 — the second design pass: what the records, the cache key and the failure path had wrong

- **A cache key must name the sampler**: `FrameScratch`'s descriptor-set key was (screen, five views, shadow view) while a
  `VkDescriptorImageInfo` binds a view AND a sampler, and a material authors its own (`setTexture(slot, texture, sampler)`).
  Two materials reading ONE texture through two samplers therefore shared one set and the second silently got the first's
  sampler. The key now carries the five samplers and the shadow sampler (`TwoMaterialsReadingOneTextureThroughTwoSamplers…`,
  whose witness is `set_builds` going up by exactly one, not a pixel - a filter difference on a 1x1 texel is invisible).
- **A resolved role is stored `DontCare`**: `PassLoad` now carries the seam's `LoadOp` / `StoreOp` instead of two booleans,
  `beginRendering` downgrades a role whose resolve consumes it (sample buffers, a resolving depth), and `loadOpOf` /
  `storeOpOf` became live - before this they had zero production call sites while the design page justified `BarrierPlan`'s
  seam include with them (G8 by accident, G6 by claim).
- **A mover is one call**: `BarrierPlan::transitionImage` (record + barrier together) and
  `TargetObjects::transitionDepthPicture` (the indirect depth record) replaced every hand-assembled
  `barrierBetween(stateOf(record), …)` + `record = to` pair - the shape that produced 09588/09592 twice.
- **A borrowed depth is a claim on the LENDER**: the borrower's pass attaches the lender's image and rewrites the lender's
  ONE record, so claiming only the borrower let two drivers submit against one depth image. The frame now claims
  `depthSource()` too (`TargetClaimFrameTest.ABorrowedDepthIsTheLendersToHold`, witnessed by `claimNumbers().refused`).
- **A frame that never reached the queue moved nothing**: `resetLayouts()` for this frame's own sets, an exact `MovedLayout`
  rewind for a sampled source / shadow map, and `WindowTarget::abandonFrame()` for an acquired-but-unpresented image (its
  `image_available` semaphore may not be handed to a second acquire). See `P17`; the texture half stayed open as `T66`.
- **The recording path allocates nothing in a pass**: the colour attachment-info array and the clear-value array are stack
  values of `kMaxColorAttachments` (Vulkan's maximum), and a shape asking for more is refused as `vk.target.too_many_colors`.
- **`ScreenSource::images` deleted**: no reader - the source's transitions belong to the pass-order loop, against the
  source's own records.
- **Measured**: `test_vk` **191 cases / 44 suites**, `test_graphics` **642 / 82**. Mutations: reverting the sampler key and
  the lender claim turns exactly the two new cases red and nothing else.

## 2026-11-24 — the third design pass: what the clock measured, and the three costs nobody counted

- **A boundary that does not wait measures arrival**: every per-pass clock boundary was written at `TOP_OF_PIPE`, so
  `FrameTiming::passes` reported when the command processor reached each boundary, not what each pass spent. Boundaries
  now wait (`ALL_COMMANDS`, one per pass, written after it), the frame's start keeps `TOP_OF_PIPE`, and the query pool
  shrank from `2 + passes + 1` to `2 + passes`. Submission moved to `vkQueueSubmit2` in the same pass, so no 1.0 stage
  mask survives next to a 1.3 command buffer.
  **The trap**: a software rasteriser cannot see this difference (it executes commands in order, so the old stage looked
  right and the mutation stayed green here) - which is why the stage is a named constant (`kPassClockBoundaryStage`,
  `kFrameClockStartStage`) pinned by `BarrierPlanTest.APassClockBoundaryWaitsForEveryCommand`, with
  `VkPassTimingTest.AHeavyPassReportsMoreTimeThanACheapOne` stating the property for hardware.
- **One pass = one value**: `FrameHead::Pass` now carries `entry` / `draw` / `target` (and, since review 9,
  `sources` / `shadow_map` / `shadow` - what the pass READS), and
  the local `Runnable` list plus the `pass_position` / `pass_index` arithmetic are gone. The frame's own working set
  (`moved_layouts`, `lent_depths`, `stamp_values`) moved onto the driver (`FrameRun`, cleared per frame), and so did the
  command buffer (`FrameDriver::commands`, created with the pool, reset by `vkBeginCommandBuffer`) - so the frame path
  has no per-frame allocate/free pair and `vk.frame.command_buffer_missing` no longer exists.
- **One pass entry = one dependency**: `BarrierPlan::recordBarriers` (and `BarrierBatch` for sampled-source moves) state
  a pass' transitions as one `VkDependencyInfo`; a seven-attachment multisampled set is now one command with seven
  barriers instead of seven commands.
- **A state command is issued when the state changes**: `ContentDraw` keeps a `RecordedState` shadow (viewport, scissor,
  line width, depth bias, cull, polygon, depth test/write/compare, blend), reset by `beginFrame()` ALONE - since review 9
  it survives a pipeline bind, because every pipeline declares the same eleven dynamic states
  (`VkContentDrawTest.TwoAlternatingPipelinesRestateNoState`). `T54` made state a command; this makes a REPEATED state
  free - the viewport and the scissor are pass facts and used to be re-stated per draw.
- **The frame names itself**: one `VK_EXT_debug_utils` region per frame and one per pass, from `RenderPass::name`, so a
  capture tree and the per-pass timing agree. `transferWriteState()` moved next to its read twin in `BarrierPlan` (the
  write half was a private helper in `ContentImages.cpp`: one table in name, two in fact).
- **The witnesses are one accessor**: `Device::recordingNumbers()` answers `barrier_dependencies` / `barriers` /
  `state_commands` / `debug_labels`, and `debugMarkers()` says whether this session can label at all.
- **Measured**: `test_vk` **196 cases / 44 suites**, `test_graphics` **642 / 82**. Mutations: one dependency per barrier
  turns `APassEntryIsOneDependencyWhateverItWrites` red; always re-stating the viewport turns
  `ARepeatedStateIsNotARepeatedCommand` red (13 vs 12 state commands); reverting the clock stage turns the value pin red.

## 2026-11-24 — the fourth pass: residency waits for the fence, and the frame has one walker

- **A texture is resident when its fill is PROVEN** (`T66`, closed): `ContentImages` keeps a recorded copy PENDING in the
  batch that recorded it and promotes it in `batchCompleted`, which is called exactly where the fence was waited;
  `batchAbandoned` parks it instead, so the last proven revision stays. The second half nobody had written down:
  another driver's frame head used to find the entry before its copy was even submitted. `Progress::pending` /
  `pending_bytes` say what is recorded and not proven, and they are deliberately NOT part of `live`.
  A batch is a DRIVER (a frame and its warm-up share one command buffer and one fence), so `endFrame(batch, completed)`
  replaced both `releaseFrame` and `releaseUploadStaging` - and a warm-up's replaced revisions are now parked instead of
  sitting in a batch nobody closed.
- **The 1x1 stand-in is a SHARED CONSTANT, not an asset**: a frame that finds it mid-fill binds it rather than refusing
  every draw that needs it (the first attempt - per-batch visibility - turned the two-driver race case black). Its
  failure is not shared: a batch that never ran drops it, so a black stand-in cannot outlive the frame that failed.
- **ONE WALKER for both frame verbs**: `FrameDriver::claimPasses` (Record / Prepare) is the one place "which passes can
  this device run, and which claims does each take"; the warm-up's copy had already lost the depth LENDER, a full-screen
  source and a declared input. `resolveFrameHead` is the one resolution, which deleted `ContentCaches::preparePass`
  (the warm-up now records the same uploads a frame would, and the frame finds them resident).
- **A pass' attachment set is a value** (`PassTarget`): extent, compatibility, `holdsPicture()`, `begin` / `end` /
  `rewind` over an off-screen set or the window's acquired image - the four `if (window) A else B` pairs in the frame
  body are gone. The load rule moved to `DynamicRendering::decidePassLoad(...)`: it was the same expression written
  twice, with only "does the attachment already hold a picture" differing. The pass' rectangle moved to
  `ContentDraw::DrawScope` (computed once per pass, not once per draw).
- **The frame is one value** (`FrameRecording`) and the command buffer is the driver's: the frame body's dozen locals
  became one value with capacity kept, and `record` / `recordFullscreen` lost four arguments each (nine to five, ten to
  six) and answer nothing now (nobody read the count).
- **Smaller**: `surfaces_mutex` deleted (the body gate is the protection - `prepareFrame` proved it by reading the list
  without it), `lent_depths` is a small vector (an `unordered_set` allocated a node per entry in the path that promises
  "a steady frame allocates nothing"), `planDraw` split back into `planCommand` / `planScreen`, and the test door
  `Device::refuseNextSubmit()` makes the failed-submission classes reachable.
- **Measured**: `test_vk` **197 cases / 44 suites**, `test_graphics` **642 / 82**. The new case
  `VkPrepareUploadTest.APrepareWhoseSubmitFailsPublishesNothingAndTheNextFrameUploadsAgain` goes red when `viewFor`
  publishes at record time (verified by mutation), and it also pins P17's rewind: a refused frame leaves the target's
  record `Undefined`, so the readback answers `NotReady`.
- **Trap**: `endFrame`'s batch id must be the DRIVER's (`uploadBatch()`), not a hard-coded 0. The warm-up and the frame
  record the same batch, and a mismatched id means `hasPendingUploads(0)` answers false, so the batch is never submitted
  (measured: a prepared texture samples black).

## 2026-11-24 — per-attachment load/store

- **The seam states one pair per attachment** (`AttachmentLoadStore` in `DrawState.hpp`): `ClearPolicy::color_ops` (one
  per colour attachment) + `depth_op`, authored through `PassDesc`'s same two fields. Empty/unset keeps the group
  spelling (`clear_color` / `clear_depth`), so every existing pass is unchanged; a stated entry overrides its own
  attachment.
- **A pass that states only ops announces a policy**: `RenderPass::clearPolicy()` returns a `ClearPolicy` when either
  group flag clears OR an op is stated - "load both, discard one" must reach the backend.
- **vk's `PassLoad` is now per attachment**: `span<const PassAttachment> color` + one `PassAttachment depth`;
  `decidePassLoad` applies the group default, the per-attachment override, and `Load` on a set with no picture ->
  `Clear` (the old safety net). The resolve downgrade stays in `beginRendering`. A BORROWED depth forces BOTH halves
  (`Load` + `Store`) - a borrower may neither clear nor discard the lender's image; the borrow test states
  `DontCare`/`DontCare` so a missing override turns it red (the load half deterministically).
- **Witnesses**: `PassLoadTest` (6 device-free cases), `VkContentTargetTest.EachColourAttachmentLoadsOrClearsOnItsOwn`
  (one pass, three answers: clear / load / DontCare, read back at a corner), and
  `RenderEngineTest.APassAnnouncesPerAttachmentOpsToTheBackend`.
- **Rejected**: backend-only arrays (no author, `G8`); renaming `ClearPolicy` in the same batch (a word sweep with no
  witness). ADR-2026-11-24.
- **`PassDraw::clear == nullopt` now means "no clear"**: it used to be read as "clear both". The demo's
  `forward_transparent` and every window overlay/preview announce no policy, so they were wiping the deferred result
  (`deferred_lighting clear=-` is a fresh target and still clears; `forward_transparent clear=-` must load). The black
  render area was this, not the per-attachment arrays. A fresh set still falls back to `Clear` (nothing to keep).
  Witness: `PassLoadTest.NoAnnouncementKeepsThePicture`.
- **A presented image is always stored**: `WindowTarget::beginPass` forces `Store` on the window's one colour
  attachment (a stated `DontCare` is ignored there); the depth is not forced, because it is not presented. Witness:
  `VkWindowPresentTest.AWindowFrameIsDrawnAndPresented` draws a second frame stating `{Clear, DontCare}` and still
  reads the clear.

## 2026-11-24 — attachment vocabulary follows the engine families (`Op` stays)

- `LoadOp`/`StoreOp` **stay**: `Op` is Vulkan (`VkAttachmentLoadOp`) and WebGPU (`GPULoadOp`/`loadOp`);
  `Action` is UE5/Unity/Metal's. The enum rename was withdrawn after review.
- `AttachmentLoadStore` → `AttachmentOps`; `ClearPolicy` → `AttachmentPolicy` (fields keep `color_ops`/`depth_op`);
  `RenderPass::clearPolicy()`/`PassDraw::clear` → `attachmentPolicy()`/`attachment_policy`.
- Backend: `PassLoad` → `PassAttachmentOps`, `PassAttachment` → `ResolvedAttachment`, `PassAttachments` →
  `PassAttachmentSet`, `decidePassLoad` → `resolveAttachmentOps`; test `PassLoadTest` → `AttachmentOpsTest`.
- Why: `ClearPolicy`/`PassLoad` no longer said what they hold; `Op` is the neutral term the seam already used.
- Verified: build 0; test_vk/test_graphics green; gates.

## 2026-11-24 — `ensure` answers the record (the claim path could be handed null)

- **The defect** (recorded 2026-11-17 in `../bugs/vk-target-fresh-data-race.md` as "neighbouring ... recorded in the
  backlog", and it was never in `../backlog/graphics.md`): `Device::claimPasses` did `ensure(*draw.target)` and then
  `*target_table.lookup(draw.target)`. `releaseRenderTarget` takes no body gate (R6), so a host thread could erase the
  entry between the two calls and the second `lookup` answered null - dereferenced unchecked.
- **Fix**: `TargetTable::ensure` returns `std::shared_ptr<TargetObjects>` (null = refused/unbuildable) taken under the
  same map lock the build ran under; `claimPasses` keeps that record and `setRenderTarget`'s announce discards it.
  One call, one answer (`G4`); the window the release used is gone by construction (`G10`).
- **Witnesses**: `TargetTableTest.EnsureWithoutASessionAnswersNoRecord` (device-free: a null record and no entry);
  `VkMultiSessionTest.AReleaseCannotTakeAwayTheRecordAPassHasJustResolved` (the answer is still whole after an erasing
  release, while `lookup` answers null); `VkMultiSessionTest.AReleaseRacingTheFramesClaimsNeverLeavesAPassWithoutATarget`
  (a release loop racing frames - the interleaving is **not** forced by any hook, so it asserts the invariant).
- **Honest boundary**: no test reproduces the original null dereference deterministically; what changed is the shape.
- Verified: `test_vk` **207**/45 green, `test_graphics` green, TSan `test_vk` 207/207 with 0 warnings, ASan+LSan focused
  PASS, eleven gates green, demo 0 error / 0 warning (session up 40 ms, `Pending -> Presenting` 183 ms).

## 2026-11-24 (later) — the pass handle owns the record it draws into

- **Why the `ensure` fix alone was not the whole answer**: the claim is an EXCLUSION book (`holders_`), not a hold on the
  record. A record survived a recording frame only because three invariants happened to hold together - one frame in
  flight, frame bodies serialised device-wide, every body waiting its fence - and no type stated it. That is `G12` debt
  that ADR-2026-11-15's own trigger (multi-frame in flight) would turn into a use-after-free.
- **Change**: `PassTarget::forTarget` takes the `std::shared_ptr<TargetObjects>` that `TargetTable::ensure` answered and
  keeps it; `claimPasses` moves the record straight into the handle. The window half still borrows (the device owns the
  window for the session). The park now dates only the SUBMITTED window, which is the one a handle cannot cover.
- **Witness** (ASan is the instrument, a plain run cannot see it): `VkMultiSessionTest.APassHandleKeepsItsTargetAliveAfterARelease`
  releases the target, runs frames until `retirementNumbers().pending == 0` (the park's release act really ran), then reads
  the handle. Mutation checked: with the borrowing handle the same case is
  `heap-use-after-free ... in vn::backend_vk::PassTarget::holdsPicture()`.
- **Note**: a head clears its pass slots at the NEXT `beginFrame`, so a released record is held at most one frame body
  longer than the frame that resolved it. Bounded by design, not a leak.

## 2026-11-24 (third) — a release ends residency, not the objects (two windows)

- **The hole**: a release does not wait (R6) and off-screen frame bodies OVERLAP (the body gate is SHARED), so one
  engine's frame end runs the park while another engine is still recording. The park only dated the SUBMITTED window;
  the recording window was covered by an implicit and partly false invariant set.
- **The rule** (`ADR-2026-11-24-residency-borrowing`): destroy only when no submitted work names the object AND no
  value the device lent still names it; a release never waits. The park asks the second question itself
  (`Impl::parkRelease`, a `use_count` probe) and re-asks at a later frame end.
- **Seam**: `RetirementQueue::ReleaseFn` is `void(const FrameTimeline*)` (null = teardown), so a release act can ask
  for another window without the clock lock it runs under - the queue's note already promised that and could not.
- **Values that keep a record alive**: the off-screen `PassTarget` (draws into) and `FrameRecording::read_records`
  (reads: a screen draw's source, a shadow map). Both dropped by `FrameHoldCloser` at the frame's end, in both frame
  verbs.
- **Two defects surfaced, both in the backlog and both trigger-HIT**: `T60` (content entries are not shared, so
  `releaseContent` beside a recording frame is a destroyed `VkBuffer` named by a live command buffer) and `T61` (an
  idle frame window freezes the park: `updateWatermark` folds a MINIMUM over live windows, so `submitted` reached 7
  while `completed` stayed at 2 - the clock's own note says an idle window constrains nothing). T61 is what currently
  hides T60, so one batch should fix both.
- Verified: `test_vk` 209/209, `test_graphics` green, TSan + ASan runs, eleven gates, demo.

## 2026-11-24 — T60 + T61: completion belongs to the submission, content is borrowed too

- **The concept that was wrong (T61)**: `DeviceClock`'s watermark was a MINIMUM over registered windows, so an engine
  that stopped framing kept its last report forever and no parked release ever ran (`submitted` 7 / `completed` 2,
  measured). Now every submit takes a number inside the queue's lock (`noteSubmission`) and reports its proof after
  the fence (`noteCompletion`); the watermark is the oldest unproven submission minus one, or every number when
  nothing is unproven. Registration (`addDriver` / `removeDriver`) is identity only. `FrameTimeline` keeps only
  frame-number minting (no `submittedFrame` / `completedFrame` / `completeUpTo` / `retirePoint`), and
  `RetirementQueue::retire/advance/retirePoint` take the clock. `ADR-2026-11-24-completion-belongs-to-the-submission`.
- **The second window, applied to content (T60)**: the caches hold `shared_ptr` entries (`GeometryEntry`,
  `InstanceStream`, `PipelineHandle`, `ContentImages::Impl::Entry` and its stand-in), `FrameBuilds::borrow` (deduped by
  address) keeps every entry a frame head resolved until the frame ends, and every release path erases or REPLACES the
  table's share before it parks. Replacing rather than mutating is load-bearing: a table's own share inside a parked
  entry makes the `use_count` probe count it and never destroy anything (that is why `releaseStreams`,
  `ensureInstanceStream`'s revision change and `failGeometry` all build a new entry).
- **Traps**:
  * A frame whose pass walk finds nothing reports `NoRunnablePass` (Info) - while a diagnostic sink is holding a frame,
    that report waits on the sink's own `report_mutex`, which is why a test that holds a frame with a sink must run a
    frame that DRAWS something (or must not run a frame at all).
  * A park is dated against the SUBMISSION sequence: a frame that submits nothing (`frame.passes` empty) proves nothing,
    so only submitting frames drain a park.
  * A test that builds a `DrawPacket` on the driver thread while another thread releases the objects it names is a race
    in the TEST (TSan names `ShaderProgram::description()`); build the `FrameDescription` once, before the threads.
  * `retirementNumbers()` is a host verb (body gate EXCLUSIVE): it cannot be read while a frame is deliberately held.
- **Evidence**: `test_vk` **212/212**, `test_graphics` 644/644, TSan `test_vk` 212/212 with **0** warnings,
  ASan+LSan focused **PASS**, 11 gates green. Mutations: an extra driver freezing the watermark reddens
  `CoreDeviceClockTest.AnIdleDriverHoldsNothingBack` and the source-keeps drain; `borrow` as a no-op reddens
  `VkMultiSessionTest.AFrameKeepsTheContentItResolvedUntilTheFrameEnds` (the picture is black).

## 2026-11-25 - review 9's first batch: the read set, the state shadow, the device floor

- **A pass owns what it READS** (`K7`): `FrameDriver::resolveReadSet` resolves each full-screen packet's source record and
  each pass' shadow map AFTER the claim walk (so a record a later pass builds, or a depth a later pass lends, is still
  seen), and the pass slot (`FrameHead::Pass::sources` / `shadow_map` / `shadow`) keeps them. `FrameRecording`'s
  `read_records` list is gone - it was the same fact in a second place. `executeFrame`'s record loop takes NO target-map
  lock; the shadow's LAYOUT transition stays there (a command, not a resolution).
- **The state shadow survives a pipeline bind** (`K6c`): every pipeline comes from `buildPipeline` with the same eleven
  dynamic states, so a bind resets nothing - a second BUILDER with a narrower list must invalidate the shadow where it
  binds. The recorder's vertex-bind lists are its own members (`K6a`), so a bind is no longer a heap operation.
- **Device choice filters before it ranks** (`K3`): `DeviceChoice.hpp`'s `candidateRefusal(DeviceOffer)` is the floor as
  one testable predicate; `initialize` drops any candidate that misses it and ranks only survivors. The three refusal
  codes are published only when NOBODY serves, and they then describe the highest-ranked device the floor turned away.
- **Still open** (backlog): `T17` (a cold build under the device-wide cache lock, and the build-admission vocabulary),
  `T67` (streams are host-visible), `T68` (the variant rule's SDK owner), `T69` (the descriptor ceiling's trigger).
- **Evidence**: `test_vk` green after the batch (`ctest -R test_vk`, 18 s), with the new cases `VkDeviceChoiceTest.*`,
  `VkContentDrawTest.TwoAlternatingPipelinesRestateNoState` and
  `VkMultiSessionTest.AReleaseBetweenTheClaimAndTheRecordStillDrawsTheSampledSource`; gates: ai-docs strict,
  design-docs, cross-refs, include-hygiene, api-style, vk-exposure, diagnostic-formats, export-annotations.

## 2026-11-25 - the window-manager cases prove their own premise (backlog T47 retired)

- **What was wrong**: two `test_gui` cases read a window's position as if it were the code's answer. A window manager
  decides it: this session's (WSLg) refuses a programmatic top-level move and reverts `pos()` with a ConfigureNotify,
  so `DockGeometryTest.TitleBarStaysReachable` could never set up "dragged off screen", and the backstage drag compared
  `before + delta` against a window that never moved.
- **The trap worth remembering**: `QWidget::move()` writes `pos()` optimistically (the widget's own crect), so a value
  read immediately after the call proves nothing; only what is observed after the event round trip is the WM's answer.
- **Shape of the fix** (test-only, `tests/test_gui/test_gui.cpp`): `placeWindowExactly()` moves and watches 60 ms for
  the value to hold; `pumpUntil()` waits (bounded) for a settled geometry; a premise the machine cannot provide becomes
  `GTEST_SKIP` naming the window manager, never an `EXPECT`; the title-bar rect is built from `frameGeometry()` (the
  rect the rule clamps), and the overlay-follows assertion stays immediate because it is Qt state, not a WM answer.
- **Evidence**: ctest `test_gui` (offscreen, what the suite runs under) passes, 13.9 s; on this session's real display
  `TitleBarStaysReachable` skips in 63 ms (the WM refuses the off-screen placement) while the drag case passes in
  189 ms - both outcomes the backlog asked for, and each failure now names the window manager instead of the code.

## 2026-11-25 - the variant rule moves to the SDK (`ProgramVariant`, backlog T68)

- **Why**: the backend derived four bits (textured / vertex colour / cube texcoord / instanced) and fixed their
  numbers, while the INPUTS are SDK values (a material's slots, a geometry's channel widths, the draw's instance
  stream) and the OUTPUT names are the SDK's own stage text (`#pragma import_defines`). A second backend would have had
  to restate "three scalars is a cube" - the divergence `ADR-2026-11-23` D1 exists to prevent.
- **What**: `sdk/vine/graphics/ProgramVariant.hpp` + `src/ProgramVariant.cpp` now state the four switches, the bit
  constants (`kTexturedBit` 1 / `kVertexColorBit` 2 / `kCubeTexcoordBit` 4 / `kInstancedBit` 8, so `bits()` IS the
  number `PipelineKey::variant` carries), `defines()` (the names the shipped stages gate on, one texcoord kind always
  named), `describe()`, and `variantOf(geometry, material, instanced)` - the one place the bits are derived.
- **Boundary**: a backend that compiles the text takes `defines()`; one that ships precompiled stages maps `bits()`.
  Neither derives the bits again. The vk plugin deletes its own `ProgramVariant.{hpp,cpp}` and imports the SDK names,
  so its `internal/` no longer owns a rule - only the use.
- **Evidence**: `VkVariantTest` 7/7 device-free, ctest `test_vk` and `test_graphics` green, gates green.

## 2026-11-25 - a backend module states its SDK ABI (backlog T65 retired)

- **What was missing**: the backend interfaces are virtual classes with no explicit ABI, so a change to one is a silent
  break for a module that was already built. Measured 2026-11-23: a stale backend plugin registered and then segfaulted.
- **What landed**: `sdk/vine/graphics/BackendAbi.hpp` - `VN_RENDER_BACKEND_ABI_VERSION` (what bumps it, and what does not),
  `RenderBackendAbi{abi_version, sdk_version}` (standard layout, revision first), `isRenderBackendAbiCompatible`,
  `vineRenderBackendAbi()` + `VN_DECLARE_RENDER_BACKEND_ABI()` (one definition per module). `registerFactory` takes the
  handshake and refuses `AbiMissing` / `AbiMismatch` BEFORE it calls any virtual; the refusal is reported on the registry's
  own `diagnostics()` route (`engine.backend.abi_missing` / `engine.backend.abi_mismatch`, severity Error).
- **The trap**: the report cannot live on the factory (a virtual) or in `RenderBackendInfo` (a layout) - reading either is
  the crash the check exists to prevent, which is also why the refusal names the two revisions and not the backend. And the
  one-argument `registerFactory` overload must stay resolvable: under `RTLD_LAZY` a missing symbol aborts the process at the
  call site, so deleting it would put a crash back where a sentence now is.
- **The cost, measured**: a program that compiles a backend's sources into itself IS a module and must define the handshake -
  `tests/test_vk/BackendTest.cpp` needed one line (its link failed with `undefined reference to vineRenderBackendAbi`).
  Letting `Registrar` pass the header constant instead was rejected: that value can be interposed by another DSO's copy, and
  it would be a second answer to "what revision is this module?".
- **Evidence**: ctest `test_graphics` (648 cases, 4 of them new registry cases) and `test_vk` (219 cases) green; gates green.

## 2026-11-25 - T64 deferred; registry identity becomes a configuration option

- **The owner's decision**: do NOT build `ADR-2026-11-23` D5.3's "keep-first + report" now; registry identity becomes a
  configuration option the host states, and it is scheduled only when a host needs that choice.
- **Trap for a future session**: the ADR's D5 text still says "keep-first", because a record is not rewritten - the
  amendment appended under it supersedes the shape. Read the amendment before implementing anything from D5.3.
- **What still holds**: for a given configuration, "which registration owns a name" must follow the module identity
  (name + `VN_RENDER_BACKEND_ABI_VERSION`), never the load order; a policy value that means "last one loaded wins" is
  excluded. The route the policy will report on already exists: `RenderBackendRegistry::diagnostics()`.

## 2026-11-25 - the colour vocabulary is `channels x family` (backlog T59 retired)

- **What was missing**: `ColorFormat` had three four-channel values, so an SSAO/coverage mask, a luminance buffer, a
  velocity buffer or a depth-like slice had no target to live in - a post-process product that is not a colour could
  not be expressed at all.
- **What landed**: nine values (`R8` / `RG8`, `R16F` / `RG16F`, `R32F` / `RG32F` are new); the rule is `channels x family`
  (1/2/4 x 8-bit UNORM / 16-bit float / 32-bit float), stated once in the enum's own note. `colorReadbackOf` now states
  one byte per channel for the 8-bit family (1/2/4 bytes), so a mask reads back like a colour; the float family still
  answers `UnreadableFormat` (a byte count does not describe a float texel - `D10`).
- **What the device decides**: a colour format is a target only when `vkGetPhysicalDeviceFormatProperties` backs colour
  attachment + sampled + transfer-source for it (the exact usage mask `buildTarget` asks for). The mask is computed once
  at session open (`buildableColorFormats` -> `TargetTable::Limits`), so the build path is an AND and not a driver call
  per frame; a refused format reports `vk.target.color_format_unsupported` once per episode and names the value.
- **The trap**: a narrow attachment is a mask, not a picture with three empty channels - size a read with
  `colorReadbackOf(format).bytes_per_texel`, never a literal 4, or the probe walks a quarter of the image at the wrong
  stride (`graphics-rendering.md` `P4`).
- **Cost of a new value, measured**: the enum + `kColorFormats` (backend "every value" list) + `colorFormatOf` +
  `colorFormatName` + `colorReadbackOf` + the extension-point table in `graphics-architecture.md` 2.11 (now 5 places).
- **Evidence**: ctest `test_graphics` (649 cases) and `test_vk` (222 cases) green; the device case asserts that a value builds exactly when the
  device mask has its bit and reads the 8-bit family's centre/corner texels; gates green.

## 2026-11-25 - a readback has one vocabulary and one classifier (`BC-42`)

- **What changed**: `readColorBuffer` / `readDepthBuffer` -> `readColorImage` / `readDepthImage`; `colorReadbackOf` /
  `depthReadbackOf` -> `colorBytesPerTexel` / `depthBytesPerTexel` returning `std::optional<std::uint32_t>` (`nullopt` =
  no packing); `ReadbackFormat` deleted; the seam's second classification (`readbackOf` / `ReadbackState` /
  `ReadbackRequest` / `ReadbackRefusal` / `readbackStatusOf` / `detail::refusalName` / `ReadbackKind`) DELETED and `T70`
  closed by deletion; `ReadbackResult` gained a constructor, so no default answer exists.
- **Why**: the seam table's `*_captured` facts described a copy-back-node model neither backend has (the reference backend
  is retired) and only `BackendSeamTest` consumed it; the precedence it carried now lives on `ReadbackStatus` - a request
  that can never be served answers permanently first, a not-yet-built target answers `NotReady` before it is judged.
- **The defect it fixed**: the vk depth verb sized its copy from `RenderTarget::depthFormat()` (the host's CURRENT
  declaration) while the image had been built with the record's format - an `attachDepth` before the next frame produced a
  half-true picture (buffer over-sized, half the staging decoded as floats). Both verbs now take the format from the record
  and share one copy sequence (`Device::copyImageToHost`).
- **Rule kept**: `Unsupported` means "this device never will" and gates a feature off; a borrowed depth is `Invalid` (the
  request names an image the target does not have), and "no host-visible memory" is answered before the wait
  (`MemoryAllocator::supportsHostVisible`), so a refusal costs no device stop.
- **Witness**: `VkContentTargetTest.ADepthReadbackAnswersAboutTheImageThatExistsNotALaterDeclaration`.

## 2026-11-25 - review 10: the corpus, the concept inventory, the extension surface

- **What this round read** (and what it did NOT): not the code paths (6/7/9), not the seam with `vsg` leaving (8) -
  the three artefacts the module's discipline rests on: the 11 normative design pages (5744 lines), the public
  concept inventory (101 headers / 139 `VN_GRAPHICS_API` types), and the extension surface. Page:
  `.ai/research/graphics-2026-11-25-review-10.md`; **nothing was changed in the tree** (findings are decisions).
- **Measured baseline** (`python3 scripts/measure_graphics_claims.py`, the repo's own stick): `RenderEngine.hpp`
  1198 lines / 59 public signatures / 25 includers; 8 `detail::` mechanisms; `RenderDevice` 19 virtuals (5 pure),
  executor 4 (1 pure), factory 3 (2 pure); seam = 8 mechanisms in a 37-header shared surface; 107 diagnostic
  codes; 154 public `bool` answers; `test_graphics` 647/82, `test_vk` 220/46.
- **`G2` is clean, and it is measured, not read**: SDK → std/base only (0 leaks); 50 API-family words in the SDK
  with **0 dependencies** (35 prose, 15 `RenderApi` vocabulary); no plugin include of a private `src/**` header
  (structural: only `sdk/` is on the include root); appfw → graphics 17 one-way, graphics → appfw 0; graphics →
  other modules 44, all into `src/base`.
- **L1 - the extension table (2.11) rots and is the table `DrawKind.hpp:31-37` sends a developer to.** Its pass-kind
  row names `RenderPass.cpp`'s kind→node mapping, which moved to `SessionShape.cpp:338-349` in 2.8's split; "6-8
  places" is ~9 SDK files + 4 backend sites; four axes have no row (a `PassStage`, a `Topology`, a built-in
  `ShadingMode`, a content node type). Fix shape: state the DECISION SITE + a **grep** per row; a file list rots,
  a grep cannot.
- **L2 - numbers in normative prose have no witness, two are wrong**: `graphics-architecture.md:106-107` says "the
  shared surface is 35 headers" (gate list = 37); `backlog/graphics.md:17` says `test_vk` "186 cases / 45 suites"
  and "186/186" (today 220/46). Class = `K9` applied to counts; the measuring script exists, the numbers just do
  not name it.
- **L3 - `RenderStateOrigin` is computed and never read outside tests**: `StateResolution.hpp:71` (7 items),
  written at `StateResolution.cpp:96-132`; every `.origin` read is `tests/test_graphics/GraphicsTest.cpp:9601-11058`;
  the backend calls `resolveRenderState` (`ContentCaches.cpp:1148,1240`) and uses `.state` only. `G8` + `G10` -
  same class `BC-26`/`BC-27`/`BC-42` closed by deletion; deleting it is a seam ABI batch.
- **L4 - which sets are closed on purpose is stated in four different @notes and nowhere as a rule**: closed =
  `DrawKind`, `PassStage`, `Topology`, `ShadingMode`; open = `PassDesc` fields, host programs, `MaterialValues`
  subclasses, node types. Peers state the split (WebGPU's topology set, Filament's five shading models). 2.11's
  "touching the core model needs an ADR" must be restated so a closed member is not "the core model".
- **L5 - the 14 defaulted `RenderDevice` verbs have per-verb reasons but no rule**, and `releaseRenderTarget`'s
  default is a bare `(void)target;` - the one whose omission LEAKS. Rule to state: a default is legal only when its
  absence is a true statement about the device, and every such default says so.
- **L6/L7 - names**: `ImageSource.hpp` declares `ImageInput` (what a host writes) and is named after the resolved
  half; the surface mixes three header granularities with no stated rule or index.
- **Verdict**: the code rounds have converged; what is left is corpus upkeep and one public fact with no consumer.
  Steps 1-2 of §5 are doc-only, step 3 (`L3`) is the only code batch.

## 2026-11-25 - review 10 landed: the corpus, BC-43 and BC-44

- **What changed, in four commits**: `d59b7585` (2.11 rewritten as decision site + grep with four new axes, 2.17 the
  closed/open split, 2.18 a header is named after what a host writes, three stale counts deleted, the count rule in the
  design-docs skill), `4140cd15` (`BC-43`: `StateOrigin` / `RenderStateOrigin` / `origin` / `topology_origin` deleted,
  ABI `2u`), `7990b2d9` (the rule that decides when a device verb may carry a default, plus `releaseRenderTarget`'s
  sentence), `a8569946` (`BC-44`: `ImageSource.hpp` -> `ImageInput.hpp`).
- **The two refinements the batches made to the review's plan**: the `L2` regex gate was WITHDRAWN - the population is
  ~40 legitimate `SomeTest.cpp (N cases)` sites, and a check that must be switched off is worse than none; the rule is
  the writer's (skill) and the three stale counts were simply deleted. `L7` needed no file moves: 2.18 states the
  granularity rule and the guide's §5.1 table is the index.
- **Traps worth keeping**:
  - `multi_replace_string_in_file` again reported a FILE-level success while one entry silently failed (`2.11`'s
    replacement did not apply; only a `grep` showed the old table still there). Verify each entry, not the file.
  - `clang-format-diff-23 -p1` output must be applied with `git apply -p0` (git apply defaults to `-p1`, which strips
    `src/` and fails); generate it with `git diff -U0 --no-prefix` and apply with `-p0`.
  - The `<!-- drift-ok -->` marker is matched PER LINE: it has to sit on the line that names the deleted symbol, not on
    the paragraph's next line.
  - clang-23 SEGFAULTED once (SIGBUS in `ParseSwitchStatement`) on `GraphicsTest.cpp` under the parallel build; the
    identical source rebuilt clean on retry, so it is an environment/compiler fault - retry once and only then look for
    a code cause.
- **Evidence**: build 0 errors / 0 warnings; `test_graphics` 647 cases / `test_vk` 220 cases green after each code
  batch; 11 gates exit 0 (ai-docs strict included); `clang-format-diff` 0 hunks; tree clean.

## 2026-11-26 - review 11 A7: a pass cost belongs to the frame (ADR-2026-11-26)

- **The defect** (review-6 P2-1 restated): `record_micros` was stored on `SessionShape::Slot` and written in two phases by
  two threads - the compile added the describe share, `publishEngineMicros` added the engine share at the START OF THE
  NEXT frame. So after `waitForFrame(N)` a host saw N's half number, the last frame ever run never got one, a `prepare()`
  zeroed the slot it had just completed, and the two threading modes meant two different things.
- **The shape now**: `detail::PassCost { const void* pass; std::uint64_t micros; }`, one per slot on
  `detail::EngineFrame::pass_costs`; both halves add their share to the frame they own;
  `FramePump::publishAnswer(FrameResult, span<const PassCost>)` publishes it under the answer's lock;
  `SessionShape::frameTable(span<const PassCost>)` joins BY PASS IDENTITY (positional folding is a silently wrong number).
  Deleted: `Slot::record_micros`, `addEngineMicros`, `publishEngineMicros`, `note/takePendingMicros`, `engine_micros`, the
  `!threaded()` branch in `runEngineHalf`, and the compile's second walk over the slots.
- **Never put a per-frame measurement in a session cell** - the two-writer cell is the defect. Peers agree: Filament's
  `Renderer::getFrameInfoHistory()` returns `FrameInfo`s keyed by `frameId`, Godot publishes `get_frame_profile()` WITH
  `get_frame_profile_frame()`, timestamp readback rings key results by frame index.
- **Traps**: `MockBackend` answers `PrepareReport::Outcome::Unsupported` from `prepare()` (it keeps nothing compiled), so a
  prepare test must assert "not NotReady", not `Prepared`; a table read is a diagnostic path, so a copy + linear scan per
  read is the right cost.
- **Witnesses** (mutation-checked): `AFrameThreadedEnginesPassCostIsWholeOnceTheFrameIsAnswered` (uses `GatedBackend` to
  make "in flight" a fact), `PreparingAFrameDoesNotRewriteTheLastFramesNumbers`,
  `APassAnswersItsOwnCostNotTheOneThatLeftItsSlot`. Positional-join mutation kills only the third; an empty published cost
  span kills all four including the pre-existing `TheFrameTableCarriesWhatEachPassLastCost`.


## 2026-11-26 - review 12: the gate was half a frame, and the report had no answer (ADR-2026-11-26 x2)

- **The defect**: "a frame is being recorded" was `FramePump::busy()` = "the runner is running a job". That is blind to
  the CONTENT half (it ends before the job is submitted) and to every INLINE engine (no runner exists). So
  `applyPlan` / `removePassesOwnedBy` were applied while `SessionShape::compile` was walking `slots_` - which
  `applyPlanRows`/`removePass` insert into and erase from, dropping a pass the frame value still names. `prepare()`
  compiled with no check at all. The episode `ReportOnce` was a plain `bool`, so two host threads raced on it too.
- **The shape now**: `FramePump` owns the phase as two facts with one answer - `compiling` (opened/closed by
  `FramePump::Build`, an RAII guard from `beginFrame()`/`beginBuild()`) and `answering` (set by `submit`, cleared by
  `publishAnswer`) - and `recording()` is their OR. `RenderEngine::beginShapeChange(verb)` answers a
  `std::unique_lock` from `FramePump::phaseGate()` (not a bool), so the decision and the change are one critical section
  against the phase flip. The lock is never held for a compile, and the episode report is emitted AFTER releasing it (a
  sink may ask for a shape change itself). `ReportOnce` is atomic (`exchange`) + copyable (an episode is carried from one
  compile to the next).
- **Second defect, same family**: the vk driver's `FrameBooks` were read live by `frameReport()` - the struct the frame
  body increments - so a HUD read while a frame ran was a data race, and the only statement of the rule was a comment in
  `DemoScene`. Now `books` (recording thread) is published into `answered_books` under `books_mutex` by
  `closeFrameBooks`, and `frameReport()` copies that. Mutation measured: live read = **15 TSan reports**, published read
  = 0.
- **Smaller ones in the same batch**: `waitForFrame(n)` now answers false at once for `n > lastNumber()` (never built is
  not "pending"; `number_` became atomic for it); `publishAnswer` is ONE verb (present word + `FrameResult` + costs +
  completion), so polling cannot see one frame's picture beside another's numbers; `setFrameThreading` from the frame
  thread reports `engine.threading.refused_from_frame` instead of silently doing nothing; `FrameGate` asserts when a
  thread nests past `kMaxEntries` (that is a caller defect, not `vk.device.reentry`).
- **Witnesses**: `PlanTest.AFrameRefusesAPlanFromInsideItsOwnCompile` (a sink applies a plan from inside the compile -
  red under the old predicate), `APrepareRefusesAPlanFromInsideItsOwnCompile`,
  `APlanFromAnotherThreadIsRefusedWhileThisOneCompiles` (the pool-thread case),
  `FramePumpTest.ThePhaseCoversTheBuildAndTheAnswer` / `...StaysOpenUntilAFrameHandedToItsThreadIsAnswered` /
  `AFrameThePumpNeverHandedOutIsAnsweredWithoutWaitingForTheOneInFlight`,
  `VkConcurrentFrameTest.TheFrameReportIsTheFrameThatEndedWhileTheNextOneRuns`.
- **Trap**: `FrameBooks::counters` is NOT optional (only `uploads`/`timing`/`locking` are), so `report.counters` is
  engaged even before a frame ends - use `uploads`/`locking` as the "a frame has ended" witness, not `counters`.
- **Evidence**: build 0 errors; `test_core` / `test_graphics` / `test_vk` green; TSan 0 warnings on the new filters
  (`PlanTest.*:FramePumpTest.*:ReportOnceTest.*:FrameRunnerTest.*` and `VkConcurrentFrameTest.*`), 15 with the mutated
  live-books read.
- **Found by the same full-suite run, and fixed in its own commit**: the process default `ContentRegistry` was a
  function-local static, so it was destroyed at finalisation BEFORE the file-scope caches whose destructors release
  content handles (`AxesMarker`'s `s_prototypes`) - a SEGV in `release()` during `__cxa_finalize`. It is immortal now
  (`static ContentRegistry* const` constructed once), which is what its header always claimed. Postmortem:
  `.ai/bugs/graphics-default-content-registry-outlived-by-statics.md`. A FILTERED TSan run cannot see this: the exit
  path belongs to no case.

## 2026-11-26 - a diagnostic cannot cost a frame its answer; device verbs and lock order stated

- **Code (F-2)**: `Diagnostics::report` now swallows a throw out of the host's sink. Every report site sits inside a
  frame path, and an escaping exception used to skip the frame's answer - `waitForFrame(N)` then timed out on a frame
  that had actually run. The counters move before the sink is called, so the report is still counted; only the host's
  observation of its own bug is lost.
- **Witness**: `RenderEngineTest.AThrowingSinkStillLeavesTheFrameAnswered` (inline + threaded; `diagnosticCount() == 1`
  proves the count survives). **Mutation**: removing the swallow turns it red (the exception escapes the test body).
- **Design correction (F-1)**: `createSurfaceTarget` / `initialize` / `setWindowHandle` were described as the host's
  between-frame duty. Measured: only `initialize()` (the creator's, before any frame) and a window target's
  `setWindowHandle` are; `createSurfaceTarget` (`Device.cpp:3764`), `setRenderTarget`, `createFrameDriver` /
  `releaseFrameDriver`, the readbacks and `shutdown` take `body_gate.enterExclusive()` and WAIT, and
  `releaseRenderTarget` / `releaseContent` park instead of waiting. `graphics-threading.md` 2.6 / 3.1.3 F9 / 6.3 P3
  now say exactly that.
- **Also stated where the verb lives**: `areaWidth`/`areaHeight` are content-thread (the engine half reads the frame's
  own size); `setDefaultContentProgram` is the one host verb that is a value write under `program_mutex`.
- **The device's lock order is written down** (2.16): the only nesting is `queue_submit` -> `clock_mutex`;
  `TargetTable`'s `map_mutex_` / `claims_mutex_` are never held together; `report_mutex` is recursive, `reentry_mutex`
  a leaf; the cache lock is taken only at the frame head. A new nesting states itself in that list first.
- **`RenderDevice.hpp`**: the sink promise is scoped to the device's OWN reports; the shared route may be entered by
  two threads, which is what `Diagnostics` already documented (the two sentences disagreed before).
- Counts: `test_graphics` 660/660 (84 suites), `test_vk` 223/223 (46 suites); TSan 0 warnings on both; 11 doc/style
  gates + `check_ai_docs.py --strict` green. No ASan run: no ownership or allocation path changed.

## 2026-11-26 - review 14: the frame vocabulary names its question, not a metaphor

Audit first, then two renames and one doc correction. Each name was judged against what the industry calls the same
thing; changes are only where the second word was a metaphor or named another concept.

- **`FrameHead::Call` -> `FrameHead::Packet`, `Pass::draws` -> `Pass::packets`.** The head held ONE DRAW PACKET under
  the word the vocabulary retired on 2026-11-20 (`DrawCall` -> `DrawPacket`, `PassDraw::draws` -> `packets`), so one
  fact had two spellings in two layers and the vk one said the opposite (a packet records as N draws - the number
  `FrameCounters::draws` counts). The resolved vector stays index-parallel to `draw->packets`, which is the contract
  (one plan per packet). `beginPass(draw_count)` -> `packet_count`.
- **`detail::FrameScheduler` -> `detail::PassScheduler`** (with `PassSchedule`, the files, the forward declarations and
  the suite `PassScheduleTest`). It sorts the frame's PASSES; Filament's `FrameScheduler` is frame pacing (begin/end,
  vsync, refresh rate), so the old name taught the other concept. Stated in the class note, `graphics-architecture.md`
  2.5 and a new glossary row.
- **`FrameHead`'s name note rewritten** - it still claimed "the seam's vocabulary", but the reference backend retired
  2026-11-23 and this header is `backend_vk/internal/`. It now says why no borrowed word exists (Filament
  `CommandStream`, UE's cached `FMeshDrawCommand`, Frostbite's compiled frame graph: three peers, three nouns) and that
  the name is the MOMENT that produces the value - which is what makes "the head resolves, the recording reads" true.
  The name stays: 24 files / 77 hits, and that phrase is load-bearing.
- Deliberately kept: `FramePump` (a software pump is a driving loop), `FrameRunner`, `DrawPlan` (the `resolve` verb chain
  is load-bearing, and the public `Plan` is another layer), `FrameBooks` / `SurfaceLedger` (two accounting metaphors,
  private and documented - the cheaper fix is tying `FrameBooks` to `FrameReport`, deferred).
- Verified: build 0 errors (`test_graphics`, `test_vk`, `gfx_backend_vk`); `test_graphics` 660/660 (84 suites),
  `test_vk` 223/223 (46); 12 doc/style gates + `--strict` exit 0; `clang-format-diff-23` clean. No TSan/ASan run: no
  ownership, allocation, locking or ABI change (identifiers and comments only).

## 2026-11-26 - review 15: entering the frame interval is a value, not a race

The audit question was "does `frame()` need a mutex". It does not - it needed the same refusal the device already makes,
stated once for both layers.

- **What was wrong**: `RenderEngine::frame()` / `prepare()` documented "one host thread drives the frames" and never
  checked it. `FramePump::beginFrame` rotated `slot_` / `current_` (plain members) after `waitIdle()`, so two host
  threads raced two frames into one slot - UB, not a refusal - while the device side already refused the re-entrant case
  (`vk.session.frame_reentry`, `vk.device.reentry`) and made the second thread wait.
- **Why NOT a mutex**: the race is not in `beginFrame` but in the content half, which writes the single-writer world
  (`Scene::publish`, the reused `PassDraw` storage). A lock there would have to span the whole frame (serialising the
  callers - i.e. a frame queue) and would still advertise an ability that is not there. `D12` already rejected the
  queue, and no peer locks its frame entry: WebGPU objects are thread-affine (a validation error off the owning
  thread), Unity throws, Godot guards the thread, D3D12's debug layer reports concurrent recording, Filament documents
  "not thread-safe"; the engines that DO take several producers (UE, Godot) take them through a command queue.
- **What landed**: `FramePump::claimInterval()` (a compare-exchange of `compiling_`, under `phase_mutex_`), and
  `FramePump::Build` became the engine's LEASE on the interval - empty with a reason, exactly like a `FrameGate` lease.
  `Build::Refusal::Reentrant` (this thread holds it) and `Concurrent` (another host thread does) map to
  `EngineNotReadyReason::{Reentrant,Concurrent}` in `frame()` / `prepare()`; `slot_` is rotated only by the winner. A
  thread-local `s_building_` (previous value saved and restored) answers "is the holder ME?", so a hook driving another
  engine does not mislabel the outer build - the `FrameGate` idiom without a hidden lock or an
  `atomic<std::thread::id>` (whose generic form may spin).
- **Contract change (public)**: `EngineNotReadyReason` gained `Concurrent` (fixed `uint8_t`, no ABI break, nothing
  switches on it). The frame clock is read AFTER the claim now, so a refused call takes no reading.
- Design point: `graphics-threading.md` **2.17** (the entering-vs-waiting table) + `T38`; `RenderDevice.hpp` and the two
  engine headers point at it instead of restating policies.
- **Verified**: build 0 errors; `test_graphics` 661/661 (84 suites), `test_vk` 223/223 (46); TSan **0 warnings** on both
  suites; 12 gates + `--strict` exit 0; clang-format clean. Mutation: `claimInterval` forced to answer `true` -
  `RenderEngineTest.AFrameFromASecondHostThreadIsRefusedInsteadOfRacingTheCompile` fails ("a refused call takes no frame
  number"), restored and green.

## 2026-11-26 - review 16: every entry that changes the interval takes it, and the wait outlives the mode

The audit question was "what else may a second host thread call". The answer was "everything except the entries" -
and one verb was neither: a thread-mode change wrote `runner_` beside a frame's reads of it.

- **`FramePump::setThreaded` is an interval verb now** (`takeInterval`, answering `Build::Refusal`), because it changes
  the fact a frame asks before it builds and switching back joins the thread an answer runs on. `frame()` and `prepare()`
  lost their own `onEngineThread()` pre-checks: the pump answers `Reentrant` for the engine's own thread (after claiming,
  then releasing - a claim won on the engine thread is the same self-wait one `waitIdle` later) and `Concurrent` for
  another host thread, which removed one TOCTOU read of `runner_` per frame.
- **`runner_` is `std::atomic<std::shared_ptr<FrameRunner>>`**, and `FrameRunner` gained `live_` (an atomic set false
  after the join). Reason: `waitForFrame` is documented as callable from any thread, and both the pointer and
  `thread_.joinable()` were read by a waiter while a mode change could free or join underneath it. A waiter now holds the
  object by shared ownership and asks `live_`, so a switch ends on a runner a waiter still owns instead of a dangling one.
- **Two codes, two causes** (`G4`): `engine.threading.refused_from_frame` (this thread holds it) and the new
  `engine.threading.refused_in_flight` (another host thread is building), both once per episode.
- Design: `graphics-threading.md` **2.17** gained the mode-change row, the "the turn is ordered, not merely tolerated"
  sentence and the `runner_`-publication paragraph; **T39** (driving in turn / concurrent prepare / two waiters) and
  **T40** (mode change as an interval verb, exceptional exit, two pumps on one thread, nested engines) are new.
- Eleven cases at the pump and the engine, plus one for the deliberate asymmetry (a switch REFUSES a build and WAITS
  for an answer already in flight): `FramePumpTest.{TheIntervalAnswersWhyASecondEntryWasRefused, ABuildThatLeavesByAn
  ExceptionLeavesTheIntervalFree, TwoHostThreadsDriveThePumpInTurnAndNumberEveryFrameOnce, OneThreadMayHoldTwoPumpsAnd
  EachKeepsItsOwnInterval, AThreadModeChangeIsAnIntervalVerb}` and `RenderEngineTest.{APrepareFromInsideAPreparesCompile
  IsRefusedInsteadOfWaitingForItself, TwoHostThreadsPreparingAtOnceLeaveOneAnswered, ASecondHostThreadDrivesTheNextFrame
  OnceTheIntervalIsFree, TwoThreadsWaitingForOneFrameAreBothAnswered, ASinkDrivingAnotherEngineLeavesTheFirstOneWhole,
  ASecondThreadCannotChangeTheFrameThreadWhileAFrameIsInFlight, SwitchingBackToInlineWaitsForAnAnswerInsteadOfRefusingIt}`.
- **Verified**: build 0 errors (three targets); `test_graphics` 673/673 (84 suites), `test_vk` 223/223 (46); TSan **0
  warnings** on both suites (309 third-party suppressions on vk: 306 `libvulkan_lvp.so`, 3 validation layer); 12 gates +
  `--strict` exit 0; clang-format clean. Mutation: `setThreaded` forced to skip `takeInterval` - both new mode-change
  cases fail ("the mode did not change under the frame"), restored and green. ASan on `test_graphics` is NOT runnable in
  this tree (pre-existing ODR violation at load: the target compiles `SnapshotSource.cpp` itself while linking
  `libvnGraphics`), so TSan carried the concurrency evidence.

## 2026-11-26 (later) - review 17: the pick sees the picture, and the display order is stated

- **W1 - an orthographic pick starts at the NEAR plane.** `CameraSnapshot::screenRay` unprojected the pixel at clip
  depths 1.0 and 0.0, believing the snapshot carried a reverse-Z projection; it carries the SDK's GL-range one, so a
  perspective camera was right by accident and an orthographic ray began in the MIDDLE of the depth range - everything
  nearer, a model fitted at its own distance included, sat behind its own origin. `screenRay` now unprojects at the
  projection's own ends (-1 and +1); the perspective answer is unchanged. Case:
  `CameraSnapshotTest.AnOrthographicPicturePicksWhatItShows` (a real triangle through the snapshot's ray).
- **W2 - the transparency class opens the blend.** `resolveRenderState` / `applyStateDefaults` take a `transparent`
  input (= `DrawItem::isTransparent`, the sort's own question); a command in the transparent queue that authored no
  blend resolves to the default pair enabled, and an authored blend (even a disable) keeps its word. Cases:
  `StateNodeTest.TheTransparencyClassOpensTheDefaultBlendAndAnAuthoredBlendWins` and the vk pixel case
  `VkContentDrawTest.ABlendedDrawFollowsTheEngineOrder`, which no longer authors a blend itself.
- **W3 - face orientation.** `vnFacingNormal` (vine_material.glsl) turns the interpolated normal on a back face and
  both shading stages call it; glTF's `doubleSided` reaches `models::Material` (`hasDoubleSided` / `isDoubleSided`, read
  from assimp's `AI_MATKEY_TWOSIDED`) and `sceneFromModel` wraps the leaf in a `StateNode` with `CullMode::None` /
  `Back`. "Stated nothing" is not "false": the engine default stays `None`, now a sentence in `graphics-scene.md` 2.13,
  and the mirrored-winding limit is recorded there and in 9.2. The vk shadow fixture had been lighting the BACKS of its
  quads (its camera saw the +z side of a +z-facing surface); the rule folded that away, so the quads' winding and
  normals and the light now face the camera together. `AStateChangeIsACommandNotAPipeline` drew its "blend off" frame
  at opacity 0.5, which now blends by itself - it authors `enabled = false` at full opacity.
- **W4 - the pick and the picture.** A pointer names a pixel, so `makeScreenRay` normalises `(index + 0.5)`; a hidden
  scene and a subtree that folds to opacity 0 are not picked. What a pick deliberately does NOT read (clip planes, cull
  mode, a topology override, a billboard's per-view rotation, lines/points with no tolerance) is stated in
  `RayIntersection.hpp` and `graphics-scene.md` F28.
- **W5 - the host-program contract.** `VineViewBlock` (288 B, uploaded by nobody) is deleted; `VinePushRange` (128 B:
  folded projection + model-view) is the whole contract and lives in `ShaderAbi.hpp`, and the vk backend's
  `PushRangeConstants` is a `using` of it. `ADR-2026-11-26-host-program-contract-is-the-push-range`; the dead
  `buildViewBlock` references now name `ContentDraw::foldToDeviceClip`.
- **W6 - the display pipeline.** Linear light becomes an encoded pixel at the swap-chain write and nowhere else; an
  offscreen 8-bit target is a DATA format (a lit picture that must survive an offscreen hop is RGBA16F); no tonemap, no
  exposure; a readback answers the stored bytes. `ADR-2026-11-26-linear-light-is-encoded-by-the-target-format`;
  `graphics-rendering.md` 2.14 / F28 and the guide's 3.9 state it.
- **W7 / W8 / W9 - the boundaries.** `graphics-geometry.md` 2.11 states the three closed geometry facts (uint32 indices
  only, no primitive restart, no per-instance attribute beside the matrix) with a trigger each. `PassDesc::clear_depth`
  says a clear is a load operation on the WHOLE attachment, not the pass' viewport. Opacity is clamped at both setters
  (`Node` / `Scene`) and a subtree that folds to 0 emits no command and answers no ray.
- **`test_graphics` did not link at HEAD**: `FramePump`'s private TU was missing from the target's source list. It is
  compiled into the binary now, following the rule that file already states for the private half.
- **Verified**: build 0 errors; `test_graphics` 677/677 (84 suites), `test_vk` 223/223 (46), `test_meshio`,
  `test_gui` green; the 12 gates + `--strict` exit 0 (no clang-format binary on this Windows host).

## 2026-11-26 (third) - review 17's follow-up: the front face was inverted all along

The default demo lost its lighting and shadows after W3, and the cause was not the normal rule: it was
`StateMapping::kFrontFace = CLOCKWISE`.

- **What was wrong.** The SDK's front face is the picture's counter-clockwise polygon. The vk backend's
  constant said `CLOCKWISE`, on the reasoning that the projection's Y fold turns CCW into CW. A W3-driven
  pixel probe (`kFacingFragment`: red for `gl_FrontFacing`, green for a back face) measured the opposite:
  the SDK's front triangle reported GREEN and a mirrored one RED. So every correctly wound surface was a
  BACK face to the shading - W3 then turned its normal away from the light, which is why the demo's ground
  and boxes went ambient-only and the shadow term had nothing to darken - and `CullMode::Back` culled the
  faces it was told to keep.
- **The fix.** `kFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE`, with the reasoning replaced by the
  measurement. `VkContentDrawTest.TheFragmentStageSeesTheSdksFrontFace` now pins four facts in pixels:
  the SDK front reads front, a mirrored placement reads back, `CullMode::Back` keeps the first and drops
  the second, `CullMode::Front` the other way. `VkStateMappingTest.TheFrontFaceStatesTheSdksCounterClockwiseRule`
  pins the constant itself; the case that asserted the old value is gone.
- **What it caught in the tests.** The shadow fixture had been "repaired" by me an hour earlier - its quads
  re-wound and its light flipped - to make the then-green suite pass under the inverted constant. Reverting
  that (`makeQuad` faces +z, light travels -z) is what the fixture's own comment always said, and it is
  front-facing under the measured rule. Lesson recorded: when a fixture has to be re-wound to keep a rule
  test green, the rule is the suspect.
- **Demo verified by picture, not by argument**: `Vine.exe --open demo`, window captured before and after
  (baseline: pre-W3 shaders; fixed: W3 + the constant). Lit faces and the ground's shading are back.
- **Verified**: `test_vk` 230/230 (46 suites) - the contract case plus the restored shadow fixture;
  `test_graphics` 677/677; `test_meshio`, `test_gui` green; the 12 gates + `--strict` exit 0.
## 2026-11-27 - navigation redesign: the viewpoint and the picture have separate owners

- **The error.** The orthographic picture's scale was `distance * tan(fieldOfView / 2)`; `apply()` rewrote the window on
  every gesture (so an authored off-centre window was erased by the first one); `pan` divided by a field of view an
  orthographic camera does not have; and the wheel and `dolly()` were two different models of "zoom" - one flew the eye
  through the scene and moved the orbit centre with it, the other shrank a radius - of which only `dolly` read
  `setDollyToCursor`. `Mode::Pan` / `Mode::Zoom` were compared nowhere, and a first-person drag called `orbit()`.
- **The model now.** The manipulator owns the viewpoint and `apply()` writes `lookAt` and nothing else; the CAMERA owns
  the picture, so `Camera::orthographicHeight()` is the parallel picture's scale and `clampEyeOutsideBounds` is clipping
  only. The wheel is `dolly(factor)`: perspective multiplies the eye-to-centre distance and stops at the picked surface,
  orthographic scales the window and does not move the eye. Travel is `moveForward` / `moveRight` / `moveUp` (WASD), and
  `Mode` is `{Orbit, FirstPerson}`.
- **Landing oracle.** `.ai/design/graphics-navigation.md` (10 chapters) and `CameraManipulatorTest` (18 cases); the
  load-bearing ones are `TheWheelIsOneFactorInBothProjections`,
  `WheelKeepsThePickedPointWhereItWasUnlessDollyToCursorIsOff` (drop the lateral slide and it reports 0.2894 vs 0.3328),
  `AnAuthoredOffCentreOrthographicWindowSurvivesGestures`, `AnOrthographicPanMovesThePictureAtTheWindowsOwnScale` and
  `AFirstPersonLookTurnsAboutTheEye`.
- **Trap for the next session.** `test_vsg` no longer exists in `build/` (the reference backend was retired on
  2026-11-23). `build/bin/test_vsg` is a stale binary that fails with an undefined `RetirementQueue::retire` symbol -
  that is not a regression from this batch.
- **Verified**: whole tree builds; `test_graphics` 684/684, `test_vk` 224/224; the doc/API gates plus `--strict` exit 0.
## 2026-11-27 - Y1 fixed at the raster, not in the shading: the front face follows the draw's placement

- **The error.** `vnFacingNormal` turns a BACK face's normal toward the viewer, which is only right while "front" means
  the AUTHORED front. A mirror (a negative scale) reverses the winding and leaves the authored normal pointing outward,
  so after `W3` every visible face of a mirrored subtree was shaded as the solid's FAR side - with the engine's default
  `CullMode::None`, i.e. on the shipped path. §9.2's recorded "costs culling only" trigger never fired.
- **Why not the review's three routes.** A draw-block flag + flat varying + per-vertex `determinant(mat3(model))` pays per
  vertex and still cannot serve an instance matrix; a dynamic front face "per instance" is not a thing; refusing a
  negative scale loses a legal placement. The shading question ("which side of the surface is this") and the raster's
  answer ("which winding is front") are ONE fact, so it is fixed where the winding is DECIDED.
- **The model now.** `DrawItem::reversesWinding()` (derived, never stored: affine determinant < 0) + `StateMapping::frontFace(bool)`;
  the backend states `VK_DYNAMIC_STATE_FRONT_FACE` per draw when the value changes (12th dynamic item, core Vulkan 1.3 -
  no new extension, no pipeline variant, no shader/varying/ABI byte). `CullMode` and `gl_FrontFacing` then both mean the
  AUTHORED side, so review 11 `V3`'s culling half is fixed by the same fact.
- **Landing oracle.** `DrawItemTest.AMirroredPlacementReversesTheAuthoredWinding` (device-free),
  `VkContentDrawTest.TheFragmentStageSeesTheSdksFrontFace` (front + authored back x both cull directions x mirrored/not) and
  `VkContentDrawTest.AMirroredPlacementIsLitAsItsUnmirroredTwin` (a symmetric quad, `scale(-1,1,1)`, same centre pixel;
  the normals-away twin is the unlit control). Mutation: forcing `false` at the call site fails exactly the mirrored
  assertions and the lit-vs-mirrored equality.
- **Remaining limit (§9.2).** A MIRRORED INSTANCE inside an unmirrored draw (one draw states one front face); trigger = the
  first host that needs it, fix = a split per handedness in the collection.
- **Trap.** The old pixel expectation "a mirrored placement is a BACK face" WAS the bug written down; if a fixture has to
  be re-wound or a front-face sign flipped to keep a rule case green, the rule is the suspect.
## 2026-11-27 - Y7: one coverage rule, and a depth pass records what occludes

- **The two halves of Y7.** (a) The depth stage declared no output and READ NOTHING, so a `Mask` material's cut-out never
  reached a shadow map: a leaf cast the shadow of its quad. That was a FRAMEWORK defect - the material block and the base
  slot were already bound in the same content set, and the threshold was spelled twice (forward, G-buffer) and missing
  from the third writer. (b) "Does blended content cast?" was a design hole with no statement anywhere.
- **The model now.** `shaders/vine_alpha.glsl` owns the coverage: the base colour slot's declaration (moved out of
  `vine_material.glsl`, which now includes it) plus `vnAlphaMode()` / `vnAlphaFactor()` / `vnAlphaCoverage(surface_alpha,
  opacity)` and `vnSurfaceCoverage(opacity, uv|direction)` for a stage with no surface. Three callers, one threshold:
  forward (`vn_alpha = vnAlphaCoverage(...)`), G-buffer (Blend discards, Mask cuts) and `builtin_depth.frag`. The opacity
  is an ARGUMENT, not a hidden `vn_draw` read (the deferred path has no draw block).
- **The policy.** `PassSelection { All, Opaque }` on `PassDesc` / `PassDraw` / `PassRow`; the shadow pass states
  `Opaque`, and the vk backend applies it in `planCommand` (empty plan, NO refusal) so the collected list stays shared and
  immutable - the same trade `program_override` documents. `Mask` casts (opaque to the sort); `Blend` and opacity < 1 do not.
- **Landing oracle.** `VkShadowTest.AMaskedCastersSilhouetteIsTheShapeTheMapHolds` (reads the map: covered half 0.5,
  cut-away half 0.0, plus an Opaque control that writes both halves) and `VkShadowTest.ADepthPassRecordsOnlyWhatCanOcclude`
  (same blended caster, selection `Opaque` vs `All`). Device-free: `PassChainTest.ARequestedShadowIsBuiltOnTheDeferredPath`
  reads `PassRow::selection` off the frame table; `ShaderAbiTest.TheAlphaModeDecidesWhatTheMaterialsAlphaMeans` now counts
  thresholds per stage (exactly one) instead of pinning a copy in each.
- **Trap.** A D24 shadow map CANNOT be read back (`ReadbackStatus::Unsupported` - combined depth/stencil), so a witness
  that judges the map by its texels must state `D32F`/`D16`. And the Mask witness only proves the variant plumbing because
  it FAILS when the depth stage compiles without `VN_TEXTURED` (the factor alone keeps the whole quad).
- **Verified.** Whole tree builds; `test_graphics` 685/685, `test_vk` 227/227; `vn_shader_check.sh` PASS.

## 2026-11-27 - Y9: the sky is a background, and its depth contract is a pair

- **The finding.** The sky had no depth contract: it was a world-sized direction box the HOST had to keep the camera
  inside of and size against the far plane (`DemoScene`'s `kSkyRadius = 400` beside a 1000 far plane), its render state
  (`LessEqual` + no write + `CullMode::Front`) was only accidentally true because the demo put it in the transparent
  scene, and "the colour of a ray that hit nothing" had two owners (the host's box, and `builtin_deferred_lighting`'s
  hard-coded `0.06` grey).
- **The model now.** `builtin_skybox.vert` removes the camera's TRANSLATION from `view * model` (the box follows any
  eye, no per-frame work, radius irrelevant) and writes `kFarClipZ` (`ShaderAbi.hpp`, 0 = the cleared depth under
  reverse-Z) as clip z while keeping the fragment's `w`; it also scales the box by the window's own extent for an
  ORTHOGRAPHIC projection (`1 / projection[0][0]`, `1 / projection[1][1]`). The state that goes with it is
  `skyboxRenderState()`: `DepthState{test, !write, LessEqual}` + `CullMode::Front` - the cull is a CORRECTNESS rule, not
  an optimisation: both shells sit at the far plane and an ortho view has no `w` to clip the near one, so keeping it
  samples the antipodal hemisphere.
- **The composition.** `ChainDesc::background` (`std::optional<BackgroundOptions{map, world_to_map}>`) makes the chain
  build a scene of its own (unit direction box + material + `StateNode` root carrying the pair) and a pass of its own:
  name `skybox`, stage `Transparent`, `order = passStageOrder(Transparent) - 1`, `clear_enabled = false`,
  `TestOnly`, and on the Deferred path `target = composite` + `depth_view = gbuffer`. A declared background therefore
  FORCES the composite on the deferred path (the same rule transparent content follows). A null map builds the row
  DISABLED; `PassChain::setBackgroundMap` sets the material's texture and the enabled flag together, so a streamed
  environment costs one call and re-registers nothing.
- **Landing oracle.** `VkBackgroundTest.TheBackgroundFollowsTheCameraAndHoldsTheFarPlane` (a quad 50 units away beats
  the unit box, and the frame is the map with the camera 5 units from the box's origin - the pre-Y9 shader fails both),
  `VkBackgroundTest.AnOrthographicViewKeepsTheShellItsRaysLeaveThrough` (identity camera: never the antipodal face),
  `PassChainTest.ABackgroundIsAChainOwnedPassAboveTheShading` (own scene, root state == `skyboxRenderState()`, row
  order in the frame table, published by the chain) and
  `PassChainTest.ABackgroundMakesTheDeferredChainCompositeAndInstallsItsMap` (composite, borrowed depth, live map
  switch, refusal without a declaration).
- **Trap.** `PassRenderState::depth` is a POLICY for content that authored none, so the sky's depth must be on the
  CONTENT (`StateNode` root) - a pass-level `TestOnly` alone would let the pass' write flag reach the far plane and
  stamp it over the whole rectangle. And a rotation for `world_to_map`, never an axis swap (det -1 mirrors the sky).
- **Verified.** Whole tree builds; `test_graphics` 687/687, `test_vk` 229/229; `vn_shader_check.sh` PASS; the demo now
  declares `ChainDesc::background` and installs its map from `installContent`.

## 2026-11-27 - Y11 + Y12: the sentences the landings left behind, and a spelling that is two states

- **Y11 (doc only, seven sites; the eighth was already gone).** `Camera.hpp` / `Camera.cpp` said "the view block"
  builds the projection - that block was deleted by `ADR-2026-11-26-host-program-contract-is-the-push-range`, so
  both now name the push range (`VinePushRange`). `BuiltinShaders.cpp`'s shadow term said the light block's clip z
  is "0 at the near plane to 1 at the far one"; it is the SDK's backend-neutral range (-1 / +1) and the LINES BELOW
  it are what fold it into the map's reverse-Z. The same file's `forwardFragmentSource()` note described review-9
  bindings (map 0 / 3, block 4, lights 5); the content set is material 0, texture slots 1-5, lights 6, shadow pair
  7 / 8. `builtin_forward.vert` cited `SceneBridgePipeline` twice (a retired backend file) - now `ProgramVariant`
  for the texcoord kinds and `ShaderAbi.hpp`'s `kFarClipZ` for the depth, and it says the pushed projection is
  the already-folded one. The first attempt also named `ContentDraw::foldToDeviceClip` and `check_vk_exposure.py`
  refused it (`C4`: a public shader names no unit a plugin keeps in `internal/`), so the fold stays the backend's
  and the stage only reads the convention. `SortKey::depth` said "near = larger"; `makeSortKey`
  computes `-(view * point).z`, so it grows away from the eye. `Light::direction()` said "only meaningful for
  directional lights"; spot lights have one and the packer zeroes a point light's. `CameraManipulator::onResize`
  said a manipulator should refresh "typically the camera projection aspect ratio"; it deliberately does not - the
  camera owns the shape and `OrbitCameraManipulator` keeps only the pixel extent (plus the ortho window).
- **Y12 (naming, doc only, no ABI).** The review called `PositionAttitudeTransform::attitude()` and
  `AutoTransform::rotation()` "one property, two names". They are not: an attitude IS the node's whole orientation
  (`transform()` → `Isometry3d::rotation`), while a billboard's rotation is the SPIN applied after the automatic
  orientation, so the two name one quantity only under `NoRotation`. Both headers now say that and cross-reference
  (§2.14); the rejected rename is `D6` - what would remove the difference is the CLASS name, and that moves both
  names together. No accessor changed, so no host breaks.
- **Verified.** Whole tree builds; `test_graphics` 687/687, `test_vk` 229/229; `vn_shader_check.sh` PASS (the
  forward stage's comment text changed, the embedded copy is regenerated); all 12 doc gates green.

## 2026-11-27 - Y6 settled: one world, one up, and the engine turns nothing

- **The decision (owner's call, not a gap any more).** The world is right-handed and Z-up, and the engine performs NO
  axis conversion and takes NO up-axis parameter: `setWorldUp` is rejected (`graphics-navigation.md` §7 `D6`), and so
  is an automatic conversion at the import boundary (`D5`). A Y-up source (glTF states +Y) is brought into the world by
  the HOST, as one placed `MatrixTransform` over the imported subtree - +90 deg about X for a +Y source (`y -> z`).
- **Why (the argument to keep).** A conversion in the engine rotates a host's content behind its back, so the numbers
  the host wrote (a camera, a light direction, a pick's coordinates) live in a different frame from the tree; and a
  per-host up axis has to reach EVERY default beside it (the sun's direction, the billboard's axis, the shadow
  camera's reference axis) or the world is half-converted: a Z horizon with a Y-up sun. A visible placement node is
  testable, reversible, and keeps picking/bounds answering in world coordinates (`G1` `G7` `G13`).
- **Where it is said / marked.** `graphics-navigation.md` §2.9 (the convention + this boundary), `docs/guides/graphics.md`
  (the 世界约定 line), and the defaults a reader actually meets: `Camera`'s private `eye_` now says the defaults are a
  Z-up TOP VIEW, not a Y-up one - review 11 read that exact line as "implies Y-up", which is the misreading to prevent.
- **What is still missing (one thing, trigger-gated).** No importer reports a source's stated up axis: a Y-up asset
  arrives lying on its back with no diagnostic. Trigger: the first glTF (or other axis-stating) asset in a Z-up scene;
  the answer is a report, never a rotation.
- **No ADR.** §2.9 + the two `D#` entries carry the argument and the rejected options, which is where the next reader
  looks; a dated ADR would restate them (design-docs skill: ADR only for a decision likely to be re-litigated).

## 2026-11-27 - Z9: the test binary is a second image of the module's objects (ASan now runs on test_graphics)

- **The defect.** `test_graphics` linked `vn::Graphics` AND compiled twelve of its private translation units into the
  same binary (the comment said the reason was Windows-only; nothing guarded it). Two definitions of every private
  class in one process: `ERROR: AddressSanitizer: odr-violation ... 'typeinfo name for
  vn::graphics::detail::SnapshotSource'` - one copy in `bin/test_graphics`, one in `libvnGraphicsd.so` - and the
  process aborts at LOAD, before a case runs. This retires the earlier note that "ASan on `test_graphics` is NOT
  runnable in this tree, so TSan carried the concurrency evidence" (2026-11-26): ASan runs now.
- **The rule, not the list.** `vn_add_library(<var> <ShortName> OBJECT_TARGET <objects_var>)` compiles the module's
  sources once into an object set named `<ShortName>Objects` and makes the shared library a shell that links it
  PRIVATE; `<objects_var>` receives the name. `tests/test_graphics` links `GraphicsObjects` (no `vn::` - that namespace
  is the installed package) and names no file: the module's own test
  is INSIDE the module, so one definition per process holds on every platform. `ADR-2026-11-27-a-module-test-links-the-
  modules-objects`; `graphics-architecture.md` 2.19 / C6 / D9.
- **Trap (measured).** An object set that feeds a shared object must be PIC:
  `relocation R_X86_64_TPOFF32 against ... FramePump::s_building_ ... local-exec is incompatible with -shared`. The
  helper now sets `POSITION_INDEPENDENT_CODE ON` on the object target (a shared library would have implied it).
- **Trap (cache).** The module's compile-time configuration must reach the OBJECT target, not only the shell: the object
  set is what compiles. `src/viz/graphics/CMakeLists.txt` sets `VN_GRAPHICS_VERSION` and the link libraries in one
  `foreach` over both images, and wires `vn_use_embedded_shaders` to the object set. `vn_add_library` now checks it at
  the end of the module's directory (a setting only the shell carries stops the configure), so the rule no longer
  depends on a reader remembering the loop.
- **Evidence.** `test_graphics` 687/687 in `build/`; `ldd build/bin/test_graphics` names no graphics library;
  `VN_ASAN_TARGET=test_graphics VN_ASAN_FILTER='*' scripts/asan_check.sh` → `RESULT: PASS` (687 cases);
  `scripts/tsan_check.sh` → 687/687, 0 warnings; `ctest -R 'test_graphics|test_vk'` 2/2 green.

## 2026-11-27 - Z7: a description is built once per revision, behind one lock

- **The defect.** `Geometry::description()` / `MaterialValues::description()` / `ShaderProgram::description()` were the
  same rule written three times by hand, and only the geometry copy had a lock (its comment records the 2026-11-16 TSan
  find). The other two compared and wrote `mutable` members raw, so two engines describing one program or one material
  could tear the cache member or build TWO values for one revision - two identities for one `(id, revision)`, which is
  the key the backend's tables use. TSan had named `ShaderProgram::description()` on 2026-11-24 and the reading then was
  "a race in the test"; it was the cache.
- **The rule (C22).** One type, `vn::graphics::detail::DescriptionCache<Description>` (SDK header, `detail` namespace -
  it is a private member of three public classes, not new vocabulary). `get(measure, build)` measures the source's
  revision, compares it with the cached value's own `revision()` and builds on a miss, ALL inside one `std::mutex`;
  `build` receives the measured revision, so the value's revision and the key cannot disagree. `peek(measure)` never
  builds. `Geometry`'s `shared_mutex` pair and `MaterialValues`' `described_`/`described_revision_` trio are deleted.
- **Trap.** The key must be measured INSIDE the lock: `MaterialInstance::revision()` resolves its parent's values on
  first read, so it WRITES. Measuring outside (the tempting shared fast path) races two readers inside `resolved()`
  before the cache is consulted. `MaterialInstance::parent_` is an `intrusive_ptr<Material>` (an asset), so the resolve
  chain is one level and never cycles.
- **Cost (measured).** Uncontended lock/unlock: `std::mutex` 2.8 ns vs `std::shared_mutex` shared 13.2 ns (20 M rounds,
  clang-23 -O2, 2026-11-27). The plain mutex is CHEAPER than the shared lock Geometry used, so the change is not a
  regression on the steady path; a describe happens per row at publication (`DrawListBake::refreshValues`), not per
  draw per frame.
- **Evidence.** `DescriptionCacheTest` (4 cases) - one build for 8 concurrent readers, a new revision is a new value,
  `peek` never builds, and one program + one material + one geometry described from 8 threads all give one identity;
  `test_graphics` 691/691; `scripts/tsan_check.sh` 691/691 with 0 warnings; ASan on `test_graphics` `RESULT: PASS`.
  `ADR-2026-11-27-a-description-is-built-once-behind-one-lock`; `graphics-threading.md` 2.4 / 2.14 / 2.16 / C22 / T41.

## 2026-11-27 - Z1 + Z2: a door is gated or locked, never neither

- **The defect (two doors).** The frame interval was stated (C20) but two ways into the engine's shared state stayed
  outside it, each because its mechanism was written for one caller: (a) `publish` / `unpublish` / `resolve` /
  `transientTarget` reached `detail::FrameResources`, whose own doc said "no synchronisation is needed", while the
  compile walks those same tables every frame - a `std::map` mutated during `seedFromHostBindings` is UB, not a
  refusal; (b) `bindPassContent` rewrote `PassDesc::content` ("the same one storage a plan writes and the compile
  reads") without the `beginShapeChange` gate `applyPlan` takes, so one door answered `PlanRefusal::Busy` and the other
  answered nothing.
- **The rule (C23 + C20).** The image tables take ONE lock (`tables_mutex_`) in every verb a host can reach - lookup,
  insert, eviction, never across a device call - and the frame head COPIES the host's bindings (`beginFrame`), so the
  compile seeds, checks and resolves against one value while the host's `resolve` keeps answering the live table
  (compile side: `frameBindings()` / `resolveForFrame()`). The content bind takes the shape's gate and answers
  `ContentBindOutcome` (`Bound` / `NotRegistered` / `Refused`) - no `operator bool`, so every call site had to decide.
- **Trap (the two patches that look like the fix).** Locking the four named host verbs is not enough while
  `hostBindings()` hands out a reference into the locked container (a fifth door that makes the lock decorative) -
  and `recordPublication` must not return a pointer into `publications_` for the same reason; it answers the target
  the name served before, so the collision report reads its other half from a value. A `bool` refusal is the other
  patch: "not registered" and "retry between frames" are different moves.
- **Trap (release outside the lock).** A replaced or retired transient is told to the device AFTER the lock is dropped
  (`releaseRenderTarget` takes the backend's locks); under the lock it would nest the table's lock under the backend's,
  an order nothing else states.
- **Cost.** One plain `std::mutex` per verb, 2.8 ns uncontended (same measurement as C22's cache); a handful of verbs
  per frame (`recordPublication` per pass, `resolve` per declared input).
- **Evidence.** `FrameResourcesTest` (the head's value; 2000 rounds of a host publishing/withdrawing/resolving/reading
  transients against a compile); `PlanTest.AFrameRefusesAContentBindFromInsideItsOwnCompile`;
  `PlanTest.TwoPassesPublishingOneNameAreReportedAndTheLastOneServesIt` (the collision report had no case before);
  `test_graphics` 695/695; `scripts/tsan_check.sh` 695/695 with 0 warnings; ASan `RESULT: PASS`.
  `ADR-2026-11-27-a-door-is-gated-or-locked`; `graphics-threading.md` 2.16 / 2.18 / C20 / C23 / T42 / T43;
  `graphics-architecture.md` 2.7 (the three-state answer).

## 2026-11-27 - Z22: a take-back is accepted, never refused

- **The defect.** `SceneView::~SceneView` (and `~PassChain` for a one-off chain) ends by asking the engine to take back
  its rows (`clearOwnedBy` → `removePassesOwnedBy`), and that ask took the shape gate and was REFUSED while a frame
  records - with a diagnostic telling the caller to wait or to change passes between frames. A destructor can do
  neither: no later call, and waiting is a deadlock when the frame it is inside is the one destroying it. So the rows
  stayed with a dead `owner` and a dropped chain, and `removePass`'s per-pass backend release never ran. Reachable from
  the shipped host: threaded `SurfaceWindow` destroys the view right before the engine whose destructor stops the frame
  thread - closing a window during a frame is enough (review 19 `Z22`).
- **The rule (C24, 2.19).** A shape change divides by "can the caller retry?". `applyPlan` / `bindPassContent` are
  retryable → refused with a value + one report. `removePassesOwnedBy` is not → NEVER refused: between frames applied at
  once, while a frame records RECORDED (`SessionShape::deferTakeBack`) and applied at the head of the next compile
  (before `compileFrame()` reads `slots_`), and by `shutdown()` after the pump stopped so it never outlives the engine.
  No `engine.pass.changed_during_frame` for this path - that sentence asks for a retry a destructor cannot make.
- **Trap (the patch that looks like the fix).** Deferring keyed by the OWNER ADDRESS is a new defect: `operator new`
  hands the next view the memory the last one freed, so a record that outlives its address by one frame deletes the
  rows of the object that registered second. `Slot::registration` (monotonic, stamped by `insertSlot`, the only place a
  row joins the table) + `next_registration_` read under the same `phaseGate` a registration takes → the drain removes
  only rows older than the ask. Capturing the doomed passes AT the ask is impossible (reading the table there is the
  very race the gate prevents).
- **Trap (the queue's lock).** `deferred_take_backs_` is appended from a destructor on any thread and swapped by a drain
  inside a frame, so it has a leaf `std::mutex`; order `phaseGate` → `deferred_mutex_` on the append path, never the
  reverse; the drain holds it only across the swap and releases backend state after it.
- **What it costs nobody.** A frame already built keeps the shape it compiled (a pass holds camera/content/program/
  target by `intrusive_ptr`), so the dead owner's pass is drawn once more at most; the drain runs after `beginFrame`'s
  `waitIdle`, so no compiled frame still holds a `raw_ptr` to a removed row. Cost: one vector append per dead owner
  (2.8 ns plain mutex) + one `swap` per frame.
- **Caveat, stated.** A row the SAME address updates IN PLACE after asking is still older than the ask and goes with it
  (a live owner that asks to be taken back and then re-declares is a contradiction, not a case).
- **Evidence.** `PlanTest.ATakeBackAskedInsideAFrameLandsOnTheNextFrame` (deferred, no refusal report, backend release
  at the next head), `PlanTest.ATakeBackThatWaitedLeavesRowsRegisteredAfterTheAskAlone` (the recycled address),
  `PlanTest.ATakeBackStillWaitingWhenTheEngineStopsIsApplied` (shutdown drains),
  `PlanTest.ATakeBackWhileAFrameIsAnsweredWaitsForTheNextHead` (threaded, the shipped host's order); `test_graphics`
  699/699; `scripts/tsan_check.sh` 699/699 with 0 warnings; ASan `RESULT: PASS`.
  `ADR-2026-11-27-a-take-back-is-accepted-never-refused`; `graphics-threading.md` 2.19 / C20 / C24 / T44;
  `graphics-architecture.md` 2.6 / 2.7.

## 2026-11-27 - Z21: a teardown is an interval entry, and a runner says where a job will run

- **The defect.** `RenderEngine::shutdown()` took no interval, so a diagnostic sink ON THE FRAME THREAD (where the vk
  backend reports a lost device) could tear the engine down under the frame it was inside. Worse, `FramePump::stop()`
  called from that thread could not join itself and returned before clearing `runner_`, while `live_` stayed true
  although the loop had exited: the pump still answered `threaded() == true`, the next `frame()` handed its job to a
  loop that had left, `has_request_` stayed set - `waitForFrame` timed out and the frame after blocked for ever in
  `beginFrame`'s `waitIdle`.
- **The rule (C25, 2.20).** `shutdown()` takes the lease `prepare()` takes (`FramePump::beginBuild`) and holds it across
  the whole teardown; a call from a frame body refuses with `engine.shutdown.refused_from_frame`, one from another host
  thread mid-build with `refused_in_flight` - once per episode, NOTHING torn down - and a frame ANSWERED on the thread is
  waited for and joined. `detail::FrameRunner` has ONE life fact (`State` Inline/Running/Stopping/Stopped);
  `stop()` publishes `Stopping` under the lock `submit` decides under BEFORE it returns (even from the frame thread, where
  the join belongs to somebody else), a job handed over just before the stop is still run, and `wait`/`waitIdle` answer
  from the job in flight. `FramePump::threaded()` publishes `runner->accepting()`.
- **Trap (a race the review's minimal patch would not have).** The loop's leave condition must be an ASKED-FOR stop
  (`Stopping`/`Stopped`), NOT "`state_ != Running`": the loop may already be waiting before its constructor publishes
  `Running`, and reading that window as a stop made the loop exit at birth. TSan caught it deterministically (the normal
  build passed, the instrumented one left the loop immediately); `wake`'s predicate now names the two leaving states.
- **Evidence.** `RenderEngineTest.AShutdownFromTheFrameThreadIsRefusedAndTheEngineKeepsRunning` (the sink case: refused
  once, the engine keeps driving and the next frame is answered - the wedge), `RenderEngineTest.ASecondThreadCannotShut
  TheEngineDownWhileAFrameIsBeingBuilt`, `FrameRunnerReentryTest.ARunnerAStopHasLeftRunsTheNextJobOnTheCaller`,
  `FramePumpTest.AStopAskedFromTheFrameThreadLeavesAnInlinePump`; `test_graphics` 703/703; `scripts/tsan_check.sh`
  703/703 with 0 warnings; ASan `RESULT: PASS`; `ctest` 31/31.
  `ADR-2026-11-27-a-teardown-is-an-interval-entry`; `graphics-threading.md` 2.17 / 2.20 / C25 / T45 / F10;
  `scripts/diagnostic_codes.txt` (two new codes).

## 2026-11-27 - Z11: a pass says whether it writes its target

- **The defect.** "Does this pass change the target it draws into" was answered three times by two rules. `scheduleRows`'
  `writes` lambda said `camera != nullptr && (content != nullptr || isClearEnabled())` with the comment "a clear is part of
  a draw, not a substitute for one", so a camera-less pass added NO producer edge while the backend recorded its clear
  (`target.begin` with its load ops); `recordProduced` and the wiring's `noteFilled` said
  `renderTarget() != nullptr`, i.e. a pass with a target fills it. A consumer of a camera-less clearing pass' target
  could therefore be ordered BEFORE the clear with nothing reported (the fill checks had already called the target
  filled), while a pass that announces nothing at all silenced its reader's `engine.pass.input_target_not_produced`.
- **The rule (2.16 / C9).** `RenderPass::writesTarget()` is the ONE answer: true when the entry the pass announces changes
  its target - a draw packet, OR an announced attachment policy (a clear is a write: the backend opens the pass' own load
  ops even with no packet). `describe()` fills the value and `passHasEffect(const PassDraw&)` already asked the same rule
  of it; the pair is pinned shape by shape. The scheduler's edge, `FrameResources::recordProduced` and the wiring's filler
  check all call `writesTarget()`; the `obj_cast<FullscreenPass>` branch and the `renderTarget() != nullptr` shortcut are
  deleted. `FullscreenPass` overrides it beside its `describe()`: no produced input in the declaration = nothing can
  resolve to be its source; past that gate, program AND camera, or a stated policy. `attachmentPolicy()` widened from
  private to protected so a subclass answers from the same rule.
- **Residual (N15).** The full-screen gate is a PREDICTION (the order is compiled before any input resolves): a
  full-screen pass whose declared input a producer does not fill this frame is a writer on paper and announces nothing -
  a superfluous edge (harmless) but the reader of its target goes unreported. `writesTarget()` may over-count, never
  under-count: an under-count is the wrong picture.
- **Test-shape consequence.** Every test that modeled "a writer" as `camera + target` now names content
  (`vn::test::emptySceneForTest()` in `tests/PlanTestSupport.hpp`): that shape announces nothing, which is what the
  corrected rule says. `RenderEngineTest.TheFrameValueIsEveryPassAndEveryDrawTheFrameRan` re-derives its run order -
  the camera-less pass it already contained now clears the target its composite samples, so it runs first.
- **Evidence.** `PassScheduleTest.TheWriterPredicateIsTheShapeOfTheAnnouncement` (7 shapes, declaration == announcement),
  `PassScheduleTest.AClearOnlyPassRunsBeforeTheConsumerThatSamplesItsTarget` (fails on the old lambda),
  `PlanTest.APassThatAnnouncesNothingDoesNotFillItsTarget` (fails on the old fill rule); `test_graphics` 706/706;
  `scripts/tsan_check.sh` 706/706 with 0 warnings; ASan `RESULT: PASS`; `ctest` 31/31.
  `ADR-2026-11-27-a-pass-says-whether-it-writes-its-target`; `graphics-rendering.md` 2.16 / C9 / N15 / F31 / T10;
  `graphics-architecture.md` 2.5.

## 2026-11-27 - Z12: a table entry holds its key, and ends when its share is the last

- **The defect.** `instance_streams` was the one content table that held NOTHING it was keyed by: the key was a
  `Buffer`'s address, the only version compared was `Buffer::revision()` (every fresh buffer starts at 0), and nothing
  but an explicit `releaseContent(Buffer)` ever removed an entry. Two facts followed: a `Buffer<float>` allocated at a
  recycled address at the same revision was served the previous object's GPU bytes (silently - the revision check
  passed, so only the picture was wrong); and every dropped buffer left a `VkBuffer` plus a pool slice for the session.
- **The rule (2.5 / N11).** `InstanceStream::source` holds the buffer its key is the address of, so the address is an
  identity (no later object can take it while the entry lives) and `revision` compares versions of ONE object - the
  rule `ADR-2026-11-09-address-keys-hold-the-object` already states and the geometry / program entries already follow.
  `ensureInstanceStream` takes the command's `BufferSlice`, so the share is GIVEN by its owner instead of invented from
  a borrowed reference.
- **The withdrawal.** Holding makes the cache an owner, so an entry ends two ways: the host's announcement, and the
  frame end where the cache's share is the last one (`Impl::sweepUnheldStreams` from `endFrame`, tested as
  `source->useCount() == 1`). One body (`withdrawInstanceStream`) serves both and ERASES the entry - the table's states
  are "held" and "absent", no placeholder. The parked act keeps the ENTRY (the park's `use_count` probe counts frames
  through that share) and the entry's source dies with the act.
- **Rejected (D9 / D10).** Hold-only (the review's own prescription) leaves the accumulation standing and makes it
  larger - the entry would pin the host's bytes too, and this table's host pattern rebuilds the instance buffer every
  frame with no verb to announce; a weak handle in `base/core` (a control block beside `RefCounted`'s in-object counter,
  and it does not answer two caches holding the same buffer); an id on `Buffer`; a TTL / LRU / byte cap (the SDK states
  the opposite rule); sweeping at table growth (quadratic in a frame that names many buffers for the first time).
- **Residuals.** A buffer two sessions' caches both hold is never swept by either (each sees a count of 2) - the
  announcement remains the way out. A GEOMETRY entry keeps the announcement-only rule: scene content's residency is the
  host's decision and its rows leave the tree, while a `Buffer` has no row that could say it is gone.
- **Evidence.** `VkBuiltinProgramTest.AnInstanceStreamHoldsItsBufferAndEndsWhenNobodyElseDoes` (the cache's share shows
  in `useCount()`; the stream leaves at a frame end with no announcement) and
  `ARecycledAddressDoesNotInheritThePreviousInstanceBytes` (the fresh buffer at the withdrawn address draws ITS matrix;
  under the old rule it drew the previous object's - the address search keeps its candidates alive, so the allocator's
  LIFO list is walked, and the case skips if no address comes back). `ADR-2026-11-27-a-held-entry-is-withdrawn-at-the-frames-end`;
  `vk-backend.md` 2.5 / N11 / D9 / D10 / T11 / F5 / 9.3; `graphics-content-lifecycle.md` 2.5 / 2.6; the backend guide.
