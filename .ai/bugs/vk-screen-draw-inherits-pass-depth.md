# vk 的全屏 draw 继承 pass 的深度策略：窗口与合成目标整块被深度测试丢弃

- **状态**：已修复（2026-11-14，`gfx_backend_vk`；回归用例 `VkContentDrawTest.AScreenCopyIsDrawnWhateverThePassDepthPolicySays`）
- **发现日期**：2026-11-14
- **模块**：`src/plugins/gfx_backend_vk`
- **涉及**：`src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp`（`Impl::planScreen`）、
  `tests/test_vk/ContentDrawTest.cpp`、参考规则：`.ai/design/vsg-backend.md` §11.16bl（M4b）

## 现象与影响

`Vine --open demo` 在 vk 上**只有 HUD 的坐标轴与 FPS 数字**，3D 区整片黑；同一份代码在 vsg 上正常。渲染区读数
98.8% 近黑。`VN_FRAME_PLAN` 的帧表 8 个 pass 全在、记录层探针显示每条 draw 都 `recorded=1`、validation 层
**零 VUID**——「录了、没画」。

两处形状同时命中：

- **窗口 pass**：`WindowTarget` 的 render pass 带 D24，第一趟窗口 pass 把它清成 0.0（reverse-Z 的远平面）；
- **deferred 链的 composite**：它没有自己的深度，`RenderPass::setDepthView` 让它**借用** G-buffer 的深度
  （`RenderTarget::shareDepth`）。

## 根因

`planScreen` 用 `resolveRenderState(RenderState{}, pass.state, Triangles)` 解析全屏调用的状态，`pass.state` 的
深度策略默认是 `DepthMode::TestAndWrite`。全屏调用是 z = 0.0 的规范三角形——在这个引擎的 reverse-Z 约定下那是
**远平面**；对上面两种目标做 `GREATER` 测试，每个片段都失败，整条 `present` / `gbuffer_preview` / 延迟光照输出
被丢弃。清屏值（黑）留在目标里，于是「画了等于没画」。

测试套件抓不到的原因：`tests/test_vk` 的 screen fixture（`screenFrame`）**显式**把 `pass.state.depth` 设成
`Disabled`，而引擎真实链的 pass 不设它；窗口路径的用例只画 `Kind::Scene`。缺的形状是「全屏 draw + 带深度的
目标 + pass 深度策略未显式关闭」。

## 修复

`planScreen` 在解析前把 pass 状态的深度策略强制为 `Disabled`：

```cpp
vn::graphics::PassRenderState screen_state = pass.state;
screen_state.depth                         = vn::graphics::DepthMode::Disabled;
plan.resolved = resolveRenderState(RenderState{}, screen_state, Topology::Triangles);
```

pass 的深度策略是给它画的**内容**的（参考后端 M4b 的规则：全屏调用恒 `Disabled`，正典三角形在 z = 0.0）；
后端不得把内容策略施加到这条调用上。

## 验证

- 新用例（带 D24 的目标 + 默认 pass 状态）：修复前红（目标中心保持 clear 黑）、修复后绿；**突变验证**：把
  `screen_state` 换回 `pass.state` 重建 ⇒ 该用例红。
- `test_vk` 全量 **71/71**（lavapipe，validation 静默）；`test_vsg` 的 `BackendParityTest` **12/12**。
- 应用：vk 渲染区有画面（composite 中心 = 受环境光照亮的地面色），vk 与 vsg 同尺寸逐像素差 **1.88%**。

## 设计约束

- **G4**（接口语义）：`PassDraw::state` 是「这个 pass 对内容说的深度策略」，不是「每条 draw 的最终状态」；
  backend 必须按 ABI 规则解析，不能整块照搬。
- **G6**（统一而一致）：同一套 `FrameDescription` 在两个后端必须画同一张图——本次是「同一帧、两种结果」的
  典型反例，`BackendParityTest` 的覆盖面（screen 目标无深度）正是漏掉的那一格。
- **G11**（可调试性）：症状是「录了、零 VUID、没画」；定位靠的是记录层探针 + 把 GBuffer 直接拷到可读目标，
  不是读代码猜。
