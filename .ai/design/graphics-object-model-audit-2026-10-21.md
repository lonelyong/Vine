# graphics 对象模型 / 层级 / 架构复核与落地（2026-10-21）

> **这是什么**：对 graphics SDK（场景图 / 渲染对象 / 后端契约）的一次只读复核，加上本轮据此落地的两个切片。
> 复核的尺子是**行业口径**（OSG / vsg / VTK / three.js / Unity / Filament / bgfx）加**仓内已裁决的偏离**
> （一区一幅画、相机带 aspect、光在 Scene、单线程串行、`Lease` 借用、pass 顺序由声明给出），两者都不重开。
> 逐条论证在 §2：每条必须同时回答"业界同位置是谁"、"为什么它在这里是缺陷而不是取舍"、"动作与判据"。

## 0. 结论

- **层级本身不需要重设计**：`Object + RefCounted<T>` 是全仓统一的对象模型，`Object` 的虚析构让派生链删除安全；
  场景图是 OSG/vsg 谱系（去掉 Geode/Drawable，`Geometry` 即叶子）；渲染侧是声明式 pass + 帧值
  （Filament FrameGraph / Unreal RDG 那一派）。这些在 `graphics-object-model-vs-3d-frameworks.md` 已逐对象对过。
- **真正的问题分四类**：① 自己的契约执行不一致；② 后端契约的 API 中立性；③ 资产→场景这条纵向切片缺失；
  ④ 若干行业标配能力（TRS 节点、材质实例、MSAA、实例化）仍是有触发条件的缺口。
- 本轮只做"论证充分、判据清楚、可回滚"的两件：**B1 混合使能按契约执行**（①）与 **G1 资产→场景桥**（③）。
  G1 的**消费者**（`model_viewer` 改用模型树）随后一并落地（ **G1b**，见 §2/§3），否则"桥已就位但没人用"仍是半件。
  其余登记进 §5，含触发条件；其中一条（引擎 `device()` 命名）复核后**撤回**——它是 2026-10-11 的前向命名，
  不是缺陷（见 §2 D1）。

## 1. 复核覆盖

| 层 | 对象 | 对照的业界位置 |
| --- | --- | --- |
| 场景图 | `Node` / `Group` / `MatrixTransform` / `StateNode` / `Geometry` | `osg::Node` / `vsg::Group` / `three.js Object3D` |
| 内容 | `Scene` / `Light` / `Material` / `Texture` / `Camera` | `vtkRenderer` / `Filament Scene` + `Light` / `MaterialInstance` |
| 声明 | `PassDesc` / `RenderPass` / `ScreenPass` / `RenderPipeline` / `PipelineStage` | FrameGraph pass / `ScriptableRenderPass` / render bin |
| 帧 | `RenderCommand` / `PassDraw` / `FrameDescription` / `FramePlan` | mapper-drawable / RDG pass 值 |
| 设备 | `RenderDevice` / `RenderFrameExecutor` / `RenderBackend` / `PresentTarget` | `Filament Engine`+`SwapChain` / `osgViewer::Viewer` |
| 资源 | `RenderTarget` / `ImageRef` / `ImageSource` / `DepthView` | FBO / `WebGLRenderTarget` / 资源图边 |
| 视图 | `SceneView` / `CameraManipulator` / `InputRouter` | `Filament View` / `osgGA` |

静态门禁在复核时为全绿（include / export / doc / diagnostic / public-docs 五个脚本 0 finding），
所以下面找不到"语法层"的问题——都是语义与职责层面的。

## 2. 逐条论证

判定取值：**做**（本轮落地） / **登记**（有触发条件，本轮不动） / **撤回**（查证后不是缺陷）。

### B1 混合使能：`BlendState::enabled` 在主内容路径上无效 → **做**

- **事实**：`vn::graphics::BlendState::enabled` 默认为 false，`ResolvedRenderState::blend` 的注释写
  "Blending (default: disabled)"；而 vsg 后端对**单颜色附件的内容绘制恒开混合**
  （`StateCommands.cpp` 的 `const bool opaque = color_attachments > 1U || !draws_content;` 与
  `blendEnable = opaque ? VK_FALSE : VK_TRUE`）。`DynamicState::blend` 已经把 `enabled` 传到后端，只是被忽略。
