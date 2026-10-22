# Graphics 视图上下文与 AutoTransform 自动缩放（2026-10-22）

> **状态：已落地（2026-10-22）**。`CollectView`、`RenderPass::setDrawSize`、`AutoTransform::setAutoScaleToScreen`
> 与 `ViewDependence` 均在树上；判据见 §5。
>
> 一句话：**收集阶段需要一个“这一遍怎么看”的值**，不是一个全帧的 current 单例。
> `CollectView` 是 OSG `CullStack` 的窄版本：projection / view / eye / up / viewport，没有 GL 状态。

## 0. 问题

`AutoTransform::setAutoScaleToScreen` 要让节点在屏幕上保持恒定大小，必须知道：

- 相机的 projection / view（已经通过 `collectRenderCommands(camera)` 有）；
- 这一遍画到多少**设备像素**（窗口、离屏 target、PiP 子矩形、DPR 各不相同）。

`Camera` 不带 viewport；`RenderPass` 知道 `desc_.viewport` / `desc_.writes`，但窗口 pass 的
surface 尺寸在 `PassChain` / 引擎那边。于是收集 API 缺一个事实，而不是缺一套渲染状态。

## 1. 业界口径

| 库 | 每遍/每视图的上下文 | 备注 |
| --- | --- | --- |
| OSG | `osg::CullStack`：viewport / projection / modelview / eye / up / `pixelSize()` | `AutoTransform::computeMatrix` 就是 `nv->asCullStack()`；GL 状态另在 `osg::State` |
| Unity SRP | `ScriptableRenderContext`（帧/管线）+ `RenderRequest` / `Camera` / `CullingResults`（每视图） | 没有“当前相机”单例 |
| Filament | `View`（camera + viewport）+ `RenderPass` | `Renderer::render(view)` |
| bgfx | `ViewId` + `setViewRect` / `setViewTransform` / `setScissor` | 每 view 显式设置 |
| wgpu / Vulkan | `RenderPassEncoder::set_viewport` / `set_scissor_rect` | 每 pass 编码 |
| O3DE Atom | `RPI::View`（camera + viewport）交给 `RPI::Pass` | frame graph 绑定 |
| Qt3D / VTK | `QRenderView` / `vtkRenderer`（camera + viewport） | 每 renderer/view |

共同点：**按阶段/视图作用域**，不是按功能（不存在 `AutoTransformContext` / `LodContext`），
也不是全帧单例（一帧有 shadow / main / PiP / HUD，viewport 不唯一）。

## 2. 决定

### 2.1 `CollectView`（本片落地）

```cpp
struct CollectView {
    Mat4d    projection;
    Mat4d    view;
    Vec3d    eye;
    Vec3d    up;
    Viewport viewport;   // DEVICE pixels；invalid = 调用方没给
};
```

- `Scene::collectRenderCommands(camera, layer_mask, const Viewport* = nullptr)` 保留为便捷入口，
  内部组 `CollectView`；主入口是 `collectRenderCommands(const CollectView&, layer_mask)`。
- `Node::localToWorldForView(parent_world, const CollectView&, world)` 返回 `ViewDependence`：
  `None` / `View` / `ViewAndViewport`。它让收集知道“这份列表是否依赖设备像素矩形”。
- `ContentMemo::Entry` 的键：view-projection + eye + layer mask 始终参与；**viewport 只在
  `entry.viewport_sensitive` 为真时参与**（由 `ViewAndViewport` 置位）。没有自动缩放节点的场景，
  同相机画到不同 PiP 矩形仍是一次收集（`RenderEngineTest.OneCameraCullsWithItsOwnShapeInEveryRectangle`
  保持绿）。
- `RenderPass::describe` 组装 viewport：
  - `desc_.viewport` 有值 → 用它；
  - 否则 `desc_.writes` → target 的像素尺寸；
  - 否则窗口 pass → `RenderPass::setDrawSize()` 给的值（`PassChain::resize` 把 device 尺寸写进去，
    引擎在 `buildFrameValue` 里按 `RenderSurface::pixelRatio()` 兜底）。
- 没有 viewport 时 auto-scale 不生效，保留作者写的 `scale()`。

### 2.2 `FrameContext`（登记，不现在做）

帧号 / delta / elapsed 属于 update / 帧级钩子；一帧一份，**不进** `Scene` 的收集键。
`UpdateContext` 是它的 update 视图，不是每节点一个；OSG 的对应物是 `NodeVisitor` + `FrameStamp`。
等真正有 update traversal 时再加。

### 2.3 明确不做

- 不做全局 `current viewport` / `current camera` 单例：一帧多 pass 下没有唯一答案，且会破坏
  collection 的纯函数/memo。
- 不把 `RenderState`（blend / depth / cull）塞进 `CollectView`：OSG 也把 `State` 放在 `CullStack` 外。
- 不做 per-feature context（`AutoScaleContext` / `LodContext`）：LOD / impostor / 小特征剔除都读同一组事实，
  加字段或 helper 即可，不改节点虚函数签名。
- 不在后端算 auto-scale：命令表共享不可变，culling/排序也会用错矩阵；收集层是唯一同时知道
  view 和 viewport 的地方。

## 3. AutoScale 公式

OSG 的口径：`pixelSize(position, 0.48)` 是该位置处 0.48 世界单位覆盖的像素数。

```text
clip = projection * view * vec4(world_position, 1)
w    = |clip.w|
px_per_world_x = |P00| * viewport.width  / (2 * w)
px_per_world_y = |P11| * viewport.height / (2 * w)
px_per_world   = min(px_per_world_x, px_per_world_y)

pixel_size = 0.48 * px_per_world
scale      = clamp(1 / pixel_size, minimum_scale, maximum_scale)
```

- 透视/正交都成立；正交时 `clip.w = 1`。
- `viewport` invalid、点在相机后面、`pixel_size <= 0`：保留作者 `scale()`。
- 计算出的 scale **不回写** `scale()`：同节点可能被两个 pass/两个 viewport 画。
- `NoRotation` + auto-scale 仍然走视图相关路径（只换 scale，不换 rotation）。
- `boundingBox()`（无相机）用 `maximumScale`（有限时）或作者 scale；自动缩放要参与 scene framing 就设 max。

## 4. 扩展规则

1. 同一阶段共用一个上下文；新功能加字段/helper，不开新上下文。
2. 跨阶段才开新上下文（collection / frame / pass / backend / host）。
3. 只被一个节点使用的局部事实放节点参数，不进上下文。
4. 一个结构不允许同时出现 scene / device / host 的字段。

## 5. 判据

- `AutoTransformTest.AutoScaleKeepsAConstantPixelSize`：近/远两个相机，屏幕高度一致。
- `AutoTransformTest.AutoScaleClampsToTheConfiguredRange`：min/max。
- `AutoTransformTest.AutoScaleWithoutAViewportKeepsTheAuthoredScale`。
- `AutoTransformTest.TwoViewportsGetTheirOwnCommandMatrices`：同 view、不同 viewport，memo 不复用。
- `RenderEngineTest.OneCameraCullsWithItsOwnShapeInEveryRectangle`：普通场景仍是一次收集/一视图。
- `test_graphics` 424 → **431**；`test_vsg` 482、ctest 24/24。
