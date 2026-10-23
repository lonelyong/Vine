# Vine 的对象模型 vs 3D 框架口径（2026-10-12）

> **2026-10-19 K1 改名**：本文中的 `Pipeline` / `PipelineOptions` 是当时的名字；现行 API 是
> `RenderPipeline` / `RenderPipelineOptions`（见 `graphics-followups-2026-10-19.md` §1.1）。

> **这是什么**：一次"对象设计合理性"复核的结论 —— 把 graphics SDK 的公开对象逐个对到三维框架的行业口径
> （VTK / OSG / three.js / Filament / Qt3D），说明哪些同构、哪些是**刻意偏离**（含代价）、哪些是**能力缺口**
> （含触发条件）。**不是**新提案：本文件不要求改任何东西，只把"我们像谁、不像谁、以后要补什么"写成一份
> 可查的对照。判据与坑见 `.ai/memory/graphics.md`；切片与裁决见 `graphics-render-next.md` §7、
> `graphics-window-device-viewer.md` §8/§9、`graphics-api-review-2026-10-12.md`。

## 0. 一句话结论

**零件齐全、口径一致，且站在"现代那半"**（声明式 pass + 帧值，同 Filament FrameGraph / Unreal RDG 那一派，
而不是 VTK 的"renderer 里塞 actor"）；两处刻意偏离（一区一幅画、相机带 aspect）是裁决过的、代价明确；
三处能力缺口（pass 内优先级、实例化、LOD/空间划分）都不是模型错，触发时按"机制进 SDK、组合归应用层"补。

## 1. 对照表

| Vine | VTK | OSG | three.js / Filament | 判定 |
| --- | --- | --- | --- | --- |
| `Scene` + `Node` / `Group` / `MatrixTransform` / `StateNode` / `Geometry` | `vtkRenderer` + `vtkActor` / `vtkMapper` | `osg::Group` / `MatrixTransform` / `StateSet` / `Geometry` | `Object3D` / `Mesh` | **同构**，最接近 OSG：状态沿节点**继承**（`StateNode::setMaterial`）且可**覆盖**（`Geometry::setMaterial`） |
| `Camera`（**自带 aspect 与投影**） | `vtkCamera` **不含 aspect**（由 renderer 的 viewport 现推） | `osg::Camera` 含投影、**也含 viewport** | `PerspectiveCamera` 含 aspect；Filament `View` 持有投影 | **偏 three.js / Unity 那派**（见 §2.2） |
| `SceneView`（相机 + 内容 + 操纵器 + **采纳的** `Pipeline`） | `vtkRenderer`（相机 + actor + viewport） | Viewer 与 Camera 的混合 | 应用自组 / Filament `View`（scene + camera + viewport + clear） | **同构 Filament 的 `View`**；我们把"谁画"显式拆出去（`Pipeline`），比 VTK 那代显式 |
| `RenderPass`（**声明**）+ `PassDesc`（值）+ `ScreenPass` | `vtkRenderPass` | 无对应（Camera + StateSet 拼） | FrameGraph pass | **同构现代派**（声明式；`PassDesc` 是值，可比较/可编译） |
| `Pipeline`（建好的 pass 链） | 无对应 | 无对应（RenderStage 由 Viewer 排） | FrameGraph 的**编译产物** | **同构 RDG / FrameGraph**；⚠️ 命名见 §3.4 |
| `RenderEngine`（pass 注册 + 一帧编译成**值** + 驱动 + 设备） | `vtkRenderWindow` 与 `vtkRenderer` 的混合 | `osgViewer::Viewer` | `Renderer` | **同构 Filament `Renderer`**，但**一引擎一块区**（§2.1） |
| `RenderCommand`（一次几何绘制指令） | `vtkMapper` + `vtkProp` | `osg::Drawable` | `Mesh` + `Material` | **同构 mapper 的现代写法**：内容产出命令（程序 / 材质 / 顶点通道自述），引擎编译成帧值 |
| `RenderSurface` + `PresentTarget` / `WindowPresentTarget` | `vtkRenderWindow`（内部 swapchain） | `GraphicsContext` + Window | canvas / Filament `SwapChain` | **同构**；且把"可呈递端"抽成两层（`PresentTarget` 是帧的契约，窗口是其中一个实现） |
| `RenderTarget`（**描述**）+ 后端按描述造图 | FBO / `vtkFrameBufferObject` | `osg::FrameBufferObject` | `WebGLRenderTarget` | **同构**（描述与 GPU 资源分离，比 OSG 那代干净） |
| `Material` + `StateNode`（可继承）+ pass 覆盖 | `vtkProperty` | `osg::StateSet` | `MaterialInstance` | **同构**（状态单点解析 + 覆盖） |
| `AxisGizmo` / `FpsOverlay`（**内容对象** + 一条 pass 声明） | `vtkOrientationMarkerWidget` / 2D actor | HUD camera | 应用自绘 | **同构 VTK 的 marker widget，但更正交**：不是 widget、不是 pass 子类 |
| `CameraManipulator` / `OrbitCameraManipulator` + 宿主输入 | `vtkInteractorStyle`（**交互器绑窗口**） | `osgGA::CameraManipulator` + event handler | `OrbitControls`（绑 DOM） | **同构 OSG**："操纵器是对象、输入归宿主"，正是嵌入 Qt 的现代做法 |
| `Light` / `LightRig` + 阴影 pass | `vtkLight` + shadow pass | `osg::Light` | `Light` | 同构；灯**属于场景**（不是面的） |

