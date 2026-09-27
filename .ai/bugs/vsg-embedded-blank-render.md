# Bug: 嵌入式渲染视图空白（SceneBridge 内容不显示）（历史登记）

- **日期**: 2026-09-02
- **模块**: gfx_backend_vsg 插件（嵌入式 `SceneBridge` 几何构建路径）
- **状态**: 已修复

## 现象

嵌入式路径即使初始化成功，视图也只有清屏色、没有任何几何体；而独立窗口路径
（`makeRawDemoNode`，`VINE_VSG_OWN_WINDOW=1`）能正常显示三角形。

## 根因

1. 本 vsg build（FetchContent v1.1.16，无 glslang）反序列化出的 phong
   `ShaderSet` 的 `defaultGraphicsPipelineStates` 是空的。
   `GraphicsPipelineConfigurator` 只会补 `DepthStencilState` /
   `RasterizationState` / `ColorBlendState` / `MultisampleState` /
   `InputAssemblyState`，**从不补 `ViewportState`** → 管线没有视口，
   什么都不栅格化（`VsgRenderer::makeRawDemoNode()` 注释里已说明必须显式给全）。
2. `SceneBridge::buildGeometry()` 用手工组装的 `VertexIndexDraw` 出图——该构造
   在本 build 已被独立验证**不产生绘制**，须用显式
   `Commands{BindVertexBuffers, BindIndexBuffer, DrawIndexed}`（参考 vsgExamples
   `utils/vsggraphicspipelineconfigurator`）。

## 修复

1. `VsgRenderer::initialize()`（`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`）：
   为共享 phong `ShaderSet` 显式补全 `defaultGraphicsPipelineStates`——含
   `ViewportState`（取当前窗口尺寸）、`RasterizationState(cullMode = NONE)`、
   `DepthStencilState` / `ColorBlendState` / `InputAssemblyState` /
   `MultisampleState`。`SceneBridge` 之后构建的每个几何管线都继承这些状态。
2. `SceneBridge::buildGeometry()`（`src/plugins/gfx_backend_vsg/src/SceneBridge.cpp`）：
   改为在 `StateGroup`（来自 `config->copyTo`）下挂
   `Commands{BindVertexBuffers(config->baseAttributeBinding, arrays),
   BindIndexBuffer(indices), DrawIndexed(n, 1, 0, 0, 0)}`，替换 `VertexIndexDraw`。
3. 补充 `vsg/commands/{Commands,BindVertexBuffers,BindIndexBuffer,DrawIndexed}.h`
   头文件；移除不再使用的 `vsg/nodes/VertexIndexDraw.h`。

## 验证

嵌入式视图能显示启动 demo 三角形与 `TestRenderLiveCommand` 的动画三角形
（显式 draw + 完整管线状态 = 已验证能出图的配置）。
