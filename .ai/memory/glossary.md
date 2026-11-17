# Glossary: one spelling per concept

> **Status**: living ｜ **Last verified**: 2026-11-17
> **Scope**: the vocabulary a design page, an ADR or a commit body uses about the render seam, the two
> backends and the frame model. It is the single mapping between a concept's **code spelling** and the
> words the Chinese pages use, so prose can be searched and translated without guessing.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/**` (the identifiers), `.ai/design/graphics-*.md`
> (the prose that uses them).
> **Rule**: prose uses the code spelling. When a concept is renamed, rename it here in the same batch
> (see `.github/copilot-instructions.md` → "Documentation Language").

## 1. Terms

| English spelling | Chinese in legacy pages | What it is (and its boundary) |
|---|---|---|
| engine, `RenderEngine` | 引擎 | One render area's frame loop and authoring root. It does **not** own the device. |
| device, `RenderDevice` | 设备 | The owner of GPU objects, memory and pipelines; several engines borrow one. |
| backend | 后端 | A plugin implementing the seam: `vk` (direct Vulkan) and `vsg` (VulkanSceneGraph). |
| seam | 接缝 | The `sdk/vine/graphics/` mechanisms **both** backends use (`FrameGate`, `DeviceClock`, `FrameTimeline`, `RetirementQueue`, `Readback`, `DrawState`, `TargetPlan`, `DiagnosticDraft`). |
| frame | 帧 | One `FrameDescription` value: one submit and one wait. |
| pass | pass (not translated) | One `PassDesc`; the only authoring shape. |
| draw call | 绘制调用 | One `DrawDescription`, not one recorded GPU command. |
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
| residency, `ResidenceTable` | 驻留 | Whether content's GPU objects exist; the three-state `Use` / `Build` / `Copy`. |
| snapshot, `SceneSnapshot` | 快照 | The published scene rows a frame's views select from. |
| publication, `Scene::publish` | 发布 | Making a snapshot current; a frame reads only what is published. |
| pipeline | 管线 | A `VkPipeline` / vsg pipeline built for one compatibility key. |
| compatibility, `RenderPassCompatibility` | 兼容性 | The render-pass half of a pipeline's key. |
| render pass key, `RenderPassKey` | 渲染通道键 | The opaque class two render passes must share to be compatible. |
| push range, `PushRange` | 推送区 | The per-draw push constants (was `ContentPush`). |
| binding set, `BindingSets` | 绑定集 | The descriptor sets a content draw binds (was `ContentSets`). |
| program halves, `ProgramHalves` | 程序两半 | The vertex/fragment halves a program compiles from (was `ContentHalves`). |
| diagnostic code | 诊断 code | The `vk.*` / `vsg.*` / `engine.*` string a report carries. |
| origin, `DiagnosticOrigin` | 来源 | Who reported: `Engine` / `Device` / `Host`. |
| validation layer | 验证层 | Vulkan's validation layer; its messages are VUIDs. |
| VUID | VUID | A validation-layer message id. |

## 2. Retired spellings (do not use in new prose)

| Retired | Use | Why |
|---|---|---|
| `Session` / 会话 (as the design noun) | frame driver / `FrameDriver` | Renamed in S2; `createSession()` is still the API verb, but the concept is the driver. |
| backend half / 后端半边 | seam | The directory and the concept were deleted (`ADR-2026-11-16-no-backend-half`). |
| `ContentHalves` / `ContentSets` / `ContentPush` | `ProgramHalves` / `BindingSets` / `PushRange` | B17 (S8). |
| `TargetImages` / `OffscreenTarget` / `HostTargets` | `TargetObjects` / `TargetTable` | S4 (B5a/B5b). |
| `TargetClaims` | the target table's lease | S4: one table answers map, shape and claims. |
| `SceneBake` as "the production collector" | `DrawListBake::buildRows` + `selectSnapshot` | The frame compiles draws from the published snapshot. |

## 3. False friends

| Word | Means here | Does **not** mean |
|---|---|---|
| engine | one render area's frame loop | the device owner (Filament/UE call that owner Engine/Device) |
| session | a borrowed frame driver (legacy spelling) | an OS, window or GPU session |
| render pass | the engine's `PassDesc` | `VkRenderPass` — the backend's own object is a render pass key plus `TargetObjects` |
| driver | frame driver (`FrameDriver`) | a GPU device driver |
| content | engine-owned scene, material, geometry, program, light | bytes on the GPU |
| residency | whether GPU objects exist | memory pressure or an eviction policy |
| command | `RenderCommand`, one scene drawable | one recorded GPU command |