## 2. 刻意偏离（有裁决、有代价，别再当缺陷提）

### 2.1 一引擎一块区、一块区一幅画

VTK / OSG 是"一个 RenderWindow 里 N 个 Renderer / Viewport"。我们的等价物在**应用层**：
多 pass 各自给子视口（`RenderPass::setViewport`，设备像素）、多窗口 = 多引擎（一个设备可服务多个引擎）。
代价说清楚：想在窗口里放四格，VTK 那套是一行 API，我们得自己算矩形、自己决定哪条清屏（`setClearEnabled`）、
自己路由输入。**这是能力差距，不是模型错** —— 与"机制在底层、组合在应用层"的裁决同源；真需要时补的是
**应用层小库**（布局 + 每格一台相机 / 一个 pass），不是 SDK 政策。

### 2.2 相机自带 aspect 与投影

与 VTK 相反（VTK 的 aspect 由 renderer 的 viewport 现推、相机可被多个 renderer 复用），与 three.js / Unity 相同。
自洽的规则是"**一台相机一张画面**"：引擎会把"pass 的矩形 vs 相机的画面形状"不一致在编译期报出来。
代价：同一台相机进两块形状不同的矩形**永远不支持** —— 要复用就复制相机（Unity `Camera.rect` 是同一个坑）。

### 2.3 声明式 pass + 帧值（领先而非偏离）

一次 `executeFrame(FrameDescription)`，编译产物是**值**（`FrameDescription` / `FramePlan`），
后端只消费值。这是 Qt3D framegraph / Unreal RDG / Filament FrameGraph 那一派；
VTK 那代的"renderer 里挂 actor、顺序由遍历决定"是它要解决的问题。

### 2.4 HUD 是内容、可呈递端是两层

- HUD：`AxisGizmo` / `FpsOverlay` 是**内容对象**（scene + 取景相机 + 盒子），画它们的 pass 由配方声明 ——
  比"pass 子类"或"引擎级叠加层"都正交。
- 可呈递端：`PresentTarget`（尺寸 + present，帧的契约）与 `WindowPresentTarget`（加句柄两条）分开，
  于是"离屏 / 编码 / 网络出口"都是一个只实现两条动词的对象。

### 2.5 拾取是机制（已有）、选择是业务（不在框架里）

`Camera::screenToWorldRay`（收归一化屏幕坐标）+ `RayIntersection::intersectScene/intersectSceneAll` 就是拾取的全部机制 ——
"点中谁"这个问题 SDK 已经能答。**没给的是选择**：没有选中集合、没有描边/高亮 pass、没有框选、没有"渲染外框"的设备路径。

行业位置与我们一致：picker / intersector 在那边同样是**库**而不是核心 —— VTK `vtkCellPicker` 用的是同一套射线数学、
OSG 的 intersector 住在 `osgUtil`、three.js 是 `Raycaster` + 用户代码；Unity / Unreal 的 "selection outline" 也在编辑器层。
所以"选中态 + 长什么样"归业务（第一个调用点：`model_viewer` 的点击选中 + 换高亮材质，2026-10-13 落地，
判据见 `.ai/memory/appfw.md` 顶条）。**触发条件**（真内建时才评）：多处调用点各自重写同一套选择与高亮
（描边 pass / 框选 / 多选），或面板/命令需要"当前选择"这个跨模块读数。

## 3. 能力缺口（都不是模型错；写清触发条件）