- **业界同位置**：three.js `material.transparent=false` ⇒ 不混合（即使片元写 alpha<1）；Filament 由材质的
  `blendingMode` 决定；Unity 由 render queue（opaque/transparent）决定；OSG/vsg 由 `StateSet` 的 blend 属性决定。
  **没有一家是"开着一律混合"**。
- **为什么这里是缺陷**：一个公开 setter（`StateNode::setBlend`）的 `enabled=false` 是**静默无效**的；而
  `resolveRenderState` 是仓库自己定的"状态唯一解析点"，解析结果与实际下发不一致 = 契约不自洽。代价还有一条：
  不透明内容也付混合（移动端带宽），且把"程序自己写 alpha"当作隐式透明通道，与 `Material` / `Node` 文档
  声明的"opacity 是引擎唯一的透明通道"矛盾。
- **动作**：把"透明度"的两个来源在**唯一有权合并**的地方合并——后端半的 `FrameCompiler::resolveCommands`：
  `blend.enabled ||= (opacity < 1)`；`makeDynamicStateCommand` 改为按 `state.blend.enabled` 决定
  `blendEnable`。多附件（数据附件）与全屏绘制仍然恒不混合（既有规则，`SeveralColourAttachmentsAreDeliveredUnblended`
  已钉住）。
- **判据**：① 设备无关用例：一个 `opacity=0.5` 的命令 ⇒ `dynamic.blend.enabled == true`；一个未声明 blend 的
  不透明命令 + 一个 `StateNode` 显式声明 blend 的命令 ⇒ false / true。② 设备用例改写成一对：不透明内容
  **不混合**（片元 alpha 0.5 也被忽略，读回 1.0 红），内容显式 `setBlend` 后**混合**（读回 0.5 红/0.5 蓝）。

### G1 资产 → 场景断裂：`geometry::Model` 没有到 `Scene` 的桥 → **做**

- **事实**：`meshio` 的 `loadModel()` 产出 `geometry::Model`（节点树 + `Entry{mesh, material}`，材质分
  Color / Phong / Pbr 三种），但 **graphics 侧没有 `Model → Node` 的构造函数**，也**没有 `geometry::Material →
graphics::Material` 的桥**；`PbrMaterial` / `materialType()` 在整个 `src/`（除 geometry 自己）**零消费者**。
  唯一的生产消费者 `model_viewer` 直接取 `IndexedTriangleMesh`，配一份中灰材质，`Group`/`MatrixTransform`
  层次只有 `demo_plugin` 与 `test_plugin` 在用。
- **业界同位置**：`osgDB` 的插件产出 `osg::Node`（不是中间数据）；three.js `GLTFLoader` 产出 `Group` +
  `Mesh` + `Material`；Filament 的 `gltfio` 产出 `FilamentAsset`（Entity 树 + MaterialInstance）。
  **loader 的产物就是场景对象**，中间数据层只在需要"同一资产喂多个渲染器"时才存在——Vine 有 `Model` 是为了
  让 meshio 不把 assimp 拖进消费者（`meshio` 87.8 MB），这个理由成立；但那就必须补上"数据 → 场景"的最后一跳。
- **为什么这里是缺陷**：缺这一跳的直接后果是 SDK 的场景图层级、材质模型、多网格模型**没有任何生产调用点**，
  只有测试与 demo 在跑；而 `Model` 的结构（每节点局部矩阵、网格可被多节点共享）恰好就是场景图要的形状。
- **动作**：新增 `ModelScene.hpp` / `ModelScene.cpp`（graphics SDK 内，只依赖 geometry，不引入 assimp）：
  - `sceneFromModel(const geometry::Model&)` → `ModelScene{root, geometries, skipped_meshes, skipped_nodes}`：节点 `transform` 包 `MatrixTransform`（单位矩阵用 `Group`），
    每个 `Entry` 造一个 `Geometry`（顶点缓冲**共享**，节点不共享——图的严格树规则），材质按源指针缓存。
  - `materialFrom(const geometry::Material&)`：Color/Phong 直译；PBR 做**显式近似**（`diffuse = base*(1-metallic)`、
    高光强度 `1-roughness`、`opacity` 落到几何体透明度），并在注释里写明"PBR 近似，直到有 PBR 着色路径"。
  - 引用不到的 entry / 节点（空 mesh、越界索引、环）跳过并计数，不静默吞掉。
