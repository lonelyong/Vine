# Bug: 放大窗口后新露出的区域先黑（以及「中间帧显示什么」的取舍）

- **日期**: 2026-09-17
- **模块**: gfx_backend_vsg + appfw/gui
- **状态**: 已修复

## 现象

最大化（或把窗口拉大）后，渲染区**新露出来的那部分先是黑的**，约 250 ms 后才被画面填满。

## 根因（Windows 实机实测，三条）

判据来源：跑 `Vine.exe`（deferred 默认管线），用 `scripts/win_maximize_probe.ps1`
（`ShowWindow(SW_MAXIMIZE)`）触发，靠**临时**插桩（`beginFrame`/`endFrame`/`resize` 打窗口 extent、
窗口 `RenderGraph` 的 `renderArea`、每个 slot 的 viewport 矩形，以及各 target/slot 的重建时间戳）读时间线。

1. **窗口共享 `RenderGraph` 的 `renderArea` 落后一帧。** `VsgRenderer::initialize()` 建的窗口图
   （`targets[nullptr].graph`）只在创建时拿到当时的 extent；之后是 vsg 自己在 `RenderGraph::accept()`
   里、**录制期**发现 extent 变化才 `resized()` 缩放它。于是 resize 后那些帧仍按**旧矩形**清/画：新露出的
   部分既没清也没画，而 swapchain 刚被重建、图像本来就是黑的。
   实测一条：`resize announced=1176x444 live=2352x888 graphRenderArea=752x480+0,0` → 该帧结束仍是
   `renderArea=752x480`。
2. **「第一帧就是最贵的那帧」。** `RenderControl::handleSurfaceUpdate()` 先跑 creator 的 layout 步骤
   （`view->onSurfaceResized()` → 重建整条离屏链：gbuffer、composite、以及采样它们的全屏程序槽），再
   `renderFrame()`；那一帧要 ~250 ms，而这期间**没有任何一帧以新尺寸 present 过**。
3. **同一次 resize 把窗口里每个全屏程序槽都变成 stale**：`ProgramSlot` 的 rebuild identity 里含目标
   **表面**尺寸（`dest_w`/`dest_h`），于是窗口一改大小，窗口里 5–6 个程序的节点全部重建（各 ~20–40 ms）。

## 修复

1. `VsgRenderer::resize()`（`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`）：当场把窗口图的
   `renderArea` / `viewportState` 写成新 extent，并把 `previous_extent` 同步成同值。
   同步 `previous_extent` 顺带**关掉 vsg 的缩放路径**——它会把已经正确的矩形再按 new/old 缩一次：实测
   HUD overlay 的矩形 `16,368 96x96` → `50,1436 300x177`（跑到 888 高的窗口外）、fps overlay →
   `7003,1561`（x > 2352）；修后 `16,776 96x96` / `2239,844 105x36`，都在窗口内。
2. 程序槽的 rebuild identity 去掉目标表面尺寸（`src/plugins/gfx_backend_vsg/src/VsgProgramSlot.cpp`，
   字段在 `include/vine/vsg/VsgRenderTargetEntry.hpp`）：节点的几何是全屏三角形、矩形是**动态状态**
   （每帧从 pass 的 viewport 写进 `slot.camera->viewportState`），而每个全屏片段阶段都按 `vine_uv` 采样、
   与尺寸无关。「矩形是动态而不是烤死的」由自检的 PiP 相位**反证**：PiP 在 96x54 的小矩形里采到**整个**
   源，而它的管线是按**表面**尺寸烤的。
3. 中间帧的取舍（`src/fw/appfw/src/gui/RenderControl.cpp::handleSurfaceUpdate`）：**保持一帧**。
   试过「在 layout 步骤前先呈现一帧」（此时离屏链还是旧尺寸 ⇒ 旧画面被**拉伸**填满新窗口，实测第一帧
   26 ms 就提交），实机看过后**撤掉**：它把画面拉伸变形，比「新区域晚 ~250 ms 才填上」更难接受。
   **约定：画面任何时刻都不变形。**

## 涉及文件

- `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`
- `src/plugins/gfx_backend_vsg/src/VsgProgramSlot.cpp`
- `src/plugins/gfx_backend_vsg/include/vine/vsg/VsgRenderTargetEntry.hpp`
- `src/fw/appfw/src/gui/RenderControl.cpp`

## 验证

- 全量构建绿；`test_vsg` / `test_graphics` / `test_gui` 通过；`vsg_backend_selftest` 绿
  （`[host-surface] move:` 行 + 0 VUID + `[selftest] done`）。
- 实机（Windows 11 + RTX 4060）：`ShowWindow(SW_MAXIMIZE)` 后重建序列正常（gbuffer + composite + 6 个
  全屏程序节点），0 VUID、无崩溃；重建帧覆盖整个新窗口（因为 `renderArea` 已当场写对）。
- **仍剩（登记在 `.ai/memory/graphics-perf-backlog.md` 的 H9）**：重建帧本身 ~240 ms（6 个全屏程序节点
  ~180 ms + 2 个 target ~40 ms；其中 **glslang 只占 ~50 ms**，其余是 vsg 每节点建管线/描述符）。这段期间
  屏上是旧画面（不变形），窗口新长出来的部分到该帧落地时才填上。要再缩短得改槽的重建策略（原地
  re-point 描述符 + 动态 viewport 状态），因为 resize 后**源的图像视图换了**、节点必须重建。