| # | 缺口 | 行业对应 | 今天的替代 | 触发条件 |
| --- | --- | --- | --- | --- |
| 1 | **同 pass 内没有逐对象优先级 / 渲染队列** | three.js `renderOrder`、OSG render bin、VTK prop 优先级、Ogre render queue group + 子组（`Renderable::setRenderQueueSubGroup`） | pass 阶段（`PipelineStage`）+ pass 内按遍历序 + 透明内容走单独的 forward pass | **已落地（2026-10-13）**：`Node::setDrawOrder`（高=后画、子树继承、pass 内为**外键**，越得过透明批次）+ `RenderCommand::drawOrder`；判据与 4 条变异见 `.ai/memory/graphics.md` 顶条 |
| 2 | **没有实例化 / 批处理抽象** | `InstancedMesh` / `DrawIndexedInstanced` | 一条 `RenderCommand` 一个几何 | 同一几何 N 份（粒子、植被）。**已落地（2026-10-23）**：实例流挂在 `Primitive`（per-draw 参数）+ 命令快照 + `VINE_INSTANCED`，见 `graphics-instancing-primitive-2026-10-23.md`；早期“模型加一个字段”的通道方案与 `InstanceScope` 作用域方案均已作废。**与 `graphics-layers.md` 的 L2b（一次写完多层）同源，建议同一触发条件一起评**。**DAG 多父（R7 的另一半）同日裁决不做**：重复子树复制节点、共享 `Buffer`；任意多父需要路径身份（OSG/vsg 的 `NodePath`），不值 |
| 3 | **没有 LOD / 空间划分** | LOD 节点、八叉树 / 可见性集合 | `Node` 自带包围盒，引擎按相机做视锥剔除（移动相机不重算子树盒） | 大型场景（节点数或几何量到"每帧收集本身成为瓶颈"） |
| 4 | **命名风险：`Pipeline` ≠ PSO** | Unreal / Unity 的 "pipeline state" | — | **已关闭（2026-10-19，K1）**：公开类改名为 `RenderPipeline`（`PipelineOptions` → `RenderPipelineOptions`，见 `graphics-followups-2026-10-19.md` §1.1），名字不再与 `VkPipeline` / PSO 混读；`PipelineStage` 保留（它是"阶段"，不是 PSO） |
| 5 | **没有内容掩码**（哪些对象被这条 pass 画） | Unity Layer + `cullingMask`、Qt3D `QLayer`/`QLayerFilter`、OSG `NodeMask` + cull mask、Ogre-Next 三级掩码 | 拆成两个 `Scene` 或整棵子树 `setVisible` | **已落地（2026-10-12）**：`Node::setLayers` + `PassDesc::layer_mask` / `RenderPass::setLayerMask`（`graphics-layers.md` L1，判据与变异见其 §5）；拾取与包围盒两个读侧消费者于 2026-10-13 按掩码落地 |
| 6 | **没有数组/切片目标**（分层渲染） | Ogre-Next 的 slice/array 目标（`TextureGpu::getNumSlices`）、Vulkan layered rendering | 多个 2D 目标 + 多次 pass + 自己合成 | **已定形**：`graphics-layers.md` L2（先逐切片 L2a，再一次写完 L2b），触发条件写在那里 |
| 7 | **外部图像没有接线身份**（材质贴图 / 导入图 / cube map 不能作某条 pass 的输入或输出） | OSG `Texture` 挂 `StateSet`、three.js `Texture` 直接进 material、VTK 图像走 `vtkImageData` 输入端口 | （2026-10-19 前的状态，已被右列取代）只有“某 pass 产出的 `RenderTarget` + attachment”能接线（`ImageRef`），静态贴图只能走 `Material::setTexture` | **已落地（2026-10-19，E2 + cube 用例）**：`ImageInput` / `ImageSource` 让 pass 的输入声明可以是 produced 图或 imported `Texture`，content 与全屏两条绑定路径都能采样它，sampler state 随声明携带；D2 与 Cube 都有真实设备用例（`VsgBackendTest` 的 imported-texture 与 cube 两条）。**仍未做**：硬件 compare sampler；array 纹理类型不存在（分层目标是 `graphics-layers.md` L2）；见 `graphics-followups-2026-10-19.md` §2 |

## 4. 与既有文档的关系

- 行业对照的**逐轴**讨论（清屏归谁、framegraph、状态解析、能力查询）：`graphics-render-next.md` §7。
- 切片与裁决：`graphics-render-next.md` §5.1（T1–T7）、`graphics-window-device-viewer.md` §8/§9。
- 前一轮**骨架与抽象**的框架级复核（区/引擎/目标的所有权与基数）：`graphics-api-review-2026-10-12.md`。
- 多幅画面的**替代做法**（自己算矩形、pass 子视口、自己决定清屏）：`docs/guides/render-views.md` §6。