- **判据**：`ModelSceneTest.*` 8 条 —— 树形/名字/矩阵、同一 mesh 两处共享同一 buffer 句柄、三种材质映射与提交计数，
  以及四种坏输入（空模型、root 越界、节点环、entry 越界）；ASan 下全绿（`VINE_ASAN_TARGET=test_graphics
VINE_ASAN_FILTER='ModelSceneTest.*'` ⇒ RESULT: PASS）。

### G1b 消费者：`model_viewer` 改用模型树（同批落地）

- **事实**：桥落地后 `model_viewer` 仍走 `MeshLoader::load()`（摊平）+ 单网格文档 + 一份中灰材质。
- **业界同位置**：`osgDB`/`gltfio` 的消费者直接拿"场景节点/实体树"渲染；"加载即摊平"是它们专门避开的做法。
- **动作**：`ModelDocument` 改持 `geometry::Model`（内存载荷由第二个构造函数包成单节点模型）；文件打开器改
  `loadModel()` + `ensureSmoothNormals()`（只在来源缺法线时补平滑法线，索引不变 ⇒ 面板两个数不变）；
  `ModelRenderView::buildScene()` 改 `sceneFromModel()`，只给"没声明材质"的叶子补一份共享中灰材质，
  选择仍按叶子换/还材质（模型自带材质时还给原对象）。
- **判据**：`test_gui` 的 `ModelViewerTest.*` 10 条全绿（文件载荷仍读回源单位、点击仍选中并高亮、
  面板三个数（顶点/三角形/部件）不变、多部件模型在场景里是两个叶子且点击只选中被点中的那个、
  场景仍带自己的灯与帧率读数），`test_gui` 整体 261（260 + 1 skip）。

### D1 `RenderEngine::device()` 返回 `RenderBackend` → **撤回**

- **复核时的判断**：属性名说 device，类型是 `RenderBackend`（三面聚合），像是命名与类型不符。
- **查证后**：这是 2026-10-11 S10a 的**前向命名**——`graphics-window-device-viewer.md` 与
  `appfw-render-view.md` 都记录了"`setBackend`→`setDevice`、`backend()`→`device()`"，理由正是"今天唯一的一等设备
  就是这个聚合对象"，并写明 S10b（设备独立成对象）时才改成借用 `RenderDevice&`。**已裁决的偏离不重开**。
- **动作**：无。留给 S10b；本文件只记录"复核者一开始把它当缺陷，查证后撤回"，避免下一轮再报一次。

### N1 后端半与 SDK 半有两个同名类型族 → **做**（低风险部分）

- **事实**：`vn::graphics::FrameDescription`（引擎产出的帧值）与 `vn::graphics::backend::FrameDescription`
  （recorder 收集值）字段不同、含义不同；`vn::graphics::ClearPolicy` 与 `backend::ClearPolicy` 同样同名不同字段；
  而 `FrameCompiler::compile()` 收的是 **backend 那一个**。
- **业界同位置**：Filament 的 `FrameGraph`（声明）与 `CompiledFrameGraph`（产物）不同名；Unreal RDG 是
  `FRDGBuilder` / `FRDGPass` 两级命名；bgfx 用 `ViewId` + 提交表，也不复用同一名字。**同名不同物是阅读成本，
  不是风格**。
- **动作**：只改**内层**（backend 半）的两个名字：`FrameDescription` → `CollectedFrame`、`ClearPolicy` →
  `ClearValues`。外层保持稳定（它是宿主读的名字），`DrawDescription::Kind` 与 `backend::DrawKind` 保留（它们不是
  同名，且一个是嵌套枚举）。
- **判据**：编译通过 + 全仓搜索不再有 `backend::FrameDescription` / `backend::ClearPolicy`；`FrameCompiler` 的
  签名读起来不再需要问"哪个 FrameDescription"。

### 登记项（本轮不动，附触发条件）

