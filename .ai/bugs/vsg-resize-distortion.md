# Bug: 窗口缩放时几何体变形（挤压 / 拉伸）（历史登记）

- **日期**: 2026-09-02
- **模块**: viz/graphics + appfw/gui + gfx_backend_vsg
- **状态**: 已修复

## 现象

拖动窗口边缘改变窗口宽高比后，三角形被压扁 / 拉伸，比例失真。

## 根因

应用没有给 `RenderEngine` 挂任何 `CameraManipulator`（`camera_manipulator_`
默认为空）。`RenderEngine::pushEvent(ResizeEvent)` 原来**只在存在 manipulator**
时才更新相机投影 aspect，否则只调 `backend->resize()`。于是：

1. 相机 aspect 从初始化起一直是默认值 `1.0`，不随窗口宽高比更新 → 投影视锥与
   视口不匹配 → 几何体变形。
2. `VsgRenderer::resize()` 原来只重建 swapchain，没有更新 vsg 相机的
   `viewportState`——`RenderGraph` 每帧从它取渲染区域，导致画面一直渲染在
   初始尺寸区域。
3. 首次 attach 成功后没有投递 `ResizeEvent`，第一帧就以错误的 aspect 渲染
   （初始窗口若非正方形，启动即变形）。

## 修复

1. `RenderEngine::pushEvent(ResizeEvent)`
   （`src/viz/graphics/src/RenderEngine.cpp`）：无 manipulator 时，用新窗口
   `w/h` 更新主相机 aspect
   （`setProjectionMatrixAsPerspective(fov, w/h, near, far)`），与
   `OrbitCameraManipulator::onResize()` 的做法一致。
2. `RenderControl::initializeBackend()`
   （`src/fw/appfw/src/gui/RenderControl.cpp`）：首次 attach 成功后，在首帧
   `renderFrame()` 之前推一次 `ResizeEvent{surfaceWidth(), surfaceHeight()}`，
   让第一帧使用正确的 aspect。
3. `VsgRenderer::resize()`
   （`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`）：`window->resize()` 后
   同步更新 `d->vsg_camera->viewportState =
   ViewportState::create(window->extent2D())`，使渲染区域跟随实时窗口尺寸。

## 验证

窗口缩放后几何体保持正常比例，不再挤压 / 拉伸。

## 后续（M2c，2026-10-11）：这一整类错已结构性消失

当时的修法是"**记得**把新尺寸推给相机"（上面第 1、2 条）—— 一份状态两处维护，漏一次就又变形。
现在 `Camera` **不持 aspect**：投影是 `projectionMatrix(aspect)` 现算，而 aspect 只在**一处**推导
（`RenderBackend::viewAspect()`：pass 的 viewport，否则目标 / 表面尺寸），剔除与 view block 读同一个数。
于是"相机里的宽高比过期"这个状态**不存在了**，`pushEvent(ResizeEvent)`、`onResize()` 都不再需要往相机里推尺寸
（surface 尺寸照旧要推给后端 —— 那是它自己的窗口/交换链，不是投影的形状）。
见 `graphics-render-next.md` §3.2.3（③）与 `.ai/memory/graphics.md` 的 "aspect 出相机（M2c）" 一条。
