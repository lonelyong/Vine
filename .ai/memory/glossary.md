# Glossary: one spelling per concept

> **Status**: living ｜ **Last verified**: 2026-11-18
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
| `FrameOutcome` | 调用结果 | What one `frame()` call did (Ran / Submitted); `NotReady` is a precondition, not a frame ending. |
| `FrameQuery` | 帧所见 | The publication and the view the picture was taken from (`FrameResult::query`). |
| pass | pass (not translated) | One `PassDesc`; the only authoring shape. |
| draw call | 绘制调用 | One `DrawCall`, not one recorded GPU command. |
| draw item | 可画项 | One `DrawItem`: a scene drawable as this frame sees it (a pass collects a list of them). |
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
| pipeline | 管线 | A `VkPipeline` / vsg pipeline built for one compatibility key. |
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

| Retired | Use | Why |
|---|---|---|
| `Session` / 会话 (as the design noun) | frame driver / `FrameDriver` | Renamed in S2; `createFrameDriver()` is still the API verb, but the concept is the driver. |
| backend half / 后端半边 | seam | The directory and the concept were deleted (`ADR-2026-11-16-no-backend-half`). |
| `ContentHalves` / `ContentSets` / `ContentPush` | `CompiledPrograms` / `BindingSets` / `PushRange` | B17 (S8), then B13 for the first (via `ProgramHalves`). |
| `ProgramHalves` / `ResourceTable` / `ResidenceTable` | `CompiledPrograms` / `PinnedResources` / `ResidencyClaims` | B13: three near-names, one per role (compiled programs / pinned values / build claims). |
| `TargetImages` / `OffscreenTarget` / `HostTargets` | `TargetObjects` / `TargetTable` | S4 (B5a/B5b). |
| `TargetClaims` | the target table's lease | S4: one table answers map, shape and claims. |
| `SceneBake` as "the production collector" | `DrawListBake::buildRows` + `selectSnapshot` | The frame compiles draws from the published snapshot. |
| `RenderCommand` / `DrawDescription` / `MaterialBase` / `EngineNotReady` / `createSession` / `releaseSession` | `DrawItem` / `DrawCall` / `MaterialValues` / `EngineNotReadyReason` / `createFrameDriver` / `releaseFrameDriver` | B12 (2026-11-18): each name now says what the thing is; no aliases kept (see `graphics-rendering.md` 2.10). |

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
