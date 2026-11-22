# Glossary: one spelling per concept

> **Status**: living ｜ **Last verified**: 2026-11-22
> **Scope**: the vocabulary a design page, an ADR or a commit body uses about the render seam, the two
> backends and the frame model. It is the single mapping between a concept's **code spelling** and the
> words the Chinese pages use, so prose can be searched and translated without guessing.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/**` (the identifiers), `.ai/design/graphics-*.md`
> (the prose that uses them).
> **Rule**: prose uses the code spelling. When a concept is renamed, rename it here in the same batch
> (see `.github/copilot-instructions.md` → "Documentation Language").
> **Reader-facing counterpart**: `docs/guides/graphics.md` §5 maps the same words to what Filament, O3DE
> and UE call them, and states the target roles and the five frame values. This page is the
> translator's table (code spelling ↔ the Chinese the legacy pages used); that chapter is the reader's.
> Do not copy one into the other - state the fact once and point at it.

## 1. Terms

| English spelling | Chinese in legacy pages | What it is (and its boundary) |
|---|---|---|
| engine, `RenderEngine` | 引擎 | One render area's frame loop and authoring root. It does **not** own the device. |
| device, `RenderDevice` | 设备 | The owner of GPU objects, memory and pipelines; several engines borrow one. |
| backend | 后端 | A plugin implementing the seam: `vk` (direct Vulkan) and `vsg` (VulkanSceneGraph). |
| seam | 接缝 | The `sdk/vine/graphics/` mechanisms **both** backends use (`FrameGate`, `DeviceClock`, `FrameTimeline`, `RetirementQueue`, `Readback`, `DrawState`, `TargetPlan`, `DiagnosticDraft`). |
| frame | 帧 | One `FrameDescription` value: one submit and one wait. |
| `FrameDescription` | 帧描述 | The compiled frame the engine hands the backend: every enabled pass' identity, order and resolved inputs plus what the pass described. |
| `FrameTable` | 帧表 | The same frame as rows a host may read (`PassRow`); not the plan, not a handle. |
| `FrameResult` | 帧结果 | What the last ANSWERED frame was: its picture, the world it saw (`FrameQuery`), its counts. |
| `FrameCallOutcome` | 调用结果 | What one `frame()` call did (Ran / Submitted); `NotReady` is a precondition, not a frame ending. |
| `FrameQuery` | 帧所见 | The publication and the view the picture was taken from (`FrameResult::query`). |
| pass | pass (not translated) | One `PassDesc`; the only authoring shape. |
| draw call | 绘制调用 | ONE submitted drawable - one `DrawItem`, the number a profiler and `FrameCounters::draws` / `FrameResult::draws` / `PrepareReport::draws` count. It is **not** a `DrawPacket`. |
| draw packet, `DrawPacket` | 绘制包 | One pass' request for ONE view: the `DrawItem`s it submits plus the state and lights they are issued with. `FrameCounters::draw_packets` counts these (one per pass and view); a backend records it as N submissions. |
| draw item | 可画项 | One `DrawItem`: a scene drawable as this frame sees it (a pass collects a list of them); one submitted item is one draw call. |
| material values, `MaterialValues` | 材质值 | The read face of a material's values; `Material` is the asset and `MaterialInstance` an instance of it. |
| content | 内容 | Scene, material, geometry, program and light as engine-owned objects. |
| target, `RenderTarget` | 目标 | The host's off-screen target; its address is the identity a frame names. |
| attachment | 附件 | One colour image or the depth image of a target. |
| lease, `TargetTable::Lease` | 租约 | One frame driver's per-frame claims; its destructor is the only release point. |
| claim | 认领 | Taking a target for one frame driver, before its attachments are built. |
| park | 停放 | What `release` does to a record: hand it to the retirement queue instead of destroying it. |
| retirement, `RetirementQueue` | 退休 | Destroys what the device's completion watermark has passed. |
| watermark | 水位 | The device's "every frame before this point is done" position. |
| frame driver, `FrameDriver` | 帧驱动者 | The per-engine frame executor the device hands out. **Use this** (see §2). |
| frame head, `FrameHead` | 帧首 | The per-frame plan storage the recorder fills. |
| body gate, `FrameGate` | 体闸 | `Shared` while a frame body runs, `Exclusive` for host verbs; a re-entrant call is refused. |
| reentry | 重入 | A host verb called from inside a frame body: refused with a diagnostic, never hung. |
| episode | 情节 | One condition's report-once cycle (`ReportOnce`): said once, re-armed when it is gone. |
| witness | 证人 | The test, counter or log line that proves a claim. |
| refusal | 拒绝 | A named diagnostic instead of silent degradation. |
| readback | 读回 | Copying colour or depth into host memory. |
| staging | 暂存 | The buffer `ReadbackStaging` copies through; it owns its pool and fence. |
| residency claims, `ResidencyClaims` | 驻留 | Who may build a key and whether its fill is published; the vk table two frame drivers share (was `ResidenceTable`). |
| pinned resources, `PinnedResources` | 固定资源 | Keyed residency with build-on-miss, pin-driven retirement and parked destruction; the vsg template every resource kind uses (was `ResourceTable`). |
| snapshot, `SceneSnapshot` | 快照 | The published scene rows a frame's views select from. |
| publication, `Scene::publish` | 发布 | Making a snapshot current; a frame reads only what is published. |
| pipeline | 管线 | A `VkPipeline` / vsg pipeline built from one `PipelineKey` (the portable identity - its vertex-layout slots carry each attribute's component count, i.e. its format). |
| `ContentPipelineKey` | — | vk's key: the seam's `PipelineKey` PLUS `RasterState` (the five items a Vulkan 1.2 device cannot set dynamically - cull, polygon mode, depth test/write/compare, blend). A name that exists because a device floor does, not because the model has two identities. |
| compatibility, `RenderPassCompatibility` | 兼容性 | The render-pass half of a pipeline's key. |
| render pass key, `RenderPassKey` | 渲染通道键 | The opaque class two render passes must share to be compatible. |
| push range, `PushRange` | 推送区 | The per-draw push constants (was `ContentPush`). |
| binding set, `BindingSets` | 绑定集 | The descriptor sets a content draw binds (was `ContentSets`). |
| compiled program, `CompiledPrograms` | 编译程序 | One pipeline layer plus its recorder per (program, revision, layout, variant, attachment count, topology); the vsg producer (was `ProgramHalves` - its prose and members still say *half*). |
| diagnostic code | 诊断 code | The `vk.*` / `vsg.*` / `engine.*` string a report carries. |
| origin, `DiagnosticOrigin` | 来源 | Who reported: `Engine` / `Device` / `Host`. |
| validation layer | 验证层 | Vulkan's validation layer; its messages are VUIDs. |
| VUID | VUID | A validation-layer message id. |

## 2. Retired spellings (do not use in new prose)

**Rule**: a renamed word leaves **no alias** - one spelling per concept. Only a SIGNATURE migration (a return value
or a parameter list that changed shape) may keep a `[[deprecated]]` forwarder, and it leaves with the batch that
finishes that migration. Two spellings of one word are a defect even when they compile
(see `ADR-2026-11-18-public-vocabulary-unification`). No forwarder is in the tree today: the last two
(`Capabilities::*` and `DeviceClock::WindowId`) left with `BC-32`.

| Retired | Use | Why |
|---|---|---|
| `Session` / 会话 (as the design noun) | frame driver / `FrameDriver` | Renamed in S2; `createFrameDriver()` is still the API verb, but the concept is the driver. |
| backend half / 后端半边 | seam | The directory and the concept were deleted (`ADR-2026-11-16-no-backend-half`). |
| `ContentHalves` / `ContentSets` / `ContentPush` | `CompiledPrograms` / `BindingSets` / `PushRange` | B17 (S8), then B13 for the first (via `ProgramHalves`). |
| `ProgramHalves` / `ResourceTable` / `ResidenceTable` | `CompiledPrograms` / `PinnedResources` / `ResidencyClaims` | B13: three near-names, one per role (compiled programs / pinned values / build claims). |
| `TargetImages` / `OffscreenTarget` / `HostTargets` | `TargetObjects` / `TargetTable` | S4 (B5a/B5b). |
| `TargetClaims` | the target table's lease | S4: one table answers map, shape and claims. |
| `SceneBake` as "the production collector" | `DrawListBake::buildRows` + `selectSnapshot` | The frame compiles draws from the published snapshot. |
| `RenderCommand` / `DrawDescription` / `MaterialBase` / `EngineNotReady` / `createSession` / `releaseSession` | `DrawItem` / `DrawPacket` / `MaterialValues` / `EngineNotReadyReason` / `createFrameDriver` / `releaseFrameDriver` | B12 (2026-11-18): each name now says what the thing is; no aliases kept (see `graphics-rendering.md` 2.10). |
| `visible_instances` / `visibleInstanceCount()` | `instance_count` / `instanceCount()` (capacity: `instanceCapacity()`) | "visible" promised a culling decision that never happened; the value is the draw's `instanceCount` parameter. |
| `LayoutParticipant` / `OverlayLayout` / `output_slot` / `InstanceSlot` / `hasState()` / `clearState()` / `LightRig::headlight()` | `ResizeParticipant` / `OverlayPlacement` / `output_name` / `InstanceRecord` / `hasRenderState()` / `clearRenderState()` / `LightRig::directionalFromView()` | 2026-11-20 vocabulary batch: one job per word (`layout`/`slot`/`state` no longer carry several), and `headlight` promised camera tracking the light never had. |
| `OrbitCameraManipulator::zoom()` / `orbitRadius()` / `setMinDistance()` / `zoomToCursor()` | `dolly()` / `orbitDistance()` / `setMinOrbitDistance()` / `dollyToCursor()` | The wheel moves the eye along the view (a dolly); "zoom" means a field-of-view change in the industry, and one distance had two nouns. |
| `FrameShape` / `frameShapeOf` | `FrameCounts` / `frameCountsOf` (with `draw_packets` / `draws`) | "shape" already meant attachment formats; the value is three counts: passes, draw packets, draws (the last is the number a profiler calls draw calls). |
| `DrawCall` / `OwnedDraw` / `PassDraw::draws` / `isDrawingCall()` / `drawingCallItems()` | `DrawPacket` / `OwnedDrawPacket` / `PassDraw::packets` / `packetDraws()` / `drawItemsOf()` | 2026-11-20: the value is one view's whole packet, and "draw call" is the industry's word for ONE submitted item. |
| `draws` (packets) + `draw_items` (items) in the frame reports | `draw_packets` (packets) + `draws` (items) | A report field called `draws` is read against RenderDoc / Nsight; it has to be the item count, and the packet count needs its own name. |
| `Screen` as a kind / `ScreenPass` | `Fullscreen` / `FullscreenPass` | 2026-11-20: "screen" is the window in this module, and the presenting pass is itself one of these; the kind fills its VIEW. |
| `FrameOutcome` | `FrameCallOutcome` | It is what one `frame()` CALL did (`NotReady` = no frame happened), not what a frame was. |
| `Capabilities::Feature` / `Capabilities::supports` / `Capabilities::bit`; `DeviceClock::WindowId`; `FrameStats.hpp` | `Features::Feature` / `features.supports` / `Features::bit`; `FrameDriverId`; `FrameReport.hpp` | The forwarders the glossary had scheduled for removal; a header named after a type that does not exist. <!-- drift-ok --> |

## 3. False friends

| Word | Means here | Does **not** mean |
|---|---|---|
| engine | one render area's frame loop (`RenderEngine`) | the device owner (`RenderDevice`) — Filament/UE call that owner Engine/Device |
| session | a borrowed frame driver (legacy spelling) | an OS, window or GPU session |
| render pass | the engine's `PassDesc` | `VkRenderPass` — the backend's own object is a render pass key plus `TargetObjects` |
| driver | frame driver (`FrameDriver`) | a GPU device driver |
| content | engine-owned scene, material, geometry, program, light | bytes on the GPU |
| residency | whether GPU objects exist | memory pressure or an eviction policy |
| command | `DrawItem`, one scene drawable | one recorded GPU command |
| shape | a target's attachment formats (`RenderPassCompatibility`, or `TransientShape` for a declaration), or a frame's counts (`FrameCounts`) | a mesh / geometry shape |
| layout | a GPU image layout (`ImageLayout`), a vertex input layout, an overlay's placement (`OverlayPlacement`), or a surface-resize step (`ResizeParticipant`) | one job - read the type's own note |
| slot | a light block index, a content arena index, an engine pass row, a published output name (`output_name`), or an instance record (`InstanceRecord`) | one key space |
| screen | a full-screen pass (`FullscreenPass`, the `Fullscreen` kind), or a screen-SPACE program (`drawScreenProgram`, `screenCopyProgram`) | the window or the surface - that is the `RenderArea` / `PresentTarget` |
| target | a `RenderTarget` (off-screen images), a pass' `Target` declaration, or a `PresentTarget` (a presenter) | one type - the three have different lifetimes |
| outcome / result | `FrameCallOutcome` is what one `frame()` call did; `FrameResult` is what the last ANSWERED frame was | two words for one value |
| draw call / draw packet | a draw call is one submitted `DrawItem`; a `DrawPacket` is one pass' whole request for one view | two words for one value - the counts `draws` and `draw_packets` follow the same split |
