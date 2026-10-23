# graphics 场景图：节点、状态、收集、层、拾取

> 单一来源：场景图与收集语义。头文件（`Node.hpp` / `Group.hpp` / `StateNode.hpp` / `Scene.hpp` /
> `RayIntersection.hpp`）是 API 细节的权威。待办见 `graphics-followups.md`。

## 1. 节点与树

```
Node
 ├─ Group                 子节点容器（无自己的变换/状态）
 │   ├─ MatrixTransform   局部矩阵
 │   ├─ PositionAttitudeTransform   position / attitude / scale / pivot
 │   ├─ AutoTransform      billboard（RotateToScreen / RotateToCamera / RotateToAxis）
 │   └─ StateNode         子树渲染状态（depth / cull / blend / polygon）
 └─ Geometry              叶子：顶点通道 + Primitive 列表
```

- **严格树**：`Group::addChild` 会把子节点从旧父脱离；成环（子节点是祖先）被拒绝。
  一个节点在任意一次遍历里只有一个世界矩阵——这是包围盒缓存与拾取的前提。
- `Node::worldMatrix()` 折父链；收集与拾取走 top-down 累积，不要逐叶调用它。
- 变换节点只贡献局部矩阵；`Geometry` 没有子节点、没有自己的变换。

## 2. 向下传递的规则

| 事实 | 规则 |
|---|---|
| `isVisible` | **硬门**：不可见节点整棵子树退出收集、拾取、场景盒 |
| `opacity` | **相乘**：scene × 每个祖先 × 自身，叶子处夹到 [0,1]；材质不贡献透明度 |
| `layers` | **最近声明获胜**；`clearLayers()` 恢复继承；默认 `kDefaultLayer` |
| `drawOrder` | **最近声明获胜**；pass 内排序的外键（高 = 后画） |
| `StateNode` state | 按项合并，深层覆盖浅层；未声明的项留给 pass 默认 |
| material / program | 图元自己的值 > 几何级默认 > 最近 `StateNode` > pass |

`resolveRenderState(authored, pass, data_topology)` 是唯一解析点，并给出每项的来源
（`StateOrigin::Content / Pass / Data`）。

## 3. 层（L1）

- `Node::setLayers(mask)` 声明子树属于哪些层；`PassDesc::layer_mask` 选择一条 pass 画哪些层。
- 判定在**可绘制节点**上：容器是结构，不因掩码被剪（后代的重新声明不会变得不可达）。
- `Node::subtreeLayerUnion(inherited)` 是 layer epoch 驱动的懒缓存：收集、包围盒、拾取三条 walk 在可见性之后
  先判 `(union & mask) == 0`，否则整棵子树跳过。
- 层声明/结构变更推进 epoch；几何/材质/变换编辑不推进（不会白丢剪枝）。
- L2 目标层（数组目标 / 一次写多层）是触发条件型设计，见 `graphics-followups.md` §2.1。

## 4. 状态与变体

- `RenderState` 的每项都是 optional：未声明与声明默认值是两个事实。
- 后端把解析后的状态映射到管线键（cull / blend / polygon / depth / topology / samples / layout / variant）；
  状态变化在 pass 内以动态命令或变体切换表达。
- 约定：reverse-Z（far = 0），世界 CCW，framebuffer 里 frontFace = CLOCKWISE；比较函数 GREATER。
- 混合：引擎唯一透明通道是 `opacity`；`BlendState::enabled` 只选因子，透明度合并由
  `FrameCompiler` 在解析时完成（`opacity < 1` ⇒ 混合）。

## 5. 收集

- `Scene::collectRenderCommands(camera, layer_mask, viewport?)` / `(CollectView, layer_mask)` 返回共享不可变命令表。
- 每条 `RenderCommand` 快照：geometry、primitive 索引、model matrix、opacity、draw_order、authored state、
  material、program、instance slice / first / count。
- 收集按视锥剔除；`BoundsCache` 在一次 pass 内去重，`Node::subtreeBounds` 在节点自身坐标系持久缓存，
  `Geometry::localBounds` 缓存键 = positions buffer + revision + 段 + 几何 revision。
- **memo 键** = view-projection + eye + layer_mask（只有出现 `ViewDependence::ViewAndViewport` 时才加 viewport）；
  失效由根子树的 content epoch 决定（编辑沿父链 +1），所以编辑一个场景不会作废另一个场景的表。
- 排序：`drawOrder` 外键；同 order 内不透明前向（近到远）、透明后向（远到近）。

## 6. 视图上下文与 AutoTransform

- `CollectView{ projection, view, eye, up, viewport }`：viewport 是设备像素。它是 OSG `CullStack` 的窄版本。
- `Node::localToWorldForView(parent_world, view, world)` 返回 `ViewDependence`：
  - `None`：普通节点；
  - `View`：billboard 只依赖相机；
  - `ViewAndViewport`：屏幕空间缩放，viewport 进 memo 键。
- `AutoTransform` 的自动旋转在节点自己的坐标系里算（OSG 语义）；`setAutoScaleToScreen` 按 0.48 参考半径
  保持恒像素；没有 viewport 时保留作者 `scale()`。
- 包围盒：`AutoTransform::boundingBox()` 是以 position 为球心的保守球盒；场景盒不随“最后哪台相机收集”变化。

## 7. 拾取

- `Camera::screenToWorldRay`（归一化屏幕坐标）+ `RayIntersection::intersectScene/intersectSceneAll`。
- 只打 `Triangles` 图元；结果 = `geometry + primitiveIndex + instanceIndex`（无实例流时 `instanceIndex` 为 `max()`）。
- 层掩码与可见性同收集；容器不因掩码被剪。
- 选择（选中集 / 描边 / 框选 / 多选）不在框架里——它是业务；触发条件见 ledger §2.1。

## 8. 判据

- `SceneTest` / `NodeTest` / `GroupTest` / `StateNodeTest` / `RayIntersectionTest` / `SceneContentCacheTest` /
  `SceneLayersTest` / `PositionAttitudeTransformTest` / `AutoTransformTest`。
- 重要回归：编辑一个场景不丢另一个场景的 memo；冲突层声明只报一次；移动子树不会藏掉移入视锥的内容。
