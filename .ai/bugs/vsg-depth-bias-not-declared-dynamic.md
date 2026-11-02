# vsg 后端的 polygon offset 不生效（深度偏移状态未声明动态）

- **状态**：待修复；静态代码路径已确认，缺少真实设备的像素/深度回归。
- **发现日期**：2026-11-02
- **模块**：`src/plugins/gfx_backend_vsg`
- **涉及**：`makeDynamicStateDeclaration()`（`internal/ContentPipeline.cpp`、`support/VsgDynamicState.cpp`）、
  `makeDynamicStateCommand()`（`internal/StateCommands.cpp`）、`SetDynamicState::record()`（`support/VsgDynamicState.cpp`）

## 现象与影响

`StateNode::setPolygonOffset` / `ResolvedRenderState::polygonOffset` 是对外承诺的能力：深偏移用于贴花、
共面表面、阴影偏移。当前后端把它写进动态状态命令，但内容管线的动态状态声明里没有它，管线 create-info 的
`depthBiasEnable` 也保持默认 `VK_FALSE`。结果是偏移值被写入命令流却**不参与光栅化**，共面表面继续 z-fighting，
而且全程没有任何诊断——一个“设了但没效果”的状态。

## 根因

三处各自成立，合起来失效：

1. `makeDynamicStateCommand()` 把 `polygon_offset.units` / `.factor` 写进 `SetDynamicState::depth_bias_constant` /
   `.depth_bias_slope`（`StateCommands.cpp:119-120`），`SetDynamicState::record()` 也真的调用了
   `vkCmdSetDepthBias`（`VsgDynamicState.cpp:90`）。
2. 内容管线实际使用的 `makeDynamicStateDeclaration()`（`internal/ContentPipeline.cpp:183-196`）列出 12 项动态状态，
   **不含 `VK_DYNAMIC_STATE_DEPTH_BIAS`**。同一仓库里另一份同名声明（`support/VsgDynamicState.cpp:117-129`）包含它，
   两份清单已经漂移；`ContentPipelineTest.TheEditableHalfIsDeclaredDynamic` 断言的是管线声明，但没覆盖深度偏移。
3. 管线 create-info 的 `RasterizationState` 用 vsg 默认值创建，`depthBiasEnable == VK_FALSE`
   （`build/_deps/vsg-src/include/vsg/state/RasterizationState.h:33`），且没有 `VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT`
   去动态打开它——即使声明补上，enable 仍是关的。

Vulkan 规则：某项动态状态未在管线里声明，`vkCmdSetXxx` 写入的值对该管线无效，驱动取 create-info 的烘焙值；
`depthBiasEnable == VK_FALSE` 时深度偏移整体不施加。两条同时命中，偏移必然是空操作。

## 修改建议

1. 让动态状态清单只有**一处**权威定义：删除重复实现，内容/屏幕管线共用一个 `makeDynamicStateDeclaration()`。
2. 在该清单里加入 `VK_DYNAMIC_STATE_DEPTH_BIAS`；把 `depthBiasEnable` 按引擎约定烘焙为 `VK_TRUE`（偏移为 0 时
   等于不偏移，是安全的默认），或改用 `VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT` 随命令下发——二选一，并在头文件里写明。
3. 加一条“清单完整性”测试：命令 `record()` 写入的每一项状态都必须在管线声明的集合里，反之亦然；这条测试是
   `graphics-rendering.md` §2.8 那条规则的可执行形式。
4. 若设备缺少 `fillModeNonSolid` 之外的所需特性，按既有策略在建会话时拒绝，而不是退化成“偏移无效”。

## 验证建议

- 单元：`ContentPipelineTest` 断言内容管线声明里含 `VK_DYNAMIC_STATE_DEPTH_BIAS`，且 `RasterizationState`
  的 `depthBiasEnable` 为 `VK_TRUE`。
- 像素：两个共面三角形，一个设 `PolygonOffset{1,1}` 一个不设；渲染后读回，设偏移的一方稳定胜出（多次运行不闪）。
  未设偏移时两者之间出现混合像素，作为对照。
- 验证层：跑一次 `scripts/vsg_rewrite_gate.sh`，确认 0 VUID（尤其 `vkCmdSetDepthBias` 相关项）。

## 设计约束

该问题涉及 **G4**（状态写入与声明语义必须一致，不存在"写了但没生效"的接口）、**G6**（同一后端两份动态状态清单是
不一致的实现）、**G10**（把"设了没效果"的真实成本藏起来，而不是在设计里说清 enable 与动态值的分工）。
