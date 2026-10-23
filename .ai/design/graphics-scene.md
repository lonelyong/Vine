# graphics 场景图：节点、状态、收集、层、拾取

> **一句话**：严格树 + 最近声明获胜的折叠；收集是纯函数，memo 由根子树 epoch 驱动；
> 拾取是机制，选择是业务。
>
> **权威来源**：`Node.hpp` / `Group.hpp` / `StateNode.hpp` / `Scene.hpp` / `Camera.hpp` /
> `RayIntersection.hpp`。**待办**：`graphics-followups.md`。

## 1. 设计要点

1.1 **严格树**：`Group::addChild` 自动从旧父脱离，拒绝成环；一次遍历里一个节点只有一个世界矩阵。
1.2 **折叠规则固定**：可见性硬门、透明度相乘、层与 drawOrder 最近声明获胜、状态按项覆盖。
1.3 **状态只有一个解析点**：`resolveRenderState(authored, pass, data_topology)` 给出每项的来源。
1.4 **收集是纯函数**：相机 + 场景内容 + 层掩码 → 共享不可变命令表；memo 只在这些输入变化时失效。
1.5 **视图上下文显式**：`CollectView` 携带 projection / view / eye / up / viewport；billboard 只在这里生效。
1.6 **拾取是机制**：射线 + 场景走查；选择/描边/框选不在框架里。

## 2. 功能点

### 2.1 节点与树

| # | 功能 | 说明 |
|---|---|---|
| F1 | `Node` | 名字、可见性、透明度、层、drawOrder、父链、世界矩阵、包围盒。 |
| F2 | `Group` | 子节点容器；`addChild` / `removeChild` / `children()`。 |
| F3 | 变换节点 | `MatrixTransform`、`PositionAttitudeTransform`（TRS + pivot）、`AutoTransform`（billboard）。 |
| F4 | `StateNode` | 子树渲染状态（depth / cull / blend / polygon）；可嵌套、深层覆盖。 |
| F5 | `Geometry` | 叶子：顶点通道 + `Primitive` 列表（见 `graphics-geometry.md`）。 |

### 2.2 折叠规则

| # | 事实 | 规则 |
|---|---|---|
| F6 | 可见性 | `isVisible` 是硬门：不可见子树退出收集、拾取、场景盒。 |
| F7 | 透明度 | scene × 每个祖先 × 自身，叶子夹到 [0,1]；材质不贡献 alpha。 |
| F8 | 层 | 最近声明获胜；`clearLayers()` 恢复继承；默认 `kDefaultLayer`。 |
| F9 | drawOrder | 最近声明获胜；pass 内排序外键（高 = 后画）。 |
| F10 | material / program | 图元 > 几何级默认 > 最近 `StateNode` > pass。 |
| F11 | 状态 | 每项 optional；`StateOrigin::Content / Pass / Data` 记录来源。 |

### 2.3 收集

| # | 功能 | 说明 |
|---|---|---|
| F12 | 命令表 | `Scene::collectRenderCommands(camera, layer_mask, viewport?)` / `(CollectView, layer_mask)`。 |
| F13 | 命令快照 | geometry、primitive、model matrix、opacity、draw_order、state、material、program、instance。 |
| F14 | memo | 键 = view-projection + eye + layer_mask（必要时 + viewport）；根子树 epoch 失效。 |
| F15 | 视锥剔除 | 子树盒 `subtreeBounds` 在节点自身坐标系持久缓存；移动相机不重算。 |
| F16 | 排序 | drawOrder 外键；同 order 内不透明前向、透明后向。 |
| F17 | 场景盒 | `Scene::boundingBox(mask)` 只算可见且命中层掩码的内容。 |

### 2.4 层与视图上下文

| # | 功能 | 说明 |
|---|---|---|
| F18 | L1 内容掩码 | `Node::setLayers` + `PassDesc::layer_mask`；判定在可绘制节点上。 |
| F19 | 子树层并集 | `subtreeLayerUnion` 懒缓存；`(union & mask) == 0` 整棵跳过。 |
| F20 | `CollectView` | projection / view / eye / up / 设备像素 viewport。 |
| F21 | ViewDependence | `None` / `View` / `ViewAndViewport`；只有后者把 viewport 进 memo 键。 |
| F22 | AutoTransform | `RotateToScreen` / `RotateToCamera` / `RotateToAxis`；`setAutoScaleToScreen` 恒像素。 |
| F23 | 保守包围盒 | AutoTransform 以 position 为球心的球盒；场景盒不随最后相机变化。 |

### 2.5 拾取

