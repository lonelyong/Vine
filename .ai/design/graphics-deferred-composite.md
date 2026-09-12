# graphics deferred-composite 设计（延迟 lit 不透明 + 深度正确的 forward 透明/叠层）

> 状态：设计稿 v1（2026-09-08）。**S1(F2 显式 occlusion) ✅ · S2(F1 排序) ✅ · S3(builder/Pipeline) ✅ · S4(app_shell) ✅ · S5(共享 depth) ✅ · F3(DepthMode) ✅**
> ——已实现并通过 Graphics/gfx_backend_vsg/app_shell 构建 + test_graphics/test_vsg；**真机像素级目检待用户**。
> S5 采用：`RenderTarget::shareDepth(source)`（借用 depth 不自有）+ `setDepthPromotion(false)`（源 depth 停在
> DS 布局不提升）→ composite 用 color CLEAR+depth LOAD 的 render pass，并在两 graph 间插 `PipelineBarrier`。
> F3：深度按 `DepthMode{Disabled, TestOnly, TestAndWrite}` 显式拆分——半透明叠层 pass 用
> `TestOnly`（depth test 开、write 关），与清屏正交。
> S5 优化收益：不透明单光栅化（删 composite_depth 重扫）。
> 背景：默认 demo 原先把 forward-only 内容（星星点云 + 半透明盒）**无深度地浮在**延迟光照结果之上
> （depth-off overlay，`addForwardOverlayDemo` 手工 pass）。本稿把 forward 透明内容与延迟几何的
> **真实深度遮挡**收进 `RenderPipelineBuilder` 单一 recipe（forward 内容作为 `setTransparentContent` 输入，
> Deferred 自动升级 composite，Forward 追加深度-on 后段）。
> 上游：`graphics-mrt-gbuffer.md`（MRT/GBuffer + deferred 主窗）、`graphics-render-pipeline.md`
> （多 pass/命名产出槽/Scene pass 表含 depth prepass）、`render-pipeline-builder.md`（preset recipe）。
> 关联：`vsg-target-unification.md`（统一 Target / (camera, order) 内容槽）、`docs/data-flow.md` §13。
- 里程碑：**S1 F2**（显式 occlusion，替代 ContentStyle）→ **S2 F1**（program/PiP slot 显式 order）→
  **S3** builder+`Pipeline` composite 升级 → **S4** app_shell 接线瘦身 → **S5** 共享 gbuffer depth
  （删 composite_depth 重扫，`shareDepth`+`setDepthPromotion(false)`+跨 graph `PipelineBarrier`）→
  **F3** DepthMode 拆分（透明叠层 = `TestOnly`，depth test 开 write 关）。

## 1. 目标
- 默认 demo（Deferred）里 forward 内容与延迟几何**真正按深度遮挡**：半透明盒被不透明塔正确裁剪、
  可被遮挡而不只是浮在顶层。
- 该能力收敛为 builder 的一条主窗 recipe，Forward / Deferred 共用同一“不透明 → 透明后段”结构；
  无透明内容时 Deferred 保持现状（光照直接出窗口，零额外开销）。
- app_shell 的手工 forward-overlay pass（`addForwardOverlayDemo`）迁入 builder（作为透明内容输入），
  demo 层只供场景、不摆 pass。

## 2. 关键事实与裁决
- **窗口 surface 深度不可用**：vsg 拥有的窗口 render graph 每帧 loadOp=CLEAR（无法 depth-LOAD），
  且所有窗口内容都在同一 render pass 内（无法在 pass 中插 `vkCmdCopyImage`）→ 不能把 G-buffer
  深度“灌回窗口深度”再让 forward 内容对着它画。**裁决：forward 内容改到与延迟光照同一个离屏
  composite target 上画**，composite 内先有“opaque 深度 + lit 色”，透明 pass 深度-on 叠进去，
  最后整屏 present 到窗口。
- opaque 深度来源选 **depth 重扫**（composite 内第一个内容槽重画不透明场景写深度；它写的颜色随后被
  全屏光照整屏覆盖，无碍），而非共享 gbuffer 的 depth image（需改 gbuffer depth finalLayout +
  跨 target framebuffer 深度别名，成本高，留 S5 优化）。
- 窗口语义标记须保持：present pass 携带 view camera 且 RT==null，使
  `RenderEngine::hasWindowPass(camera)` 为真 → `SceneView::ensureWindowPass`（RenderControl::init）
  不追加默认 Forward（见 graphics-mrt-gbuffer §1 同款机制）。

## 3. Pass 图（transparent 存在时 Deferred 自动升级为 composite）
```
order -3  gbuffer pass            → GBuffer 目标（不变：albedo/normal+shininess/spec/view-pos + D24）
order -2  opaque 深度重扫         → Composite（clear → main/depth-on 写深度；颜色被下一步覆盖）
order  0  延迟光照全屏 program    → Composite（depth-off，采样 GBuffer，写 lit 色）[F1 使 program 可排序]
order +1  forward 透明/叠层内容   → Composite（不清屏 + depth-on + 逐几何 alpha blend）[F2]
order 100 present（全幅采样 Composite）→ 窗口（携带 view camera）
窗口 HUD：gizmo(10)/fps(30) 内容槽叠在 present 之上；G-buffer/PiP 预览 order ≥120 照旧。
```
- 无 transparent 内容：Deferred 保持现状（光照 ScreenPass 即窗口 pass，不建 composite/present）。
- Forward + transparent：主 opaque 内容 pass(order0, 清屏 depth-on) 后追加透明内容槽(order+1,
  Main 样式，同一窗口 render pass 内深度-on)。
