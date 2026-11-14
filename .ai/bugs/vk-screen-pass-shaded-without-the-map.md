# vk 的全屏 set 只绑阴影的 stand-in：延迟光照没有阴影

- **状态**：已修复（2026-11-14，`gfx_backend_vk`；回归用例 `VkShadowTest.AScreenPassShadesWithTheMapItResolved`）
- **发现日期**：2026-11-14
- **模块**：`src/plugins/gfx_backend_vk`
- **涉及**：`src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp`（`Impl::makeScreenSet` / `recordScreen`）、
  `src/plugins/gfx_backend_vk/src/internal/Device.cpp`（把 pass 解析出的 `ShadowSource` 交给 screen 录制）、
  参考行为：`src/plugins/gfx_backend_vsg/src/internal/ContentPass.cpp`（screen 路径按 pass 解析的 map 绑定）

## 现象与影响

`Vine --open demo`（默认 deferred）在 vk 上**没有太阳阴影**，同一场景在 vsg 上有。内容路径（前向、G-buffer
阶段）的阴影正常——只有**全屏**的延迟光照程序拿不到阴影贴图。修复前两后端同尺寸渲染区逐像素差 1.88%，修复后
**0 像素差**（逐字节相同）。

## 根因

引擎的延迟光照程序有**带阴影的变体**（`BuiltinShaders::deferredLightProgram(shadowed)`，在文本里插入
`VN_SHADOW_BINDINGS`：binding 7 的 map + binding 8 的 `VineShadowBlock`）。vulkan 的全屏 set 在 2026-11-14
早些时候补上了这两个绑定位（否则带阴影的管线建不出来，VUID-07988），但 `makeScreenSet` **恒定**写白 stand-in
采样器/视图与零 `VineShadowBlock`：

- `recordScreen` 拿不到 pass 解析出的 map（`Device::executeFrame` 已经为**每个** pass 调了 `resolveShadow`，
  只是没把它传进 screen 录制）；
- 于是带阴影的光照程序在 vk 上永远读“白图 + 零块” ⇒ `shadow term = 1` ⇒ 没有阴影，而且**没有任何诊断**
  （绑定合法、管线合法、像素有输出）。

vsg 的同一条路径按 pass 解析的 map 绑定真实深度图，并在程序声明了 map 而 pass 解析不出可读图时**拒绝**该调用。

## 修复

- `ContentDraw::recordScreen` 收一个 `ShadowSource`（与内容路径同一个解析结果）；
- `recordScreen` 用它 `buildShadowBlock(shadow, draw)` 打包 binding 8 的块（空 source ⇒ `params.x = 0`，
  即引擎的“无阴影到达本 pass”），binding 7 在 `shadow.view != VK_NULL_HANDLE` 时绑**真实 map** + 阴影采样器，
  否则仍是白 stand-in；
- `Device::executeFrame` 把该 pass 已解析的 `shadow` 传进去。

## 验证

- 新用例 `VkShadowTest.AScreenPassShadesWithTheMapItResolved`：一个屏幕程序直接采样 binding 7 并写深度。左边
  条带在 map 里是 0.25（内容阶段把 clip z = 0.5 折成设备深度），右侧是清屏 0；白 stand-in 会在两处都读 255。
  **突变验证**：把 screen set 的 7 改回恒白 stand-in ⇒ 用例红；恢复 ⇒ 绿。
- `test_vk` **72/72**（lavapipe，validation 静默）；`test_vsg` 的 `BackendParityTest` **12/12**。
- 应用：vk 与 vsg 同场景 332×247 渲染区 **逐字节相同**（0 像素差，修复前 1.88%）。

## 设计约束

- **G4**（接口语义）：`makeScreenSet` 的绑定位存在 ≠ 绑对了图；“这个 pass 解析到的 map”只有一个来源
  （`Device::resolveShadow`），screen 路径必须取同一份答案。
- **G6**（统一而一致）：同一个 `FrameDescription` 在两个后端必须画同一张图；本次是“内容路径有阴影、全屏路径
  没有”的形状差。
- **G11**（可调试性）：症状是“前向有阴影、延迟没有、零诊断”。判据应当能直接问“binding 7 绑的是哪张图”，
  用例用“采样 map 并写深度”把这个问题变成一个数字。