| # | 功能 | 说明 |
|---|---|---|
| F24 | 射线 | `Camera::screenToWorldRay` 收归一化屏幕坐标。 |
| F25 | 场景查询 | `RayIntersection::intersectScene` / `intersectSceneAll`。 |
| F26 | 命中身份 | `geometry + primitiveIndex + instanceIndex`；只打 `Triangles`。 |
| F27 | 掩码 | 与收集同一层规则；容器不因掩码被剪。 |

## 3. 公开接口

### 3.1 `Node` / `Group`

| 接口 | 作用 |
|---|---|
| `name()` / `setName()` | 调试与查找 |
| `isVisible()` / `setVisible()` | 子树硬门 |
| `opacity()` / `setOpacity()` | 子树乘法 |
| `layers()` / `setLayers()` / `clearLayers()` | L1 内容掩码 |
| `drawOrder()` / `setDrawOrder()` / `clearDrawOrder()` | pass 内排序 |
| `parent()` / `worldMatrix()` / `boundingBox()` | 父链查询 |
| `Group::addChild()` / `removeChild()` / `children()` | 结构 |

### 3.2 变换与状态

| 接口 | 作用 |
|---|---|
| `MatrixTransform::setMatrix()` / `matrix()` | 局部矩阵 |
| `PositionAttitudeTransform::setPosition()` / `setAttitude()` / `setScale()` / `setPivot()` | TRS |
| `AutoTransform::setAutoRotateMode()` / `setAutoScaleToScreen()` / `setAxis()` / `setNormal()` | billboard |
| `StateNode::setRenderState()` / `renderState()` / `setMaterial()` / `setProgram()` | 子树状态 |
| `resolveRenderState(authored, pass, topology)` | 唯一解析点 |

### 3.3 场景与收集

| 接口 | 作用 |
|---|---|
| `Scene::setRoot()` / `root()` / `clear()` | 内容根 |
| `Scene::setVisible()` / `setOpacity()` | 场景级折叠 |
| `Scene::addLight()` / `lights()` / `setLighting()` | 灯 |
| `Scene::collectRenderCommands()` | 收集 |
| `Scene::boundingBox(mask)` | 可见内容盒 |
| `Scene::setContentFrame()` | memo 帧令牌 |
| `Scene::findNode(name)` | 按名字查找 |

### 3.4 拾取

| 接口 | 作用 |
|---|---|
| `Camera::screenToWorldRay()` | 屏幕 → 世界射线 |
| `RayIntersection::intersect()` | 单几何 |
| `RayIntersection::intersectScene()` / `intersectSceneAll()` | 场景 |
| `RayIntersectionResult` | hit / point / normal / distance / geometry / primitiveIndex / instanceIndex |

## 4. 注意项

| # | 注意项 |
|---|---|
| N1 | 一个节点只有一个父；需要“同一子树多个放置”时复制节点，不要放宽树（见 D1）。 |
| N2 | `Node::worldMatrix()` 会走父链；遍历里用 top-down 累积，不要逐叶调用。 |
| N3 | memo 的失效靠公告：编辑必须走 setter 或 `invalidateBounds()`；直接改字节只公告 buffer revision。 |
| N4 | 编辑一个场景不应作废另一个场景的 memo（内容 epoch 是根子树级）。 |
| N5 | 层判定在可绘制节点上；容器不因掩码被剪，否则后代的重新声明会不可达。 |
| N6 | AutoTransform 的自动旋转在节点自身坐标系里算；父缩放进入最终大小。 |
| N7 | 拾取只打三角形图元；线/点不是表面。 |
| N8 | `Camera` 自带 aspect；pass 矩形与相机形状不一致会在编译期报出来。 |

## 5. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 多父 / DAG 场景图 | `worldMatrix()` / `boundingBox()` / 拾取身份失去唯一答案；重复子树复制节点即可。 |
| D2 | 选择态（选中集 / 描边 / 框选 / 多选）进框架 | 拾取是机制、选择是业务；行业里 picker 也是库。 |
| D3 | 在框架里内置 LOD / 空间划分 | 等“每帧收集本身成为瓶颈”的真实负载；见待办。 |
| D4 | 层掩码在容器上剪枝 | 最近声明获胜会让后代重声明变得不可达。 |
| D5 | 把状态折叠做成每叶上行查询 | O(depth) 且分配；自顶向下 `InheritedState` 是唯一实现。 |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | `SceneTest` / `NodeTest` / `GroupTest` / `StateNodeTest` / `RayIntersectionTest`。 |
| T2 | `SceneContentCacheTest`：编辑一个场景不丢另一个场景的 memo。 |
| T3 | `SceneLayersTest` / `AutoTransformTest` / `PositionAttitudeTransformTest`。 |
| T4 | 移动子树移入视锥不被陈旧盒藏掉；层冲突只报一次。 |

## 7. 相关文档

`graphics-architecture.md` · `graphics-geometry.md` · `graphics-rendering.md` · `graphics-followups.md`