| # | 事项 | 业界依据 | 触发条件 |
| --- | --- | --- | --- |
| R1 | `backend/` 半是"可移植后端契约"，但内容是 Vulkan 策略：`Keys` 的 `VkFormat`/`ImageLayout`、`TargetPlan` 的 subpass；测试夹具（`PhaseTable` / `AllocationGate` / `PixelProbe` / `DepthProbe`）随 SDK 安装。**`DeviceRequirements` 已于 2026-10-21 移入 vsg 插件**（Vulkan 1.4 地板 + `VK_EXT_*` 要求、`VK_VALIDATION_FEATURE` 之类只有 Vulkan 能用） | bgfx/Dawn 的后端契约是中立的，API 细节留在 backend 内 | 第二个后端（D3D12/Metal/WebGPU）出现；剩余部分（键里的 `VkFormat`/layout、证据夹具随 SDK 安装）同日登记，未动 |
| R2 | `RenderPipeline` 同时是"通用 pass 链"与"具体配方"（window/offscreen/composite/gizmo/fps 各自成员），第三种 overlay 要改类。**2026-10-22 已关闭**：`PassGroup` 收掉注册的第二份清单、`addOverlay` 让第三种 overlay 变成一次调用（剩下的"通用链 / 具体配方"两半即 `graphics-followups-2026-10-22.md` 的 F2 / F3，2026-10-22 晚均已关闭——R2 已无附属片） | Unity 用 `ScriptableRenderer` + `ScriptableRenderPass` 组合，overlay 是 pass 不是字段 | ~~第二个 overlay 类型出现~~（已触发，已做） |
| R3 | `Geometry` 没有 topology（在 `StateNode` 上），点云与三角面同子树要两个 `StateNode`。**2026-10-21 裁决：先不做**（详见下方"R3 裁决"） | OSG `Drawable` 的 `PrimitiveSet`、vsg `VertexIndexDraw`、glTF 的 `primitive.mode`、Unity 的 `MeshTopology`、Filament 的 `PrimitiveType` 都随 drawable | **同一子树需要两种图元**（点云/线 + 三角网格）**且调用点开始复制 `StateNode`**，或某个导入器要保留 `POINTS`/`LINES` 图元 |
| R4 | 变换只有 `MatrixTransform`（无 TRS 节点），`math` 已有 `Quaternion`/`Isometry3` | three.js `Object3D` 的 position/quaternion/scale、Unity `Transform`、OSG `MatrixTransform`+`PositionAttitudeTransform` | 逐帧驱动节点变换并需要插值（URDF 关节回放） |
| R5 | 材质是单一平铺 Phong + 一张贴图 + 采样器按 magic uniform 名；无 `MaterialInstance`；几何侧 `PbrMaterial` 到渲染侧只能是近似 | Filament `Material`/`MaterialInstance`、three.js `Material` 子类 + `uniforms` | 逐对象参数（高亮/tint）在第二个调用点出现，或需要真实 PBR |
| R6 | ~~无 MSAA：`TargetShape::samples` / `PipelineKey.samples` 存在但宿主无法设置，`Capabilities` 无 `max_samples`~~ **已关闭（2026-10-22）**：`RenderTarget::setSamples` 是唯一入口，采样数进目标形状 ⇒ 管线键 ⇒ 通道与图像；离屏目标自建多重采样图并**颜色与深度各解析一张单采样图**（`picture()` / `depthPicture()` 即"画面"，深度 `sample0` 不平均），R6b 后"MS 深度提升/出借/回读"全部合法，只留"MS 目标不能**借入**深度"一条显式拒绝；默认窗口仍单采样。`Capabilities::max_samples` 报设备限制 | 所有引擎都有 MSAA 开关 | ~~画面锯齿成为用户可见问题~~（用户 2026-10-22 直接指定做） |
| R7 | 无实例化、无 DAG（`Group::addChild` 自动脱离旧父） | OSG/vsg 允许一节点多父；`InstancedMesh` | 重复件数量让收集/内存成为瓶颈 |
| R8 | 无异步上传/编译入口（契约是单线程串行），大模型首帧卡顿 | Filament/bgfx 的异步资源上传、vsg 自己的异步编译 | 一次加载超过一帧预算 |
| R9 | 场景 memo 用进程级 `ContentEpoch`，任何场景的编辑会让所有场景失效 | — | 多视图/多场景下编辑频繁 |
| R10 | `OwnedFrame` 是零生产消费者的公开类型（仅自测）；保留理由：它是"帧可跨线程/跨进程搬运"的设计件 | Filament 的 `FrameGraph` 也有 owned 编译产物 | 远程渲染/录制真正开工时再决定去留 |

### R3 裁决（2026-10-21）：拓扑暂不归位到 `Geometry`

- **判断**：模型上拓扑应当随数据走（业界口径见 R3 行），Vine 现在把它放在会继承的 `StateNode` 上，属于"用状态表达
  数据事实"；继承的真实风险是 `StateNode{Points}` 的子树里混进三角网格时，后端按类编译 pipeline、无告警地按点
  画三角索引。
