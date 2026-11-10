# graphics 场景图：节点、状态、收集、层、拾取

> **状态**：现行
> **最后核对**：2026-11-02
> **范围**：严格树、状态折叠、收集 memo、层与拾取。
> **权威来源**：`Node.hpp` / `Group.hpp` / `StateNode.hpp` / `RenderState.hpp` / `StateResolution.hpp` /
> `Scene.hpp` / `Camera.hpp` / `RayIntersection.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

本文件定义 graphics 场景图：节点、状态、收集、层、拾取 的现行设计：先给对象与边界，再给功能、接口、约束与验收。

### 1.2 目标

- 把 §2 的设计要点落成 §3 的功能点、§4 的接口与 §8 的验收。
- 让“现在是什么”与“为什么这么定”分开：理由在 §2/§7，历史看 git。

### 1.3 非目标

- 不在本文件重复头文件里的完整签名；接口只给契约与指针。
- 不在本文件保存已关闭的历史或逐轮审查记录。

### 1.4 一句话结论

> 严格树 + 最近声明获胜的折叠；收集是纯函数，memo 由根子树 epoch 驱动； 拾取是机制，选择是业务。

## 2. 设计要点

2.1 **严格树**：`Group::addChild` 自动从旧父脱离，拒绝成环；一次遍历里一个节点只有一个世界矩阵。
2.2 **折叠规则固定**：可见性硬门、透明度相乘、层与 drawOrder 最近声明获胜、状态按项覆盖。
2.3 **状态只有一个解析点**：`resolveRenderState(authored, pass, data_topology)` 给出每项的来源。
2.3.1 **深度带属于 pass，不属于节点**：`RenderPass::setDepthRange` 把一条 pass 的 draw 映射进深度缓冲的一段；
节点不能携带它，因为两棵子树映射同一个 framebuffer 会让一个像素的深度值失去唯一含义（没有 per-pixel tag
就无法反解，拾取也没有唯一答案）。映射保序，但重映射后的深度**不再是距离**：一张 target 的写者若用不同区间，
它的深度就不能再被当作几何距离读回。
2.3.2 **解析出来的每一项有一个终点**：pipeline 变体（`topology`）/ 动态状态（depth、cull、blend、`polygonMode`、`lineWidth`）/ **数据块**（`pointSize` → draw 块的 `params.y`，因为点大小是顶点着色器输出而不是 API 状态）。终点决定"改它要不要重编"：三类都不重编（本引擎的 cull/polygon/线宽都是动态状态），只有 `topology` 进变体键 —— 但 `pointSize` 的终点是**数据**，所以它连动态状态都不是。这一点必须写在 `StateResolution.hpp` 的 item 注释里，否则下一项很容易被顺手塞进 `PipelineKey`。
2.4 **收集是纯函数**：相机 + 场景内容 + 层掩码 → 共享不可变命令表；memo 只在这些输入变化时失效。
2.5 **视图上下文显式**：`CollectView` 携带 projection / view / eye / up / viewport；billboard 只在这里生效。
2.6 **拾取是机制**：射线 + 场景走查；选择/描边/框选不在框架里。
2.7 **灯是场景的环境量**：灯挂在 `Scene` 上而不是节点上（一个场景一份光照，每个 pass 拿到同一份，所以主/离屏/阴影 pass 的灯永远一致），位置与方向都是世界坐标。节点灯（灯跟着子树动）与逐 draw 选灯是触发条件型（backlog T7 / S7），不是今天的模型。

**场景不设灯数上限**：`addLight` / `setLighting` 有多少收多少，因为容量是光照程序的属性而不是内容的语义 —— 同一个 Scene 今天被 forward 与 deferred 两条路径画，将来可能被容量不同的宿主程序画。上限在 `kMaxLights`（`ShaderAbi.hpp`），超出的灯在打包时不被表示，按 episode 报一次 `ChannelIgnored` 警告（见 `graphics-shader-params.md` §2.2）。要限制灯数的是选灯策略（backlog S7），不是 `Scene` 的一道上限。
2.8 **根必须是根**：场景从一个无父节点进入。`setRoot` 先把已在别的 Group 下的节点摘下来（与 `Group::addChild` 自动脱离旧父同一条规则）；根在之后被别的树收养，就不再是这个场景的根——收集与包围盒一律为空，且检查在 memo 查询之前，所以不会返回移动前的列表。两个场景可以共用同一棵仍是根的子树（各自的场景级状态、memo 独立），接缝只在“它还是不是根”。

2.9 **透明顺序是对象级近似**：同一 `drawOrder` 内不透明前→后、透明后→前，顺序键是**一个代表点**到眼睛的距离平方
（今日取命令世界矩阵原点），不是几何范围，也不是逐片元顺序。它保证“看着分得开的两个物体”顺序正确，不保证自相交、
非凸、同中心多层的顺序。需要更强顺序的内容走 `Node::setDrawOrder`（已支持）、拆分几何，或另开一个排序独立透明的 pass
（触发条件见 backlog）。代表点的选取口径见 `../bugs/graphics-transparent-sort-uses-origin.md`。

2.10 **节点类型分派走元类型**：场景走查对 `Node` 派生类型的判断用 `obj_cast<T>`（`vine/Object.hpp` 的元类型父链 + `static_cast`），
不用 `dynamic_cast`。为什么：每节点有若干次判断，实测 `obj_cast` 在命中 / 未命中都约为 `dynamic_cast` 的 2–2.6 倍（Debug / MSVC，
20k 节点）；它也统一了项目已有的写法（`PassChain` / vsg 后端）。边界：这是**契约**——`T` 必须有正确的 `VN_OBJECT_META_DECL/IMPL`，
且 meta 父链必须与 C++ 继承一致；错链会让 `static_cast` 静默落到错误地址。不是 `Object` 派生的类型（`CameraManipulator`、第三方 vsg 类型）
继续用 `dynamic_cast`。满足 `G6` / `G10`，代价是元类型链必须维护正确。

## 3. 功能点

### 3.1 节点与树

| # | 功能 | 说明 |
|---|---|---|
| F1 | `Node` | 名字、可见性、透明度、层、drawOrder、父链、世界矩阵、包围盒。 |
| F2 | `Group` | 子节点容器；`addChild` / `removeChild` / `children()`。 |
| F3 | 变换节点 | `MatrixTransform`、`PositionAttitudeTransform`（TRS + pivot）、`AutoTransform`（billboard）。 |
| F4 | `StateNode` | 子树渲染状态（depth / cull / blend / polygon / polygonOffset / pointSize / lineWidth）；可嵌套、深层覆盖。 |
| F4a | `DepthRange` | pass 级深度带：把一条 pass 的 draw 映射进 `[min,max]`（默认整段 = 不重映射），用于语义分层；保序但不再是距离。 |
| F5 | `Geometry` | 叶子：顶点通道 + `Primitive` 列表（见 `graphics-geometry.md`）。 |

### 3.2 折叠规则

| # | 事实 | 规则 |
|---|---|---|
| F6 | 可见性 | `isVisible` 是硬门：不可见子树退出收集、拾取、场景盒。 |
| F7 | 透明度 | scene × 每个祖先 × 自身，叶子夹到 [0,1]；材质不贡献 alpha。 |
| F8 | 层 | 最近声明获胜；`clearLayers()` 恢复继承；默认 `kDefaultLayer`。 |
| F9 | drawOrder | 最近声明获胜；pass 内排序外键（高 = 后画）。 |
| F10 | material / program | 图元 > 几何级默认 > 最近 `StateNode` > pass。 |
| F11 | 状态 | 每项 optional；`StateOrigin::Content / Pass / Data` 记录来源。 |

### 3.3 收集

| # | 功能 | 说明 |
|---|---|---|
| F12 | 命令表 | `Scene::collectRenderCommands(camera, layer_mask, viewport?)` / `(CollectView, layer_mask)`。 |
| F13 | 命令快照 | geometry、primitive、model matrix、opacity、draw_order、state、material、program、instance。 |
| F14 | memo | 键 = view-projection + eye + layer_mask（必要时 + viewport）；根子树 epoch 失效。 |
| F15 | 视锥剔除 | 子树盒 `subtreeBounds` 在节点自身坐标系持久缓存；移动相机不重算。 |
| F16 | 排序 | drawOrder 外键；同 order 内不透明前向、透明后向。分类在收集时求值、顺序冻结在表里（见 §6.3）。 |
| F17 | 场景盒 | `Scene::boundingBox(mask)` 只算可见且命中层掩码的内容。 |

### 3.4 层与视图上下文

| # | 功能 | 说明 |
|---|---|---|
| F18 | L1 内容掩码 | `Node::setLayers` + `PassDesc::layer_mask`；判定在可绘制节点上。 |
| F19 | 子树层并集 | `subtreeLayerUnion` 懒缓存；`(union & mask) == 0` 整棵跳过（缓存按进程级层 epoch 失效，见 N11）。 |
| F20 | `CollectView` | projection / view / eye / up / 设备像素 viewport。 |
| F21 | ViewDependence | `None` / `View` / `ViewAndViewport`；只有后者把 viewport 进 memo 键。 |
| F22 | AutoTransform | `RotateToScreen` / `RotateToCamera` / `RotateToAxis`；`setAutoScaleToScreen` 恒像素。 |
| F23 | 保守包围盒 | AutoTransform 以 position 为球心的球盒；场景盒不随最后相机变化。 |

### 3.5 拾取

| # | 功能 | 说明 |
|---|---|---|
| F24 | 射线 | `Camera::screenToWorldRay` 收归一化屏幕坐标。 |
| F25 | 场景查询 | `RayIntersection::intersectScene` / `intersectSceneAll`。 |
| F26 | 命中身份 | `geometry + primitiveIndex + instanceIndex`；只打三角形拓扑（list / strip / fan，`isTriangleTopology`）。 |
| F27 | 掩码 | 与收集同一层规则；容器不因掩码被剪。 |

## 4. 公开接口

### 4.1 `Node` / `Group`

| 接口 | 作用 |
|---|---|
| `name()` / `setName()` | 调试与查找 |
| `isVisible()` / `setVisible()` | 子树硬门 |
| `opacity()` / `setOpacity()` | 子树乘法 |
| `layers()` / `setLayers()` / `clearLayers()` | L1 内容掩码 |
| `drawOrder()` / `setDrawOrder()` / `clearDrawOrder()` | pass 内排序 |
| `parent()` / `worldMatrix()` / `boundingBox()` | 父链查询 |
| `cacheStats()` | 两个缓存的只读见证（`bounds_recomputes` / `layer_union_recomputes`），按值返回；观测口，不参与任何判定 |
| `contentEpoch()` | 子树内容公告时钟；`Scene` memo 的失效键，也是宿主自建子树缓存的失效键（契约，不是观测口） |
| `Group::addChild()` / `removeChild()` / `children()` | 结构 |

### 4.2 变换与状态

| 接口 | 作用 |
|---|---|
| `MatrixTransform::setMatrix()` / `matrix()` | 局部矩阵 |
| `PositionAttitudeTransform::setPosition()` / `setAttitude()` / `setScale()` / `setPivot()` | TRS |
| `AutoTransform::setAutoRotateMode()` / `setAutoScaleToScreen()` / `setAxis()` / `setNormal()` | billboard |
| `StateNode::setDepth()` / `setCullMode()` / `setBlend()` / `setPolygonMode()` / `setPolygonOffset()` / `setPointSize()` / `setLineWidth()` / `clearState()` / `renderState()` | 子树状态 |
| `StateNode::setMaterial()` / `setProgram()` | 子树 material / program |
| `RenderPass::setDepthRange()` / `depthRange()` / `clearDepthRange()` | 整条 pass 的深度带（`DepthRange`） |
| `resolveRenderState(authored, pass, topology)` | 唯一解析点（`StateResolution.hpp`） |

### 4.3 场景与收集

| 接口 | 作用 |
|---|---|
| `Scene::setRoot()` / `root()` / `clear()` | 内容根（已在别处的节点先被摘下） |
| `Scene::setVisible()` / `setOpacity()` | 场景级折叠 |
| `Scene::addLight()` / `lights()` / `setLighting()` | 灯 |
| `Scene::collectRenderCommands()` | 收集 |
| `Scene::boundingBox(mask)` | 可见内容盒 |
| `Scene::setContentFrame()` | memo 帧令牌 |
| `Scene::findNode(name)` | 按名字查找 |

### 4.4 拾取

| 接口 | 作用 |
|---|---|
| `Camera::screenToWorldRay()` | 屏幕 → 世界射线 |
| `RayIntersection::intersect()` | 单几何 |
| `RayIntersection::intersectScene()` / `intersectSceneAll()` | 场景 |
| `RayIntersectionResult` | hit / point / normal / distance / geometry / primitiveIndex / instanceIndex |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 场景图是严格树；`addChild` 自动脱离旧父并拒绝成环。 |
| C2 | 状态折叠是最近声明获胜；容器不做层掩码剪枝。 |
| C3 | 收集是纯函数；memo 由根子树 epoch 驱动。 |
| C4 | 灯在场景上：`Scene::lights()` 是唯一列表，没有节点灯，也没有逐 draw 的灯子集；灯值是世界坐标。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 选择/高亮/框选是业务，不放进框架。 |
| R2 | 拾取是机制：`Camera` 射线 + `RayIntersection`，不引入选中态。 |
| R3 | 包围盒/世界矩阵只有单父链上的一个答案。 |

### 5.3 不变量（N#）

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
| N9 | 根必须仍是根：`setRoot` 会摘下已在别的 Group 下的节点；根被 `Group::addChild` 收养后，该场景的收集与包围盒为空，且不复用收养前的 memo。 |
| N10 | 透明顺序是对象级近似：顺序键取代表点距离，不处理逐片元 / 自相交 / 同中心多层；依赖它之前先确认内容不重叠，否则用 `drawOrder` 或拆分几何。 |
| N11 | 层并集缓存用**进程级** epoch（`detail::g_layer_epoch`）：任一场景的层 / 结构编辑，会让所有场景下一次走查各自重算一次层并集。层编辑是作者期操作，重算只发生在下一次走查，陈旧缓存不会被用来误剪；若多文档同时编辑时走查耗时可见，改为每根子树 epoch（先记录测量再动，`G10`）。 |

## 6. 注意事项与陷阱

### 6.1 一事实一家

- 会怎么错：同一份状态被两处保存，改一处另一处过期。
- 怎么判：出现两份可写的值/两个可写的注册表。
- 怎么防：回到 §5 的 N#；只留一个 owner，其余只读。

### 6.2 后端/平台的边界

- 会怎么错：把平台差异写进引擎/应用层，换平台时到处分叉。
- 怎么判：出现“如果 Windows/X11 就……”的分支。
- 怎么防：平台事实留在后端，引擎只消费编译好的帧值。

### 6.3 透明分类的“顺序”冻结在命令表里

- 会怎么错：`RenderCommand::isTransparent()` 在**收集时**参与排序（F16），而 F14 的 memo 键里没有材质 revision——材质的 setter 只推进 `Material::revision()`，没有通往场景的公告通道（N3 讲的是节点侧）。所以运行时改 `Material::setAlphaMode()`（Opaque ↔ Blend/Mask）后，被 memo 命中的那张命令表**不会重新分批**：这条命令还留在原来的批次与位置，直到下一次图编辑（改位姿、增删节点、`Scene::invalidateContent()`）。
- 冻结的只有**顺序**。计划里的透明标志是每帧从活材质重新读出来的（`src/viz/graphics/src/backend/FrameRecorder.cpp` 的 `target.transparent = source.isTransparent()`），混合当帧就跟上；着色端的 alpha 模式随材质块 `flags`（位 8-9）在 revision 前进时重新打包。过期的是“它在不透明批还是透明批、排第几”。
- 怎么判：改 alphaMode 后 `Scene::contentCollectCount()` 不涨（说明走的 memo 命中），而重叠物体的前后遮挡关系仍按旧分类排列——典型表现是“刚变成透明的那个被画在了不透明批的位置”（先画、后被盖），或者半透明物体压在了它后面才该盖住的东西上。
- 怎么防：运行时切 alphaMode 之后调一次 `Scene::invalidateContent()`；或者把“先画/后画”表达在会公告的入口上（`Node::setOpacity`、`Node::setDrawOrder`、层）。只有一个 drawable 时无影响——没有别的命令可与它排错。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 多父 / DAG 场景图 | `worldMatrix()` / `boundingBox()` / 拾取身份失去唯一答案；重复子树复制节点即可。详见 [ADR](../decisions/ADR-2026-10-23-no-multi-parent-dag.md)。 |
| D2 | 选择态（选中集 / 描边 / 框选 / 多选）进框架 | 拾取是机制、选择是业务；行业里 picker 也是库。 |
| D3 | 在框架里内置 LOD / 空间划分 | 等“每帧收集本身成为瓶颈”的真实负载；见待办。 |
| D4 | 层掩码在容器上剪枝 | 最近声明获胜会让后代重声明变得不可达。 |
| D5 | 把状态折叠做成每叶上行查询 | O(depth) 且分配；自顶向下 `InheritedState` 是唯一实现。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `SceneTest` / `NodeTest` / `GroupTest` / `StateNodeTest` / `RayIntersectionTest`。 |
| T2 | `SceneContentCacheTest`：编辑一个场景不丢另一个场景的 memo。 |
| T3 | `SceneLayersTest` / `AutoTransformTest` / `PositionAttitudeTransformTest`。 |
| T4 | 移动子树移入视锥不被陈旧盒藏掉；层冲突只报一次。 |
| T5 | `SceneTest.SettingAParentedNodeAsRootDetachesIt` / `SceneTest.ARootAttachedElsewhereTakesTheSceneOutOfThePicture`。 |
| T6 | 透明顺序：局部坐标远离原点的两个透明物体，命令顺序与其包围盒远近一致（当前缺口见 `../bugs/graphics-transparent-sort-uses-origin.md`）。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

旧名/旧路径只在 §7 已否决选项或 git 历史里出现；新代码按 §4 的接口写。

## 10. 参考

`graphics-architecture.md` · `graphics-geometry.md` · `graphics-rendering.md` · `../backlog/graphics.md`