- 一个 composite target = 一个 render pass：loadOp CLEAR 后子槽按 order 画，-2 写的深度被 +1 的
  透明内容自然 depth-test——不需要跨 render pass 保存深度，也就无需共享深度图（B2 变体）。

## 4. API
### 4.1 RenderPass 内容深度（F2→F3 前端，最终形态）
- **没有“类型/样式”枚举**：深度与 clear、光照正交，全显式。
  - `setClearEnabled/clearColor/shouldClearDepth`：是否清屏（显式，不变）。
  - 顶层枚举 `vine::graphics::DepthMode { Disabled, TestOnly, TestAndWrite }`（F3）：
    `Disabled` = 不测不写（HUD 浮层）；`TestOnly` = 只 depth test 不 write（半透明叠层，写深度的
    不透明几何不破坏其后半透明）；`TestAndWrite` = 默认，普通不透明几何。
  - `setDepthMode(DepthMode)`（默认 `TestAndWrite`）；便捷 `setOcclusionEnabled(bool)` 保留，
    映射 `true→TestAndWrite / false→Disabled`。**不从 clear 推断**。
  - 光照：由**内容 scene 决定**，不再由样式选——后端对每个内容槽一律应用场景灯（空则保留 seed：
    window 的 presenting(清屏整幅) 槽默认 headlight，其余 ambient）。
- 透明 pass = `setClearEnabled(false)` + `setDepthMode(TestOnly)`（depth test 开但不清屏不写深度），
  光源来自其自身 scene。
- `RenderBackend` 通道 = `setDepthMode(DepthMode)`（后端 render() 消费 → 选 depth test/write 组合
  对应的 shader set）；`main_pending`（clear 置位）只用于标记 **presenting**（整幅主 pass，管整幅
  视口 + 默认灯 seed），不再管深度。
### 4.2 Builder（S3）
- `RenderPipelineBuilder::setTransparentContent(intrusive_ptr<Scene>)`（或
  `PipelineOptions::transparent_content`）；`setContent()` 语义 = 不透明内容。
- `build(Deferred/Forward)` 依据是否有透明内容自动分支到 composite / 直出。
- `Pipeline` 需能持有第二个离屏 target（composite）+ present pass；`resize()` 双 target 保持
  （gbuffer 与 composite 同尺寸同帧）；`offscreenTarget()` 仍指 gbuffer，新增
  `compositeTarget()`。
### 4.3 后端原语（S1/S2，能力点，只被 recipe 消费）
- F1：`ProgramSlot`/`ScreenSlot`（PiP/present）携带显式 order 并按 order 插入目标 render graph
  （现 program=INT_MIN 最前、PiP=INT_MAX 最后；统一为按 order 排序）。
- F2：`RenderBackend::setDepthMode(DepthMode)` 显式通道（F3 将 occlusion bool 升级为三态），
  render() 消费（content slot 拆成 `depth_mode` + `presenting` 两个量：前者来自 setDepthMode，
  后者来自 clear 的 main_pending）。

## 5. 后端改动落点
- `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`：
  - render()：内容槽深度从显式 `setDepthMode` 取（`depth_mode`），clear 只标 `presenting`；
    窗口/离屏各备三套 shader set（depth-on / depth test-only / depth-off）。
  - setupContentSlot / drawScreenProgram / drawScreenTexture：统一按 order 插入（F1）；
  - 光照一律来自内容 scene（空则 seed：window presenting → headlight，其余 ambient）。
  - `buildOffscreenTarget` 已按 target 名打日志（前置切片已做）。
- `src/viz/graphics/sdk/vine/graphics/RenderPass.hpp|RenderBackend.hpp|RenderPipeline.hpp` + builder。

## 6. 收益 / 风险
- 收益：默认 demo 半透明盒与 opaque 正确深度遮挡；无透明内容时零开销；builder 单一 recipe。
- 风险：每帧多一次不透明深度重扫（代价小，S5 消掉）；program/PiP 排序语义变化需回归
  gizmo/fps/G-buffer 预览叠加；present 必须携带 view camera（否则 RenderControl 插默认 Forward）。
- 测试：无透明内容路径行为不变 → 现有 builder/engine 单测全绿；新增单测在 mock backend 层验证
  “有透明内容时 composite/present pass 拓扑与顺序”。（已加：`DeferredPresetWithTransparentContentBuildsComposite`
  （S5 后 4 pass + compositeTarget + present 采样 Composite + 双 target resize，composite 共享
  gbuffer depth）、`ForwardPresetWithTransparentContentStacksDepthOnPass`、
  `RenderPassTest::OcclusionIsExplicitAndIndependentOfClear`）