- **为什么先不做**：
  1. **没有生产触发点**：`geometry` 侧没有点云/线类型，导入器不产 `POINTS`/`LINES`；全仓唯一调用点是
     `demo_plugin`（`StateNode{Points}` 一行包裹）。
  2. **加了就多一种拼法**：`StateNode::setTopology` 必须保留作"子树默认拓扑"，于是同一事实有两种写法 —— 在没有
     第二个调用点之前，这比现状更差。
  3. **真正的缺口是数据模型**：只加叶子拓扑不会让点云出现，必须先有点云/线数据与导入器；两者应在同一次里决定。
- **做的那天**：`Geometry::topology()/setTopology()/clearTopology()`，解析规则 `叶子 > 最近 StateNode > Triangles`
  （与 material/program 同一条优先级）；`Scene::collectRenderCommands` 折叠后覆盖 `authored_state.topology`；
  后端零改动（`PipelineKey::topology` 与按类选半片已就位）；判据 = 同一子树里的点云与网格产生两条 pipeline，
  并去掉 `demo_plugin` 的 `StateNode` 包裹。

## 3. 本轮切片与验收（已落地 2026-10-21）
| 切片 | 内容 | 判据（实测） |
| --- | --- | --- |
| B1 | `FrameCompiler::resolveCommands` 合并 opacity；`makeDynamicStateCommand` 按解析状态下发；`BlendState` 文档改写 | `FrameCompilerTest.AnOpaqueCommandDoesNotBlendAndATranslucentOneDoes` 绿（不透明 / opacity 0.5 / 显式 StateNode 三条）；设备用例 `VsgBackendTest.AnOpaqueContentDrawDoesNotBlendAndATranslucentOneDoes` 绿（不透明读回饱和色、opacity 0.5 读回 0.5/0.5）；**变异 M1**：删掉 `opacity < 1` 合并 ⇒ 设备无关用例红（实测） |
| G1 | 新增 `ModelScene.hpp` / `ModelScene.cpp`：`sceneFromModel()` + `materialFrom()` | `test_graphics` **403**（398 + 5 条新增，全绿）：树形/名字/矩阵、同一 mesh 两处共享同一 buffer 句柄、Color/Phong/PBR 三种映射、四种坏输入（空模型 / root 越界 / 节点环 / entry 越界）；ASan 专项 PASS；公开用法写进 `docs/guides/graphics.md` §3.17 |
| G1b | `model_viewer` 改用桥：文档持 `Model`、打开器 `loadModel` + 补法线、视图 `sceneFromModel`；面板多一行"部件数"、选择按部件 | `ModelViewerTest.*` 10 条全绿（含多部件场景 + 逐部件选中）；`test_gui` 261（260 + 1 skip） |
| N1 | backend 半两个类型改名 | 全量构建 0 error；`grep` 不再有 `backend::FrameDescription` / `core::ClearPolicy`；test_vsg **481（480 + 1 skip）** |

总门禁：test_graphics 398、test_vsg 481（480 + 1 skip）、test_gui 260（259 + 1 skip）、
`check_{include_hygiene,doc_symbols,export_annotations,diagnostic_formats,public_docs,ai_docs}.py` 均 0 finding。

**坑（记给下一次）**：设备读回的像素是 BGR 字节序，断言 1.0 这种"只有一个通道非零"的值必须用现成的 `isRed()`，
不能读 `pixel[0]`；改名 `backend::ClearPolicy` 后**必须全量构建**，否则旧插件 `.so` 在加载时
`undefined symbol ...setClearPolicy`（目标构建不会重建它）。

## 4. 与既有文档的关系

- 行业对照的逐轴讨论：`graphics-object-model-vs-3d-frameworks.md`（本文件只在它之上补"这一轮怎么处置"）。
- 状态解析的唯一实现：`graphics-state.md` §4（B1 改的是它的下游执行，不是表本身）。
- 后端半的定位（为什么它叫 `backend/` 而不叫 `detail/`）：`graphics-layering.md`（R1 是它的代价）。
- 骨架与所有权：`graphics-skeleton-review-2026-10-20.md`（D1 的 S10b 出处）。
- 面向使用者的那一跳（`sceneFromModel` 的用法与三条不做的事）：`docs/guides/graphics.md` §3.17。
