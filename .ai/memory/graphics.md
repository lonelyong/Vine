# graphics / vsg 后端 —— 现役要点

只记**现役**：机制、判据、坑，以及"当时为什么这么定"。过程叙述与重写前那一代（`VsgRenderer`/`SceneBridge`/
`VsgContentSlot`/`OwnedCache` 等）已删除（2026-09-27 清掉 1839 行），历史见 git；分层与依赖方向见
`.ai/design/graphics-layering.md`，逐条实施记录见 `.ai/design/vsg-reimplementation.md`。

**读法**：每条是"一天一片"的记录，条目之间互相引用（`见 M11j`／`收掉 M3d-2`／`§11.16xx`），按时间倒序排。
**写新条目时**：结论在前、判据（门禁数字/变异 N/N 红）与坑不可省；出现已不存在的东西时——句子本身在讲
"旧名/已删"就加 `<!-- drift-ok -->`，整节都是记录就把标题标成 `（历史登记）`（`scripts/check_ai_docs.py` 会查）。

## 索引（按主题找，条目按时间倒序在下面）

| 主题 | 先看这些条目 |
|---|---|
| **模块布局与依赖** | 顶部 2026-09-27 四条：帧机器归属 `core`→`backend` / 诊断路线合并 / 帧时钟查询面 `FrameProgress` / 放置判据与 `frame/` 最终形态；规则全文在 `.ai/design/graphics-layering.md` |
| **一帧怎么走** | M11h 空绘制调用不是绘制调用 · M11m episode 归半片 · M11o 拒绝报告合并 · M11w 块预算按需增长 · M11x B5 触发规模 · M11i 租约顺序相位 · B8 跨帧差异 · B9 在飞槽数地板 |
| **着色器与 ABI 契约** | shader 文件命名 `builtin_<角色>.<阶段>` · `vn_`→`vine_` 前缀 · texcoord kind 显式化 · P0.A/P0.B1/P0.C1（前向着色归 SDK / location 表 / L1 块布局） · P12 文件化 + 构建期嵌入 · program 槽 P1 三步 |
| **证据与门禁** | M7 每帧编译 + 描述符（`PassDesc`）· M8 SDK 级设备夹具 + 像素判据（`SdkPixelTest`） · M11k 背景看写掩码 · M11l 第三条画面判据"整片背景灰" · FPS 窗口真平均 · ASan 两片（余量收口 / 全量 test_vsg 首通） · V3/V4 上游能力门禁 · R5 LSan 抑制 · M11g 计数分配器（**`skipped == 0` 才是跑过设备电池**） · 2026-10-11 修：G-buffer 几何 pass 缺 `program_override`（像素判据抓到，见顶条） · **2026-10-12 两条"已知红"清零（`test_vsg` 429 = 426 + 3 skip，见顶条）** |
| **宿主窗口与 X11** | M11j 读窗口偶发红（VIEWABLE 之前呈递到不了 / 空帧不是 settle / 读错误 0 vs 8） · M11ae·M11af 首帧叠层与启动尺寸 |
| **SDK 与引擎形态** | 后端契约（规范性在 `RenderBackend.hpp`） · Design C（RenderEngine 瘦身 + SceneView） · Scene 收敛单根 · Material 透明度移除 · 拓扑归位 · State 切片 · 边界框 `Aabbd` · **2026-10-22（下午）`PassChain` 自己就是注册归属（`PassGroup` 已删）** · **2026-10-22（夜二）TRS 节点 `AutoTransform`（R4 关闭）** |
| **面向使用者** | `docs/guides/graphics.md`（模块文档一节） |
| **对象模型与行业口径** | `.ai/design/graphics-object-model-vs-3d-frameworks.md`（2026-10-12）：逐个对象对到 VTK / OSG / three.js / Filament / Qt3D；**刻意偏离**（一区一幅画、相机带 aspect，含代价；**拾取是机制、选择不在框架（§2.5，2026-10-13）**）与**能力缺口**（同 pass 内优先级 / 实例化 / LOD·空间划分 / `Pipeline` ≠ PSO 的命名风险 / 内容掩码 / 目标层），各带触发条件；后两条的定形见 `graphics-layers.md`（L1 内容掩码 `Node::setLayers` + `PassDesc::layer_mask` / L2 目标层 `layerCount` + 层区间，L2 分两步、读侧先行；**§9（2026-10-13：拾取 / 包围盒掩码参数已落地；"子树层并集"整段跳过仍登记）**） |
| **重新设计（含后端契约）** | `.ai/design/graphics-redesign.md`（2026-10-11 提案）：病根 = 帧在引擎里已是值、边界上又被打散成一串有顺序的可变调用；**六片全部落地**：S1 / A / S2 前半 / S3（前半：帧由值驱动；之一：`executeFrame`；之二：预热删除；**收口：平坦路径与 `Protocol` 一起删除**）/ **S4b（窗口成为可选依赖：`surface()` + `RenderSurfaceBinding`）** / **S5（能力与观测各收成一个值：`Capabilities` / `FrameReport`）** / **S6（宿主侧：尺寸唯一 owner + `SceneView` 六角色 + 呈现知识归表）** / **S7（用户点名的收口：配方归 `Pipeline`，视图只采纳 —— 删掉 `ensureWindowPass` 那一层）** |
| **窗口 / 设备 / 多视图（下一轴）** | `.ai/design/graphics-window-device-viewer.md`（2026-10-11 提案）：**S8（appfw 的 `Viewer` 管 N 个 `View`）、S9（句柄与尺寸归区）与 S10a（三张契约 + 目标由设备造）已落地**，见下；**2026-10-11 晚裁决推翻 S8 一半：多格下线，一块区只画一幅画**（见顶条与 Q5 的后续裁决）；**2026-10-12 又两条**：呈递端拆成 `PresentTarget`（帧的契约）+ `WindowPresentTarget`（窗口那一面，见顶条），命名仍否掉 `RenderTargetWindow` / `RenderTargetOffscreen` 一对（`RenderTarget` 是描述、不是同族，理由在 §8）、云渲染的落点是"**第二个呈递端**"而不是把区通用化（§8 末条 + §9 第 6 条，带触发条件）；**§9.7（共享设备的 initialize/shutdown 归谁）已于 2026-10-18 裁定并落地：形状②——会话归创建者、引擎只借用设备（见顶条）**；**S10b 的 1:N、离屏会话、执行器自持帧尾与 `DeviceDesc{presentation}` 创建输入均已落地（2026-10-18/19/20）**，S10b 的登记项已清空；S11（登记不做）未落地 |
| **待办与口径** | **现行登记：`.ai/design/graphics-followups-2026-10-22.md` 已无开放行**（F1/F2/F3 全部关闭，2026-10-22 晚）；活账本回到 **`.ai/design/graphics-followups-2026-10-19.md` §3 触发条件型**，外加对象模型复核（`graphics-object-model-audit-2026-10-21.md`）的 R1/R5–R10（R4 于 2026-10-22 晚关闭）。10-19 是历史盘点（§2 现在可做：空 / §3 触发条件型 / §4 明确不做 / §5 不再是待办），它的触发项仍然有效。底层两个来源：**2026-10-11 按"机制/组合"重评的待办清单**（`graphics-render-next.md` §5.1：**T1 表面尺寸扇出（已落地）** / T2 M6（**已由 S10b 覆盖并关闭，2026-10-19**：一设备 N 窗口、每窗口一个目标，见 `graphics-window-device-viewer.md` §7）/ **T3 设备夹具探测（已落地）** / T4 布局归应用层 / T5 组合归应用层 / T6 便利边界 / T7 叠层跨重建（**2026-10-12 复核：不做**））；这两条的落地记录见下面 2026-10-11 T1+T3 那条 |


## 2026-10-22（夜二）R4 关闭：`AutoTransform` 是 TRS 节点，不是 OSG 的 billboard

- **形状**：`AutoTransform : Group` 持 `Point3d position_` / `Quatd rotation_` / `Vec3d scale_`（默认零 /
  单位四元数 / 1）；每次 set 重算 `T * R * S` 并 `invalidateBounds()`（围盒与收集记忆体一起失效）。
  `matrix()` 是只读合成结果；`transform()` / `setTransform(Isometry3d)` 只换平移 + 旋转，缩放另存。
- **为什么并列而不继承 `MatrixTransform`**：矩阵是父类的唯一事实；子类若继承 `setMatrix`，同一位置会出现
  “属性说一套、矩阵说一套”两种真相。两个 `Group` 子类各自完整。
- **命名坑**：OSG 的 `AutoTransform` 是面向相机的 billboard；这里的 “auto” 只指矩阵随三个属性自动合成。
  头文件与用户指南 §3.22 都写明。要 billboard 是另一条（命令收集目前视图无关，把相机放进去要另设计）。
- **判据**：`AutoTransformTest.*` 5 条 —— `ComposesTranslationRotationAndScale`（`T*R*S` 数值）、
  `SetTransformTakesAnIsometryAndLeavesScaleAlone`、`NestedTransformsAccumulate`、
  `WorldBoxFollowsTheProperties`（围盒跟着属性走）、`AnEditInvalidatesTheCollectedCommands`
  （收集记忆体失效）。`test_graphics` 414 → **419**，全绿。

## 2026-10-22（夜）R3 收口：一条几何体三条图元的**端到端**证据，顺带修好点图元

- **用例**：`PrimitiveDrawTest.OneGeometryWithThreePrimitivesDrawsThreePartsFromOneUpload`（`tests/test_vsg`）。
  10 个顶点 + 一个 12 索引的 arena，三条 primitive = 三角形（红，左三分之一）/ 线（绿，中间一列）/ 点（蓝，
  右三分之一四颗）；`StateNode` 材质故意给白色当陷阱（图元材质被丢 ⇒ 全白 ⇒ 三段计数一起归零）。
- **判据**（从"按构造"变成"测量"）：像素 `red=1334 / green=58 / blue=4`；`FrameReport::counters` 是
  `passes=1, draws=1`（`draws` 数是**绘制调用**不是图元：一个几何体仍是一次调用，三条图元是这次调用里的
  三条命令）；`StreamUploads` 首帧 `uploads=3 / aliases=6 / live=3`、第二帧 `uploads` 不变而 `aliases` 再涨 9
  ⇒ 一份顶点、N 条 draw、每条流只传一次。
- **坑（这条用例抓到的真 bug）**：`Topology::Points` + 内置前向程序 = 4 条
  `VUID-vkCmdDrawIndexed-primitiveTopology-10748`——POINT_LIST 要求顶点阶段**静态写 `gl_PointSize`**
  （`maintenance5` 没开时）。修在源头：`builtin_forward.vert` / `builtin_gbuffer.vert` 写 `gl_PointSize = 1.0`
  （Vulkan 默认值，三角形无影响）；`Topology.hpp` 与指南 §3.21 写明"点是程序与数据双方的契约，要大于 1 像素
  就自备程序"。自建程序忘写时 Vulkan 在**建管线**就报 08773，后端不必反射 SPIR-V。
- **顺手**：`VINE_SKIP_WITHOUT_SDK_DEVICE` 从 `SdkPixelTest.cpp` 移进 `SdkPixelFixture.hpp`，设备用例跨文件共用
  同一道门（三道检查漏一道就会把"没有设备"报成"功能坏了"）。
- 判据：`test_vsg` 482（+1）、`test_graphics` 414、ctest 24/24、网关每阶段干净（0 VUID、0 hazard）。

## 2026-10-22（晚）R3 第一片：拓扑归图元，几何体只剩数据

- **形状**：`Geometry` = 通道集；`Primitive` = `topology` + `BufferSlice<uint32_t> indices` + `material`
  + `program`；`StateNode` 不再有拓扑（`RenderState::topology` 与 origin 的 `Content|Default` 两档一起
  删），`PassRenderState::topology` 是"整片当点看"的视图覆盖（origin 报 `Data` / `Pass`）；
  `resolveRenderState(authored, pass, data_topology)` 第三个参数是数据的话。
- **数据即绘制**：`Geometry::addBuffer` 装**位置**通道时若还没有 primitive 就放一个默认的（三角形、
  非索引）——"设了数据却没画"结构上不可能；`setIndices/material/program` 保留为单 draw 便捷写法
  （读写 primitive 0；材质/程序写给所有），过渡项登记进 followups。
- **收集与计划**：`RenderCommand::primitive_index` + `primitive()`；Scene 一条 primitive 一条命令；
  `CollectedCommand`/`CompiledCommand` 带 `primitive`，前者还带 primitive 的 `topology`；拾取只打
  `Triangles` 图元，命中带 `primitiveIndex`。
- **后端（同日第二片）**：`GeometryFacts::primitives` 每 draw 一行（topology + 索引流 + span + 计数）；
  `ContentPass` 读 `command.primitive` 那一行（越界一次报告并跳过）；索引流仍归一化到整段 buffer ⇒ 同 arena
  的多个 primitive 共享一次上传；`channelsMatchLayout` 逐 primitive 判可否组装。判据
  `GeometryFactsTest.AMultiPrimitiveGeometryDescribesOneRowPerDraw`（三图元 ⇒ 三行、span 各归各的、
  同 arena 共享流身份、非索引用几何体顶点数）。
- 判据：`test_graphics` 414、`test_vsg` 480、ctest 24/24 绿；网关每阶段干净（单图元逐像素不变）。
  设计见 `.ai/design/graphics-primitives-2026-10-22.md`。


## 2026-10-22（晚）R6 MSAA：采样数进目标形状，颜色解析、深度不解析

- **规则**：`RenderTarget::setSamples(n)` 是唯一入口（0 当 1），值只走
  `TargetShape::samples` → `RenderPassCompatibility::samples` → 管线
  `MultisampleState` + 渲染通道附件描述。`HostTargets::facts()` 上行**宿主的**值
  （不再取已建目标的旧值），所以活目标换计数 ⇒ 计划答 `Rebuild` ⇒
  `VsgExecutor` 带新计数重建；`Capabilities::max_samples` = 颜色/深度两个
  framebuffer 计数上限的较小值，初始化时同时写进 `HostTargets`（一处来源）。
- **图像**：MS 颜色图只作 attachment；单采样 resolve 图（`COLOR_ATTACHMENT｜
  SAMPLED｜TRANSFER_SRC`）才是"画面"——`Attachments::Color::picture()` 是唯一
  定义，`capture()`/`probe()` 都走它，读回路径不分叉。MS 深度只给
  `DEPTH_STENCIL_ATTACHMENT`（这一轮不做深度解析）。
- **坑（会崩）**：帧缓冲视图顺序必须与通道附件列表一一对应（颜色…、深度?、
  resolve…）；漏了 resolve 视图 ⇒ lavapipe 段错误，没有诊断。`fillClearValues`
  的条目数也要按**附件数**补齐。
- **三条显式拒绝（不兜底）**：超 `max_samples`、`samples>1` 且
  `depth_promotion`、`samples>1` 且 `shareDepth` ⇒
  `HostTargets::State::SamplesUnsupported` + 一条按原因分岔的诊断；MS 目标的
  深度回读 ⇒ `ReadbackRefusal::Multisampled`（永久事实，先于格式/拷贝判定，
  但"没有深度"仍先答 `UnknownAttachment`）。
- **判据**：`MsaaTargetTest`（同一三角形同一 shader：1 样本 0 个混合像素、4 样本
  >0；深度拒绝；三种拒绝 + 普通 MS 深度能建的对照组；活目标换计数是重建）、
  `CoreReadbackTest.AMultisampledTargetIsReadableInColourAndRefusedInDepth`、
  `HostTargetsTest.TheSampleCountLimitStartsAtOneAndZeroMeansOne`、
  `FrameScheduleTest.ATargetsSampleCountTravelsWithItAndDefaultsToOne`。全仓
  ctest 24/24、网关全阶段干净（**1 样本像素零变化**）、6 个静态网关 0。
- **R6b（同日）：深度也解析**。MS 深度图只作 attachment，另一张单采样 resolve 图
  给 `TRANSFER_SRC[｜SAMPLED]`，子通道 `depthStencilResolveAttachments` +
  `depthResolveMode = sample0`（**不平均**：打开多重采样不改变深度值）。回读/借出/
  提升全指向 `depthPicture()`。于是"MS 目标不能提升/出借/回读深度"三条解除，
  `ReadbackState::multisampled` 与 `ReadbackRefusal::Multisampled`（含宿主侧）
  **一起删掉**——不留不会发生的理由。只剩一条：**MS 目标不能借入深度**（借到的是
  单采样解析图，一个帧缓冲的附件共用样本数），`HostTargets` 与 `OffscreenTarget::create`
  两处都拒。
- **坑（会崩）**：`Image::usage` 必须在**建视图之前**定好——vsg 在第一次
  `createImageView` 时才 `vkCreateImage`，事后 `|=` 不生效（实测：阴影图路径段错误、
  无诊断）。
- **有意没做**：resolve mode 的宿主词汇（min/max）、sample shading / alpha-to-
  coverage、交换链多重采样（归窗口后端）。见
  `.ai/design/graphics-msaa-2026-10-22.md`（§7 记 R6b）。


## 2026-10-22（下午）名字与注册归属一起收口：`PassChain`，`PassGroup` 删除

- **名字的裁决**：`ScenePicture` 被否（`Picture` 挨着 `Texture`/`RenderTarget` 会被读成"场景图片"）；
  `PassPipeline` 被否（重开 K1 关掉的 PSO 混读，`PipelineKey`/`VkPipeline` 还占着那个词）。取 **`PassChain`**
  —— 类注释与指南本来就写 "PASS CHAIN"/"一条 pass 链条"，改完那两段"这不是 PSO"的免责说明可以整段删。
- **改名（一次性公开面）**：`RenderPipeline`→`PassChain`、`RenderPipelineBuilder`→`PassChainBuilder`、
  `RenderPipelineOptions`→`ChainDesc`、`SceneView::setPipeline/pipeline`→`setChain/chain`、
  `checkPipelineContent`→`checkChainContent`；文件同名（`PassChain.hpp/.cpp`、`PassChainBuilder.hpp/.cpp`）。
  `PipelineStage`/`pipelineStageOrder`→`PassStage`/`passStageOrder`，并从链头挪进新叶子头 **`PassStage.hpp`**：
  `PassDesc.hpp` 不再反向 include 链头（文件顶部那条"为 `PipelineStage` include"的注释删除）。
- **`PassGroup` 删除（令牌折进链）**：引擎 `Slot.group(uint64)` → `Slot.owner(raw_ptr<Object>)`；
  `createPassGroup/removePassGroup/groupPass(es)` → `addPassOwnedBy/removePassesOwnedBy/pass(es)OwnedBy`。
  `PassChain : Object, RefCounted<PassChain>`，所以引擎能以擦除的引用记 owner（引擎从不调用它，只比较指针）；
  析构一句 `removePassesOwnedBy(this)`。存储仍然只有引擎那张槽位表，R2 不重开。
- **判据**：`ChainRegistrationTest.*` 四条（撤自己那批 + 外来 pass 不受影响 + role 查回同一结点；链/引擎
  两种死亡顺序；两条老 HUD 用例不改即绿；宿主第三条 overlay 不改类）。移动语义那条用例随令牌一起消失
  （链不可移动，`PassGroup` 那个"移动赋漏撤"的坑不再存在）。
- **一套数字**：全量构建 0 error；`test_graphics` 407、`test_vsg` 481、`test_gui` 261、`test_core` 165；
  6 个静态门禁 0 finding（`check_ai_docs --strict` 也 0，历史条目按惯例加 `<!-- drift-ok -->`）；
  `docs/guides/graphics.md` §3.18 从"注册归一个句柄"重写成"注册归链自己"。
- **仍开放**：无（F3 于当日晚关闭，见上条 2026-10-22（晚））；F1/F2 已关闭。
- **S2 落地（同日）**：引擎的三个窄句柄 `PassRegistry` / `OutputRegistry` / `DiagnosticsRoute`（各自带 `Lease`：
  引擎先死 ⇒ `isValid()` false，调用安全空转）；引擎加 `passes()` / `outputs()` / `diagnostics()`，原平铺方法
  （`addPass` / `publish` / `diagnosticCount` …）保留为同一表的简写——一套实现，两张门面。`PassChain` 不再持
  `Lease<RenderEngine>`，改持 `PassRegistry` + `DiagnosticsRoute`；诊断仍走 `reportEngineProblem`，所以编译期的
  episode 规则不变。
  判据：`ChainRegistrationTest.TheHandlesOutliveTheirEngineAndThenAnswerNothing`（三个句柄跨引擎死亡、report 空转）/
  `AChainRegistersThroughTheHandleItWasGiven`（链走句柄注册，引擎数同一张表）；`test_graphics` 409。
- **坑（S2 踩到）**：在 `VN_GRAPHICS_NS_BEGIN` 里写 `class Object;` 会声明一个**新的 `vn::graphics::Object`**，
  把 `vn::Object` 遮住（症状：一片 `base class has incomplete type`）。引用仓库的擦除基类要么 include
  `<vine/Object.hpp>` 并裸写 `Object`，要么写全 `vn::Object`。
- **S3 落地（同日）**：`PassChainBuilder` 删除。宿主入口是静态 **`PassChain::build(engine, desc)`**；
  `ChainDesc` 吸收 `camera` / `content` / `transparent`（原来 builder 的三个 setter）；`effectivePath` 成为
  `PassChain` 的静态。预设件搬到 **`ChainPresets.hpp` 的自由函数**（`defaultGbufferTarget` /
  `defaultGbufferGeometryProgram` / `defaultDeferredLightProgram` / `directionalShadowMatrix` /
  `addOffscreenToScreen`）。配方实现退成 `src/ChainBuilder.hpp` 里的私有 `detail::ChainBuilder`——公开类型
  少一个，实现一行没重写。判据：全量构建 0；四套件 409 / 481 / 261 / 165；6 门禁 0；指南 §3.3/§3.5/§3.10
  的示例改成值 + 静态 build。

## 2026-10-22（晚）调度上移：引擎是唯一的调度者，`backend::FrameGraph` 退场

- **调度进引擎**：派生层新单元 `src/viz/graphics/src/FrameScheduler.hpp/.cpp`（`detail::FrameScheduler`：迭代
  Tarjan 检环 + 堆就绪队列；所有表都是成员并跨帧保留，稳态帧零分配）。`EngineCompile.cpp` 的 `scheduleRows()`
  先按宣告建行（pass/content/order），再建边（`input_targets` → 写它的那条 pass；`declaration().inputs` 的
  ImageRef 按 inputName/outputName 配对；借用深度 `RenderTarget::sharedDepth()` 指回 lender），然后排序，
  `buildFrameValue()` 按 `frame_order_` 解析与发布。**`PassStage` 只是同优先级 tie-break**；注册位置只作历史记录
  ——跨引擎关闭/重开 pass 不再改变执行序。
- **环与跳过**：环组件整块不进 `frame_order_`，引擎报一次 `ContentSkipped`（`noteProblem` + 既有 episode 规则）；
  后端不再检环、`frame_.cycles` 恒 0、`invalid_schedules` 不再由编译器累加。
- **后端只剩物化**：`backend::FrameGraph`（`.hpp/.cpp`）、`FrameCompiler::graph()`、`FrameGraphTest`（文件已删）
  全删；`FrameCompiler::compile()` 按交来的 `description.passes` 顺序发布（`compiled` 按可服务数分配），
  `targets_` 仍按**宣告序**建表——计划的应用顺序与执行顺序是两件事，`applyTargetPlans` 自己按 lender-first 排。
- **踩到的坑之一（测试所有权）**：`PassDesc::camera` 是**拥有型** `intrusive_ptr<Camera>`。`FrameScheduleTest`
  里把栈上 `Camera` 的地址交进去 = 把栈对象交给引用计数，析构时 `double free or corruption (out)`；ASan 报
  `stack-use-after-scope in RefCounted<Camera>::release`。测试相机一律 `make_intrusive<Camera>()`。
- **踩到的坑之二（设备顺序）**：`DevicePhaseTest` 的 leased 相位直连 `FrameCompiler`，原本靠旧的 `FrameGraph`
  把 borrower 排到 lender 之后。删掉图以后 borrower 先跑会 `LOAD` 一张从未写过的借用深度 ⇒
  `VUID-vkCmdDraw-None-09600`（"expected DEPTH_STENCIL_ATTACHMENT_OPTIMAL, current UNDEFINED"）。改法是分开两件事：
  计划顺序仍 borrower-first（只编译、不提交 → 断言 `growth->targets[0]` 与两次 ResizeInPlace），设备证据用
  lender-first 的一帧提交。**规律：直连编译器的调用者自己负责执行顺序，borrower 永远不能先于 lender 提交。**
- **门禁**：`vsg_rewrite_gate.sh` suite 两跑 0 VUID / 0 SYNC-HAZARD、相位行全闭合；`app` 阶段内容 87.04%→85.14%、
  预览 244、背景 0%（基线记录一致）。`boot occupancy` 327–437 ms 超 150 ms 预算，**同一台机器同一方法在基线上
  测得 311 ms**（lavapipe 软件渲染），登记为环境项。一套数字：`test_graphics` 412（+3 `FrameScheduleTest`）、
  `test_vsg` 470（−11 `FrameGraphTest`）、`test_gui` 261、`test_core` 165；6 门禁 0。
- **文档**：`FrameDescription` / `FramePlan` / `FrameStats` / `RenderEngine::frameDescription` /
  `RenderFrameExecutor` 的"顺序只是优先级、执行器可重排"全部改成"交来的顺序就是执行顺序"；`framePlan()` 仍是
  **宣告序**（文档已写明它与执行序的区别）；`docs/guides/vsg-data-flow.md` §0/§2 与 `docs/guides/vsg-plugin.md` 的单元表
  去掉 `FrameGraph`。

## 2026-10-22（晚五）B6 收口：一个对象一条版本号（两级公告做完又整体回退）

- **两次尝试都比问题复杂**：①把折叠哈希（FNV-1a）当 `Geometry::contentRevision()` 公开 + `StreamKey` 加
  `buffer_revision`（提交 `a02b9500`）——功能成立，但 `revision` 在本仓库的语义是**单调计数器**（`setRevision`
  的 `@warning` 整段讲只升不降），哈希穿着 revision 的衣服；②把哈希藏成私有探测器、公开单调计数器——语义对了，
  但给 `Geometry` 加了可变状态 + 每帧每几何一次 O(通道) 检测，而它存在的唯一理由是"行键只能是一个数"。
- **共同的根（记住这条）**：**缓存键必须包含它要响应的每一个公告**。粗（几何）与细（buffer）是两个事实，一个数
  装两个只能折叠，折叠就是哈希。问题不在命名，在前提。
- **结论**：`a02b9500` 整体回退；规则收回成**一个对象一条版本号**——渲染器跟 `Geometry::revision()`（几何是一个
  描述，通道集是它的一部分），`Buffer::revision()` 只对"自己读这块字节"的消费者有效（几何的局部包围盒缓存就是，
  它两级都认，不动）。三处文档（`Geometry::revision()`、`Buffer.hpp`、`StreamKey`）改成同一口径并点名各自读者，
  宿主不再有一份"看着该生效、实际无效"的承诺。
- **将来真要省带宽**：形状记在 `graphics-geometry-revision-2026-10-22.md` §4——`ChannelFacts` 的标量拷贝改懒
  （上传 miss 时才读 `values->view()`），行键只需覆盖 layout（几何 revision 够），通道键 = `(buffer, 通道自己的
  revision)`，由 `Geometry::setChannelRevision(location, n)` 指明是哪条。**不需要派生版本号**。触发仍是"宿主报
  上传带宽"（16.98 MiB / +30 ms 是构造的形状，无人报过）。
- **B6 行**：登记表里改回开放（只留带宽那一半），并指向复盘。`test_vsg` 回到 470（三条新用例随回退删除）。

## 2026-10-22（晚六）revision 只由宿主显式公告——连"换了 buffer"也不自动

- **决定（用户）**：`Geometry::revision()` 只由 `setRevision` / `bumpRevision` 移动。结构变化（`setPositions(new)` /
  `addBuffer` / `removeBuffer` / `setIndices` / 换切片）**也不**自动 bump，理由不是"看不见"而是**批量**：宿主一次编辑
  常同时改多条通道，甚至先把新 buffer 装上再往里填，只有它知道这一批何时结束。公告 = 每批一次、由宿主发出。
- **代价写在 `setRevision` 的 `@details` 里**：忘喊就是静默的（渲染器继续画先传上去的那份）。换来的是公告点由宿主决定，
  而不是被 setter 的调用次序切碎。
- 所以三条输入各有归属：`Geometry::revision()` = 渲染器跟的那一条（粗、宿主写）；`Buffer::revision()` = "自己读这块
  字节"的消费者跟的那一条（几何局部包围盒缓存就是）；派生缓存不得自己发明版本号（两次尝试的教训见同日"晚五"条）。

## 2026-10-22（晚七）B6 定稿：形状归几何、字节归 buffer，**没有兜底**（逐通道重传落地）

- **规则**：`Buffer::setRevision()` 是**唯一**的重传公告——`StreamKey` 的 revision 只取该 buffer 自己的
  （`(buffer, buffer revision, 切片, kind/location/components)`）；`Geometry::setRevision()` 只描述形状
  （喂哪些通道、每条读哪段），驱动事实行（layout/计数）按 `(geometry, geometry.revision())` 重建，**不进上传键**。
  谁都没喊 ⇒ 字节视为没变（渲染器从不比较字节，也没有"几何 revision 兜底"）。共享 arena 被改 ⇒ 每个读者都跟着重传。
- **实现三条**：①键在录制时拼（`ChannelFacts::streamKey()` 读 live `values->revision()`，事实行不存 revision）；
  ②字节在上传时读（事实行只借 buffer 句柄 + 切片，`scalars()` 返回 live 视图；`StreamUploads` miss 时才建 vsg 数组）；
  ③结构变化靠身份（换 buffer / 换切片 ⇒ 新键，不需要任何 revision）。
- **实测**（`VsgBackendTest.MeasureWhatAMultiChannelMeshCostsWhenOneChannelChanges`，512²、4 通道 + 索引、整份 16.98 MiB）：
  稳态 0 上传/0 rebuild；单通道就地编辑 **1 上传/帧、0 rebuild**；换 buffer **1/帧 + 1 rebuild**；四通道就地编辑
  **4/帧、0 rebuild**。旧契约一律 **5/帧**。
- **契约变更**：`Geometry::setRevision()` 不再表示"字节变了"。`FpsOverlay` 这类"装新 buffer"的路径天然是新 stream；
  真正依赖旧语义的宿主需要改成 `buffer->setRevision()`（仓库内除测试外无此调用点）。
- **判据**：`tests/test_vsg/StreamRevisionTest.cpp` 5 条（含"几何公告不改变 stream 键"这条钉住没兜底）；
  变异：`streamKey()` 不读 live ⇒ 第 1、5 条红。设计/复盘：`graphics-geometry-revision-2026-10-22.md`。
- **记账**：B6 关闭（触发条件被用户 2026-10-22 直接跳过："框架问题，不要等"）。

## 2026-10-22（晚三）几何 revision：两级公告已经存在，但只有一个生效（B6 的契约定位）

- **发现（比登记里写的严）**：`StreamKey` 的字段注释写着 revision 来自 `Geometry::revision()` **或** `Buffer::revision()`，
  `Buffer.hpp` 也把 revision 定义成"缓存字节的人在用之前比一比"的契约，几何自己的局部包围盒缓存**两级都认**
  （键 = `(buffer, 该 buffer 的 revision, 段) + 几何 revision`，`ChannelSliceTest` / `GraphicsTest` 都钉着）；但
  `GeometryFacts.cpp` 把每个通道与索引流的 `key.revision` 都写成 `geometry.revision()`。⇒ 按 buffer 契约公告的宿主
  **静默拿到旧字节**，且任一道通道变动都重传全部通道（16.98 MiB / +30 ms）。
- **契约（已定，写进 `graphics-geometry-revision-2026-10-22.md`）**：两级都生效——buffer bump ⇒ 只重传读它的通道；
  几何 bump ⇒ "我没说是哪条"，全部（与今天同义）。派生一个只用于"事实表要不要重算"的 `Geometry::contentRevision()`
  = fold(自身 revision, 每通道 (buffer, buffer revision, 段))；**不**用作单通道上传键；派生通道（无 buffer）用它。
  代价：每次"内容变了"判断从 O(1) 变 O(通道数)。
- **状态**：S1（契约/落点/判据）已落地；S2（后端两行 + `contentRevision()` + 三条判据）待拍板。登记行 B6 已指向本文。

## 2026-10-22（晚二）门禁的启动占用预算按设备类别分档（F3 那条"环境红"的收口）

- **实测**：`[boot-timing]`（`src/app/src/main.cpp`，`VINE_BOOT_TIMING=1`）在本机 lavapipe 上是 304–437 ms；
  历史硬件基线是 74–92 ms（同步形状 414 ms）。把探针加了一句 `ending <N> ms after start` 后一眼看清：**一整段**
  从计时开始连续到 ~304 ms，覆盖"插件已加载 → 会话 up → 首次 Presenting → 启动框收起"——也就是说它测的是**等设备**
  的时长（软件光栅器慢 4 倍，同一个形状就读 4 倍），不是"活干错了线程"。
- **决定**：`scripts/vsg_rewrite_gate.sh` 的 `app` 阶段按 **ICD 类别**给预算——硬件 150 ms（不变），软件光栅器
  （`lvp_icd.json` / `lavapipe` / `swiftshader`）650 ms（本机最差值的 ~1.5 倍）；`VINE_GATE_BOOT_OCCUPANCY_MS`
  仍可覆盖。红行证据带上"那一段结束在启动后多少毫秒"，所以下次变红能直接对日志时间线说出是哪一段，不必重建探针。
- **诚实的边界**（写进了脚本文档）：软件设备上这一行只能挡**成倍**的回归（把等待变成工作），挡不住细微的；
  形状证明仍然在硬件机上。这不是放宽门禁，是把"硬件量的阈值"换成"设备类别的阈值"。
- 判据：完整跑 `scripts/vsg_rewrite_gate.sh` 全阶段绿；`VINE_GATE_BOOT_OCCUPANCY_MS=50` 强制红时证据可读
  （`... for 304 ms (it ended 304 ms after start), over the 50 ms allowed for a software rasteriser ...`）。

## 2026-10-22 注册归一个令牌：`PassGroup` + `presentingPass`（历史登记；当日下午已折进 `PassChain`，设计见 `.ai/design/graphics-frame-picture-view-redesign-2026-10-22.md` §9）

- **S2 落地（引擎仍是唯一注册表）**：新增 `vine/graphics/PassGroup.hpp/.cpp` —— 一个 **RAII 令牌**：`Slot` 加
  `group`（分组号）/`role`（名字），`RenderEngine` 加 `createPassGroup()` 与私有的
  `addPassToGroup/removePassGroup/groupPass(es)`。`RenderPipeline` 删掉 `passes_`（第二份"我注册了什么"），析构不再有
  for 循环（令牌走 = 那批 pass 撤下），两条 HUD pass 按 role 登记、`gizmoPass()/fpsOverlayPass()` 按 role 查回 ⇒
  `gizmo_pass_/fps_overlay_pass_` 两个字段消失。`role` 存在槽位上，成本是短字符串（"gizmo"/"stats" 落在 SSO 里，不分配）。
- **坑 1：`PassGroup` 的移动必须手写**。默认移动赋只覆盖 `engine_`/`id_`，**不撤原来那批** ⇒ 一次
  `group = PassGroup()` 就让两条 pass 永远留在帧里（`passCount()` 3 而不是 1）。现成用例当场抓到；现在的形状是移动赋先
  `release()` 再接管，源句柄 `id_ = 0`（不转移 = 两个主人各撤一遍）。
- **坑 2（比第一版 B1 更值钱）：插件 `.so` 不重建 ⇒ 用例假绿**。`ContentDrawTest.TheDynamicMappingFollowsTheEngineConventions`
  还写着 B1 **之前**的断言（"一个附件恒开混合"）——B1 那一轮各门禁都绿，是因为 `gfx_backend_vsg` 的 `.so` 没跟着重建；
  这次动 `RenderPipeline.hpp`（插件包含它）被迫重建，老账才现形。修法：断言改成交付的契约（不 opt-in ⇒ `blendEnable` 为 <!-- drift-ok -->
  false，opt-in 且默认因子 ⇒ 标准 alpha 对）。**教训**：改 `StateCommands`/`FrameCompiler` 这类"被插件实现、被 SDK 测试
  断言"的规则后，必须重建插件再跑 `test_vsg`，否则绿的是旧二进制。
- **S4 的改名部分**：`RenderPipeline::windowPass()` → `presentingPass()`（"呈现的那条"不一定是"画内容的那条"，延迟链尤其
  如此），`RenderSurface`/`SceneView`/demo/appfw 注释/用例/两份指南同步；`RenderPipeline` 自己的三参 `addPass` 删除（无人
  调用，注册只走 group）。
- **S3 落地（同轮补上，overlay 值化）**：`RenderPipeline::addOverlay(role, content, PassDesc, layout)` 是**公开扩展点**
  —— 配方里的 gizmo / 读数也走它；`overlayPass/overlayContent/setOverlayVisible/isOverlayVisible` 按 role 读写；
  `resize()` 只跑每条 overlay 自己的 `OverlayLayout`（`(pass, w, h, ratio)`），不再特判 gizmo / 读数；`gizmo_` /
  `fps_overlay_` 字段删除，类型化访问器成了 `obj_cast<AxisGizmo>(overlayContent(kGizmoRole))`。
  **纠正自己上一版的说法**：不需要新的类型擦除容器 —— 擦除的**引用**仓库里早有（`Object` 是公共基类，读侧
  `raw_ptr<Object>` + `obj_cast<T>`，`appfw::DocumentManager` 存 payload 就是这么做的），overlay 也不需要擦除的**拥有者**
  （内容由登记方的 layout lambda 捕获 `intrusive_ptr` + pass 的 `before_draw` 保有）。`intrusive_ptr<Object>` 仍然编不过
  （计数在 `RefCounted<T>` 里，不在 `Object` 上）。
  判据：`PassGroupTest.AHostRegistersAThirdOverlayWithoutTouchingThePipeline`（宿主注册第三条 overlay：同一张引擎表、
  自己的 role、自己的 layout、`resize` 里被调用、`setOverlayVisible` 只影响这一条、丢管线后 pass 表清零），加现有
  Gizmo / FPS 用例不改即绿；`test_graphics` 407。
- **判据**：`test_graphics` 的 `PassGroupTest.*` 四条（撤掉自己那批 + 外来 pass 不受影响 + role 查回同一结点；引擎先
  死 ⇒ `isValid()` false / `passes()` 空 / `add()` null；管线按 role 找到两条 HUD pass 且丢管线后 `passCount() == 0`）；
  两条老 HUD 用例不改即绿（`TheBuildOptionsDecideWhichOverlayPassesExist`、`OverlayVisibilityIsASwitchThatNeverRebuilds`）；
  `test_graphics` 406、`test_vsg` 481、`test_gui` 261、`test_core` 165；`PassGroupTest.*` ASan RESULT: PASS；6 个静态脚本
  0 finding。
- **明确推迟**（带触发条件，见设计 §9 末与 `.ai/design/graphics-followups-2026-10-22.md`）：引擎三门面
  （S1 / F1，等第二个门面消费者）、`ScenePicture` 全库改名与归属（S4 的另一半 / F2，与"注册 vs 画面"的拆分同轮做）、
  调度上移（S5 / F3，唯一改像素的一片，单独一轮走 `vsg_rewrite_gate.sh`）。`OverlayContent` 接口**取消**（不再推迟）：
  overlay 已是"值 + role + layout"。

## 2026-10-21 对象模型复核落地：混合使能归位 + 资产→场景桥（设计见 `.ai/design/graphics-object-model-audit-2026-10-21.md`）

- **B1 混合使能按契约执行**：`ResolvedRenderState::blend` 写着 "default: disabled"，而 vsg 对单附件内容绘制**恒开混合**
  （`StateCommands.cpp` 的 `opaque` 规则）⇒ 公开 setter `StateNode::setBlend(enabled=false)` 静默无效。修法：两个透明
  来源在**唯一有权合并的地方**合并——`FrameCompiler::resolveCommands` 里 `opacity < 1` ⇒ `blend.enabled = true`；
  `makeDynamicStateCommand` 按 `state.blend.enabled` 下发 `blendEnable`（多附件仍恒不混合、全屏绘制仍恒不混合）。
  下发通道是 `VK_EXT_extended_dynamic_state3` 的 `vkCmdSetColorBlendEnableEXT`（早已接线，只是没被用来关）。
  判据：`FrameCompilerTest.AnOpaqueCommandDoesNotBlendAndATranslucentOneDoes`（不透明 / opacity 0.5 / 显式 StateNode
  三条）+ 设备用例 `VsgBackendTest.AnOpaqueContentDrawDoesNotBlendAndATranslucentOneDoes`（原"恒按 SRC_ALPHA 混合"那条改写）。
  **变异 M1**：删掉 opacity 合并 ⇒ 设备无关用例红（实测）。**坑**：设备读回是 BGR 字节序，1.0 的单通道断言要用 `isRed()`，
  不能写 `pixel[0]`。
- **G1 资产→场景桥**：`geometry::Model`（节点树 + `Entry{mesh, material}`）此前**零生产消费者**（`model_viewer` 只画单网格 +
  中灰），场景图层级与材质模型只有测试/demo 在跑。新增 `ModelScene.hpp`：`sceneFromModel()`（identity 变换 → `Group`，
  非 identity → `MatrixTransform`；每个 Entry 一个 `Geometry`，**顶点 buffer 共享、叶子不共享**——严格树；材质按源对象缓存）
  与 `materialFrom()`（Color/Phong 直译；**PBR 显式近似**：diffuse = base*(1-metallic)、高光 = mix(白, base, metallic)、
  强度 1-roughness、指数 1+(1-roughness)*127；`PbrMaterial::opacity` 落到叶子 `Geometry::setOpacity`）。
  判据：`test_graphics` 的 `ModelSceneTest.*` **8 条**（树形/共享 buffer/三种材质 + 空模型、root 越界、节点环、entry 越界
  四种坏输入；ASan 专项 RESULT: PASS），`test_graphics` 403。
- **G1b 消费者（同批）**：`model_viewer` 改用桥 —— `ModelDocument` 持 `geometry::Model`（内存载荷包成单节点模型）、
  文件打开器改 `loadModel()` + `ensureSmoothNormals()`（只补缺法线的网格，索引不变 ⇒ 面板两个数不变）、
  `ModelRenderView::buildScene()` 改 `sceneFromModel()` 并只给"没声明材质"的叶子补一份共享中灰；面板加一行
  **部件数**（`ModelDocument::partCount()` = 可画的 entry 数），选择本来就按叶子、现在有“多部件只选中被点中的那个”
  的用例。判据：`ModelViewerTest.*` 10 条全绿、`test_gui` **261**（260 + 1 skip）。
- **N1 后端半改名**（零行为）：`backend::FrameDescription` → `CollectedFrame`、`backend::ClearPolicy` → `ClearValues`
  （外层 `vn::graphics::FrameDescription` / `ClearPolicy` 不变）。**坑**：`FrameRecorder` 的符号变了，改名后必须**全量构建**，
  否则老插件 `.so` 报 `undefined symbol ...setClearPolicy`。
- **撤回的一条**：`RenderEngine::device()` 返回 `RenderBackend` 不是缺陷——S10a 的前向命名，S10b 才改借用 `RenderDevice&`
  （见 `graphics-window-device-viewer.md`），下一轮别再报。
- **R1 部分关闭（同批）**：`DeviceRequirements`（Vulkan 1.4 地板 + `VK_EXT_*` 要求）从 SDK 后端半**移入 vsg 插件**
  （`include/vine/vsg/internal/` + `src/internal/`，导出宏改 `VN_VSG_API`，命名空间仍是 `vn::graphics::backend`
  因为插件把它 alias 成 `core`）。剩余部分（`Keys` 里的 `VkFormat`/layout、证据夹具随 SDK 安装）仍登记。
  **坑**：库与插件的源列表都是 GLOB 且无 `CONFIGURE_DEPENDS` —— 移文件后要 `touch` 两个 CMakeLists 再构建，
  并且 `tests/test_vsg/CMakeLists.txt` 的 `SRC_FILE_LIST` 要手动加上新路径。
- **拓扑归位（R3）2026-10-21 裁决：先不做**——模型上拓扑该随数据走（glTF `primitive.mode` / Unity `MeshTopology` /
  OSG `PrimitiveSet` / Filament `PrimitiveType`），但当前没有生产触发点（无名点云/线类型、导入器不产 `POINTS`/`LINES`，
  唯一调用点是 demo 的 `StateNode{Points}`），且加了就会与 `StateNode::setTopology` 形成两种拼法。做的那天：
  `Geometry::topology()`（叶子 > 最近 StateNode > Triangles）+ 收集折叠时覆盖，后端零改动。
- 门禁：test_graphics **398**、test_vsg **481（480 + 1 skip）**、test_gui **260（259 + 1 skip）**、6 个静态脚本 0 finding。

## 2026-10-21 颜色值类型收口：`Color` 的文本与分序打包入口（接下面那条）

- **`Color::toString()` / `Color::parse()`**：**一种语法**（CSS hex：`#rgb[a]` / `#rrggbb[aa]`）—— 小写写出、两种大小写都收，
  **alpha 对只在非不透明时写**（`#ff0000` / `#ff0000fe`）⇒ 一个值只有一份文本；失败在 `std::optional` 里
  （没有哨兵值，也不靠注释）。**短式是同一语法的产出**（`#CCC` = `#CCCCCC`，`#C0F8` = `#CC00FF88`，逐位重复）；
  收 3 就必须收 4，否则是任意的洞。写侧**只有规范形式**（`#CCC` 读进来写出去是 `#cccccc`）。
  文本永远属于**编码那一半**：线性 0.5 写成 `#bc0000`（188），要线性值的文本先 `toColor()`；`Colorf` 刻意没有
  文本（文本必须先说空间）。
- **拒掉的邻居（有意）**：`0x` 前缀、裸数字、`rgba(...)`、三位 `#f00`、前后空白 —— 每一种都是同一个值的第二种
  拼法。十六进制数字手写 `hexDigit()`，**不走 `String::toInt(&ok, 16)`**：后者收符号 / 前导空白 / `0x` 前缀，
  正好是要拒的那几种；写侧复用 `String::hex(span)`。
- **分序打包入口**：`static constexpr Color fromArgb(0xAARRGGBB)`（Qt `ARGB32` / Win32 `ARGB`）与
  `fromBgr(0x00BBGGRR)`（Win32 `COLORREF`；**高字节忽略、结果不透明**）。**不收通用移位**，也**没有**
  `toArgb32()`/`toBgr()`（反方向无读者）。`static_assert` 钉住两者 `constexpr`。
- **坑 / 代价**：`std::optional<Color>` 当 `Color` 自己的成员函数返回类型，在 gcc 15 与 clang 22 上
  `-fsyntax-only` 都通过；但 `Color.hpp` 因此多了一条 `#include "String.hpp"`（与 `Uuid` / `Version` 同形）。
- **判据**：`ColorTest` 8 条（文本 4 + 拒绝 1 + 字节序 3），`test_core --gtest_filter='Color*'` **24 条全绿**。
- 设计记录：`.ai/design/graphics-colour-space.md` §9；待办总表 §3.1 **已清空**（§2 仍是空的，剩下的都是触发条件型）。

## 2026-10-21 颜色把空间进类型：`Colorf` 线性 / `Color` sRGB，清屏色不再是字节

- **形状**：两个类型各说一件事 —— `vn::Colorf` 是**浮点通道值**（0..1、HDR 可 >1），`vn::Color` 是
  **sRGB 编码的 8 位**（调色板 / PNG / `QColor` 给你的那种）。中间只有一对有名转换：
  `Color::toLinear()`（解码）/ `Colorf::toColor()`（编码 + 量化）。退场三个半真话：
  `Color::toColorf()`、`Colorf::fromColor()`、裸量化的 `Colorf::toColor()`。
- **定位（2026-10-21 修正）**：core 的类型注释只声明"**值 + 转换**"；**空间由消费方声明** ——
  graphics 的 `ClearPolicy` / `PassDesc::clear_color` / `RenderPass::setClearColor` 才写"交给引擎的颜色一律线性，
  编码只在硬件两头（D3）"。第一版把这条写进了 `Colorf` 的类注释，那是过度限定：同一个 `Colorf` 还被
  `geometry` / `robotics::io` / `loaders` 用来表示空间未声明的 URDF 颜色（见设计文档 §6/§7）。
- **alpha 不参与编码**（覆盖率，只做 /255 与 ×255）——最容易顺手写错的一行，混合与透明读回全看它。
- **反色（2026-10-21 用户定为“基础接口”后补上）**：一对拼法跟 math 类型一致 —— `invert()` 就地、`inverted()` 返回
  副本（副本由前者实现）。`Colorf` 补**线性光**（`1 - channel`），`Color` 补**编码样本** —— **两个数**
  （0.2 线性 → 0.8；#333333 的屏幕反色是 #cccccc = 0.6039 光）。判据：`ColorfTest.TheLinearComplementIsNotTheEncodedComplement`
  与两边的 `InvertMutatesInPlaceAndTheCopyAgrees`。
- **插值（2026-10-21 同批）**：`static Colorf lerp(from, to, t)` —— 静态对称（跟 `Quaternion::slerp` 同形），
  逐通道插值（对 graphics 就是线性光下的 `mix`）、alpha 一起插、`t` 不夹紧（越界外推）。判据
  `ColorfTest.LerpBlendsLightNotSamples`：黑白中点是 188（混编码样本会得 128）。
- **常用颜色预设（2026-10-21 同批）**：`Colorf::kTransparent/kWhite/kBlack/kRed/kGreen/kBlue`，与 `Color` 同名同义，
  `static const` + `.cpp` 定义（类内 `inline static constexpr` 自引用不合法）。这套可以两边共享是因为 **0 与 1 是
  sRGB 曲线的定点**——所以预设里**没有灰**（灰与空间有关）。生产读者：`AxisGizmo` / `FpsOverlay` / `DemoScene`
  的 ambient/specular 六处。
- **传输函数只有一份实现**：`src/base/core/src/ColorTransfer.hpp`（私有头，两方向各一个 inline）。sRGB 分段曲线，
  **不是** `pow(2.2)`（128 解码 0.2159 vs 0.2195 = 两个量化级）。
- **SDK 只剩一种作者颜色**：`PassDesc::clear_color` / `clear_extra_colors`、`RenderPass::clearColor()` /
  `setClearColor(const Colorf&, int)`、`ClearPolicy::color` / `extra_colors`、`FramePlan::Pass::clear_color`
  全部从 `Color` 改成 `Colorf`；后端 `announceClear` 不再 `/255`（线性浮点直接进 `core::ClearPolicy::color_value`）。
  默认清屏色写成 `Color(51,51,51,255).toLinear()`（≈0.0331）：保留"#333333"这个**原意**，同时只说一次空间。
- **修掉的真实缺陷**：以前 `Color(51,51,51)` 被 `/255` 当线性 0.2，在 sRGB 交换链上被硬件编码成 #7C7C7C ——
  背景**亮一个 gamma**。这一轮之后 demo 窗口背景是 #333333（可见变化，不是回归）。
- **报表**：`FramePlan` 的文本从 `clear=rgba(1,2,3,255)` 变成 `clear=linear(0.216,...)`——空间写进文本。
- **判据**：`ColorfTest` 4 条（256 值往返 / 曲线不是 2.2 / 越界夹紧 + 0.5→188 / alpha 不编码）、
  `RenderEngineTest.APassAnnouncesItsClearPolicyToTheBackend`（r/g/b 都是线性值）、
  `RenderEngineTest.ADeclaredPassClearsTheWayItsKindDoesUntilItSaysOtherwise`（默认 = `Color(51,51,51).toLinear()`）、
  `RenderEngineTest.FramePlanDescribesThePassesInTheOrderTheyWillRun`（文本）、
  **设备级** `VsgBackendTest.AClearColourIsAStoredLinearValueNotAnEncodedOne`（UNORM 离屏清 0.2159 读回 **55**；
  编码过会是 128）。
- **不做**（理由在 `.ai/design/graphics-colour-space.md` §4）：`UnitScalar`（无读者）、`RenderTarget::ColorFormat`
  加 sRGB 变体（设备格式已经承担身份，加一列只会出现第二个真相）、`Colorf→LinearColor` 改名（247+105 处，且
  geometry/robotics 用 `Colorf` 表示**空间未声明**的 URDF 颜色）、8 位图色彩空间（`imaging::PixelFormat` 已有）。
- 门禁：test_core 全绿 / test_graphics **395** / test_vsg **480**（新增 1 条）/ test_gui 259(1 skip) / 8 条静态脚本 0 finding。

## 2026-10-20 F2b 落地：内容记忆体跨帧存活（"内容纪元"），稳态收集 8.7 ms → 36 µs

- **机制**：`Node::announceContentChange()` 自增一个**进程级 content epoch**（relaxed atomic，见
  `src/ContentEpoch.hpp`）。`Scene::collectRenderCommands` 比对 epoch：变了就清空记忆体，然后再按
  (revision, eye, view-proj, mask) 命中。`setContentFrame()` 从"换 token 就清空"改成"老化上一帧没人用过的
  条目"——记忆体因此被**实际在画的视图数**界定，不是帧数。
- **公告点**：`Node::invalidateBounds()`（顺带，移动的子树既改盒也改命令）、`Node::setVisible/setLayers/
  clearLayers/setDrawOrder/clearDrawOrder/setOpacity`、`StateNode` 的 15 个 mutator、
  `Geometry::setMaterial/setProgram/setIndices`。**自定义节点必须自己调**，这点写进了 Node 的注释。
- **实测**（调试构建，`MeasureWhatOneCollectionCosts*` 的 `per_frame`）：200 drawable 880 µs → **5 µs**；
  2000 drawable 8701 µs → **36 µs**。`moving` 列不变（相机每帧动本来就该走树）。
- **契约变化**：Scene 看不见的改动（直接写缓冲、自定义节点状态）现在必须调 `Scene::invalidateContent()`，
  否则永远不被收集看到（以前靠"记忆只活一帧"兜底）。
- **判据**：`SceneContentCacheTest.EveryMutationThatChangesACommandEndsTheMemo`（11 种改动逐个验证"改了就必须
  换一张表"）——它当场抓到 `Geometry::setMaterial`/`setProgram` **没有公告**（改了材质还是旧表），两条都补上了。
- 门禁：test_graphics **395** / test_vsg 479 / test_gui 259(1 skip) / 8 条静态脚本 0 finding。

## 2026-10-20 F2b 量化 + F3 关闭：数据流成本登记只剩一条

- **F2b 有了数**：`MeasureWhatOneCollectionCosts*` 多打印一列 `per_frame`（引擎自己的稳态：同一相机、同一内容、
  每帧换内容帧 token）。调试构建 **200 drawable = 880 µs/帧、2000 = 8.7 ms/帧**；`still` 那一列仍是 1 µs
  （同一帧内记忆命中）。差的就是那次遍历——**记忆体只活一帧，所以静态场景每帧都付**。
- **为什么不当场改**：跨帧复用要回答"节点被直接改动时谁通知 Scene"（今天靠记忆只活一帧保证下一帧看见；改成
  跨帧，动画会冻住）。两种设计写进 review §8.1：**A 全局变更纪元**（几十个 setter 都要 ++，漏一个就是静默画面
  错误，且任何场景改动作废所有记忆）、**B 节点回指 Scene**（一条新的 ownership 边）。二者互斥，等真实负载。
- **F3 关闭**：`DrawDescription::lights` 每 pass 每帧复制 N 个指针（N 个位数）。span over `intrusive_ptr<Light>`
  会把拥有式 `OwnedDraw::lights` 的元素类型一起拖进来；span over `LightRef` 要 `Scene` 维护一份镜像（同一事实
  两份存储，模块反复拒绝的形状）。**不做**，理由写在 review §8.2。
- 至此 graphics 的待办登记簿只剩 **F2b** 一条（带触发条件与两套设计）。

## 2026-10-20 F2a 收口：程序覆盖成为 pass 的事实（不再逐帧复制命令表）

- **形状**：`PassDraw::program_override`（null = 每条命令自己的程序）。`RenderPass::describe()` 只把它写进
  描述，`draw.commands` 永远是收集来的**共享不可变表**。后端在**记录时**解析：`VsgBackend::collectContentDraw(
  commands, camera, override)` → `FrameRecorder::render(..., override)` → `snapshotCommands()` 把每条命令的
  程序 identity 换成生效程序。计划、管线变体、程序表因此完全不知道"覆盖"存在。
- **与 M1 同一条路**：2026-10-19 把光栅化覆盖从"改写命令"改成"随 pass 状态走"。程序覆盖是同一件事的剩余
  一半；它不是 render state，所以有独立字段而不是进 `PassRenderState`。`StateNode.hpp` 里那段旧解释已改正。
- **契约变化（要记住）**：应用覆盖现在是**后端契约**的一部分，和 `PassDraw::state` 一样。SDK 不再替后端改写。
  vsg 与测试里的 mock 都已实现（mock 的 `consumeCommands` 现在收 override）。
- **判据**：`FrameRecorderTest.ThePassProgramOverrideIsResolvedWhenTheDrawIsRecorded`（记录的身份是生效程序、
  宿主表未被改写）、`PassDrawTest.WhatAScenePassDescribesIsWhatItAnnounces`、`RenderEngineTest.
  TheFrameValueIsEveryPassAndEveryDrawTheFrameRan`（后两条断言改成"覆盖在 pass 上、共享表未改写"）。
  门禁：test_graphics 394 / test_vsg **479** / test_gui 259(1 skip) / 8 条静态脚本 0 finding。
- 剩下两条数据流成本：**F2b**（内容记忆体按帧失效 ⇒ 静态场景每帧重走树；这是刻意的，节点被直接改动 Scene
  看不到）、**F3**（灯光列表每 pass 复制；改 span 需要镜像或改后端 `LightRef`）。

## 2026-10-20 抽象/概念/API/数据流复核：三组改名，零行为变化

- **口径**：名字相差一个词的两个类型 = 陷阱；类型名要说出真身份；一帧六跳每跳都要说清"谁拥有/借多久/复制几次"。
  全文：`.ai/design/graphics-abstraction-review-2026-10-20.md`。
- **改了什么**（全部零行为，编译器全量兜底）：
  - `PassDescription` → **`PassDraw`**（+ `OwnedPassDraw`、`FrameDescription::Pass::draw`、
    `OwnedPass::draw`、`PassDrawTest`）。和 `PassDesc`（声明）终于不是一个词的距离。
  - **`FrameContext` 整个删除**：它只有"表面尺寸"这一个**非每帧**事实，`dt` 只有一处写、零个读者
    （违反"不装旋钮"）。改成 `RenderEngine::surfaceWidth()/surfaceHeight()`；`frame(double dt = 0.0)` →
    `frame()`；`FrameContext.hpp` 下线。<!-- drift-ok -->
  - `Camera::setProjectionMatrixAsPerspective/Ortho` → **`setPerspective`/`setOrthographic`**（52 处），
    与类内 `ProjectionType{Perspective, Orthographic}` 同词。
- **成本点**（同一轮的后半段收掉了 F1）：**F1 已收口**——`Scene` 只剩一个
  `collectRenderCommands()` 返回共享不可变表，复制版删除（111 处测试调用点迁移成 `*list`）；
  F2a program override 每帧复制整表（正解：覆盖随 `PassDraw` 走、后端逐命令解析）、
  F2b 静态场景每帧重走一遍树（记忆体按帧失效是刻意的：节点被直接改动 Scene 看不到）、
  F3 灯光列表每 pass 每帧复制（改 span 需要镜像或改后端 `LightRef`）。
- **坑**：`check_ai_docs.py` 的漂移豁免是**精确字符串** `<!-- drift-ok -->`，写成 `<!-- drift-ok: 原因 -->`
  不算；被删掉的头文件名只要出现在反引号里就会被查。
- 门禁：test_graphics 394 / test_vsg 478 / test_gui 259(1 skip) / 8 条静态脚本 0 finding（`--strict` 也 0）。

## 2026-10-20 SK1：引擎拥有它的区（所有者矩阵最后一条"父不拥有子"的边）

- **形状**：`RenderEngine::createSurface()` 是唯一创建入口（第二次回 `nullptr` + 报一次
  `UnsupportedRequest`）；引擎持 `intrusive_ptr<RenderSurface> area_`；`RenderSurface` 构造转私有
  （`friend class RenderEngine`）。宿主 `SurfaceWindow` 那句从"造一个对象"变成"要一个句柄"。
- **删掉的协议**：`adoptArea` / `releaseArea`。区不再在构造/析构里回指引擎——引擎自己就是那个关系，
  `~RenderEngine` 先 `shutdown()`（解绑目标）、把 `area_` move 到一个局部、清租约、卸设备诊断路线，
  作用域结束时才释放区（此时租约为空）。**区永远不会看到半死的引擎**，宿主先销毁引擎也安全。
- **判据**：`RenderSurfaceTest.ASecondAreaIsAConstructionFailureNotASecondObject`（第二次/第三次
  `createSurface()` 都是 null、只报一次、既有的区照常呈现）、
  `ARefusedSecondAreaCannotReshapeTheFrame`。门禁：test_graphics 394 / test_vsg 478 /
  test_gui 259(1 skip) / 8 条静态脚本 0 finding。
- **坑**：把 `area_` 从 `raw_ptr` 换成 `intrusive_ptr` 后，`RenderEngine.hpp` 必须**完整包含**
  `RenderSurface.hpp`（否则 `intrusive_ptr_release` 在别的 TU 里找不到，报的是一串模板实例化错误）；
  `announceAreaSize` 里的比较要写成 `area_.get() != from`（`raw_ptr<const T>` 与 `intrusive_ptr<T>`
  没有比较运算符）。
- 这一轮之后 graphics 的登记簿**空了**（S1/S2/S3 + SK0/SK1/SK2/SK3 全落地）；剩下的都是带触发条件的
  能力项（实例化、LOD、分层目标、Windows 读像素等），见 `graphics-followups-2026-10-19.md` §3。

## 2026-10-20 S2 / SK3：纹理只剩 layer 词汇；视图采纳了画别的内容的链条会报一次

- **S2（纹理 API 单拼法）**：`Texture` 只留 `layerCount()` / `layer(i)` / `hasLayer(i)` /
  `setLayer(i, image)`；`faceCount` / `source(face)` / `hasSource` / `setSource` 全部退场。
  `CubeMap::setFaceImage`/`faceImage`（命名面）保留，内部就是 `setLayer`/`layer`——命名面是类型自带的糖，
  不是第二套编号。本地助手 `faceCountOf`→`layerCountOf`，vsg 的 `filledFaceCount`→`filledLayerCount`。
  判据：`TextureLayersTest.OneNumberedSpellingWalksAnyKind`（原"两种拼法一致"的用例改写）。
- **SK3（视图/链条不一致诊断）**：`RenderPipeline::draws(raw_ptr<const Scene>)` 问的是**整条链**
  （延迟链条在 G-buffer pass 画场景，窗口 pass 只呈现目标），`SceneView::checkPipelineContent()` 在
  `setPipeline` / `setScene` 各跑一次 + `ReportOnce`。`setScene` 会重绑窗口 pass 的内容，所以修好后自动
  re-arm。判据：`SceneViewTest.AChainBuiltForOtherContentIsReportedOnceAndIsANewEpisodeOnceTheWireIsRight`。
- **坑**：`test_vsg` 偶发"27 skipped + 6 failed"不是代码问题，是**设备被上一个进程占着**（前一轮刚跑完
  立刻再跑）。重跑即绿；看 `[... SKIPPED ]` 的数量就能认出来。
- 门禁：test_graphics 394 / test_vsg 478 / test_gui 259(1 skip) / 8 条静态脚本 0 finding。
- 剩余登记：**只剩 SK1**（区归引擎所有，动生命周期，单独一轮）。

## 2026-10-20 SK2 落地：一个 pass 声明只存一份（+ 两处每帧分配收口）

- **`RenderPass` 现在 IS 一份 `PassDesc`**：`declaration()` 读它、`setDesc()` 整体采纳它，24 个镜像字段
  全部退场，访问器是它的一行投影（多数内联）。`Slot` 从 9 个字段掉到 4 个（pass / order /
  record_micros / transient），content 回到声明里（`PassDesc::content`，`bindPassContent` 写穿）。
  `addPass(PassDesc)` 的 25 行逐字段抄写退场。判据：`test_graphics` 393 / `test_vsg` 478 /
  `test_gui` 259（1 skip）。
- **`PassDesc` 补了 `output`（`intrusive_ptr<ImageRef>`）**：之前值形式**无法**表达细粒度输出，
  `addPass(desc)` 会静默丢掉那条声明；现在采纳声明就带着它。这是"值形式与节点形式不一致"的最后一个洞。
- **kind 决定默认值**：`clear_enabled` / `clear_depth` 为 unset 时按 `PassKind` 回答（Scene 清、
  Screen 不清），`ScreenPass` 构造函数里那两句 `setClear*` 因此消失——默认值不再是"某个构造函数顺手设的"。
- **`setDesc()` 不清 depth 边**：采纳一份声明时只有**有效**的 depth view 会施加到目标；无效/缺席不动目标
  （`setDepthView(invalid)` 仍然会清——它是显式声明）。原来 `addPass` 对无效 view 走 `setDepthView` 会
  把别的 pass 建立的边清掉。判据：`DepthViewTest.TwoDifferentDepthViewsForOneTargetAreRefusedOnce`。
- **`clearInputs()` 收掉 `clearInputNames()`**：一个入口清 images + targets + names（"清一半"是个真陷阱）。
- **`Camera::setViewMatrixAsLookAt` → `lookAt(eye, center, up)`**，74 处调用点一次改完（与 S1 同批，
  一次公开变更窗口）。
- **两处每帧分配收掉**（模块规矩是"稳态帧不分配"）：`collectPlanProblems` 的
  `map/map/set/map/vector` 换成引擎成员扁平 vector（pass 数是个位数，线性查找；`clear()` 保容量）；
  `buildFrameValue` 按索引**复用** `FrameDescription::Pass`（`inputs.assign` 与已就位的
  `PassDraw` 都不再每帧重建），只有运行的 pass 变少时才 `resize` 收缩。
  `OutputIdentity` 因此补了 `operator==`。
- **坑**：`RenderPass::desc()` 拿不到——`VN_OBJECT_META_DECL` 已经提供了一个静态 `desc()`（元对象），
  所以访问器叫 `declaration()`。新加同名的"读声明"访问器前先查宏。
- 剩余登记（未做）：S2 `Texture` face/layer 双拼（60+ 处，等下一次碰纹理面）、SK1 区归引擎所有、
  SK3 视图/管线场景不一致诊断。

## 2026-10-20 骨架复核：先画图再动手（区对引擎的借用改 Lease）

- 全文与四张 mermaid 图：`.ai/design/graphics-skeleton-review-2026-10-20.md`。判据：全量构建 0 error；
  `test_graphics` **393**（+1 新用例）、`test_vsg` **478**；8 条门禁 0 finding。
- **本轮唯一落地的结构改动（SK0）**：`RenderSurface::engine_` 从 `raw_ptr<RenderEngine>` 改成
  `Lease<RenderEngine>`。原来 `引擎→区` 与 `区→引擎` 都是裸借用、一对一并由宿主当第三方保证顺序，
  而 `~RenderSurface()` **会调用** `engine_->releaseArea(this)` —— 宿主先销毁引擎就是 UAF。
  视图与管线 2026-10-19 已经换成 Lease（F5），**唯独区漏了**；现在三条向上边一套说法。
  每个用点改成 `engine_.get()` + 显式处理"引擎已消失"（不展示、不报诊断——诊断路线随引擎走了）。
  判据：`RenderSurfaceTest.AnAreaOutlivingItsEngineAnswersNothingInsteadOfCallingIntoIt`。
- **登记的三条切片（按收益÷风险）**：
  - ~~**SK1 区归引擎所有**~~ **已于 2026-10-20 落地**（见顶条）：`RenderEngine::createSurface()` +
    引擎持 `intrusive_ptr<RenderSurface> area_`，区构造转私有（`friend class RenderEngine`），
    `adoptArea`/`releaseArea` 两个回指协议整体删除。与 Filament `Engine`/`SwapChain`、
    O3DE `WindowContext` 同形。
  - **SK2 `RenderPass` 只存一份 `PassDesc`**（= followups §3.9 的 S1，最大的一处，单独一轮）：节点持 `desc_`
    + 编译期事实，公开面 = `desc()`/`setDesc()`；可分三步（加 desc() → 迁读者 → 删镜像访问器）。
  - **SK3**：`SceneView::setPipeline` 时若管线任何 pass 都不画视图的场景，报一次诊断（只诊断不改结构，
    因为"一个管线画多个场景"合法）。
- **核过但不算缺陷的三条**（别下次再"发现"）：`LightRig::addTo` 是配方（调 `Scene::addLight`），
  不是第二个门；"尺寸三个入口"里 `RenderPipeline::resize` 是管线自己的重排，语义不同；
  `RenderPass` 的 `shouldClearDepth` 属"规矩 vs 可读性"，留给人裁决。
- **未跑**：SK0 的变异证据要 ASan 树（`scripts/asan_check.sh`）；本轮只给出结构论证 + 契约用例。

## 2026-10-20 公开面瘦身：删旋钮、收拼法、改命名（零行为变化）

- 设计与逐条依据：`.ai/design/graphics-api-simplification-2026-10-20.md`。判据：全量构建 0 error；
  `test_graphics` **392**、`test_vsg` **478**；`vsg_rewrite_gate.sh --no-build --quick` 阶段 2–6 全 ok
  （`failed=0 vuid=0 hazard=0 skipped=0`），阶段 7（deferred demo 启动占用 658–942 ms > 150 ms）在本容器
  是环境问题，未碰启动路径，脚本注释记录参考环境 74–92 ms。
- **删掉的两种"谎话 API"**：`ShadowFilter` / `ShadowSettings::filter` / `Light::setShadowFilter`（头文件自己
  写着 RESERVED，`src/` 唯一读者就是写它自己，阴影只做一次深度比较）；`ImageRef::unbind()`（= `bind(nullptr)`）。
  `ShadowSettings` 的注释改成"为什么故意没有 filter"，防下一个人以为是漏做。
- **一个事实一种拼法**：`Group::children()` 只剩借用版（原来还有一个每次复制整个列表的，是纯陷阱，测试里那条
  "复制版与借用版一致"的用例随之删除）；`Geometry` 五个 canonical setter 的**整段/段**两套 overload 各并成一个
  （`first_vertex=0, vertex_count=0` 就是整段，调用点零改动）；`RenderPass::clearColor`/`setClearColor` 四合一为
  **颜色在前、附件在后**；`RenderTarget::colorFormat(int=0)`；`RenderSurface`/`SceneView` 的 `resize` 两套并成
  `resize(w,h,ratio=0.0)`（两边的实现本来就当 `<=0` 是"沿用当前"）；`ImageSource::imported` 三合为一
  （`optional<Sampler> = {}`，`Sampler` 隐式转换，调用点零改动）；`SceneView::layoutParticipant()` 两个访问器
  零调用，删。
- **`LayoutParticipant` 顺带修掉一个怪行为**：原来两条 step 列表（无比例 / 带比例）分两遍跑，所以不管注册顺序
  所有两步 step 都排在带比例 step 前面；现在一条列表、注册顺序即执行顺序（注释写的一直是 registration order）。
- **命名对齐仓库规矩**：`Light::castShadow`→`isCastingShadow`、`RenderTarget::depthPromotion`→`isDepthPromoted`、
  `DepthView::valid`→`isValid`、`Camera::aspectRatio`→`aspect`（setter 叫 `setAspect`）、
  `RenderCommand::{modelMatrix,drawOrder,authoredState}`→snake_case（后端记录 `FrameRecorder` 的那一份 `authoredState` 一起改）；`RenderSurface::target()` 裸指针→`raw_ptr`。
  用 LSP rename 时**测试目录不一定被索引**（`aspectRatio` 24 处里测试是手工补的），收尾要 grep 旧名。
- `Capabilities::max_color_attachments` 的注释原来声称"MRT planning reads this"，全仓无读者；改成它真实的身份
  （天花板，后端拒绝超出的声明并报诊断）。
- 本轮明确不做、已进 `graphics-followups-2026-10-19.md` §3 三条（**2026-10-20 均已落地**，见顶条与本条：
  `RenderPass`↔`PassDesc` 收敛、`Texture` 的 `faceCount`/`layerCount` 与 `source`/`layer` 双拼法、
  `Camera::lookAt` → `lookAt`）；`RenderPass::shouldClearDepth` 属于"规矩 vs 可读性"，留给人裁决。

## 2026-10-19 后端 7 条重审：A4 落地（材质自带 revision），B6 的“修法”被驳回

- **A4 落地**：`Material::revision()`（每个 shading setter **先比后 ++**，`setName` 不算）——材质是唯一“所有变更都走
  setter”的内容类型，所以它能自己数；`ContentStore::updateMaterial` 从“逐命令建块 + 逐成员比”变成“比一个整数”，
  `materialBlockAgreesWith` 删除。`LiveMaterial` 多一个 `seen_revision`（= 当前行是按哪个 revision 建的，在
  `ensureMaterial` 里写），所以“刚描述完就 touch”不是一次多余重建。判据：
  `MaterialTest.TheRevisionMovesWhenAShadingValueChanges`、`ContentStoreTest.ATouchWithNoEditChangesNothing`
  （5 帧反复赋同值：0 build / 0 park）、`AMaterialEditReplacesItsEntryAndParksTheValue`。
- **A4 落地时故意没采纳登记里的形状**：§11.16bw 写的是“加 revision 但**保留** compare-and-write 作为“没公告”的
  兜底”。没保留——材质的每个变更都走 setter，它自己能报，不存在“宿主写了字段没人知道”的洞（那正是 `Geometry`
  需要外部 `setRevision()` 的原因：它的字节在宿主能改的 buffer 里）。留着兜底只会把已经免费的信息再算一遍。
- **B6 保持登记（重要判断）**：`StreamKey.revision` 取 `Geometry::revision()` 是真的，但改成逐 `Buffer::revision()`
  会把“多传”换成“画错”：`Geometry::addBuffer/removeBuffer` 的契约就是“改动不公告，用 geometry 的 setRevision 报”，
  按这个契约办事的宿主要么静默拿旧字节、要么被要求多报一次。**先定契约（谁公告），再省带宽**；触发不变。
- **A6 的“改 LRU”也是错解**：已量化形状（200 张=0 缺失、300 张=每轮全重建）是**工作集 > 容量**；同序循环访问下
  LRU 与 FIFO 表现相同（插入序 = 使用序）。真解法是容量可配 / 按字节上界 / 淘汰可见。
- **A3/B7/B8/flake**：保持登记（A3 是 SDK 面决定、B7/B8 需复现、flake 等第三次）。
- **A6 可观测面已落地（2026-10-20）**（不是“本来就有”，别把它当先例乱引用）：`DiagnosticCategory::ResourcePressure`
  + `MaterialImages::evictions()`（与 `releaseAbandoned` 的“没人持有”严格分开）+ `VsgBackend::sweepAbandonedContent`
  里的**水位 + `ReportOnce`** 一条句子。判据两条：`MaterialImagesTest.TheBoundDropsTheOldestEntryAndCountsOnlyThat`
  （计数语义）与 `VsgBackendTest.AWorkingSetLargerThanTheTextureCacheIsReportedOncePerEpisode`（宿主真收到一条、且
  持续超限时仍只一条；**变异“去掉 `shouldReport()` 守护” ⇒ 1/1 红**——只留水位不够，因为持续超限时计数器每帧都在动）。
  A6 剩下的**只是淘汰键**（插入序 vs “最近 acquire 帧号”），而 LRU 不是已量化形状的解（工作集 > 容量时同序循环下
  两者表现相同）。
- **坑（浪费了一轮排查）**：给 `DiagnosticCategory` 加一个枚举值 → `Diagnostics` 里的
  `std::array<uint64_t, Count>` 变了大小 → **`RenderDevice` 布局变** → 任何在测试二进制里定义 `MockBackend :
  RenderDevice` 的用例 (`test_graphics`/`test_gui`) 如果没全量重编，就会在
  `RenderDevice::setDiagnosticsRoute` 的 `push_back` 里 **SIGSEGV**（`construct_at(__location=0x0)`）。看到那个栈就
  先全量构建，别去查代码逻辑。
- **口径**：这是本轮唯一一次“未命中触发条件也动手”的项，理由是它同时是**一致性修复 + 删代码**（不是加机制），
  且改完 A4 的前提（“唯一没有 revision 的内容类型”）就消失了。

## 2026-10-19 F1–F7 重审：只有 F5 需要动手，落地 `vn::Lease<T>`

- **结论**：`graphics-render-next.md` §1 的七条摩擦逐条对代码核过：**F1/F2/F3/F4/F7 已被后来的切片关闭**
  （M3 的 `resolveRenderState`、M2a 的呈现声明、M1 的几何阶段覆盖、M7 的 `PassDesc`+`FramePlan`、`SurfaceState`
  事件协议 + `Capabilities::supports`），**F6 留触发**（一个窗口第二个 3D 区 / 命令要指向非活动窗口），
  **F5 是真缺陷并落地**：新增 `vn::Lease<T>`（`src/base/core/sdk/vine/Lease.hpp`）+ `RenderEngine::lease()`；
  `SceneView::engine_` 与 `RenderPipeline::engine_` 从 `raw_ptr` / `intrusive_ptr` 改成 `Lease<RenderEngine>`；
  `RenderPipelineBuilder` 里那句 `intrusive_ptr<RenderEngine>(engine_)`（临时加引用）删除。
- **为什么这么选（对照其它引擎）**：租约 = Filament `Engine`/O3DE `RHI::Device` 那条"父持有子、子只借 + 借了要
  能判活"的口径；wgpu 的"资源持 `Arc<Device>` + 设备侧注册表"是另一条成立的路，但 Vine 没有那张注册表，抄一半就是
  现在的样子（子持有父、没有注册表、还要靠约定）。租约把"它还在吗"（借用方随时可问）与"它活多久"（所有者一人
  决定）分开，不需要注册表。
- **行为改变（有意）**：宿主释放引擎之后，`view->engine()` / `pipeline->engine()` 回答 **nullptr**，不再出现
  "管线替宿主续命"。原用例 `SceneViewViewportTest.RebuildingAfterTheHostReleasedTheEngineIsSafe`（钉的是续命）
  改写为 `AViewWhoseEngineWasReleasedReportsIt`（钉"释放即没了、视图自己的尺寸/相机照旧可用"）。
- **判据**：`test_core` 的 `LeaseTest` 5 条（活着可读 / 空租约 / 死后回答 nullptr / 副本同一条有效性 / **不延长寿命**）；
  `RenderPipelineBuilderTest.APipelineOutlivingItsEngineNeitherHoldsNorCallsIntoIt`（`engine->useCount()` 仍为 1，
  之后 `pipeline->engine()==nullptr`、`setShadingMode` 仍改 pass 状态、`setGizmoVisible` 不再报给死者）；
  `test_graphics` 391 全绿、`ctest` 23/24（唯一红是既存 async）。
- **坑**：`vn::graphics::detail` 已存在（`FrameResources`），所以 `Lease` 的单元必须写 `vn::detail::LeaseCell<T>`；
  `Lease` 没有 `operator->`（故意的：逼每处先判空），管线里 4 处 `engine_->` 全部改成先取 `engine_.get()` 再判。
- **顺手清掉的两处过时注释**：`ScreenPass.cpp` 与 `backend/FrameGraph.hpp` 还在引用 `validateWiring`（该函数自 M7
  起已不存在），改成"引擎每帧编译声明时报告 / `collectPlanProblems`"。

## 2026-10-19 登记回写：四处"已落地但文档仍写未做"的漂移清零

- **结论**：按现行实现回写四处登记，代码零语义改动（`ImageRef.hpp` 只改 `@note`）。
  ① `graphics-object-model-vs-3d-frameworks.md` §3 缺口 4（`Pipeline` ≠ PSO 的命名风险）随 K1 改名关闭；
  ② 缺口 5（内容掩码）改为 L1 已落地（2026-10-12，读侧拾取/包围盒 2026-10-13）；
  ③ 缺口 7（外部图像没有接线身份）改为 E2 已落地（Cube 同日经验证，见下条），只剩 compare sampler 与 array；
  ④ `graphics-render-next.md` §5.1 的 T2/M6 由 S10b 覆盖并关闭——形状不是 `setSurface(i)`，而是
  "每窗口一个 `createSurfaceTarget`"，所以 SDK 侧不需要再加"一会话多交换链"入口。
  顺带把 `graphics-image-source.md` 顶部状态与 `ImageRef::` 的 `@note` 里"两套模型未搭桥"去掉。
- **为什么值得写回**：这四条都是"缺口清单"，不写回的话下一个 session 会照着它重做已完成的事；
  S10b 的落地记录原本只在 `graphics-window-device-viewer.md` §7，T2 那一行读起来仍是"已推迟"。
- **判据**：`scripts/check_ai_docs.py` / `scripts/check_doc_symbols.py` / `scripts/check_public_docs.py`
  0 finding；无代码语义改动，不跑设备电池。

## 2026-10-19 D1 / E1：present 计数与 appfw 布尔 getter 改名

- **D1（画面已落地）**：`FrameProgress` 新增 `presented`，来自窗口会话的 `framesPresented()`（设备级汇总并取
  历史最大值，窗口离开不回卷）。**精确含义**：帧被交到平台的 present 调用且成功——**不是**“已经上屏”
  （后者需要 present-timing 扩展）。窗口读图应等这个数到目标帧再读。判据：
  `VsgBackendTest.TheFrameReportSaysHowManyFramesWereHandedToPresent`（两帧空帧 ⇒ +2；再问不变）。
- **E1（命名）**：`Control::enabled/visible`、`RibbonAction::enabled/checkable/checked`、`RibbonButton::checkable/checked`
  → `isEnabled/isVisible/isCheckable/isChecked`；调用点与测试同步；`VN_APPFW_PLUGIN_ABI_VERSION` 7u→8u。
- **坑**：改名会让 `check_public_docs.py` 把新名字当“新未文档化声明”（旧名字在基线里）——这些 getter 原本
  只有 `///` 一行，现在补了 `@brief/@return`，`--update` 把基线从 1042→1028（真的降了欠账，不是加账）。
- **判据**：全量构建 0 error；`test_gui` 259 + 1 skip、`test_appfw` 32、`test_vsg` 476、`test_graphics` 390；
  `ctest` 23/24（既存 async 红）。

## 2026-10-19 B1：MRT 每个附件自己的清屏色

- **结论**：`ClearPolicy` 加 `extra_colors`（附件 1..N-1，缺省仍是透明黑）；`PassDesc::clear_extra_colors` /
  `RenderPass::setClearColor(attachment, colour)` 是声明侧；后端 `core::ClearPolicy::extra_color_values` +
  `planClearValues` 逐附件发值（bootstrap 也尊重已声明的颜色）。`clearColor(int)` 对未声明的附件回透明黑。
- **判据**：`CoreClearPlanTest.AClearRequestPutsEachAttachmentItsOwnColour`、`…ABootstrapPassHonoursTheStated…`；
  `RenderPassTest.EachAttachmentClearColourTravelsWithThePassPolicy`；
  `VsgBackendTest.EachAttachmentClearsToItsOwnColour`（两附件红/绿分别读回）。`test_vsg` **475**、
  `test_graphics` **390**。
- **坑（重要）**：改了 `libvnGraphics` 的导出布局（这里是 `ClearPolicy` / `RenderPass`）后，**必须全量构建**
  再跑 `test_vsg` —— 只建 `test_vsg` 不会重链 `gfx_backend_vsg` 插件，旧插件与新库布局不一致会在
  `VsgBackendPluginTest.CreateBackendByName` 里段错误。

## 2026-10-19 B2：`RenderPass::execute` 去虚，只剩 `describe()` 一个扩展点

- **结论**：`execute()` 本来就是“`describe()` → `singlePassFrame()` → `executeFrame()`”的糖；只要可覆写，
  子类就能画出编译期看不到的东西（一条 pass 两套真相）。现在它是普通成员函数，扩展点只剩 `describe()`
  与声明侧 `PassDesc::before_draw`。
- **连带**：旧用例 `TheEngineRunsTheFrameValueAndNeverCallsExecute` 的 `CountingPass::execute` 覆写不再能编译
  （这正是想要的效果），改写成 `…AndDescribesEachPassOnce`：每帧 `describe` 一次 + `frames_executed` 每帧 +1。
- **判据**：`RenderPassTest.ExecuteIsSugarOverDescribeAndTheOnlyExtensionPoint`；`test_graphics` **389**、
  `test_vsg` **473**；6 条 hygiene 门禁 0 finding。

## 2026-10-19 补登的 5 条：3 条落地，2 条按设计关闭

- **结论**：盘点时新登记的 5 条缺口已全部处理，不留“登记了但没人做”的空条目。
- **落地（代码 + 用例）**：
  ① `Capabilities::max_anisotropy` / `max_color_attachments`（vsg 从 physical device 回答；anisotropy 走
  `detail::anisotropyFor` 与 sampler 同一条夹紧；判据 `CapabilitiesTest.TheLimitsTravelBesideTheFeatureBits`
  与设备用例里 `supports(AnisotropicFiltering) == max_anisotropy > 1`）；
  ② `DepthView::layer()`：分层声明**报一次并拒绝**，不静默退回整张图（判据
  `DepthViewTest.ALayeredDepthViewIsRefusedRatherThanPointingAtTheWholeImage`）；
  ③ 同一 target 的冲突声明：首个立住，后续不同声明被拒，按 `(target, image)` 只报一次（判据
  `DepthViewTest.TwoDifferentDepthViewsForOneTargetAreRefusedOnce`）。
- **按设计关闭**：`OwnedFrame` 不加引擎入口/序列化（`OwnedFrame.hpp` 已写“为什么没有”）；`InputRouter` 不加
  工具表（只有一个工具，表只有一行——正是本模块拒绝的空抽象）。
- **坑**：`RenderPass::setRenderTarget` 不能因为“这个 pass 没声明”就去清 target 的借用——新 pass 挂到同一 target
  会把别人设好的 depth 边清掉（被冲突用例抓到）。清空只属于**显式**的 `setDepthView(DepthView{})`。
- **判据**：构建 0 error；`test_graphics` **388**、`test_vsg` **473**；`ctest` 23/24（既存 async 红）；6 条
  hygiene 门禁 0 finding。

## 2026-10-19 待办盘点：收敛成一份清单，移除 13 条过时项

- **结论**：`graphics-followups-2026-10-19.md` 重写为**唯一待办入口**（§2 现在可做 / §3 触发条件型 /
  §4 明确不做 / §5 不再是待办）。全部 graphics 登记点逐条对**现行代码**核实，移除/合并 **13 条**、
  补登 **5 条**。
- **移除（不要再捡）**：B9、A2 残留、首帧叠层、窗口随日志长大、B1/B2/B3、T2/M6、Cube 作输入、
  >3 方向光、内容掩码 L1、`Pipeline` 命名风险、外部图像接线、MRT 文档“deferred 默认被 env 门控”、
  perf-backlog 的 P1–P17/R1–R5/V1–V7（老渲染器时代）。理由逐条在 §5。
- **补登（本轮新发现）**：`Capabilities` 没有数值上限；`DepthView` 没有“某一层”；同一 target 的两条
  pass 声明不同 depth view 时 last-writer-wins 无诊断；`OwnedFrame` 缺引擎入口与序列化；
  `InputRouter` 没有工具选择。
- **仍成立且已核**：per-attachment clear 色（`ClearPlan` Rule 4；`graphics-mrt-gbuffer.md` 的 S4 剩余）；
  appfw 布尔 getter 命名仍违约定（等下次公开面变更）。
- **判据**：`check_ai_docs.py` / `check_doc_symbols.py` / `check_public_docs.py` 0 finding（纯文档轮）。
- **顺带**：本机 `ctest` 是 **23/24**，唯一红为 `AsyncDefectRegressionTest.YieldLoopStackIsConstant`
  （async 模块，单跑同样红；已记到 `.ai/memory/async.md`）。

## 2026-10-19 H1 / L1 / I1 / J1 提前落地：先核前提，再收窄范围

- **结论**：四条登记都挂着“触发条件”，本轮按用户要求提前做。**先核了前提是否过时**：H1 / L1 不过时；
  I1 前提成立但**范围收窄**（资源图的边早就在 `backend/FrameCompiler.cpp` 里：`depth.borrowed` / `depth.source`
  与“生产者先写”的顺序校验都在，缺的只是**声明点**在 target 字段上）；J1 **部分过时**（输入已经在
  `CameraManipulator::onMouse*`，配方已归 `RenderPipelineBuilder`/`Viewer`，尺寸已归 `RenderSurface`），只拆剩下的两小块。
- **落地**：① `Capabilities::Feature{GpuTimings, Wireframe, AnisotropicFiltering}` + `features` + `supports()`，
  vsg 后端从 physical device 的 `timestampComputeAndGraphics` / `fillModeNonSolid` / `samplerAnisotropy` 回答
  （与 `DeviceRequirements` 的地板分工：那里是“不做就不跑”）；② `DepthView` 值 + `PassDesc::depth_view` /
  `RenderPass::setDepthView`（引擎在 `addPass` 应用到 target，`setRenderTarget` 携带边），builder 不再代写 target 字段；
  ③ `OwnedFrame.hpp`（拥有式帧 + `borrowed()` 借用视图，命令集 `shared_ptr`、输入 `ImageInput`）；
  ④ `InputRouter` / `LayoutParticipant` 成为 `SceneView` 的协作对象，`pushEvent` / `resize` / `addSurfaceLayout` 行为不变。
- **判据**：`test_graphics` **385**、`test_vsg` **473**、`ctest` **23/24**（唯一红是既有
  `AsyncDefectRegressionTest.YieldLoopStackIsConstant`，属 async 模块）；新用例 7 条；8 条 hygiene 门禁 0 finding。
- **坑**：① `raw_ptr<T>` 是裸指针别名，写 `.get()` 编译不过；② 布局步骤必须按**有效尺寸**执行——呈现时尺寸归
  `RenderSurface`，`LayoutParticipant` 记录的只是“不呈现时”那份，否则 `TheAreaSizeHasOneOwnerAndTheViewReadsIt`
  与 `RenderSurfaceTest` 对应用例红；③ 新 `.cpp` 在 glob 里，要重新 `cmake -S . -B build` 才会进构建，否则链接报 undefined。

## 2026-10-19 Cube 作 pass 输入：不需要改后端，补两条用例即关闭登记

- **结论**：`addInput(cubeMap)` 今天就能用。后端不区分 kind——`MaterialImages::acquire` 对 cube 建
  `VK_IMAGE_VIEW_TYPE_CUBE` 的六层视图、失败时 `whiteCube()`；输入 set 的 layout 来自**程序声明**
  （`sampledSetLayout(颜色数, 深度数)`），而 `VkSampler` 本身不带维度。程序侧只要在该 binding 声明
  `samplerCube`。此前文档写成“只支持 D2”，是把“没验证”误读成了“被拒绝”。
- **判据**：`test_vsg` **472**（470 + 2）：`VsgBackendTest.ACubeMapInputReachesAContentProgramAsACubeSampler`
  （六面异色、+X 绿，固定方向 `(1,0,0)` 采样读回 `(0,1,0)`，`diagnosticCount() == 0`）与
  `VsgBackendTest.AnIncompleteCubeInputBindsTheWhiteCubeAndSaysWhy`（少面 ⇒ 白 cube 读回 `(1,1,1)` + 一条
  Warning）；`test_graphics` 不回归。
- **坑**：1×1 面在 LINEAR 过滤下会跨面渗色（`ContentPassTest` 已记录），用例用 4×4 并在面心方向采样。
- **既存红（与本次无关）**：`ctest` 23/24，唯一红是 `AsyncDefectRegressionTest.YieldLoopStackIsConstant`
  （`depth 319936` vs `< 65536`，单跑同样红，属 async 模块；本次只动 graphics 用例与文档）。
- **登记**：`graphics-followups-2026-10-19.md` §2 #5 关闭；**array 仍不存在**（`Texture::Kind` 只有 D2 / Cube，
  分层目标是 `graphics-layers.md` L2）。

## 2026-10-19 引擎 episode 收口：最后一个布尔 seen/reported 改用 `ReportOnce`

- **结论**：`RenderEngine::unsupported_target_reported_` 从手写 bool 改为 `ReportOnce`（`shouldReport()` /
  `rearm()`），行为逐字不变；`graphics-layering.md` §1 的“6 组手写集合”从此收口。其余 episode 都是 KEYED 的
  （按句子 / `(pass, image)` / pass / host name），不是 `ReportOnce` 的适用形状，登记为保持现状。
- **判据**：`test_graphics` 379（`ABackendThatCannotDrawTargetsIsToldOncePerEpisode` 覆盖一次 / 重新武装）；
  `ctest` 24/24；8 条 hygiene 门禁 0 finding。

## 2026-10-19 E2 收尾 #4：imported input 的 sampler state

- **结论**：新增 `vn::graphics::Sampler` 值类型（min/mag filter、mipmap mode、UVW address、border color、
  anisotropy、LOD bias / range）；`ImageInput(texture, sampler)` / `RenderPass::addInput(texture, sampler)`
  把状态放进**输入声明**，`ImageSource` / `CompiledInput` 用 `optional<Sampler>` 携带。不写状态仍是后端
  输入采样器。`MaterialImages` 的上传键仍是 `(Texture*, revision)`，显式状态按值缓存在旁边：同一纹理多状态
  共享 image/view，只多 `VkSampler`。`InputImages::color_samplers` 和 `InputSetCache` 的 `(view, sampler)`
  键把状态送到 content / full-screen 两条路径。
- **坑**：`InputSetCache` 原来只键 view；同一 view 配不同 sampler 会静默复用旧 set。现在键含 sampler，测试
  必须覆盖“同 pass 两次声明同一纹理、不同 wrap”的像素差异。`MaterialImages::setMaxAnisotropy` 变化会清空
  显式 sampler 缓存，避免旧 limit 的 sampler 继续被复用。
- **判据**：`VsgBackendTest.TwoInputStatesOverOneTextureSampleDifferently`（clamp vs repeat 同 pass：
  (0,1,0)；`count()==1`、`samplerCount()==2`）；两条 `MaterialImagesTest`；`FrameCompilerTest` 与
  `RenderPipelineBuilderTest` 各一条结构用例；构建 0/0、`test_vsg` 470、`test_graphics` 379、`ctest` 24/24。

## 2026-10-19 E2 收尾 #3：imported input revision 失效用例

- **结论**：`(Texture*, revision)` 已是 `CompiledInput` / `MaterialImages` 的缓存键，后端无需改动；补真实设备
  用例证明 `setImage` 后下一帧重新上传。
- **判据**：`VsgBackendTest.ARefilledImportedTextureIsReuploadedForTheNextFrame`（第一帧品红，`setImage`
  后第二帧黄色；`MaterialImages::count()` 始终 1，说明是替换而非新增条目）；`test_vsg` 466；`ctest` 24/24；
  ASan PASS；8 条 hygiene 门禁 0 finding。

## 2026-10-19 E2 收尾 #2：produced 输入按 attachment 绑定

- **结论**：`CompiledInput` 增加 `attachment` / `depth`；`ProducedImage` 的 `color_attachments` 跟随单图
  （彩色 1 / depth 0），`ProducedTarget` 仍为整 target。`VsgBackend::recordContent` 对 ProducedImage 只 push
  声明的 view，内容 sampled set 因此只绑一张图。全屏 ABI 的 `binding N = attachment N` 用显式兼容字段
  `InputImages::screen_colors`（source 的整 target）保留，`recordScreenDraw` 优先取它；`ProducedTarget` 路径不变。
- **坑**：`ScreenPass::addInput(ImageRef)` 的 engine 用法（G-buffer 四格预览）解析成 `ProducedImage`，但全屏
  ABI 读的是整 target；只收窄 `InputImages.colors` 会让 `screenCopyProgram(n>0)` 找不到 binding。`screen_colors`
  只服务全屏 path，内容 path 的 `colors` 保持收窄，混合 pass 也不会绑错。
- **判据**：`VsgBackendTest.AProducedInputBindsOnlyTheAttachmentItNames`（先红：读回绿=attachment 0；后绿）+
  `ScreenDrawTest.TheBindingNamesTheAttachmentAndNotTheProgramsFirstOne` / SdkPixelTest G-buffer 预览不回归；
  `test_vsg` 465；`ctest` 24/24；ASan 相关用例 + 8 条 hygiene 门禁 0 finding。

## 2026-10-19 E2 收尾 #1：全屏 ABI 绑定 imported texture

- **结论**：`ScreenPass` 的 imported 输入不再被引擎拒绝。全屏绑定规则：source 颜色附件占 `0..C-1`，depth
  跟随（跳过文本自己声明的 binding），imported 图按输入声明序填程序**已经声明的其余 sampler binding**
  （升序）；数量对不上由 backend refusal，不静默绑错图。`sampledSetLayout` 的现有规则（文本声明的
  sampler 保留自己的 binding）不变，`recordScreenDraw` 按同一规则填槽。
- **判据**：`VsgBackendTest.AnImportedTextureInputReachesAScreenProgram`（像素：全屏程序采样导入贴图）；
  `RenderPipelineBuilderTest.AScreenPassKeepsImportedTextureInputsForItsProgram`；`test_vsg` 464；
  `ctest` 24/24；ASan 两条 imported 像素用例 PASS；内容与全屏路径的既有 copy 不回归。
- **未做**：sampler state、Cube、按 attachment 绑 produced；见 `graphics-followups-2026-10-19.md` §2。

## 2026-10-19 K1 落地：`Pipeline` 改名 `RenderPipeline`（公开面破坏）

- **结论**：公开类 `Pipeline` → `RenderPipeline`，`PipelineOptions` → `RenderPipelineOptions`；头/源文件改成
  `RenderPipeline.hpp` / `RenderPipeline.cpp`。`PipelineStage` / `pipelineStageOrder` 保留（它们是“阶段”， <!-- drift-ok -->
  不是 PSO），`RenderPipelineBuilder` 名字不变。这样 builder、返回值和 options 同一族，类名不再与
  `VkPipeline` / PSO 混读。
- **落点**：graphics SDK 公共头、`RenderPipelineBuilder`、`RenderEngine` 注释、appfw `Viewer*`、model_viewer
  命令、demo、tests，以及 `docs/guides/graphics.md` / `docs/guides/render-views.md` 和 `.ai/design` 的现行引用。
- **判据**：全量构建 0 error / 0 warning；`test_vsg` 463；`ctest` 24/24；`check_public_docs.py` 0 new；
  `check_doc_symbols.py` 通过；8 条 hygiene 门禁 0 finding。任务列表见
  `graphics-followups-2026-10-19.md` §1.1。

## 2026-10-19 E2 落地：pass 输入统一成 `ImageInput` / `ImageSource`（内容路径；全屏 ABI 仍登记）

- **结论**：`RenderPass` / `PassDesc` 的输入声明是一个有序的 `ImageInput` 序列（produced `ImageRef` 或
  imported `Texture`）；已解析形状是 `ImageSource`（`None` / `ProducedImage` / `ProducedTarget` /
  `ImportedTexture`，带 target/attachment/depth 或 texture/revision）。`FrameDescription::Pass::inputs`、
  `RenderEngine::resolveRowInputs`、`FrameRecorder::InputRef`、`FrameCompiler::CompiledInput` 全部改用它。
  produced 图的单 producer / wiring 规则不变；imported 图没有 producer，`FrameCompiler` 不为它加依赖边。
- **绑定**：vsg 的 `recordContent` 对 imported 输入调用 `MaterialImages::acquire`（键为 `Texture*` +
  revision），把 view 填进该输入的 `InputImages`；内容 pass 按声明序采样。上传失败绑白图并报告。
  **全屏 ABI 仍只绑 produced source + shadow map**：`ScreenPass` 声明 imported 图会被引擎以
  `ContentSkipped` 报告并跳过（不静默采样错图）。
- **未收窄的语义**：produced 输入的 `attachment` / `depth` 目前是选择符（全屏 ABI 的 binding N =
  attachment N），绑定层仍给整个 target；按 attachment 绑定是后端能力项。
- **判据**：构建 0 error / 0 warning；`test_vsg` 463；`ctest` 24/24；8 条 hygiene 门禁 0 finding；ASan
  `VsgBackendTest.AnImportedTextureInputReachesAContentProgram` + offscreen 用例 PASS。设计见
  `.ai/design/graphics-image-source.md` §7。

## 2026-10-19 F2a 落地：逻辑→设备像素换算收进 PixelGeometry

- **结论**：`Pipeline::resize`、`AxisGizmo::onSurfaceResized`、`FpsOverlay::onSurfaceResized` 三处重复的 `static_cast<int>(logical * ratio)` 改为私有 `PixelGeometry.hpp` 的 `LogicalSize` / `DeviceSize` / `PixelRatio` / `toDeviceSize`；保留截断规则，`EngineCompile` 的形状容差说明改为指向它。公开签名不变。
- **未做**：色彩空间（linear/sRGB）与数值域（0–255 / 0–1）仍按 F2b 的触发条件。
- **判据**：构建 0 error / 0 warning；`ctest` 24/24；8 条 hygiene 门禁 0 finding。清单见 `graphics-abstraction-review-2026-10-19.md` §1.2。

## 2026-10-19 G1 落地：`RenderEngine` 的图源表拆到私有 `FrameResources`

- **结论**：`publish`/`unpublish`/`resolve`、当前帧的 production/publication 表和 transient target 表从 `RenderEngine` 移到私有 `FrameResources`（`FrameResources.hpp` / `FrameResources.cpp`，不导出）；引擎保留 pass 注册、顺序、编译、诊断与 episode 规则。公开面不变。
- **顺带修掉两个真实缺陷**：① `clearPasses()` 原来跨 `slots_.clear()` 使用 `raw_ptr<RenderPass>`（UAF 风险），现在持有 `intrusive_ptr`；② `VsgBackend::~VsgBackend()` 原为 `= default`，`Data` 成员顺序先销毁 `DeviceClock` / `RetirementQueue` / device，再销毁仍借用它们的 `OffscreenSession`，ASan 报 `OffscreenSession::shutdown -> DeviceClock::removeWindow` heap-use-after-free；析构现在调用幂等 `shutdown()`。
- **判据**：构建 0 error / 0 warning；`test_vsg` 461；`ctest` 24/24；8 条 hygiene 门禁 0 finding。清单见 `graphics-abstraction-review-2026-10-19.md` §1.1。

## 2026-10-19 抽象层契约收口：帧 IR 的 borrowed 语义、顺序权威、Pass 双路径（comment-only）

- **结论**：不改公开形态，只把七处抽象边界写死：`FrameDescription` 是 borrowed snapshot（不是 owned 帧值）；
  `passes` 的顺序是 announced priority，依赖解析执行器可重排（`FrameGraph` 的稳定拓扑排序）；`PassKind`
  封闭集是声明式主路，`RenderPass::describe()` 是逃生口；`Pipeline` 是 pass 链不是 PSO；`FramePlan` 是
  宿主快照、`FrameDescription` 是后端值；`ImageRef`（pass 产出）与 `Texture`（导入）是两个图源根；
  `Viewport` 是设备像素。清单见 `graphics-abstraction-review-2026-10-19.md`。
- **落点**：`FrameDescription.hpp` / `RenderFrameExecutor.hpp` / `RenderEngine.hpp` / `FramePlan.hpp` /
  `PassDesc.hpp` / `RenderPipeline.hpp` / `Texture.hpp` / `Viewport.hpp`；顺带修掉 `BackendHalfGuard.hpp` <!-- drift-ok -->
  注释里的 `graphics/*.hpp`（GCC `-Wcomment` 在每个包含它的 TU 报一次，145 条）。
- **判据**：构建 0 error / 0 warning（修前 145 条 `-Wcomment`）；8 条 hygiene 门禁 0 finding
  （include / diagnostic / doc symbols / export / ai-docs / public docs / vsg window state / vsg upstream）；
  不跑测试（comment-only）。
- **未做（登记触发条件）**：能力预检 `Feature`、强类型
  `DevicePixels`/`LogicalPixels`/`LinearColor`/`SrgbColor`、owned 帧、`ImageSource`、`Pipeline` 改名、
  `shareDepth` 的资源图边。触发条件见清单第 2 节。

## 2026-10-18 §9.7 落地：会话归创建者，引擎只**借用**设备（S10b 的前置；诊断路线改扇出）

- **结论**：`RenderEngine::initialize()` 不再调 `device_->initialize()`、`shutdown()` 不再调 `device_->shutdown()`
  ——设备由**建它的那一方** bring up / tear down。一设备两引擎因此成立：B 的 `initialize()` 不再拆 A 的会话，
  A 的 `shutdown()` 也不再拆 B 还在用的设备。文档：`graphics-window-device-viewer.md` §9.7（裁决 + 落地 + 仍未收口）。
- **做了什么**：① `RenderDevice` 加 `initialized()`（纯虚）；引擎附着前问一句，**没起来 ⇒ 一次报告**
  （`ReportOnce device_not_up_`，换设备时 rearm）+ 附着失败 —— "借用来的设备没起来"是一等状态，不是静默空画面。
  ② `RenderEngine::setDevice` 把默认内容程序公告给设备（设备由创建者 bring up，程序必须早于那一刻到），
  换设备 / 析构时 `removeDiagnosticsRoute`（路线是**借**给设备的，引擎死了不能留悬垂指针）。
  ③ **诊断路线从"一条"改成"扇出"**：`setDiagnosticsRoute` 追加、`removeDiagnosticsRoute` 撤销，`reportDiagnostic`
  发给所有借来的路线 ⇒ 第二个引擎不再覆盖第一个的计数；设备自己的 `diagnosticSink()` 答第一条非空路线（这就是
  "引擎装上去的 sink 代表这块设备"的意思），`diagnosticCount()` 读设备自己那条（同样收到全部报告）。
  ④ 宿主侧：`SurfaceWindow::init()` 在目标的创建输入与 `feedSurfaceSize` **之后**调 `device->initialize()`
  （尺寸是创建输入 ⇒ 窗口按最终尺寸出生，而不是先 640×360 再跟随一次、付一次 counted device wait），再
  `engine->initialize()` 附着；设备起不来是**可重试**拒绝（状态留 Pending），不是 Failed 判决。
  ⑤ 插件：`Session::wouldKeep(options)` 让 `VsgBackend::initialize()` 在"同一块窗口、已经起来"时**不丢内容世界**
  ——原来它无条件 `releaseContentWorld()`，于是每次附着都把全部管线重建一遍。
- **判据**：全量构建 0 错 0 警；`ctest` **24/24**（`test_graphics` 新增 `AnAttachToADeviceThatIsNotUpIsRefusedOnce`、
  `OneDeviceReportsIntoEveryEngineThatBorrowsIt`、`AStoppedEngineLeavesTheDeviceItBorrowsAlone`，
  `InitializeCallsBackend`/`ShutdownReleasesBackend` 按新语义改写；`HeadlessBackendTest.AReAnnouncedWindowMovesTheTargetAndTheDeviceStays`
  改成宿主驱动设备，`initializes == 2` 的含义不变）。**变异 2/2 红**：去掉诊断扇出 ⇒
  `OneDeviceReportsIntoEveryEngineThatBorrowsIt` 红；去掉"借用设备没起来"的守 ⇒
  `AnAttachToADeviceThatIsNotUpIsRefusedOnce` 红。**未覆盖（登记）**：插件的 keep 路径（同一块宿主窗口再
  `initialize()` 不丢内容世界）需要设备 + 宿主窗口，本机那条用例区是 X11-only；它只是省一次重建，正确性由
  `Session::wouldKeep` 与 `planSessionMove` 同一张表保证。
- **坑**：`RenderEngine::shutdown()` 里的 `bindTarget(nullptr)` 仍然要留着（目标是设备的对象，设备随时可能被它的
  创建者拆掉），但**不能**顺手 `device_->shutdown()`——那正是共享设备被拆掉的那一刀。测试双实现（`MockBackend` /
  `HeadlessBackend` / `WindowedBackend` / `FleetBackend` / `StubBackend`）一律"默认 up"：引擎只借用，借用来的
  设备按定义是起来的；要测"没起来"那条，把 `up` 翻掉。
- **同日半步（S10b 插件侧"一设备多窗口"的地基）**：`SessionOptions::device`（`void*`，值是 `vsg::Device*`，
  会话自己加引用 —— 这个头仍然不出现图形类型）+ `detail::adoptDevice(window, device)`。两个窗口类各自暴露
  `adoptDevice`：`VsgHostWindow`（采纳宿主窗口的那个）与**新增** `VsgOwnedWindow`（本后端自建窗口的薄壳，
  `Inherit<Win32_Window|Xcb_Window, …>`，析构**销毁**自己的窗口）。为什么必须是子类：vsg 把
  `Window::share(device)`（= setDevice + `_initSurface` + `_initFormats` + `_initRenderPass`）放在 `protected`，
  公开的 `setDevice` 只设 instance/physical device、**不算 formats**，直接用会在建 render pass 时读到格式 0。
  自建窗口从此走 `new VsgOwnedWindow(traits)` 而不是 `vsg::Window::create(traits)`。
- **2026-10-18 S10b 落地：一设备 N 窗口（插件侧 1:N + 设备级完成水位）**
  - **设备 + 窗口表**：`VsgBackend::Data` 从"一个 `api::Session`"变成 `windows`（每窗口一个 `Session` + 一把目标
    对象）+ `device`（`ref_ptr<vsg::Device>`，设备比窗口长命，内容世界建在它上面）+ `driving`（这一帧属于哪块窗）。
    第一块窗口的目标就是聚合自己（`createSurfaceTarget` 返 `this`），其余各一个 `detail::WindowFacade` 转发器；
    **同一块窗口要第二次 ⇒ `WindowOccupied` 报一次**（一窗一条活交换链），不同的窗口是**服务**它 —— 那正是共享的
    意义。`executeFrame(frame, present_to)` 按目标认窗口（`nullptr`/`this` ⇒ 第一块），`present()` 走窗口 0，
    `presentWindow(i)` 走任意块。
  - **完成证据升到设备级**：新 `core::DeviceClock`（`backend/DeviceClock.hpp`）—— 一条 `FrameTimeline`（帧号是设备的）
    + 每个窗口报自己的证据（"我提交的 ≤F 都完成了"）+ 设备水位 = **各窗口的 min**（每个窗口只证明自己的工作；
    只做过的才是真空完成）。窗口加入时从当前水位起（不回卷），离开时不再约束它。`api::SessionOptions` 可收一对
    **借来的** clock/回收队列（为空 = 会话自持，单窗口路径一字未改）；主线程唯一时没有新锁。回收队列只有设备一处，
    `setSlots` 用 `widestSlots()`（最宽的那块窗口决定停放窗口）。
  - **为什么不是"在每条会话队列上各停一次"**：那是同一个结论的 N 份实现，且"最后一个放手"靠引用计数隐式实现；
    设备级水位是 Filament `destroyDeferred` / UE RHI deferred delete / wgpu submission index 的形状（见
    `graphics-window-device-viewer.md` §11）。
  - **判据**：全量构建 0 错 0 警；`ctest` **24/24**。新增 `CoreDeviceClockTest.TheWatermarkIsWhatEveryWindowProved`
    （min / 未学槽数时不证明 / 单调 / 加入不回卷 / 离开不约束 / 最宽槽数）、`VsgBackendTest.TheSameWindowIsNeverServedTwiceAndADifferentOneGetsItsOwnTarget`
    （第一块由聚合答、同窗二次 ⇒ 占用报一次、异窗 ⇒ 自己的目标对象）、`SessionTest.ASecondSessionRendersOnTheDeviceTheFirstOneCreated`
    （真设备：两个会话一个 `vsg::Device`）。**变异 2/2 红**：水位取 max ⇒ clock 用例红；每块窗口都返 `this` ⇒ 窗口用例红。

## 2026-10-18 S10b 第二片：`executeFrame(frame, nullptr)` = 没有窗口（`api::OffscreenSession`）

- **结论**：`nullptr` 不再落到窗口 0。它走**离屏会话**（`api::OffscreenSession`，借用设备 + 图形队列族；
  `vsg::Viewer` 零窗口 + `CommandGraph(device, family)` + 一个 task）：`beginFrame()` 只推进时间线（**不取像**），
  `commitFrame()` 提交、**不 present**，并按 `frame - slots` 向 `DeviceClock::windowCompleted` 报证据。帧在
  `executeFrame` 返回前完成（没有人会替它 present）。`VsgBackend::Data::driving = kNoWindow` 表示"这一帧没有窗口"；
  窗口/离屏的"记录 + 提交"合成 `commitFrameThrough(window, offscreen)`，只差会话、默认帧缓冲行与 present。
- **共用件**：不泵事件的 viewer 与"保留 VkResult 的提交"抽到 `SessionDrive.hpp`（`detail::EmbeddedViewer` /
  `detail::submitViewerTasks`），窗口会话与离屏会话各只有一份。
- **`recordContent`**：窗口参数变指针；没有窗口时跳过"命名默认帧缓冲"的 pass（编译期已因缺 facts 行排除）。
- **判据**：构建 0 错 0 警；`ctest` 24/24；`test_vsg` 434 = 431 过 + 3 skip；五道门禁 0 finding。新用例
  `VsgBackendTest.ANullTargetRunsTheFrameWithoutAnyWindow`（不 present / 不新增窗口 / 无诊断 / 进队列 / 进水位）。
  变异：① `nullptr` 回窗口 0 ⇒ 红；② 离屏不报证据 ⇒ 水位红。
- **连带发现**：此前 `SdkPixelFixture::available()` 在本机（Windows）恒为 false —— 因为 `nullptr` 帧只录不提交，
  宿主目标的像素从未画出来，整套设备用例一直在**跳过**。这一片之后它们真的跑起来了；`VsgBackendTest` 里用假句柄
  （`int second = 0`）建第二块窗口再 `initialize()` 的那半段因此在 Windows 上不成立，改成"设备起来之后再要第二把目标"。

## 2026-10-20 `DeviceDesc{presentation}` 成为**创建输入**（S10b 最后一项；`None` = 无窗口设备）

- **结论**：设备的“呈现意图”在**建它的时候**说（`vn::graphics::DeviceDesc{presentation, native_handle}`），
  `RenderBackendFactory::create(const DeviceDesc&)` → `RenderBackendRegistry::create(name, desc)` → `VsgBackend(desc)`
  一路传下去，一个值贯穿创建。三种取值：`Probe`（默认，今天的形状：宿主没指名窗口就自己造一块）、
  `None`（**永不上屏**）、`Handle`（为宿主那块窗口造设备，句柄是创建输入）。
- **`None` 的形状**：`initialize()` 走**无窗口缝** `createDevice()`（`Instance` 由设备自己拿住，`Data::instance`），
  因此 `windowCount()` 在整个生命周期里恒为 0；`capabilities().presentation == false`；`createSurfaceTarget` 答
  `NoPresentation` 且**静默**；`initialized()` = “设备有没有起来”；它的帧就是“不命名目标”那种（离屏会话提交、不 present）。
  `Handle` + 没给句柄 ⇒ `initialize()` 答 false + 一条 `InitFailed`（**不**改成自己造一块窗口——那是另一种意图）。
- **队列族**：`graphicsQueueFamily(const vsg::Device&)` 从**设备自己的队列**读（不再问物理设备）；离屏会话因此
  在 `None` 设备上也能起来，且窗口设备的族与它当初建的族必然一致。
- **坑**：给 `tests` 的 `DirectDriveBackend` 加构造函数之后，`new X()` 的**零初始化不再覆盖成员** ⇒ `target_`（`raw_ptr`，
  就是裸指针）是垃圾 ⇒ `nativeHandle()` 里解引用 0xc0000005；`target_{nullptr}` 修好。与 S10a 那条同一课：
  `raw_ptr` 必须自己写初值，不能指望 `new X()`。
- **判据**：构建 0 错 0 警；`ctest` 24/24；`test_vsg` **436 = 433 + 3 skip**（新 2 条：
  `ADeviceCreatedWithNoPresentationRunsWholeFramesWithoutAWindow` / `ADeviceCreatedOnAHandleWithoutOneIsRefusedWithItsReason`）；
  `test_graphics` **377**（新 1 条 `TheDescriptionReachesTheFactory`：注册表把描述原样交给工厂）；六道门禁 0 finding。
  变异：`createSurfaceTarget` 的 `None` 早退拿掉 ⇒ 新用例 5 条断言红（已回退）。

## 2026-10-19 执行器自持帧尾 present（`graphics-window-device-viewer.md` §5 规则 8 / §9 待决 5）

- **结论**：`executeFrame(frame, present_to)` 在返回前把帧驱动到尾巴——有目标时经它 `present()`（本后端里那就是
  “提交 + 呈递”那一步），`nullptr` 时走离屏会话只提交。引擎（`RenderEngine::frame()`）与直接驱动一格的宿主
  （`RenderPass::execute()`）都不再 present；test_vsg 的 drive 直接调 `executeFrame`，不再跟一个 `present()`。
- **契约文字**：`PresentTarget::present()` 改为“执行器在帧尾调它，驱动者不调”；`RenderFrameExecutor` 的
  “一帧是参数不是区间”一条补上“有目标时也在返回前完成”。
- **测试双实现**：`test_graphics` 的 `HeadlessBackend/WindowedBackend/FleetBackend/MockBackend`、`test_gui` 的 stub
  在 `executeFrame` 尾部调 `present_to->present()`（计数因此不变）；否则它们的 `presents` 见证会归零。
- **判据**：构建 0 错 0 警；`ctest` 24/24；`test_vsg` 434 = 431 + 3 skip；`test_graphics` 376；`test_gui` 259；
  六道门禁 0 finding。变异：`VsgBackend::executeFrame` 不再驱动帧尾 ⇒ `TheSdkFacingBackend...` 5 条断言红。

## 2026-10-13 `SceneView::onSurfaceResized` → `resize`（公共 API 改名；`on*` 从此只留给接收点）

- **结论**：视图的尺寸入口改名 `resize(w,h)` / `resize(w,h,ratio)` 两个重载，与 `RenderSurface::resize` 同词
  ——"拿着谁就调谁的 `resize`"：挂着表面时视图转发给表面，不挂表面时记自己那份（`own_*`）。动机：本仓已定
  "resize 跨过宿主边界是**状态公告**不是事件"（引擎那半边 2026-09-07 已由 `pushEvent(ResizeEvent)` 改名
  `resize(w,h)`），视图这个事件味的 `on` 名是该次改名的遗留。候选 `notifyResized` 否掉：把"通知"框回来
  （通知可忽略，这条必须落地）、语法缺宾语、破坏与 `RenderSurface::resize` 的配对。`on*` 保留给**接收点**：
  `CameraManipulator::onMouse*/onKey*/onResize`、`AxisGizmo`/`FpsOverlay::onSurfaceResized`（管线通知它们重排，
  **故意不动**）、`DockPanel::onClosing`。
- **判据**：语言服务级改名（两个重载各一刀，9 + 13 处）；`SurfaceWindow::feedSurfaceSize` 调用点、
  `GraphicsTest`/`RenderSurfaceTest`/`SdkPixelTest`/`ModelViewerTest` 全部改形；SDK 交叉引用注释
  （`RenderSurface` / `RenderPipeline` / `AxisGizmo` / `FpsOverlay` 四个头）与两份指南
  （`docs/guides/render-views.md` / `docs/guides/graphics.md`）、`appfw-render-view.md` 现役文字跟进。全量构建 0 错 0 警；
  `ctest` **24/24**（`test_graphics` 375 例）；六道门禁 0 finding（`public_docs` 1156 基线 / 0 new / 0 stale）。
- **边界**：历史记录（memory 旧条目、`graphics-render-next.md` T1 行里删旧代码的引文、旧设计稿、bug 单）
  保留旧名，读旧名处一律读作 `resize`。

## 2026-10-13 层掩码的后续消费者落地：拾取与包围盒（`graphics-layers.md` §9.1 / §9.2）

- **结论**：`RayIntersection::intersectScene/intersectSceneAll` 带 `layer_mask`；`Scene::boundingBox(layer_mask)`。
  语义与收集同一条公式、同一判定点（可绘制节点；容器是结构；最近声明赢），`isVisible` 仍是先过的硬门；
  两条默认 `kAllLayers` ⇒ 逐位老行为。
- **判据**：`RayIntersectionTest.TheLayerMaskSelectsWhatIsPicked`、`SceneTest.TheSceneBoxFollowsTheLayerMask`
  （默认不变 / 正反掩码 / 重声明计入 / 隐藏优先 / 空掩码空盒）；全量 `ctest` **24/24**（`test_graphics` 375 例）。
- **未做**：§9.3 子树层并集整段跳过（仍等触发）；阴影拟合 / 取景的消费方接线（按 §9.2 落点，等真实负载）。

## 2026-10-13 `Scene::boundingBox` 语义补齐：可见性按层级剪（此前只盖场景/根两级）

- **结论**：`Scene::boundingBox()` 现在回答"**可见**场景"的盒：`Scene.cpp` 新增 `visibleWorldBox()`，
  场景内**任何层级** `setVisible(false)` 的节点**整棵子树不贡献**（与收集/拾取遍历同一条规则）。
  `Node::boundingBox()` 保持**纯几何**（设计与文档原文不改，Node.hpp 加了"可见性不在此折叠"的交叉引用）；
  空盒情形不变：场景隐藏 / 无根 / 根隐藏。
- **此前**：只在顶层"场景可见 / 有根 / 根可见"三个门口剪，树内隐藏节点仍贡献盒 —— 与用例名
  `InvisibleNodeExcludedFromBoundingBox`、以及"取景/夹持/阴影拟合只应看可见内容"的消费方
  （`OrbitCameraManipulator`、`RenderPipelineBuilder`）不一致。
- **判据**：新用例 `SceneTest.HiddenSubtreeContributesNothingToTheSceneBox`（中间层隐藏 → 盒从 0..11
  缩回 0..1；全部隐藏 → 空盒；同用例钉住 node 级仍非空）；`test_graphics` 373 用例全绿；文档门禁
  `check_public_docs` / `check_doc_symbols` / `check_include_hygiene` 全绿。

## 2026-10-13 门禁口径：`ctest` 全量 24/24（修前 23/24，`test_gui` 直接 abort）

- **结论**：`docs/guides/graphics.md` §4.4 把 `ctest --test-dir build` 列进门禁，而它**修前是红的**：
  `ctest -R test_gui` = **Exit code 0xc0000409（3.34 s）**（Qt：`Could not find the Qt platform plugin "offscreen"`）。
  根因是**部署缺口**，不是用例错：`windeployqt` 只带宿主平台插件（`qwindows.dll`），而 CTest 给 `test_gui`
  强制 `QT_QPA_PLATFORM=offscreen`（`tests/test_gui/CMakeLists.txt`，为了 WSLg / 远程桌面不弹窗）；
  `cmake/VineDeployQt.cmake` 里那段"把 offscreen 插件复制过去"的代码**从来没被跑过** —— `DeployQt` 是自定义目标，
  无 preset、无依赖、也不是 `ALL`。
- **修法**：插件发现提成 `vn_find_qt_offscreen_plugin_dir()`（Release/Debug 两种拼写**都查磁盘**），
  `tests/test_gui` 给测试目标挂 POST_BUILD 复制（插件与 Qt 库同目录 = 测试可执行文件旁边）；
  那条**脚本化真实文件对话框**的用例在 `offscreen`/`minimal` 下 `GTEST_SKIP()`（无头平台没有可脚本化的模态窗口，
  等它的人只会空转到死）；§4.4 的"三个套件既有失败"过期句按实测改掉（那三个今天 5/5、28/28、12+1 skip 全绿）。
- **判据（实测）**：`ctest -R test_gui` **Passed 15.33 s**；全量 `ctest --test-dir build -C Release` **24/24 通过**
  （含 `test_cppstd` 36.6 s / `test_vsg` 18.5 s / `test_gui` 15.6 s）；手跑本模块套件仍按约定加
  `--gtest_filter=-UserIOTest.PickingFilesFillsTheLineWithoutSubmitting`（**Windows 原生对话框**下那条今天仍会红，
  脚本拿不到 `QFileDialog`），259 条全绿。
- **坑**：①`if(EXISTS)` **吃不了生成器表达式** —— 原来的 Windows 分支靠 `WIN32 OR` 绕过检查就是这个原因，
  所以"插件文件名带 `$<CONFIG:Debug>`"时只能两种拼写都查磁盘；②`DeployQt` 这种"只改部署、不产出文件"的
  自定义目标**没人记得跑**，凡是被门禁依赖的部署都要挂在门禁真正构建的目标上（POST_BUILD）。

## 2026-10-13 同一 pass 内的画序：`Node::setDrawOrder`（对象模型缺口 1 落地）

- **结论**：`Node::setDrawOrder(int)` / `drawOrder()`（`std::optional`，只答"本节点声明了什么"）/ `clearDrawOrder()`，
  值落在 `RenderCommand::drawOrder`。**高 = 后画**（压在上面），默认 0；**子树继承，最近的声明赢**（与 `setLayers`/
  `opacity` 同一个 `InheritedState` 折叠）；**它是 pass 内那张列表的外键** —— 同序之内仍是老画面（不透明 前→后、
  透明 后→前）。**跨 pass 与它无关**：那是 `PassDesc::order` / `PipelineStage` 的事。
- **它买到的**：不透明的高亮 / 描边 / 贴花能**越过透明批次**画在最后（老规则里不透明永远在透明之前 ⇒ 做不到），
  而不用为它开一条 pass —— 这正是"同一 pass 内覆盖式绘制"那个触发条件要的东西。
- **判据（实测）**：全量 Release 0 error / 0 warning；`test_graphics` **372**（368 + 4：外键 / 跃迁 / 继承 / 叶子声明），
  `test_vsg` **431** = 428 + 3 skip（430 + 1 像素用例），`test_gui` 258 / `test_appfw` 32 / `test_meshio` 34 /
  `test_iobase` 94 不动；五 gate 0 finding、`--strict` 仍 13。
- **像素判据**（`SdkPixelTest.ADrawOrderDecidesWhichOfTwoCoplanarQuadsIsOnTop`，真设备）：两张**共面**四边形（红 / 绿）
  画进同一张目标，读到中心像素 —— 默认（都没声明）**绿**在上面 `(0,255,0)`（树里靠后的后画，稳定排序保住遍历序）；
  给红的一句 `setDrawOrder(1)` ⇒ 中心变 **红** `(255,0,0)`。同一个像素换了主人，这就是"后画的赢"。
- **坑 1（写像素判据时会踩）**：共面 + 深度测试 ⇒ **后画的那张会被深度拒绝**（同深度 LESS 不成立），量到的是深度不是
  顺序。像素用例必须 `DepthMode::Disabled`（我这条就是这么写的），否则无论画序怎么变，中心像素都不变。
- **坑 2（测试 helper 的形状）**：`makeTriangleNode` 这类 helper 把声明落在**几何的父节点**上 ⇒ 那条路径走的是"继承"，
  所以专门留了一条用例把声明直接放在**叶子几何**上（`DrawOrderTest.TheDeclarationAppliesAtTheDrawableToo`）——
  变异 3（去掉继承）只让前三条红、这一条仍绿，正好说明两条路径是分开的。
- **变异 4/4（各自单独实测红）**：① 键被收集但排序不看它 ⇒ 外键/跃迁/像素三条红；② 键放在批次**之内**（先比不透明/
  透明再比画序）⇒ **只有跃迁那条红**（这条变异正是"外键 vs 批内键"的判别器）；③ 去掉继承（`value_or(0)`）⇒ 前三条红、
  叶子那条仍绿；④ 比较符号翻过来 ⇒ 外键/跃迁/叶子/像素红，继承那条仍绿（它量的是值不是序）。
- **后端零改动**：列表次序就是录制次序，后端不读 `drawOrder` 也已正确。

## 2026-10-12 设备槽位用尽：两扇门都答成理由，而不是把 vsg 的异常丢给宿主（`VSG_MAX_DEVICES`）

- **结论**：本 build 的同时渲染设备数**就是 4**（vsg 的 `VSG_MAX_DEVICES`），而"一块渲染面 = 一次 attach = 一个设备"
  （`RenderControl` 每开一个 3D 文档一个）⇒ 开第 5 块面是**用户真会撞上的边界**。现在两扇门都把它答成**理由**：
  无窗 seam（`createDevice`，无头相位/测试走它）返回 `ok == false` + 句子；会话（`Session::initialize`，应用走它）
  报 `InitFailed` + 同一句子并返回 false ⇒ 控件的 `Failed` + `failureReason()`。
- **坑 1（这次的真病根）**：vsg 拒绝创建设备的方式是**抛** `vsg::Exception`，而它**不是** `std::exception`
  （裸 struct：message + result；`gfx_backend_vsg` 自己的注释在 `submitViewerTasks` 里早就写明了这一点）——
  `Session::initialize()` 的 `catch (const std::exception&)` 接不住它，异常会逃出 SDK 进 Qt 事件循环（= 崩）。
  修法：`Session::initialize()` 前面加 `catch (const ::vsg::Exception&)`；`createDevice()` 里把 `Device::create`
  包进 try/catch（这一处原来只处理"返回 nullptr"）。
- **一句话一个 owner**：`deviceFailureMessage(const ::vsg::Exception&)`（`vine/vsg/internal/Device.hpp` 导出）——
  vsg 的原话在前（driver 拒了什么只有它知道），后面接本 build 的固定事实（上限数字取自
  `vsg::Device::maxNumDevices()`，**不硬编码 4**）+ 宿主能做什么（"一块视图一个设备，先关一个视图"）。两扇门共用它，
  所以宿主读到的句子与哪道门无关。
- **坑 2（文档三方矛盾，一并收口）**：`gfx_backend_vsg/CMakeLists.txt` 的注释声称"取 `>4` 的动态分支、设备数不受限"
  ——**假话**（值就是 4，选中的是 `<=4` 的固定 `std::array` 分支；`>4` 分支在 1.1.16 坏在 **const** `operator[]`
  不增长、`size()` 只反映写过的槽）；`ModelViewerPlugin.hpp` 说 `VSG_MAX_DEVICES=1`（旧值）；`ModelRenderView.hpp`
  说"设备数上限已解开"（假）。三处按事实改写，`.ai/memory/appfw.md` 那条"必须由我们拒（未做）"改成"已落地"。
- **判据（实测）**：全量 Release 0 error / 0 warning；`test_vsg` **430**（429 + 1 新用例
  `VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException`：填满上限 ⇒ 无窗 seam 与**会话**都答理由、
  句子含上限数字；放掉一个槽 ⇒ 下一次创建成功）；其余套件计数不动；五 gate 0 finding、`check_ai_docs --strict` 仍 13。
- **变异（各自单独红）**：① 去掉 `createDevice` 的 catch ⇒ 用例在第一步就带着 vsg 异常中止；② 去掉 `Session` 的
  `vsg::Exception` catch（恢复成只接 `std::exception`）⇒ 用例在第二步（会话门）抛出去。
- **副产品**：`VsgBackend::initialize()` 注释里"re-initialize 会同时要两个设备，vsg 拒绝"这句现在有了明确的失败
  面（同一句话），不必再靠推断。

## 2026-10-12 内容掩码（分层渲染 L1）落地：一条 pass 只画它要的层（`graphics-layers.md`）

- **结论**：内容选择是**两个值** —— 对象侧 `Node::setLayers/clearLayers/layers`（子树继承，最近的声明赢；`clearLayers()` 回到继承），
  pass 侧 `PassDesc::layer_mask` / `RenderPass::setLayerMask()`；判定是**一条 AND**，落在**可绘制节点**上。默认（掩码 `kAllLayers`
  + 未声明节点在 `kDefaultLayer`）⇒ 今天的行为**逐位不变**。
- **同一份设计的 L2（目标层）裁决：现阶段不做**（2026-10-12，登记在 `graphics-layers.md` §6 开头）：没有消费者（无点光立方体阴影 / CSM / 双眼），
  替代派（N 张 2D 目标）字节数一样，而 L2a **省不了收集与提交**（每层一台相机 ⇒ 每层一次 walk）—— 省钱的那半是 L2b/`multiview`，
  且三条语义（清屏只洗"本条 pass 写的层"、深度层数一致、读侧带层）该由第一个消费者拍板。触发条件与落地顺序见 §6。
- **两个常量一个 owner**：`kAllLayers`（掩码默认）与 `kDefaultLayer`（未声明节点的层，bit 0）都在 `Node.hpp`；需要它的公共头
  直接 include（`FramePlan.hpp` / `RenderPass.hpp` / `RenderPipeline.hpp` 各加了一行，hygiene gate 要直接 include）。 <!-- drift-ok -->
- **掩码进 memo key**（`ContentMemo::Entry::layer_mask`）：同一相机、同一帧、两条不同掩码的 pass **各走一次**（`+content_collect_count_`），
  同掩码则复用 —— 这是本片最容易错的一处：不进 key 就会"画面看着对、只是多了/少了几个物体"。
- **帧值如实但进对的地方**：掩码进 `FramePlan::PassPlan::layer_mask`（`describe()` 只在非默认时打 `layers=0x..`），**不进**
  `PassDraw` —— 后者是后端消费的绘制形状（目标/视口/清屏/状态/绘制），掩码是**选择的理由**。
- **配方入口**：`PipelineOptions::layer_mask`（`build()` 开头取一份 `content_layers_`，与 `shadows_built_` 同款"一次 build 一个值"），
  六个走收集的内容 pass 各盖一次（阴影 / 前向主 / 前向透明 / G-buffer / 延迟透明 / 预览）；**两条 Screen 灯照 pass 不盖**
  （content 只用来取灯，盖了是假话）、**gizmo / 帧率 overlay 不盖**（UI 不该被内容选择藏掉）；appfw `ViewerOptions::layer_mask` 原样转发。
- **掩码 0**：合法（选不中任何内容）⇒ 内容 pass 零绘制，并且 compile 报一次（`ContentSkipped` + episode 规则）——
  "跑了但没东西"而不是静默空画面。
- **坑 1（第一版错、被新用例抓到）**：掩码**不能在遍历里剪枝容器**。第一版在 `collectNodeCommands` 入口判 AND ⇒ 根 `Group`
  未声明（bit 0）⇒ `(bit0 & 0x2) == 0` ⇒ 整棵树被剪掉，**声明了层的子内容不可达**。现在只对 `Geometry` 判（容器是结构、不是内容），
  与"Unity 的层在渲染器上"一致。
- **坑 2（默认层的选择）**：未声明节点取 `kDefaultLayer`（bit 0）而**不是**"所有层"：选择性掩码的用处是**排除**没声明的内容
  （阴影只画 caster 时不必给其它所有物体声明层）。three.js / Unity / Qt3D 都是这个默认层模型；Ogre-Next 两边全 1，**刻意不跟**。
- **读侧刻意不对称**：`Node::layers()` 只回答"本节点声明了什么"（`std::optional`），**不做上溯解析** —— 与 `StateNode` 的两套读法
  不同，理由写在文档 §2.2（问"为什么没画"的是 pass，答案在收集时算一次，第二份实现只会漂移）。
- **已给后续留的入口**：L2 的读侧先行（`ImageRef` 带层 + 回读带层）写在 `graphics-layers.md` §3.3；缺口的行业依据（Ogre-Next
  的 `getNumSlices` / 层级 render pass 描述）见 §7。
- **判据（实测）**：全量 Release **0 error / 0 warning**；`test_graphics` **368**（361 + 7 新用例）、`test_gui` **258**（257 + 1）；
  `test_vsg` **429** = 426 + 3 skip、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 不掉；五 gate 0 finding，`--strict` 仍 **13**。
- **变异 5/5 各自单独红**：① 掩码不进 memo key ⇒ 复用那条红（C1 仍绿）；② 掩码收下但不过滤 ⇒ C1/C2/C3/C4/C7 红；
  ③ 继承不生效 ⇒ C1/C3/C4 红；④ 不进 `PassPlan` ⇒ C5/C6/C7 红；⑤ overlay 也盖掩码 ⇒ C8（graphics 配方那条与 appfw 那条）红。

## 2026-10-12 框架级 API 复核的落地：区/引擎/目标的基数与契约收紧（`graphics-api-review-2026-10-12.md`）

- **结论**：独立会话的只读复核给了 6 条发现，其中 1/2/3 是**同一条链上的同一类错** —— "一条事实有多个持有者，
  其中一个永远在说'我仍然有效'"，今天全靠"只有一块区、一块窗口、后端把自己当目标"三个巧合成立。四条已修、
  一条不做（改名并进下次 ABI 变更）、一条改口径。复核全文与"看着可疑但确实合理"的清单见
  `.ai/design/graphics-api-review-2026-10-12.md`。
- **发现 1（引擎 : 区 = 1:1 曾是静默顶替）**：`RenderEngine::adoptArea` 现在**拒绝**第二块区并报一次
  （`UnsupportedRequest`，`ReportOnce`）；`announceAreaSize(raw_ptr<const RenderSurface> from, w, h)` 只认引擎
  自己那块区（被顶替的区再也不能改写 `frame_ctx_`）；不是引擎的区 `present` 也拒绝 + 报一次。原先"一引擎两区"
  只在 `RenderSurfaceTest` 里被当作便利使用（尺寸扇出那条），改成"第二块窗口 = 第二个引擎"。
- **发现 2（`present(RenderPass*)` 不校验 + 呈现永不失效）**：`present(pass)` 要求
  `engine_->passOrder(pass).has_value()`，否则拒 + 报一次；`presentingPass()` / `hasPresentation()` 按"还在帧里"
  过滤（`removePass` / 清掉 pass 之后区**不再**说自己在被画）。顺带一条**规则退场**："视图已呈现另一块区"在
  "一引擎一区 + 视图必须同引擎"之下**不可达**，检查与它的用例一并删除（留一条注释说明为什么不需要）。
- **发现 3（借来的目标没有撤销点）**：契约定成"**活到设备销毁或区解绑**，窗口关闭只是设备自己的 detach
  （句柄没了，此后 `resize` / `present` 是 no-op 并如实回答）"；`RenderEngine::shutdown()` 在拆设备**之前**
  `area_->bindTarget(nullptr)`。后端零改动（`VsgBackend` 仍把自己当目标）。
- **发现 4（可空引擎的区）**：`RenderSurface(RenderEngine&)`（非空由签名保证，与 `present(SceneView&)` 同款）；
  调用点 31 处机械替换；`present`/`resize`/析构里的 `engine_ != nullptr` 死分支删掉。
- **发现 5（公开头注释漂移）**：`SceneView.hpp` 六处"视图自己的矩形"改回事实（一个区一幅画）、`passOrder` 的
  读者改成宿主/测试、`PresentTarget.hpp` 的 `RenderEngine::render()` 改成 `frame()`、`RenderEngine` 类注释块与
  `class` 拆行。
- **判据（实测）**：全量 Release 0 error / 0 warning；`test_graphics` **361**（357 + 5 新用例 − 1 条不可达用例）；
  `test_gui` **257**、`test_vsg` **429** = 426 + 3 skip、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 不掉；
  五个 gate 0 finding（`--strict` 仍 13）。新用例：`ASecondAreaOnOneEngineIsRefusedAndReportedOnce` /
  `OnlyTheEnginesOwnAreaFeedsTheFrameContext` / `APassThatIsNotInTheFrameCannotPresentTheArea` /
  `APresentationWhosePassLeavesTheFrameStopsPresenting` / `AStoppedEngineUnbindsTheAreasTargetFirst`。
- **变异 5/5 各自单独实测红**：① `adoptArea` 恢复静默顶替 ⇒ 第二块区那条红；② 去掉 `announceAreaSize` 的
  "只认自己那块区" ⇒ 帧上下文那条红；③ `present(pass)` 去掉注册校验 ⇒ 未注册 pass 那条红；④ `hasPresentation()` /
  `presentingPass()` 去掉"在帧里"过滤 ⇒ pass 离开帧那条红；⑤ `shutdown` 不解绑 ⇒ 目标寿命那条红。
- **坑**：① "一引擎一区"一落地，**另一条规则就没法被构造出来了**（视图已呈现另一块区）——删规则时要连它的用例
  一起删，别留一条断言恒真的用例；② 现有用例里"顺手建第二块区"的便利写法（`RenderSurfaceTest` 量到尺寸那条）
  必须改成第二个引擎，否则它测的其实是"死区"；③ 改公开签名（本次：引擎从可空改成引用）之前先数调用点，31 处
  机械替换比手改稳。

## 2026-10-12 两条"已知红"清掉：都是**测试侧**的锅（机器相关偏移 / 读像素早于完成点）

- **结论**：`test_vsg` 不再有已知红 —— **429 = 426 passed + 3 skip**（此前 424 + 3 skip + 2 failed）。两条都不是产品缺陷，
  是 T3 把 23 条误跳恢复后才暴露出来的**用例自身**问题：
  1. `BlockDescriptorsTest.TheBindCarriesTheOffsetsInBindingOrderAndReusesOneSet` 硬写 `{0, 80, 0, 0, 0}` /
     `{256, 336, 64, 112, 160}` —— 只在"`minUniformBufferOffsetAlignment` 整除 16"的设备上合法（lavapipe 那类），
     本机 alignment = 64 ⇒ `bind()` 如实**拒绝**并计数（`usableOffset`），用例拿到的第一个绑定就是 nullptr。
     **修法**：第二组偏移改用存储自己的**区域步长**（`BlockStorage::strides()`，建环时已按设备对齐向上取整），
     于是"同一角色、再进一块"在**任何设备**上都合法；用例想钉的两件事（一绑定一偏移、按绑定序；两次绑定复用同一个 set）不变。
  2. `SessionTest.ADriveThatCommitsThroughTheSessionMarksWhatALostFrameWrote` 在 `commit()` 之后**立刻**探针读像素 ——
     探针读的是 GPU 的 copy 节点填的**映射缓冲**（`OffscreenTarget::probe` 自己不等待），此时提交还在飞 ⇒ 读到全零。
     同文件的姊妹用例早就把这条写清楚了（"a frame waits the fence of the slot it enters"），并且用"再驱动几帧"来同步。
     **修法**：把这一次读取挪到**会话自己停下之后**（`shutdown()` 是这一半唯一现成的完成点），并写明"顺序就是要点"。
- **判据（实测）**：那两条各自单独跑绿；`test_vsg` **429 = 426 + 3 skip**、0 failed；其余套件不动（graphics 357 / gui 257 /
  appfw 32 / meshio 34 / iobase 94）；全量 Release 0 error / 0 warning；五个 gate 0 finding（`--strict` 仍 13）。
- **变异 3/3 各自单独实测红**：① 把对齐检查从 `usableOffset` 拿掉 ⇒ `AMisalignedOffsetIsRefusedInsteadOfBound` 红
  （证明改偏移用例**没有**把"拒绝"这条契约放松）；② 绑定把 offsets **倒序**写进命令 ⇒ 修好的那条红（+ 声明形状那条），
  即它现在真的在钉"按绑定序"（挨拒时它什么都钉不住）；③ 把探针读**挪回** stop 之前 ⇒ 那三条 `EXPECT_NEAR` 红
  （r/g/b 差 64/128/191 = 读到全零），证明绿的原因是顺序而不是别的。
- **坑**：① 硬写的"魔法偏移"会把**设备能力**（alignment）冻进用例，换台机器就红 —— 偏移要从存储/设备**问出来**；
  ② 探针读的是**映射内存**，不是"读回即等待"：任何"提交后马上读"的断言都必须先有一个完成点（帧栅栏、设备 idle、
  或驱动足够多帧），这条在姊妹用例里已经写过一次，这次是第二条用例犯同一错；③ 两条红都在 `test_vsg`（真设备电池），
  在没设备的机器上会**跳**过 —— 所以"绿"要在**跑过设备电池**的地方才成立（`skipped == 0` 才算）。

## 2026-10-12 呈递端拆成两层：`PresentTarget`（帧的契约）+ `WindowPresentTarget`（窗口那一面）

- **结论**：`RenderSurfaceTarget` 这个名字下压着两件用途不同的事 —— 帧需要的（尺寸 + `present`）与只有拥有窗口的宿主
  需要的（`setWindowHandle` / `nativeHandle`）。拆成两层、两个头文件：`PresentTarget`（`resize(w,h)` + `present()`，
  **帧的契约**）与 `WindowPresentTarget : PresentTarget`（加那两条窗口动词）；`RenderSurfaceTarget.hpp` 删除。 <!-- drift-ok -->
- **谁认哪一层**：**帧只认基类** —— `RenderFrameExecutor::executeFrame(frame, PresentTarget* present_to)`、
  `RenderPass::execute(scene, backend, present_to)`、`RenderEngine::own_target_`（引擎只在帧尾 `present()`）。
  **窗口那一面归造它 / 持它的两处** —— `RenderDevice::createSurfaceTarget` 答 `raw_ptr<WindowPresentTarget>`，
  `RenderSurface::bindTarget/target()/target_` 是 `WindowPresentTarget`（宿主在平台重建窗口时经它重公告句柄）。
  appfw 的逻辑一字未改（只换类型名）；`SurfaceTargetResult` 名字不动（它答的是"这把窗口的目标造没造出来"）。
- **所以"第二个呈递端"（云渲染 / 编码 / 网络出口）从"要改契约"降成"纯实现"**：一个只实现 `resize` + `present` 的对象
  就能被帧命名、被宿主驱动（见新用例）。
- **判据（实测）**：全量 Release 0 error / 0 warning；`test_graphics` **357**（356 + 1 新用例）、`test_gui` **257**、
  `test_vsg` **429** = 424 + 3 skip + 2 已知红（未动）、appfw 32 / meshio 34 / iobase 94 不掉；五个 gate 0 finding
  （`--strict` 仍 13，同一批既有路径漂移）。
- **新用例**：`HeadlessBackendTest.APresenterThatIsNotAWindowIsNamedByTheFrameItsHostDrives` —— 一个只有
  `resize` / `present` 的 sink 被 `RenderPass::execute` 命名、并被那次驱动的帧尾 present（整条路径不问一个句柄问题）。
- **契约级变异（实测）**：把 `RenderPass::execute` 的参数改回 `WindowPresentTarget*`（声明与定义两处一起改，并换 include）
  ⇒ **新用例编译不过**（C2664：`RecordingSink*` 不能转 `WindowPresentTarget*`），另有一处用例内的 `CountingPass::execute`
  override 失配（C3668）。即"帧收基类"这条是**编译期**钉住的，别的用例替不了它。
- **坑**：① 纯改名也要认"哪半"：`RenderSurfaceTarget` 在 20 个文件里出现 114 次，其中约 20 处属于帧那半、其余属于窗口那半；
  机械全替成窗口面**能编译**（派生 → 基类永远隐式可转），只有"基类用在窗口处"才报错 ⇒ 批量替换抓不出"帧该收哪种"，
  要靠上面的新用例钉；② 同一文件里 `executeFrame` override 的文本可能完全相同（`HeadlessBackendTest` 有三个双实现），
  批量替换必须按出现次序逐个确认（`replace(..., 1)` 一次只替一处）；③ 漏改一个 override = 派生类没实现纯虚 ⇒
  `C2259 无法实例化抽象类`；④ `raw_ptr<T>` 是裸 `T*`：`RenderPass::execute` 收 `Scene*`，`intrusive_ptr` 要 `.get()`。

## 2026-10-11 多格下线：`RenderSurface` 收成"一条呈现 + 尺寸 + 目标"（S8/S9 的表与路由删掉）

- **结论**：区不再是一张表，而是"**哪条 pass 画这块窗口**"这一条声明 + 尺寸 + 目标：
  `present(SceneView&)` / `present(RenderPass*)` / `dismiss(view|pass)` / `hasPresentation()` /
  **`presentingPass()`**（解析出来的那条 pass，视图重建后自动跟上）/ **`presentedView()`** / `resize(w,h[,ratio])` /
  `pixelRatio()` / `bindTarget`·`target`·`hasTarget`。删掉：`present(view, rect)`、`setViewport`、`presentations()`、
  `Presentation`、`viewAt`、`focus`/`focusedView`、`viewportOf`、`applyClear`（清屏合并）、`Entry`/`Resolved` 那套；
  `SceneView` 删掉自己的矩形（`viewport()/setViewport`、`paneSize`、`deviceViewport`）—— 它的画面就是这块区，
  `applySurfaceGeometry` 只从区尺寸推：相机 aspect、管线 resize、布局步，**不再给窗口 pass 设视口**（全幅 = pass 不设
  viewport 的默认含义）。**归一化矩形这个类型一起删**（`Viewport.hpp` 的 `NormalizedViewport`：修完之后一个使用者都
  没有），`SceneView.hpp` 顺带去掉那个已经没人用的 `#include "Viewport.hpp"`。
- **两条新硬规则**：**一块区只画一幅画** —— 第二条呈现（不论视图还是 pass）**被拒 + 报一次**（`ReportOnce`，撤掉第一条后
  再报是新一轮）；但"同一个东西再来一次"不算第二条（`present` 同一视图 / 命名它自己的 window pass ⇒ 返回 false 不报）。
- **为什么可以删掉清屏合并**：一次只画一幅画 ⇒ "谁第一"没有歧义，条目的 ask 就是它的 ask（旧 `applyClear` 在单条情形下
  本来就是恒等变换）；**区从此不碰呈现者的清屏策略**（有判据：`present` 前后 `isClearEnabled()` 不变，重建后的新 pass 也
  带着它自己的默认 ask）。
- **判据（实测）**：全量 Release 0 error / 0 warning；`test_graphics` **356**（364 − 8）、`test_gui` **257**（261 − 4）、
  `test_vsg` **429** 未动（424 + 3 skip + 2 已知红）、`test_appfw` 32、`test_meshio` 34、`test_iobase` 94；
  `RenderSurfaceTest` 13 条（原 19）覆盖：声明的解析与幂等、宿主 pass、pass 随重建走、三种拒绝（别的引擎 / 已有区 /
  无 pipeline）、一块区一幅画、尺寸唯一 owner（含"挂上时立刻拿到"与"摘掉后不再被喂"）、不碰清屏、
  视图先死 / 区先死两种生命周期。五个 gate 0 finding（`check_ai_docs --strict` 仍 13 条既有路径漂移）。
  文档同步：`docs/guides/graphics.md` §3.10/§3.11 与 `docs/guides/render-views.md` 整篇（单幅画面 + "要多幅画面自己
  接线"），设计记录里给 §12.2 与 S8 两条加了"已下线"抬头。
- **变异 5/5 各自单独实测红**：① 重建不搬活链的运行期选择（`stats_was_hidden = false`）⇒
  `RenderControlTest.ARebuildCarriesTheLiveChainsRuntimeChoices` 红；② `Viewer::release` 不撤声明 ⇒
  `ReleasingTheViewTakesItOutOfTheArea` + `AHostPassTakesTheAreaAndTheViewerStandsDown` 红（后者正因为接管要两步）；
  ③ 第二条呈现"报道后照样落座" ⇒ `RenderSurfaceTest.OneAreaDrawsOnePicture` 红（同套件另 12 条绿）；④ 拒得静默
  （`reportSecondPresentation()` 空转）⇒ 同一条红在诊断计数上（0 vs 1、0 vs 2）；⑤ `Viewer::isActive()` 改成读记忆
  （`view != nullptr`）⇒ `AHostPassTakesTheAreaAndTheViewerStandsDown` 红。
- **坑**：① 删除"表"时必须一起删**读它的两半**（appfw 的 `Viewer` 与 `SurfaceWindow` 的输入路由），否则会留下"表面
  路由"的死代码路径 —— 输入现在只有一条路：`SurfaceWindow::routedView()` 返回控件那个视图；② 删实现容易漏赋值
  （见 `.ai/memory/appfw.md` 同条坑 ①）；③ `SceneView::surfaceWidth/Height/PixelRatio` 提成公开读法：宿主与用例都要
  问"我按哪个尺寸算画面"，而它现在是"有区时读区、没区时读自己的公告"这**一个**答案。

## 2026-10-11 修：延迟预设的 G-buffer 几何 pass 从来没拿到过自己的几何程序（默认 demo 后三格预览全黑）

- **结论**：`RenderPipelineBuilder::buildDeferredPath` 把 G-buffer 几何程序写进了 `PassDesc::program`，而那是
  **Screen pass 的字段**（`RenderEngine::addPass(PassDesc)` 只对 `PassKind::Screen` 用它，Scene pass 只读
  `PassDesc::program_override`）⇒ 那条 pass **一个程序都没有**：每个物体退回自己的 program（没有的退引擎默认的
  `forwardProgram()`），而前向程序只写 **一张** 颜色输出 ⇒ 几何只落进 albedo，法线 / specular / 视图位置三张附件
  停在**透明黑**清屏值上。用户看到的就是默认 demo 左上的**后三格全黑**。改成 `gbuf_desc.program_override = ...`
  一行即修（程序选择本身没变、pass 表没变、`PipelineOptions::gbuffer_program` 的语义没变）。
- **判据（实测，无头像素夹具 `test_vsg`）**：修前四格复制回来 `albedo=(38,38,38,255)`（= 清屏色，几何根本没画进来）、
  `法线=specular=位置=(0,0,0,0)`；修后 `(255,255,255,255)` / `(0,0,255,32)` / `(127,127,127,255)` / `(21,0,0,255)`
  —— 法线格的蓝拉满 + alpha=32（默认 `shininess` 32 ⇒ `32/256`）正是"附件 1 就是法线+光泽"的指纹。同一台机器上
  手工搭一条 `setProgramOverride(...)` 的 pass 写同一张目标，四格**当场全是真数据** ⇒ 断点在这条**声明**的字段上，
  不在交付链上（这条对照就是把范围一刀切开的那个实验）。
- **同一个 bug 的更大一半（主画面）**：附件 3 没写 ⇒ 光照程序对**每个**像素都走 `pos4.a < 0.5` 的 background 分支
  （固定 `0.06` 灰；夹具实测主画面 `(15,15,15)` = `0.06*255`），阴影、光照全落空 —— 所以"后三格预览黑"只是这个 bug
  最容易看见的那一面（夹具里连 albedo 都停在清屏色：四张附件的 pass 配一支**只写一张输出**的前向程序，几何一张都没
  落进去）；修完四格预览与主画面**一起**变对。
- **变异 3/3 红**（各自单独实测）：A 把这一行改回 `program` ⇒ 接线用例与像素用例**都红**（像素回到 38 / 0,0,0,0）；
  B 屏内 ABI 的绑定恒取附件 0（`source->colors[0]`）⇒ **只有**像素用例红（四格复制回来全一样），接线用例仍绿，两条
  判据互不代劳；C 在 `EngineCompile::addPass(PassDesc)` 里把 `program` 当成 Scene pass 的 override ⇒ 两条都红。
- **坑**：① `PassDesc` 有两个程序字段、**按 pass 种类二选一**，写错了**不报错**——Scene pass 静默忽略 `program`，
  于是症状落在"少写了几张附件"而不是"程序没生效"，这是本仓库最难从结构断言里看出来的那一类；② 后三格的**透明黑**
  骗人：MRT 的额外附件清屏值就是 `(0,0,0,0)`（见 `core/ClearPlan`），而全屏复制**不混合**（`draws_content=false`
  ⇒ `ONE/ZERO`），所以"黑"只说明**源是清屏值**，不说明"复制没发生"——判"附件写没写"要选一张**内容不可能为黑**的
  （法线格的蓝、albedo 格的颜色）；③ 视图位置格**天生就暗**（视图空间的 z 为负，8 位窗口钳成 0），别把数据本身当缺陷。
- **判据（套件 / 门禁）**：全量 Release **0 error / 0 warning**；`test_graphics` **364**（条数不变；`DeferredPresetBuildsGbufferAndLightingPasses`
  加了接线断言：几何 pass 的 `programOverride()` 就是请求的那支程序、lighting 是 Screen pass 所以程序在它自己的
  `program()` 上、G-buffer 是 4 张彩色附件）；`test_vsg` **429** = 424 + 3 skip + 2 已知红（+1：`SdkPixelTest.TheDeferredPresetFillsEveryGbufferAttachment`
  —— 按 demo 的写法发四格复制并读像素）；五个 gate 0 finding（`--strict` 13）。文档侧：`docs/guides/graphics.md`
  的"默认 demo"一节补了四格预览各是什么、怎么看懂"黑"，§4.2 开头补上"**demo 是启动后打开的一份文档**"
  （`Vine --open demo` 或控制台 `open_demo`：只启动外壳既不建场景也不建管线），运行配方也带上 `--open demo`。

## 2026-10-11 S10a 落地：三张契约（设备 / 执行器 / 目标）+ 目标由设备造（插件仍单窗口）

- **结论**：`RenderBackend` 那一个对象今天起拆成**三张契约**（各自一个头文件，公开面）：
  `RenderDevice`（`initialize`/`shutdown`/`capabilities`/`executor`/`createSurfaceTarget`/资源公告 + 诊断路线）、
  `RenderFrameExecutor`（`executeFrame(frame, present_to)` + `frameReport`）、`RenderSurfaceTarget`（当时一个类型带四条
  动词：`setWindowHandle`/`nativeHandle`/`resize`/`present`；**2026-10-12 拆成 `PresentTarget` + `WindowPresentTarget`**，
  见顶条）。`RenderBackend` **退成"三面一体的聚合"**（`Object + RefCounted<RenderBackend> + RenderDevice + RenderFrameExecutor`）——工厂返回它、引擎收它、
  测试双实现它，所以"一个对象什么都会"的实现不必先拆开；`RenderDevice` 的**窗口那半**是可选的（默认答
  "无呈现"，一个字都不用写），这是 S4b 那条"窗口是可选依赖"在设备层的同一句话。
- **目标由设备造（§9 待决 1 的裁决落地）**：`RenderDevice::createSurfaceTarget(void* handle, SurfaceTargetResult*)`
  → 借用的 `WindowPresentTarget*`；三因 `NoPresentation`（**静默**答，显示无关设备是一等形态）/ `UnsupportedSurface`
  / `WindowOccupied`（**报一次**）。区收**对象**：`RenderSurface::bindTarget(target)` / `target()` / `hasTarget()`；
  句柄只是造它的输入，`targetHandle()` 删掉；重建窗口是**在旧目标上重公告**（`target->setWindowHandle`），
  不是第二把目标；尺寸的窗口那一半由**区→目标**直推（`RenderSurface::resize` 一并 `target_->resize`），引擎只记
  帧上下文。
- **帧命名目标**：`executeFrame(frame, present_to = nullptr)`；引擎交的是**区**那把目标（没有区时是设备为自己造的
  那把，`own_target_`），`nullptr` ⇒ 不解析、不取像、不 present（离屏一等形态）。**present 仍由驱动者在帧尾经目标
  调用**（`PresentTarget::present`）：执行器自持帧尾 present 会让直接驱动者（test_vsg 那几十处
  `executeFrame` + `present`）present 两次 ⇒ 与 S10b 的 N 目标一起落（§9 待决 5）。
- **`Capabilities::presentation` 换答题人**：由**设备**自己答（引擎不再算"后端有没有窗口那一半"）。
  插件今天答 `true`（它有窗口那半），无头双答 `false`（S9 的行为不变）。
- **插件（仍单窗口、缓存不动）**：`VsgBackend` 实现三面（`executor()` 答自己、`createSurfaceTarget` 答自己）；第二把
  目标 ⇒ `WindowOccupied` + 一次 Warning；帧命名**外来目标** ⇒ 不记录 + 一次 Warning（`sweep`/帧账一笔不动）。
  `Data` 里多了 `target_taken` + 两个 `ReportOnce`。
- **判据（实测）**：全量 Release **0 error / 0 warning**；`test_graphics` **364**（362 + 2 新增：设备"无呈现"静默答
  / 窗口已被占用报一次；另有 S9 的用例改形为设备工厂语义，`HeadlessBackendTest` 共 7 条，含"一台设备服务两个区、
  各自的目标不串"的结构判据）；`test_gui` **261**（259 + 2：控件向设备要窗口的目标、帧的 `present_to` 就是区那把、
  设备"不呈现"⇒ attach 以理由失败）；`test_vsg` **428** = 423 + 3 skip + 2 已知红（+2：单窗口的交付与占用拒绝 /
  外来目标拒绝且自己的目标照常 present）；appfw 32 / meshio 34 / iobase 94 不掉；五 gate 0 finding（`--strict` 22）。
- **变异 4/4 红（各自单独实测）**：① 引擎不认区那把目标（只交 `own_target_`）⇒ 3 条红；② 区不把尺寸推给目标 ⇒
  `OneDeviceServesSeveralAreasWithoutCrossingThem` 红；③ 插件"窗口已被占用"不报 ⇒ test_vsg 那条红；④ 区已有目标时
  引擎仍去要一把 ⇒ 4 条红。
- **坑（今天又交了一次学费）**：`raw_ptr<T>` **就是裸 `T*`**（`using raw_ptr = T*`），`RenderSurface::target_` 少了
  `= nullptr` ⇒ 非确定性 `0xc0000005`（老测试一起崩）——与 S9 的 `RenderEngine::area_` 同一课：**新增字段一律带
  初始化值**。另一条：`intrusive_ptr<WindowPresentTarget>` 需要**该静态类型**的 ADL 计数钩子，而目标由各自的设备
  拥有（窗口关了设备放掉）⇒ 目标用**借用指针**（与 `RenderDevice::executor()` 一致），别给它第二个 `RefCounted` 基
  （两个计数器 = 双重删除）。
- **契约上的可观察变化**（给下一个片）：`RenderEngine::backend()` → `device()`，`setBackend()` → `setDevice()`；
  `RenderPass::execute(scene, backend, present_to = nullptr)`（自己驱动一格的人现在说得出它 present 到哪）。

## 2026-10-11 S9 落地：句柄与尺寸归区（引擎不再收裸句柄；`Capabilities` 加 `presentation`）

- **做了什么**：`graphics-window-device-viewer.md` 的第二片（§7 S9，半步、可逆）：`RenderEngine` **删**
  `setWindowHandle(void*)` 与 `resize(w,h)`；**区**（`RenderSurface`）成为窗口与尺寸的 owner —— 新增
  `bindTarget(void*)` / `targetHandle()` / `hasTarget()`（`nullptr` = 解绑），`resize(w,h,ratio)` 从"只扇出给视图"
  变成**唯一**的尺寸公告（先 `engine_->announceAreaSize(w,h)`：帧上下文 + 窗口那一半，再扇出给每条呈现的视图）；
  引擎留私有入口 `announceAreaSize`（`friend class RenderSurface`）。<!-- drift-ok: `adoptArea`/`releaseArea`
  是当时的做法，2026-10-20 的 SK1 已换成"引擎创建并持有区"（`RenderEngine::createSurface`）。 -->
- **判定与报告**：无呈现意图的设备（后端 `surface() == nullptr`）上 `bindTarget` **被拒（false）+ 报一次**
  （`PassProtocolViolation`，"报一次"是 episode：同一次会话里再公告还是那一条）；**公告本身留在区上**（窗口是宿主的
  话，能呈现的会话起来时接手）——拒绝说的是"这次会话接不住"，不是"这个区没有窗口"。会话起来时（`initialize()`）
  按**区当时那个**句柄交出去：公告先到、后端后到也成立，句柄换了（平台重建窗口）**搬移**而不是重建（设备、
  会话、已编译管线都留）。
- **`Capabilities::presentation`**：本片由**引擎**答 —— "这个会话能不能上屏"就是"后端有没有窗口那一半"
  （`setBackend` 时问一次、缓存；`capabilities()` 把这一位填上），**插件不动**（S5 的"调用自己能答的不进
  Capabilities"在这里的形态：设备成为一等公民（S10 的 `DeviceDesc`）之前，"呈现意图"只能由结构推出来，而引擎是
  同时驱动那一半的人）。
- **appfw**：`SurfaceWindow` 不再直接拿句柄调引擎 —— `init()` 里 `surface->bindTarget(handle)`（重公告=移动），
  尺寸只报一次给区（`feedSurfaceSize`），`Session::session_handle` 那份**句柄副本删掉**，改问
  `surface->targetHandle()`（S6"一个值只有一个 owner"：控件不再存第二份）。
- **判据（实测）**：`test_graphics` **362**（360 + 2：`AReAnnouncedHandleMovesTheWindowAndTheDeviceStays` /
  `TheAttachHandsOverTheAreasCurrentWindow`；S4b 的三条与两条尺寸用例改形到"按区公告"，计数不动）；
  `test_gui` 259 / `test_vsg` 426（421 + 3 skip + 2 已知红）/ appfw 32 / meshio 34 / iobase 94 全绿；全量 Release
  0 error / 0 warning；五 gate 0 finding（`--strict` 仍 22）。**变异 4/4 红（各自单独实测）**：① 无窗口那一半也
  收下（不拒）⇒ 1 条红；② 拒得静默（不报）⇒ 1 条红；③ 引擎留一份自己的句柄副本（重公告时交旧的）⇒ 1 条红；
  ④ 尺寸不走区（`RenderSurface::resize` 不公告引擎）⇒ 4 条红。
- **坑 / 记账**：① **引擎的新成员必须给默认值** —— `area_`（借用的区指针）第一版漏了 `= nullptr`，构造函数不碰它，
  于是随机指针 → `initialize()` 里 `area_->targetHandle()` 把无关测试的堆打烂（SEH 0xc0000005，失败集每次不同、
  单跑又全绿 —— 典型的野指针）⇒ "新增字段一律带初始化器"这条在这片交了学费；② `setBackend` 现在会问一次
  `surface()`（缓存 `presentation`），按"问了几次窗口那一半"计数的夹具要把这一次算进去；③ 引擎**读**区的句柄是
  对的（它是同一块区的另一半），错的是**存**一份自己的（变异③钉着这条）；④ `RenderSurface::resize` 现在也到帧与
  窗口那一半，所以"视图转发尺寸"（`SceneView::onSurfaceResized` 为没有表面的视图保留的那条路）在挂上表面后也会
  把尺寸带给窗口那一半 —— 与宿主的直接公告**同值**（同一份尺寸），不是第二份状态。

## 2026-10-11 S8 落地：appfw 的 `Viewer` 管 N 个 View（一块渲染区一个查看器）

> **已被取代（2026-10-11 同日，"多格下线"）**：本条的"每格"面（`addView` / `views()` / `setPaneRect` / 每格的请求与档、
> 表面侧的呈现表与矩形）**已从代码里删除**，现役形态见本节最上面"多格下线：`RenderSurface` 收成'一条呈现 + 尺寸 +
> 目标'"。本条留作那一片的落地记录，里面的 API 名不要再当现状引用。

- **做了什么**：`graphics-window-device-viewer.md` 的第一片（§3 Q5 的裁决）：appfw 多了一个导出的 `Viewer`
  （`sdk/vine/appfw/gui/Viewer.hpp`，实现在 `src/fw/appfw/src/gui/Viewer.cpp`）—— 视图集合与矩形**读 `RenderSurface` 的呈现表**
  （`views()` 就是 `presentations()` 里带视图的那些，按引擎执行序；矩形只经 `setPaneRect` →
  `RenderSurface::setViewport` 写表，查看器一个矩形都不存）；查看器自己只存**每格一条请求**（`ViewerOptions`）+
  建链/采纳/重建（S7 的 `SurfaceWindow::buildViewerPipeline` 整段搬进去，运行期选择照旧从活着的链搬：档由请求记着，
  两个 HUD 档读 `isGizmoVisible/isStatsVisible`）。`addView` = 建/采纳 + 在表上声明；`removeView` = `release`（撤声明 +
  忘请求 + 放引用，链留在视图上）；每格 `setShadingMode` 的两条路（链上覆盖 / 带档重建）一字未改地跟着走。
- **归属怎么切**：`RenderControl` 收窄成"一块窗口渲染区"（原生面 + 会话 attach/状态机 + 尺寸/DPR + 帧驱动 +
  `viewer()`）；`SurfaceWindow` 只剩原生面/会话/尺寸/输入/状态机 + `init()` 里一行 `viewer()->takeArea()`；老三条
  （`viewerOptions` / `setViewerOptions` / `setShadingMode`）**保留为"作用于第一格"**（= 控件自己的视图）的转发，
  所以 `model_viewer` 的调用点一行未改。
- **`isActive()` 与 `takeArea()` 是一对，而且都读表**：`isActive()` = 自己的格子至少有一条在表上、且表里没有
  **宿主自建的 pass**（`view == nullptr` 的呈现 ⇒ 宿主接管，false）；`takeArea()` 在每次 attach（`SurfaceWindow::init()`）
  被调：表里有宿主 pass 就**什么都不做**（默认链一条都不建 —— demo_plugin 那条路一字未改，已实测），否则给还没有链的
  格子按请求建链、再逐个声明（幂等）。**"查看器是不是主人"不是一个标志位**，是表的事实（S6/S7 同一条：不存第二份）。
- **上下文从哪儿来**：gizmo 源相机 = 每格自己的相机；两个 HUD 的像素比 = **表面比例** ⇒ `RenderSurface` 新增读数
  `pixelRatio()`（比例本来就只有表面这一份 owner —— T1 把它和尺寸一起收在表面；查看器不存副本）。
- **判据（实测）**：`test_gui` **259**（254 + 5 条新用例：两格各自预设〔设备/引擎数不变〕/ 宿主 pass 接管
  〔声明先落 + 上线后接管两种次序〕/ `release` 撤声明 / 重建搬运行期选择 / 两格路由与焦点）；`test_graphics` 360、
  `test_vsg` 426（421 + 3 skip + 2 已知红）、appfw 32 / meshio 34 / iobase 94 全绿；全量 Release 0 error / 0 warning；
  五 gate 0 finding（`--strict` 仍 22）。**变异 5/5 红（各自单独实测，只有该条用例红）**：① 重建不搬运行期选择 ⇒
  `ARebuildCarriesTheLiveChainsRuntimeChoices` 红；② 两格共用一份请求 ⇒ `TheViewersPanesEachDrawFromTheirOwnPreset` 红；
  ③ `release` 不撤声明 ⇒ `ReleasingAPaneTakesItOutOfTheArea` 红；④ `views()` 存第二份（不读表）⇒ 同 ② 红；
  ⑤ `takeArea` 不因宿主 pass 让位 ⇒ `AHostPassTakesTheAreaAndTheViewerStandsDown` 红。
- **坑 / 记账**：① `release(view)` 与 `removeView(view)` 是**同一个动作的两个拼法**（`addView` 的逆 / 接管的口径）；
  ② `addView` 只吃绑在本引擎上的视图（别的视图报一次 `UnsupportedRequest` 后拒绝），表的声明被拒时把视图**还原**
  （链与请求都还回去，不留半个格子）；③ `views()` 因此**包含宿主自己声明的格**（它是"这块区有哪些视图"，不是
  "我的格子清单"），`setShadingMode` 只认自己的格（别人的如实回 false）；④ 导出的 `Viewer` 用 PImpl（数据成员只在
  `Viewer.cpp` 里），查看器在 `SurfaceWindow::Impl` 里声明在 engine → view → surface **之后**（借用三者，先析构）。

## 2026-10-11 S7 落地：配方归 `Pipeline`，视图只采纳（删掉 `ensureWindowPass` 那一层）

- **起因（用户点名的那条"不清不白"）**：`SceneView::ensureWindowPass()` 一个名字干两件事 —— ①它是**配方**
  （建哪条链、带哪些档），②它还是**表**要的默认 pass 入口（`RenderSurface::present()` 反过来调它）。于是
  "什么时候该调用"没有答案，而它内部还带着"引擎里已有同相机的窗口 pass 就让位"的旧推断。三选一（① 改名+查询 /
  ② 声明式结构 / ③ **删掉这一层**）里用户选了 ③：**结构归 `Pipeline`，视图只采纳一个**。
- **分层判据（本片的核心结论）**：配方的字段按"**它在建链时决定什么**"分成两类 ——
  **结构性**（走哪条链 / 非 Lit 档要求几何趟 / 哪些档**存在** / 离屏尺寸 / 程序）进 `PipelineOptions`；
  **运行期**（同一条链上能表达的档、档的**显示/隐藏**）归 `Pipeline` 的 setter（零重建、零重新注册）。
  两个枚举各自成叶子头：`ShadingMode.hpp`（原 `SceneView::Options::Mode`）、`ShadingPath.hpp`（原在
  `RenderPipeline.hpp` 里）—— 与 S5 同一条判据：读者该能独立拿到值，不被拖进整张契约。 <!-- drift-ok -->
- **`Pipeline` 记下自己是哪条链**：`shadingPath()` 答的是**实际建成**的那条（延迟请求配非 Lit 档 ⇒ 前向，
  见 `RenderPipelineBuilder::effectivePath()`，它现在公开成静态，宿主也能问"这个请求会建成哪条链"）；
  `shadingMode()`/`setShadingMode()` 换档是 pass 上的覆盖；**链表达不了就如实拒绝 + 报一次**（`ReportOnce`），
  `setGizmoVisible()/setStatsVisible()` 在没有那一档时同样拒绝 + 报一次（以前是静默 `false`）。
- **`SceneView` 删掉整层配方**：`Options`/`setOptions()`/`options()`/`ensureWindowPass()`/`effectivePath()`/
  `applyModeToGeometryPass()`/`applyOverlayIntent()`/`setOverlayVisible()`/`isOverlayVisible()`/
  `rebuildDefaultWindowPass()`/`removeWindowPass()`/`default_pipeline_` 全删，换成一条
  `setPipeline(intrusive_ptr<Pipeline>)`（采纳 = 应用矩形几何 + 让表面重述清屏；传 null = 这个视图不画）。
  `setScene()` 改为"重绑**已采纳那条链**的窗口 pass 的内容"（行为不变）。
- **`RenderSurface` 不再建任何东西**：`present(view)` 只把声明**解析**成"视图采纳的那条链的窗口 pass"，
  没有链的视图被**拒绝并报出来**；宿主顺序因此固定成**先建链 → 采纳 → 声明**。
- **appfw 拿走"查看器"这一层**：新 `gui/ViewerOptions.hpp`（`path`/`mode`/`gizmo`/`stats`/离屏尺寸 = 宿主面对的
  那一份预设）+ `RenderControl`/`SurfaceWindow` 各三个入口（`viewerOptions` / `setViewerOptions` /
  `setShadingMode`）；**上下文字段由控件补**（gizmo 源相机 = 视图相机、两个 HUD 的像素比 = 表面比例）。
  `setShadingMode()` 的两条路：活着的链 == 这个请求会建成的链（`effectivePath` 比一次）⇒ 只在链上换档；
  否则**带着档重建**（所以"延迟请求 + 非 Lit 档"期间选回 Lit 会建回延迟 —— 老行为，靠这条比较才成立）。
- **重建保住运行期选择**：`buildViewerPipeline()` 重建前读活着的链（档由预设记着、显示/隐藏读
  `isGizmoVisible()`/`isStatsVisible()`），在新链上重新落一遍 —— 与 S6 同一条判据：**一个值只有一个 owner，
  重建的那一方负责搬**。（"从预设里删掉再加回来仍然保持隐藏"这条老规则随之消失：一条**从来没有过**那一档的链
  没有"使用者关过它"可言，记账。）
- **判据（实测）**：`test_graphics` **360**（‑1：`TurningOnAnOverlayThatWasNeverBuiltCostsOneBuild` 与
  `AnOverlayRemovedFromTheRecipeAndAddedBackKeepsTheRequestedVisibility` 并成
  `AddingAnOverlayIsABuildOptionAndShowingItIsNot`，"关掉的档不因重建复活"改成**控件级**、挪进
  `ModelViewerTest.TheSceneCarriesItsOwnFrameRateReadout` 后半，所以那个套件计数不动）、
  `test_gui` **254**（253 + 新用例 `RenderControlTest.ADeferredViewerSwitchesChainOnlyWhenTheRequestNeedsIt`：
  延迟预设 ⇒ 非 Lit 档重建前向 ⇒ 选回 Lit **建回延迟** ⇒ 预设跟着运行期那一档走）、`test_vsg` **426**
  （421 + 3 skip + 2 已知红；`SdkPixelTest` 那条真设备用例按新顺序改写后仍绿 —— 像素是这套改动的端到端证据）、
  appfw 32 / meshio 34 / iobase 94 / robotics_core 95 / robotics_io 27 / brepio 5 全绿；全量 Release
  0 error / 0 warning；五个 gate 0 finding（`--strict` 仍 22 条）。**变异 5/5 红**：① 换档不落覆盖 ⇒ test_graphics
  4 条红；② 拒绝呈现时不报 ⇒ 1 条红；③ 建链用 `options.path` 而不走 `effectivePath` ⇒ 2 条红；
  ④ 重建不搬"关掉的档" ⇒ test_gui 1 条红；⑤ `setShadingMode` 只问"链能不能表达"、不比请求 ⇒ 1 条红。
- **坑 / 记账**：① 视图不再有"默认管线"，于是**每个**声明呈现的宿主都要自己建链（`RenderControl` 替自己的视图
  做了，别人加的格子归别人）——好处是"谁建的谁负责"，代价是旧的 `present(view)` 顺手建链不见了；
  ② 那条"延迟请求 + 非 Lit 档"的比较**必须连链一起比**：只比"档能不能表达"的话，前向链表达得了 Lit，
  于是永远回不到延迟链（这个坑是本片唯一一处"看起来多余的比较"，变异是它的判据）。

## 2026-10-11 S6 落地：宿主侧收口（尺寸唯一 owner + `SceneView` 六角色 + 呈现知识归表）

- **尺寸唯一 owner**：`SceneView` 不再存表面尺寸的副本 —— `surface_width_/surface_height_/pixel_ratio_` 改名
  `own_*`（含义＝"**没有表面时**这个视图的尺寸"），挂上表面后几何**问**表面
  （`surfaceWidth()/surfaceHeight()/surfacePixelRatio()`）；`RenderSurface::resize`/`present` 不再推三个数，
  而是让视图重算矩形（`applySurfaceGeometry()`）；**摘下来时表面把尺寸交还**给视图（同矩形那条规矩）。
  `onSurfaceResized` 语义不变（挂着转发 / 没挂记自己那份）。
- **六角色分组**：`SceneView` 公开面按 recipe / camera and content / navigation / geometry / input / presentation
  重排并命名（对齐文件里既有的 `// ---- … ----` 分段，写进类注记）；**不改行为、不动 ABI**。
- **三条"不是宿主 API"点名**：`setSurface`（表绑/摘视图 + 交回尺寸）、`applySurfaceGeometry`（表在尺寸变化后
  让视图重算）、`ensureWindowPass`（表要的默认窗口 pass；**保持公开**，因为"没有表面的视图"是合法配置）。
  呈现知识（哪条 pass 呈现 / 矩形 / 清屏 / 顺序 / 尺寸）本来就全在 `RenderSurface`，本片只把它写进文档。
  **记账（S7 之后）**：第三条已在 S7 删除（配方归 `Pipeline`、视图只采纳，`present(view)` 只解析不再建）——
  上面这句是 S6 当天的形态。
- **判据（实测）**：`test_graphics` **361**（360 + 新用例
  `SceneViewViewportTest.AViewReadsTheSurfaceSizeAndHoldsNoSecondCopyOfIt`）、`test_gui` **253**（不变，appfw 一行未改）、
  `test_vsg` 426（421 + 3 skip + 2 已知红）、appfw 32 / meshio 34 / iobase 94 全绿；全量 Release 0 error / 0 warning；
  五个 gate 0 finding（`--strict` 仍 22 条）。**变异 3/3 红**：视图读自己那份 ⇒ 3 条红；表面不扇出 ⇒ 2 条红；
  摘下来不交还 ⇒ 1 条红。
- **坑 / 记账**：两份实现在正常路径**行为等价**，所以判据只能是"挂上之前喂给视图的话在挂上之后失效"；同理
  "交还尺寸"只在"摘下来后改矩形"这条路上可观察（`setViewport` 对相同矩形提前返回）。

## 2026-10-11 S5 落地：能力与观测各收成一个值（`Capabilities` / `FrameReport`）

- **做了什么**：`RenderBackend` **删** `supportsRenderTargets()` 与三个出参
  （`frameCounters`/`retentionStats`/`frameProgress`），**换成两个值**：`capabilities()`
  （新头 `sdk/vine/graphics/Capabilities.hpp`，今天一个字段 `render_targets`）与 `frameReport()`
  （`FrameStats.hpp` 新增 `FrameReport`：三个部分各自 `std::optional`，**不保留＝不 engage**）。
  引擎同样两条（`capabilities()`/`frameReport()` 原样转发；无后端＝能力全假、报告全不 engage），
  `frame()` 改读 `capabilities().render_targets`（**仍然每帧问**）。插件答
  `{ .render_targets = true }`，报告由 `d->observe` + `Session::timeline()` 拼出。
- **两条判据（写进文档）**：① **能力是属性**（初始化期间不变、宿主计划一帧前必须知道）；**调用自己能答的不进
  `Capabilities`**（能不能读回 → `ReadbackResult::Unsupported`），否则是同一件事的第二处拼写。
  ② **"值放哪个头"看能否独立到达读者**：`Capabilities` 若跟 `ReadbackResult` 一起放契约头，
  引擎头就得包含整张后端契约（它今天只前置声明 `RenderBackend`）——所以它单独成头。
- **判据（实测）**：`test_graphics` **360**（不变；`FrameCountersForward…` 改写为
  `TheFrameReportCrossesTheEngineWholeAndAbsentPartsStayAbsent`：三部分 engage 与否 + 值穿过引擎 + 能力穿过）、
  `test_gui` **253**、`test_vsg` **426 = 421 + 3 skip + 2 已知红**（设备用例改名
  `TheHostReadsTheBackendsOwnFrameReport`；**在 `#if !defined(_WIN32)` 里，本机不编译**）、appfw 32 /
  meshio 34 / iobase 94 全绿；全量 Release 0 error / 0 warning；五个 gate 0 finding（`--strict` 仍 22 条，同一列表）。
  **变异 3/3 红**（各自单独实测）：引擎不转发报告 ⇒ 红；引擎忘记能力 ⇒ 红；契约默认"说零"而不是"沉默" ⇒ 红。
- **坑 / 记账**：收成一个值之后"三个各写一半"的中间状态**从存在变成不可能**，而"不保留"仍说得出来
  （不 engage）—— 删的是形态而不是信息。老条目里点名的旧符号/旧用例名按本仓规矩加 `<!-- drift-ok -->`。

## 2026-10-11 S4b 落地：窗口成为可选依赖（呈现拆成 `RenderSurfaceBinding`）

- **做了什么**：`RenderBackend` **删** `setWindowHandle`/`nativeHandle`/`resize`/`swapBuffers`，**新增** `surface()`
  （默认 null —— "窗口是可选依赖：只画离屏的后端答 null，四个入口一个都不实现"）；窗口那一半成了一张新契约
  `vn::graphics::RenderSurfaceBinding`（`setWindowHandle`/`nativeHandle`/`resize`/`present`），插件 `VsgBackend`
  同时实现两张契约、`surface()` 答 `this`（它一直有窗口：宿主的或自己建的），`swapBuffers()` 更名 `present()`。
  引擎把句柄与尺寸转发给它、`frame()` 末尾 `if (surface) surface->present()`；**宿主 API 与 appfw 一行未改**。
- **新可见性**：句柄公告给 `surface()==nullptr` 的后端 = 接线错误 → `PassProtocolViolation` **上报一次**
  （分裂前这个形态根本表达不出来：入口在契约上、人人有空壳）；反向的**尺寸公告不报**（它仍进引擎 frame context，
  pass 按它排布 —— 宿主对一张只离屏的表面报尺寸是合法的）。
- **判据（实测）**：`test_graphics` **360**（357 + 3，新增 `tests/test_graphics/HeadlessBackendTest.cpp`：只离屏后端
  **零窗口代码**跑完整帧 + 帧真的到达 pass 声明的那张 `RenderTarget` + 一处 present 都没有 / 句柄公告给它的上报 /
  **有**窗口那一半的同批公告静默到达 + 每帧一次 present〔对照组〕）；`test_gui` **253**、`test_vsg` **426**
  （421 + 3 skip + 2 已知红，**含 SDK 像素夹具 ⇒ 真设备上 present 路径逐项不变**）、appfw 32 / meshio 34 / iobase 94
  全绿；全量 Release 0 error / 0 warning；五个 gate 0 finding（`--strict` 22 条，与改动前同一列表）。
  **变异 3/3 红（各自单独实测）**：不问 `surface()` 也无 present ⇒ 3 条红；句柄收了却不交给表面 ⇒ 1 条红；
  句柄公告改回静默 ⇒ 1 条红。
- **坑 / 决策**：设计里那个 `format()` **取消**（全仓找不到读者：窗口的*实际*格式由插件的 `WindowTarget` 从平台读，
  宿主从没问过）—— 真要问的那天，加在 `RenderSurfaceBinding` 上是一行。判据里"并读回"那半句改成"帧到达 pass 声明的
  `RenderTarget`"（用双打读自己写下的字节只证明双打；真设备读回由 `test_vsg` 的既有窗口用例兜住）。X11 那半边的
  54 处窗口调用是**纯机械替换**，Windows 上不编译，未验（同 S3 的记账）。

## 2026-10-11 S3 后半收口落地：平坦路径与 `Protocol` 一起退场

- **做了什么**：契约**只留一个帧入口** —— `RenderBackend::executeFrame(frame)` 变**纯虚**，
  `beginFrame`/`endFrame`/`beginPass`/`endPass`/`setPassOrder`/`setPassState`/`setClearPolicy`/`setViewport`/
  `setLights`/`setPassInputs`/`render`/`drawScreenProgram` **12 个全删**（`RenderBackend.hpp` 的 CALL ORDER 一节
  整段消失：值里没有调用序列可写）。`setRenderTarget` **留下但改义**："pass 画进哪张目标"在值里
  （`PassDraw::target`），它只剩"宿主宣布一张它要用/回读的目标"（持握 + 懒建 + 上报）；
  `RenderPass::announceClear`、`announcePassTo`/`announceDrawsTo`、"默认 `executeFrame`"随平坦路径删除。
  **宿主自己驱动一条 pass** 落成 `singlePassFrame(pass, order, inputs, description)`（`FrameDescription.hpp`
  + `RenderBackend.cpp`）：`RenderPass::execute()` = 描述它 + 把这一帧交给后端 —— **引擎与宿主同一个入口、同一个形状**。
  引擎 `frame()` 去掉 `beginFrame()`/`endFrame()`（帧的身体在 `executeFrame` 里），`swapBuffers()` 仍是唯一呈现。
- **插件**：`beginFrame`/`endFrame` 的体 → 私有 `openFrameBooks`/`closeFrameBooks`；逐条入口的体 → 私有 helper
  （`openPassScope`/`closePassScope`/`announcePassOrder`/`announcePassInputs`/`announceTarget`/`announceClear`/
  `announcePassState`/`announceLights`/`collectContentDraw`/`collectScreenDraw`；目标那两半共享
  `adoptHostTarget`+`reportUnbuildableTarget`）；`executeFrame` 按值走它们。**"description 缺席 ⇒ 一个属性都不发"
  仍成立**（S1 那条门禁原地不动）。
- **`Protocol` 退场**：`backend/Protocol.hpp` + `Protocol.cpp` 删除。`FrameRecorder` 只留书（帧/scope 开合） <!-- drift-ok -->
  与契约里**唯一不能被值表达**的规则 —— **被释放的目标**（scope 公告的目标被 `releaseRenderTarget` 掉 ⇒
  该 scope 的效果丢弃、绘制**跳过而不改画到窗口**；另外半条"释放别的目标不影响本 scope"从 `CoreProtocolTest`
  搬进 `FrameRecorderTest`）。**顺序判决 + 那套 per-episode 上报（含 `PassProtocolViolation` 的三句话）一并删除**：
  值按构造有序，"没有 scope 可归属的调用"不是错误而是**没有去处**（不生效、返回 false、不上报）。
- **判据（实测）**：`test_graphics` **357**（359 − 2：值→线序列冻结那条与"整帧一个值"那条被值驱动本身取代）、
  `test_gui` **253**（`--gtest_filter=-UserIOTest.PickingFilesFillsTheLineWithoutSubmitting`）、
  `test_vsg` **426 = 421 + 3 skip + 2 已知红**（433 − 7 = `CoreProtocolTest` 的 7 条；**含 SDK 像素夹具 ⇒
  真设备像素不变**）、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 构建
  0 error / 0 warning；五个 gate 0 finding（`check_ai_docs --strict` 仍 20 条历史引用）。
- **测试怎么改的**：`GraphicsTest` 的 `MockBackend` 改成**从值记录**（同一批见证量 + 新增 `frames_executed` /
  `pass_entries` / `entries_closed`）；`VsgBackendTest` 的平坦驱动改成**值驱动**（新增测试头
  `tests/test_vsg/TestFrame.hpp`：把"开 scope / 公告 / 绘制"的老拼写换成同一批事实的链式写法，两个测试文件共用）；
  `FrameRecorderTest` 5 条把上报断言改成"不生效、无上报"；`test_gui/RenderControlTest.cpp` 的 stub 换成
  `executeFrame`；`CoreProtocolTest` 7 条删除并记账（上一条"开工清单"里列的 3c 到此做完）。
- **坑（三条，都进设计）**：
  1. **`announcePassTo`/`announceDrawsTo` 不是"那条路的地基"**（3c 行原话如此）：它们是"描述 → 逐条调用"的映射，
     逐条调用没了，它们就没有读者 —— 宿主自驱一条 pass 只需要"造一个单 pass 的帧"。
  2. **X11 那半边的用例早已编译不过**：`VsgBackendTest.cpp` 里 `#if !defined(_WIN32)` 的 2800 行在 Windows 上不编译，
     而它们**自 M3 起**还在调已删的 `RenderBackend::setDepthMode()` —— Linux 上 `test_vsg` 从那时起不编译。
     本片顺手把这批驱动改成值（`TestFrame`）；**这台机器上没有 X display，它们仍无法在此编译/运行**（Linux 才验）。
  3. **`ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt` 偶发红的原因查明**：它断言"新实例 ≠ 旧指针地址"，
     而被释放的 `RenderTarget` 的**地址会被复用**（十几次里抓到一次）；已改成"活着的实例只有一张 + extent 是新形状"。


## 2026-10-11 S3 前半落地：帧由值驱动（`execute()` 退成宿主直呼）

- **做了什么**：`frame()` 的运行段**不再调 `pass->execute()`**，改为宣告编译期建好的那条 entry
  （`beginPass` / `setPassOrder(entry.order)` / `setPassInputs(entry.inputs)` /
  `announcePassTo`/`announceDrawsTo`（`entry.description == nullopt` ⇒ 一个属性都不发）/ `endPass`）；
  `drawScenePass` 删除。**每条 pass 每帧只被描述一次** ⇒ S2 那条"描述两次"的过渡代价消失，
  两条 memo 见证回到紧值（1/2、1）。**每条 pass 的耗时改在编译里量**（`buildFrameValue` 包住"钩子 + 输入 + 描述"）：
  `record_micros` 还是"这条 pass 上一帧的占比"，只是量在**记录**发生的地方（运行那半的耗时是后端的，见 `FrameCounters`）。
- **契约变化（这一片的实质）**：`RenderPass::execute()` **不再是引擎调用 pass 的方式**，它是"宿主自己驱动一条 pass"
  （预热 / 测试 / 宿主自绘）的拼写；想让引擎跑的活必须放进 `describe()` 或声明的 `before_draw` 钩子。
  写进了 `RenderPass::execute()` / `RenderEngine::frame()` / `FramePlan::record_micros` 的文档与 `docs/guides/graphics.md` §2.4/§2.5。
- **判据（实测）**：`test_graphics` 356→**357**（新用例 `TheEngineRunsTheFrameValueAndNeverCallsExecute`：
  覆写 describe/execute 各计数，两帧后 describes = 2、**executes = 0**，且这条 pass 确实到了线上）。
  **变异 1/1 红**：运行段改回 `pass->execute(...)` ⇒ 3 条红。`TheFramePlanCarriesWhatEachPassLastCost` 的 `SlowPass`
  按新事实改写（"覆写 `execute()` 睡觉" → "覆写 `describe()` 睡觉"）；`test_gui` 253 / `test_vsg` 428(+3 skip
  +2 已知红，**含 SDK 像素夹具 = 真设备上像素不变**) / `test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94；
  全量 Release 0 error / 0 warning；五个 gate 0 finding。
- **坑**：这次落地把 S2 记录的那条发现**当场兑现了** —— 把 `frame()` 改成由值驱动的第一版，红的正是
  `TheFramePlanCarriesWhatEachPassLastCost`（覆写 `execute()` 的 `SlowPass` 静默不跑）。所以"改写这条见证"不是
  为了绿，而是**契约变化必须记账**：它的睡觉得搬到 `describe()` 里，才继续说明"计划里的耗时是这条 pass 的占比"。
- **还剩（S3 后半）**：平坦路径本身（10 个方法）+ `backend/Protocol.hpp` 的 8 条顺序规则 +
  `CoreProtocolTest` 的改写记账（**预热那一拍已删**，见下一条）。

## 2026-10-11 S3 后半：执行计划（3a 已取消，剩 3b / 3c）

- **这一片为什么不能一次做完**：它要求**引擎 / 插件 / 测试三边同时到位**（先删接口后迁移插件，或反过来，中间态都是
  一棵跑不起来的树 —— §5 禁止的形态）。拆成两步，每步都能独立验收：
  - **3b 插件吃值**：`VsgBackend::executeFrame` 覆写 = 值的循环 —— `d->passes.adopt(pass.pass)` 取身份 →
    recorder 的 per-pass/per-draw 入口 + **每 draw 的内容副作用**（`d->store->updateMaterial(...)` 那类，
    今天在 `VsgBackend::render()` 里）。逐条方法退成薄转发到同一批 helper，供 3c 删。
    判据：`test_vsg` 全量逐项不变（**含真设备像素夹具**）。
  - **3c 删契约**：`RenderBackend` 删那 10 个方法、`executeFrame` 变纯虚、`Protocol` 的 8 条规则退场、
    `CoreProtocolTest` 记账改写。`RenderPass::execute` + `announcePassTo`/`announceDrawsTo` **留在 SDK**
    （"宿主自己驱动一条 pass"那条路的地基）。
- **调查结论（省掉一步）**：原计划的"3a 给 recorder 加按值采集入口"**不必要** —— recorder 的入口本来就是
  per-pass/per-draw 的，且它们**不是契约**（是插件内部的采集层）；值按构造有序，所以"值 → 这些入口"只是换个调用者。
  而 `PassId` 的所有权在插件（`d->passes.adopt`），"按值采集"反而要往 recorder 里塞 key 函数。
- **3b 的判据已冻结（2026-10-11，test_graphics 359 绿）**：`TheFrameValueBecomesExactlyThisCallSequence`
  把"值 → 线上调用"的序列逐项钉死（帧括号 / 每 pass 的 scope〔身份·order·inputs〕/ 它声明的
  target·viewport·clear·state〔只发它说了的〕/ 每 draw 的 lights + 绘制 / scope 关闭 / 什么都不发的 pass
  只开合 scope / 被禁用的 pass 不在帧里）。`MockBackend` 加了一条 `wire` 调用日志。
  **插件自己的 `executeFrame` 必须产出同一序列** —— 这是"不改行为的重构"该有的安全带（先冻结，再动）。
- **3b 收尾 / 3c 的开工清单（下一次从这里开始）**：
  1. `VsgBackend.cpp`：把 `beginPass`/`endPass`/`setPassOrder`/`setPassInputs`/`setRenderTarget`/`setViewport`/
     `setClearPolicy`/`setPassState`/`setLights`/`render`/`drawScreenProgram` 的**体**逐个搬成私有 helper
     （`applyX(...)`），覆写方法退成一行转发；`executeFrame` 改调 helper。判据：**冻结的
     `TheFrameValueBecomesExactlyThisCallSequence` + `test_vsg` 全量（含真设备像素夹具）逐项不变**。
  2. 3c：`RenderBackend` 删那 10 个方法（`beginFrame`/`endFrame`/`beginPass`/`endPass`/`setPassOrder`/
     `setPassState`/`setClearPolicy`/`setViewport`/`setLights`/`setPassInputs`/`render`/`drawScreenProgram` 中
     属于契约的那些），`executeFrame` 变纯虚；`backend/Protocol.hpp` 的 8 条顺序规则退场；
     `CoreProtocolTest` 用例改写/删除并记账；`MockBackend`、`RenderControlTest` 的 stub、`SdkPixelFixture`
     里逐条驱动的地方同步迁移（**同一提交里改完两侧**）。
  3. 验收命令：`cmake --build build --config Release --target gfx_backend_vsg test_graphics test_vsg` →
     `test_graphics`（359）、`test_vsg`（433 = 428 + 3 skip + 2 已知红）、`test_gui --gtest_filter=-UserIOTest.*`（253）、
     五个 gate。

## 2026-10-11 S3 后半之二落地：预热那一拍删除

- **做了什么**：`RenderEngine::initialize()` 里"每个启用且不清屏的 pass 先执行一遍"的预热**整段删除**（理由留在注释里：
  它当年是为了"让后端在呈现第一帧前看到整个 pass 集合、并把内容编译好"，当时帧是一串调用、内容惰性构建；
  现在**一帧就是一个值**，后端动手前就拿到整个 pass 集合，主内容本来就在 `initialize()` 里预热）。
  于是 `pass->execute()` 在引擎里**一个调用点都不剩**（只剩宿主/测试直呼），与 S3 前半那条契约完全一致。
- **判据（实测）**：`test_graphics` **358** 与删之前逐项相同；**`test_vsg` 433 = 428 + 3 skip + 2 已知红，
  与删之前逐项相同** —— 含 **SDK 像素夹具（真设备渲染 + 读回像素）**，即"没有预热的第一帧画面不变"的实测；
  `test_gui` 253 / `test_appfw` 32 / meshio 34 / iobase 94；五个 gate 0 finding。
- **风险记账**：这条删除的风险点是"第一帧内容构建"（当年正是它引出预热）。实测（真设备像素夹具 + 全量 vsg）没有回归；
  若将来第一帧出现构建类回归，先看 `initialize()` 里那条注释。

## 2026-10-11 S3 后半之一落地：整帧交给后端（`executeFrame`）

- **做了什么**：契约新增 `virtual void RenderBackend::executeFrame(const FrameDescription&)`，**默认实现 = 原先那条
  逐条宣告序列**（每 pass：`beginPass`/`setPassOrder`/`setPassInputs`/pass 自己的事实/它的绘制/`endPass`；
  `description == nullopt` 的 entry 一个属性都不发）。引擎 `frame()` 里那段逐条循环**整体换成一次调用**
  `backend_->executeFrame(frame_value_)`。于是**边界上是值**：后端可覆写它直接从值构建（那时逐条调用一份都收不到），
  不覆写的后端（含 vsg 插件）**行为逐调用不变**（同一个序列，只是从一个输入来）。`RenderBackend.hpp` 的
  CALL ORDER 段、`docs/guides/graphics.md` §2.5 已同步。
- **判据（实测）**：`test_graphics` 357→**358**（`ABackendIsHandedTheWholeFrameAsOneValue`：覆写 `executeFrame`
  的后端一帧收到**一次**调用，passes 按运行序带 order 与绘制数、被禁用的不在里面，且
  `beginPass`/`setPassOrder`/`render`/`drawScreenProgram` **一次都没被调**）；**变异 1/1 红**（引擎改回逐条发）。
  其余套件全绿（gui 253 / vsg 428 + 3 skip + 2 已知红，**含 SDK 像素夹具 ⇒ 真设备像素不变** / appfw 32 /
  meshio 34 / iobase 94）；全量 Release 0 error 0 warning。`MockBackend` 补了 `setPassOrder` 记录
  （`order_sets` / `orders`）。
- **意义**：那 10 个逐条入口**已经不再有"引擎的运行"这个读者**了（只剩默认实现、宿主自驱一条 pass、后端自己），
  所以删它们是一次**纯删**。
- **注意**：跑第一次全量时 `ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt` 红过一次，随后 4 次全绿
  （单跑也绿）—— 记在这里，若复现再查。

## 2026-10-11 S2 前半落地：帧的值（引擎产出，可观测）

- **做了什么**：`FrameDescription` 有了帧那一层（`FrameDescription::Pass` = pass 身份 / order / 解析后的 inputs +
  该 pass 自己的 `PassDraw`，nullopt = 这帧什么都不发）；`compileFrame()` 末尾 `buildFrameValue()`
  按运行序跑每条启用 pass 的 `before_draw` 钩子、告诉它解析好的 inputs、请它描述自己 —— **整帧的值在派发前存在**；
  `RenderEngine::frameDescription()` 观测它。`announcePassTo`/`announceDrawsTo` 从 `RenderPass.cpp` 匿名命名空间
  **搬到 `RenderBackend.cpp` 并导出**（声明在 `FrameDescription.hpp`）：描述→平坦调用的映射只有一份。
  **帧仍由 `execute()` 驱动**（`drawScenePass` 原样）；"帧由值驱动 + 新入口 `executeFrame(desc)`"归 S3/S2 后半。
  → **同日已由 S3 前半接上**（见上一条）：帧跑的就是这个值，`execute()` 退成"宿主直呼"。
- **判据（实测）**：`test_graphics` 354→**356**：`TheFrameValueIsEveryPassAndEveryDrawTheFrameRan`
  （5 条 pass 的帧：值里的 pass 与后端 `beginPass` 的逐条对齐、值声称的绘制数 = 后端收到的 `render`+`drawScreenProgram`；
  无内容档 = 有 entry 无 description、禁用档 = 没有 entry）、
  `TheFramesValueIsBuiltAfterThePerFrameHooks`（钩子改内容 ⇒ 值描述的是这一帧的版本）。**变异 2/2 红**
  （值不填 description ⇒ 4 条红；钩子不跑 ⇒ 4 条红）。其余套件与五个 gate 同前（gui 253 / vsg 428+3skip+2 已知红 /
  appfw 32 / meshio 34 / iobase 94；全量 Release 0 error 0 warning）。
- **三个发现**：①**`before_draw` 必须搬进编译期**（钩子写内容 ⇒ 描述要在它之后，否则值描述上一帧的画面；
  仓库里两条钩子都是"只写内容、不碰后端"，唯一变化是 `record_micros` 不再含钩子耗时）；
  ②**过渡代价：每条 pass 每帧被描述两次**（编译建值 + 运行 `execute()`）⇒ 两条 memo 见证的 reuse 数变大
  （树走的次数不变），用例已按新事实改写并注明 S3 会复原；
  ③**S3 的第一件事是回答"覆写 `execute()` 的 pass 怎么办"** —— `TheFramePlanCarriesWhatEachPassLastCost` 的
  `SlowPass` 就靠引擎调它的 `execute()` 睡觉来当耗时，帧一旦由值驱动，这个 pass 的工作会**静默消失**
  （把 `frame()` 改成由值驱动时当场红的就是它）。
- **坑**：`raw_ptr<T>` 是 `T*`，`.get()` 不存在（自己写用例时踩了两次）；`addInputTarget` 才是"按目标声明输入"
  （`addInput` 收的是 `ImageRef`）；`setPassInputs` 收的是非 const 指针表，所以值的 inputs 也用
  `raw_ptr<RenderTarget>`。

## 2026-10-11 A 落地：画面形状回到相机（重新设计的 A 片；取代 S4a，也把 S1 那道缝合掉）

- **口径（用户定）**：**一台相机 = 一张画面**。形状是 `Camera` 自己的状态（`setAspect(h/w 的反)` / `aspectRatio()`；
  `setAspect` 忽略非正数），投影、剔除、view block、拾取射线**读同一个数**；"同一台相机被两块形状不同的矩形用到"
  不再是"各自都对"，而是**画面被拉伸、引擎报出来**。行业是分裂的（VTK/Filament/Unreal 从矩形推；
  three.js/Babylon/Godot/Unity 放在相机上），本仓**文档**曾选前者、**用法**一直是后者 —— 现在统一到后者。
- **为什么**（评估过程）：M2c 换来的"相机可跨矩形复用"代价是形状成了**只有后端能答、且答案取决于之前公告了什么**的
  问题（`viewAspect()` 读开着的 scope），S2 在这条依赖解除前写不出来；而"复用"的实测用途（同相机两块矩形）在
  本仓根本不出现（分屏是每格一台相机）。
- **落地形态**：`Camera::{setAspect,aspectRatio,projectionMatrix(),screenToWorldRay(pos)}`；`Scene::collectRenderCommands*(camera)`
  （memo 仍以 view-projection 为键 ⇒ 两种形状两条 memo）；`RenderBackend::viewAspect()` **删除**（纯虚 9→8）；
  `PassDraw` 无形状字段；`FrameRecorder::setViewAspect` 删除（快照投影读相机）；`RenderPass`/`ScreenPass`
  合成**一个** `describe(scene, out)` 虚函数（S1 的那道缝没了）；`SceneView::applySurfaceGeometry` 把窗格形状
  push 给相机（与 `manip->onResize` 同一个矩形，一行）；`AxisGizmo` 声明 1:1、`FpsOverlay` 声明自己 box 的形状；
  `OrbitCameraManipulator::viewportAspect()` 读相机（无相机才回退自己的矩形 —— 否则交互与拾取射线读两个数）；
  **新增** `RenderEngine::collectPictureShapeMismatches()`：矩形（viewport 否则目标/表面）vs 画面（perspective =
  aspect，ortho = 窗口形状），全屏 pass（`ScreenPass`）不参与（它的三角形在 clip space 读不到投影），
  一条 pass 的错**每 episode 一次**（拖窗格会让句子里的数字变，所以按 pass 记 episode 而不是按句子）。
- **判据（实测）**：`test_graphics` 349→**354**（新 5 条：报告+episode+修好又坏 / 视口才是矩形且全屏 pass 不参与 /
  `SceneView` push 形状 / HUD box 形状 / 操纵器读相机；M2c 那三条**反转**：同相机两次 setAspect = 两次走树、
  `OneCameraCullsWithItsOwnShapeInEveryRectangle` 宽矩形不再变宽视锥且一个相机一次走树、
  `TheViewBlockIsProjectedForTheCamerasOwnShape`）；`test_gui` 253、`test_vsg` 428(+3 skip +2 已知红)、
  `test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 0 error / 0 warning；五个 gate 0 finding
  （`--strict` 仍 20 条历史引用）。**变异 4/4 红**（检查永不报 / 矩形不读 viewport / 不 push 相机形状 / 不 push box 形状）。
- **坑（值得记）**：①**改契约半必须重建插件** —— `viewAspect()` 一删，老 `gfx_backend_vsg.dll` 与新头文件不匹配，
  `VsgBackendPluginTest` 两条红得像"插件注册丢了"；而且**测试目标不会顺带构建插件**，得显式
  `--target gfx_backend_vsg`（已写进 redesign 的风险表）。②自己的 episode 状态别"先 rearm 再读" —— 那等于每帧都当新
  错误报（第一版就踩了：`shape_mismatches_last_frame_` 先统一 `rearm()`，carry-over 时读到的永远是 false）。
  ③文档同步点很散：`docs/guides/graphics.md` 4 处、`graphics-render-next.md` 的 M2c 行（标已回退）、redesign 的 S4a 行/读者表/风险表。

## 2026-10-11 S1 落地：pass 描述它的绘制（重新设计的第一片）

- **做了什么**：新增导出的 `sdk/vine/graphics/FrameDescription.hpp`（`DrawDescription` + `PassDraw`；
  `ClearPolicy` **从 `RenderBackend.hpp` 移进来** —— 它是"一条 pass 描述自己"的东西，后端是它的读者之一，
  于是 `RenderBackend.hpp` 反过来 include 它）。`RenderPass` 拆出两个虚拟扩展点
  `describePass(scene, out)` / `describeDraws(scene, out)`，`execute()` 成为薄壳
  （描述 → **两半之间**问画面形状 → 描述绘制 → 按契约顺序宣告）；`ScreenPass` 不再覆写 `execute`。
  **（同日 A 之后：两半合成一个 `describe(scene, out)`，`execute()` 仍是薄壳 —— "描述 → 宣告"）**
- **判据（实测）**：`test_graphics` 346→**349**（`PassDrawTest` 三条：场景档 / 全屏档 / "什么都不发"，
  每条都把**描述的值**与记录型后端**收到的调用**逐项对照）；`test_gui` **253**、`test_vsg` **428**（+3 skip
  +2 已知红，逐项与落地前相同）、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 构建
  0 error / 0 warning；五个 gate 0 finding。**变异 1/1 红**：`describeDraws` 少填 `lights` ⇒ 只有场景档那条红。
- **两个坑（都写进设计了）**：①**顺序错了** —— `RenderBackend::viewAspect()` 的答案**取决于之前公告了什么**
  （读开着的 scope 的 target/viewport），所以"整帧描述在派发前拼好"（S2）在形状不再是后端查询之前**写不出来**：
  当时拆出 **S4a**（画面形状变编译期字段）并提到 S2 之前 —— **随后被 A 整体取代**（形状回到相机，见上一条）。
  ②**空场景的 pass 一个调用都不许发** —— 今天它在第一次公告前就返回；门禁必须放在 `describePass`（虚拟，
  因为"什么算没东西可发"每种 pass 不同），否则会静默多发 `setRenderTarget`/`setPassState`（多宣告 target、
  多一次清屏归属 = 行为变化）。有用例钉"连目标都不许发"（A 之后这条门禁在 `ScreenPass::describe` 里）。

## 2026-10-11 重新设计提案（**S1 已落地，见上一条；其余未落地**）

- 用户 2026-10-11 定"重构，重新设计"，边界含**后端契约**（`RenderBackend.hpp` + `backend/` 半 + vsg 插件）。
  文档：`.ai/design/graphics-redesign.md`（S0–S6，每片独立可逆；总判据 = 两棵树逐证据行相同）。
  **S1 已落地**；落地时把 S4 拆出 **S4a（画面形状变编译期字段）提到 S2 之前** —— 原因见上一条的坑①。
- 一句话病根（有代码证据）：**帧在引擎里已经是一个值（M7 的 `compileFrame`/`FramePlan`），但在边界上又被打散成
  ~15 个有顺序的可变调用（`RenderEngine.cpp:192,273-288` + `RenderPass.cpp:248-301`），后端再把它拼回一个值
  （`backend/FrameRecorder.hpp` 的 `FrameDescription`）。** 连带：窗口烧进帧（**8** 个纯虚里 3 个只有带窗口的后端有用）、
  顺序靠 `backend/Protocol.hpp` 的 8 条规则 + Allow/Drop/Refuse 兜、"能力/观测"各是 4 个 bool 出参。
- 本稿作废了 `graphics-render-next.md` §7 的"整块重写否决"（已在那边标作废），并保留其技术内核
  （配方/几何/开关三分不动）。**还没动一行代码**。

## 2026-10-11 T1 + T3 落地：表面尺寸扇出 / 设备夹具可用性探测（§5.1 的两条机制）

- **T1 表面尺寸扇出（M5b 的机制半边）**：表面尺寸从"宿主逐个喂视图"变成**表面的一件事**。
  - `RenderSurface::resize(w, h, ratio)` 记住尺寸并扇出给表里每条**呈现的视图**；`present` 把记着的尺寸**立刻**
    交给新呈现（新加的格子不用手喂）；`SceneView::onSurfaceResized` 在视图挂表面时**转发**给它（不挂表面时才
    是独立入口），新增 `SceneView::hasSurface()`；`SurfaceWindow::feedSurfaceSize` 改调表面，控件自己那个视图
    **不呈现**时才单独喂（`SceneView::applySurfaceSize` 是"记下来 + `applySurfaceGeometry()`"那一步，由表面
    调用 —— `RenderSurface` 是 `SceneView` 的 friend）。
  - 判据：`test_gui` **253** 绿（单视图路径一字不变）；`test_graphics` 345→**346**
    （`SceneViewViewportTest.APaneDeclaredOnASizedSurfaceIsSizedAtOnce`：present 后**零手喂**，layout 步收到
    640×720、窗口 pass viewport = {640, 0, 640, 720}）；`RenderControlTest` 里手写的 `right->onSurfaceResized(...)`
    全删（原稿说"两条"，实际**四条**分屏用例都有）。**变异 1/1 红**：`present` 不把尺寸给新呈现 ⇒ 那条新用例红。
- **T3 设备夹具的可用性探测**：`SdkPixelFixture::available()` 从"有台满足下限的物理设备"变成**共享门禁** =
  "设备 + 会话起得来 + 离屏像素给得回来"（进程内只答一次：`up()` + 一条手搭 pass 画一帧 + `read()`）。
  `BlockStorageTest` / `BlockDescriptorsTest` / `SessionTest` / `VsgBackendTest` 的**十处**设备门禁改问它；
  `getenv("DISPLAY")` 的四处假门禁删除（另外 5 处在 X11-only 的文件里、与紧随其后的 `xcb_connect` 重复，一并删）。
  - 判据：`test_vsg` **407 passed / 26 skipped → 428 passed / 3 skipped / 2 failed**。23 条误跳全部恢复运行，其中
    2 条**本就红**（`BlockDescriptorsTest.TheBindCarriesTheOffsetsInBindingOrderAndReusesOneSet` /
    `SessionTest.ADriveThatCommitsThroughTheSessionMarksWhatALostFrameWrote`，都是这些用例第一次在 Windows 上真跑
    才暴露的，**未修**，先记）；剩下 3 skip = 计数分配器 2 条 + `HostWindowReadTest` 1 条（X11-only，合理）。
  - 坑：`available()` 现在**会起一次设备**（静态缓存，整个进程只一次），所以它不再是"随手可调的便宜探测"；
    X11-only 的用例（`SessionContentTest` / `SessionMoveTest` / `GfxBackendVsgPluginTest` / `WindowCompositionTest`）
    **保留**各自的门禁 —— 它们本来就要真 X11，`DISPLAY` 对它们不是假代理，删的只是与 `xcb_connect` 重复的那几条。

## 2026-10-11 按"机制/组合"重评待办（`graphics-render-next.md` §5.1）

- **口径（用户 2026-10-11 定）**：底层提供**机制与接口**，复杂场景由业务自己组合、自己控制，交给应用层。
  已成文为不变量 6（§6）：SDK 配方只保留最常见的两三条（forward / deferred / shadow）；不造 SDK 自用的图，
  不为某个组合发明引擎政策。判据：删掉任一便利，机制路径（`engine->addPass(desc)` + 自己的 program / target）仍成立。
- **重评结果**：**M5b 拆两半** —— 机制半边 = **表面尺寸扇出**（T1，进 SDK：当时表面尺寸只喂控件自己那个视图，
  第二格靠宿主手写 `right->onSurfaceResized(...)`，`RenderControlTest` 四行；**T1 已落地 2026-10-11，见上一条**），
  组合半边 = 布局树 / 比例 / join / 格子生死（T4，**归应用层**；`SplitLayout` 那份定稿作废 —— 原稿把它当 SDK 片是错的）。
  ② 的定案同源
  （T5：多 transient 互借深度是应用组合）。其余：T2 M6（一设备 N 交换链，**已由 S10b 覆盖并关闭，2026-10-19**，见顶部索引）、T3 设备夹具可用性探测
  （删 `getenv("DISPLAY")` 假门禁，恢复 21 条被误跳的设备用例，其中 2 条本就红；**已落地 2026-10-11，见上一条**）、
  T6 便利边界（守）、
  T7 叠层对象跨重建存活（**2026-10-12 复核：不做** —— 重建罕见且本身已付新 pass/新目标；跨链继承读数与 `FpsOverlay`
  "每个数字是自己区间的真值"顶牛；记录里的做法要新增配方收对象 + builder 造/采纳两条路 + 同对象进两链的规则。
  真要有需求从最小那条起：`window_seconds` 作为值进 `FpsOverlayOptions`）。
- **已落地不需回退**：预设、内建程序 / 目标工厂（`defaultGbufferTarget`、`screenCopyProgram`…）、HUD 内容对象
  （`AxisGizmo` / `FpsOverlay`）、`addOffscreenToScreen` 都属"**可选便利**"，机制路径始终成立 —— 记为 T6 守边界，
  每次动它们自查一句"删掉它，机制路径还成立吗"。


## 2026-10-11 M7：Pass 描述符 + 每帧编译的 FramePlan（唯一的大改）

- **做了什么**：帧的真相从"声明动了才校验 + 运行时逐 pass 解析"换成**每帧编译**。`PassDesc`（一个值：name / tag / stage /
  kind / camera / content / writes(`writes_image`+`writes_shape`) / output_name / output_target / inputs / input_names /
  clear· / viewport / program / program_override / polygon_mode / depth_mode / enabled / `before_draw`）由
  `RenderEngine::addPass(const PassDesc&)` **一次性**变成那种 kind 的执行结点（`Scene`→`RenderPass`、`Screen`→`ScreenPass`），
  结点即声明的唯一存储；`compileFrame()`（新文件 `src/viz/graphics/src/EngineCompile.cpp`）在录制前把这一帧定下来：
  行（pass/content/order/解析后的 inputs）、transient 目标、以及这组声明的问题；`frame()` 只执行行。
- **删掉的东西（判据①）**：`RenderPass::wiringRevision()` + `bumpWiringRevision()`（12 个 setter 里）、
  `RenderEngine::wiringDeclarationsChanged()`、`wiring_validated_`、`wiring_validation_count_`、`wiringValidationCount()`、
  `validateWiring()`（420 行）、`WiringState` 里 10 组 seen/reported 集合、每帧的名字注册表（`outputs_`/`produced_targets_`
  变成编译期的一次走表：`plan_rows_` / `plan_produced_` / `plan_published_`）。
- **判据（实测）**：`test_graphics` **344**（341 旧 + 3 新 + builder 侧 2 条见下、一条旧用例改写：`TheWiringIsCheckedWhenADeclarationMovesAndNotPerFrame`
  → `ADeclarationChangeLandsInTheFrameThatFollowsIt`）、`test_vsg` 407、`test_gui` 253、`test_appfw` 32、`test_meshio` 34、
  `test_iobase` 94 全绿；demo 两条冒烟 11 pass 表**逐列与基线相同**、0 error/warn（判据④）；五个 gate 0 finding
  （`--strict` 仍 20 条基线）。
- **判据②③④ 的用例**：`APassCanBeDeclaredAsAValueAndRunsLikeAnyOther`（场景档：stage→order、tag 进 plan、`before_draw`
  每帧跑、禁用档不跑、output_name 当帧生效）；`AScreenPassCanBeDeclaredAsAValueToo`（全屏档：program + 名字输入，
  mock 后端看到 `last_program_source` 就是编译**解析出来的**那张目标）；`ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt`
  （一个声明一张目标、同身份同形状共享、稳态 4 帧指针不变、形状变则替换、`clearPasses` 后计数归零）。
- **变异 4/4 红**：①编译不解析输入（12 条红）②transient 每帧新建（4 条断言红）③去掉 episode 比较（14 条报告用例红）
  ④描述符的 `before_draw` 不落地（2 条红）。
- **两处记账（详见 `graphics-render-next.md` §3.4）**：①"一次一集"按**句子**比（问题就是它那句话；跨检查的那个抑制用一个
  per-compile 集合，语义与旧代码逐字相同）；②**一处行为变化**：旧代码在"两个 pass 抢一张未绑定图"那一集里把图记成"已报告"
  却从未说出口，于是碰撞消失后"图没绑定"永远不会被说 —— 编译模型里 episode 是"真的报过什么"，现在会说（用例已改写并注明）。
- **builder 侧（同片续做）**：`RenderEngine::addPass` 三个重载 + `Pipeline::addPass(const PassDesc&)` 都**交回注册的结点**，
  builder 的普通 pass（shadow / forward main / transparent / gbuffer / lighting / present / preview）已按描述符声明，
  order 一律由 `stage` 决定；结点只留在真要调的地方（`setWindowPass` / `setOffscreenTarget` / `setCompositeTarget`）。
  当时未做（其中 HUD 叠层随后续做，见下条）：HUD 叠层与宿主持有的 G-buffer。
  **新用例**：`TheRecipeIsAWrittenDeclarationAndThePlanSaysSo`（4 行的 tag/order = `GBuffer`/`Lighting`/`Transparent`/`Present`
  且 order == `pipelineStageOrder(stage)`）、`ADeclaredPassClearsTheWayItsKindDoesUntilItSaysOtherwise`（描述符不写 clear 就
  跟 kind：Screen 不洗、Scene 洗且洗到 `51,51,51`；写了就压过 kind）。**变异 4/4 红**：①clear 无条件落地（3 条红）
  ②`Pipeline::addPass(desc)` 不记账（8 条红）③present 的 stage 写错（1 条红）④`tag` 不落地（2 条红）。
- **一处默认值纠正**：`PassDesc` 的 `clear_enabled`/`clear_color`/`clear_depth` 改成 `optional` —— **不写就跟随 kind 自己的
  策略**。原因是 `ScreenPass` 构造里 `setClearEnabled(false)`/`setShouldClearDepth(false)`，描述符原来的 `true` 默认值会把
  它翻掉（搬 builder 时两条既有用例立刻变红）；描述符不能有第二套默认值。
- **② 的定案（2026-10-11，同一轮）**：G-buffer 不再"等搬"，**保持宿主创建就是对的**。把 deferred 配方
  （composite 借 G-buffer 深度那条）搬成"SDK 造两张 transient 互相借用"违反"机制在底层、组合在应用层"
  （`graphics-render-next.md` §6 不变量 6）：`PassDesc` 能声明"我要一张图"，表达不了"我这辆的深度是那辆的"，
  而那条组合是配方自己的事。SDK 这侧补齐**机制缺口**：新增 `RenderEngine::transientTarget(identity)`
  —— 拿回自己声明的那张图（"此刻活着的那一张"，形状一变即作废；这正是不该把实例长留在应用手里的原因）。
  用例：`ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt` 追加四条断言（首帧前 null / 首帧后等于
  plan 里那张 / 未声明的身份回答 null / 形状变后回答新实例且不再是旧的），变异 1/1 红（查询不看身份 ⇒ 该用例红）。
  用法（应用自己组 transient 配方）写进 `docs/guides/graphics.md` §3.13。
- **叠层那一续（2026-10-11，① 完成）**：`AxisGizmo` / `FpsOverlay` 不再是 `RenderPass` 子类，降为**内容对象**
  （`Object + RefCounted`：自己的 scene、取景相机、盒子 `boxViewport()`、每帧 `update()`），由配方声明成一条
  HUD 档的 pass（`content`/`camera`/`viewport`/`clear_enabled = false`/`depth_mode = Disabled`/`before_draw`）——
  `PassDesc::before_draw` 注释里点名的两个用例到这一刻才真的用上。句柄分两个：`gizmo()`/`fpsOverlay()` 给对象，
  `gizmoPass()`/`fpsOverlayPass()` 给结点（可见性读结点）。盒子是值，布局它的地方（`Pipeline::resize`）在告知
  对象尺寸后把它重述到 pass 上。**一处行为变化**：盒子初值是零面积矩形（还没人报过尺寸），后端按空矩形跳过 ——
  从前那种 pass 没有 viewport，会整屏画一遍叠层。**如实记账**：叠层对象仍由 builder 每次构建时新建，`setOptions()`
  重建会重置 FPS 读数窗口。证据：`test_graphics` **345**、其余五套件同基线、demo 冒烟 plan 表逐列不变（HUD 两行的
  `content` 由 `-` 变成它自己的 scene）、变异 3/3 红。

## 2026-10-11 M8：SDK 级设备夹具 + 像素判据（`tests/test_vsg/SdkPixelFixture.hpp` / `SdkPixelFixture.cpp` / `SdkPixelTest.cpp`）

- **做了什么**：`test_vsg` 原来只对着 vsg 内部测（`RenderEngine`/`SceneView`/`RenderPipelineBuilder` 一次没用过），
  于是"该画的 pass 在画"（结构）与"画出来的像素对"之间没人守。M8 补那一段：夹具用 **SDK 自己的入口**（引擎 + 视图 +
  管线 + 一条配方）在**真设备**上画一帧到**离屏** `RenderTarget`（64×64，RGBA8+D24，清屏**黑**、内容**白** ⇒
  "覆盖" = `PixelProbe::nonBlackPixels()`），再用 `RenderBackend::readColorBuffer` 读回来判。
- **两条用例与实测数字**：
  - `SdkPixelTest.AWireframeModeDrawsTheEdgesOfTheSameGeometry`：**延迟配方 + 非 Lit 档**（链条按前向建）⇒
    `Mode::Unlit` 一版 filled=1296（36×36 的实心块），换 `Mode::Wireframe` 一版 lines=180 —— 同一个视图、同一台相机、
    **同一条 `windowPass()` 对象**、同一张目标，唯一变的是光栅化模式。180 条边**全部**落在实体块的一个像素之内；
    实体版 140 个轮廓像素**全部**有边走过。
  - `SdkPixelTest.TheFixtureAlsoDrawsContentWithNoViewAtAll`：**不经视图**、手搭一条 pass 画同一批内容 ⇒ 覆盖
    3364/4096 ≈ 82%（正交窗 81% 的预期），四角仍是清屏色（既证"画进去了"也证"没涂满整张"）。
- **坑一：引擎是引用计数对象，`Pipeline` 强持它 ⇒ 栈上的引擎会被管线析构时 `delete`**。症状不发生在那一刻：第一版夹具把
  `RenderEngine` 做成夹具的值成员，结果每次都崩在 `~SceneView → removeWindowPass → free`，报 `0xC0000374`（堆损坏），
  而崩溃前每一步 `HeapValidate(GetProcessHeap())` 都是好的 —— 引擎被成员析构**删了第二遍**。夹具已改
  `intrusive_ptr<RenderEngine>`；`docs/guides/graphics.md` §3.1 的样例原来写的是栈上的 `RenderEngine engine;` + `builder.build()`
  （教了一个必崩的形态），一并改成 `make_intrusive<RenderEngine>()`。详见 `graphics-pipeline-builder.md` 同日校。
- **坑二：判据要容一个像素**。填充光栅器按**像素中心**覆盖、线光栅器按**线段触到的像素**覆盖，边界上两者选不同的像素
  （实测：轮廓那一圈整整齐齐落在实体块外一步，180 条边里只有斜边那 36 个在块里）。所以"同一批几何"写成"每个边像素都
  **紧贴**实体块"（另一个形状、或整体挪两个像素的同一形状都会红），而不是"像素集合包含"。
- **门禁问能力，不问 `getenv("DISPLAY")`**：老设备用例用 `DISPLAY` 代理"有没有窗口系统"，在 Windows 上恒为"没有"。
  新夹具问"有满足后端下限的设备 + 会话起得来 + 本后端服务离屏像素"，不满足就 `SKIP` 并说出原因。**实测把老那条放开**：
  这台机器 405/26 → **426 passed / 3 skipped / 2 failed**（`BlockDescriptorsTest.TheBindCarriesTheOffsetsInBindingOrderAndReusesOneSet`
  少了 bind、`SessionTest.ADriveThatCommitsThroughTheSessionMarksWhatALostFrameWrote` 读回全黑）——两条都是这些用例
  **第一次在 Windows 上真跑**才暴露的，**未修**（待办：按平台收窄或修实现）。
- **变异 3/3 都被像素判据杀**：①删 `SceneView::applyModeToGeometryPass()` 里 Wireframe 的
  `setPolygonModeOverride(Line)` ⇒ lines=1296 == filled ⇒ 红；②`SceneView::effectivePath()` 恒等（非 Lit 档不切前向）
  ⇒ 画面变成全屏光照 pass（4096/4096、轮廓=0）⇒ 红；③后端 `StateCommands.cpp::mapPolygon` 把 `Line` 也映成 `Fill`
  ⇒ lines=1296 == filled ⇒ 红（这一条证明**后端那一段**也在判据里）。**第一版用例里那句结构断言
  （`polygonModeOverride().has_value()`）会先红、抢掉像素那一刀，已删** —— 这一条只判像素，结构判据归 M1。
- **门禁**：`test_vsg` 407 passed / 26 skipped / 0 failed（基线 405 + 两条新用例；26 skipped 是设备/窗口环境的既有
  skip）；`test_graphics` 339、`test_gui` 253（扣掉既有挂死的 `UserIOTest.PickingFilesFillsTheLineWithoutSubmitting`）、
  `test_appfw` 32、`test_meshio` 34、`test_iobase` 94 全绿；五个 gate：`check_ai_docs` 0 finding（`--strict` 仍 20 条
  基线）、`check_doc_symbols` 95 单元一致、`check_include_hygiene` 878 文件、`check_export_annotations` 57/57、
  `check_diagnostic_formats` 43 文件；两条 demo 烟雾（`--open demo` 与 `VINE_FRAME_PLAN=1`）11 pass、0 error/warn。

## 2026-09-27 帧机器的归属：`gfx_backend_vsg/core` → `vine/graphics/backend`

- 帧机器（协议/编译/键/寿命/证据，25 头 25 源 ≈3.7k+4.1k 行）从插件的 `core/` 摘出来：先成独立模块
  `src/viz/rendergraph`（`vn::rendergraph`、`vn::RenderGraph` STATIC），**最终并入 graphics**：<!-- drift-ok -->
  `vine/graphics/backend/`、`vn::graphics::backend`、同一 target `vn::Graphics`（include 路径与命名空间在第二次
  迁移中没再变）。为什么是“一层”而不是“第二个模块”/“两个 target”，以及 A/B/C 三个复用桶，写在
  **`.ai/design/graphics-layering.md`**（开发者问“依赖哪个模块”就看它）。
- **名字：`frame` → `backend`（2026-09-27）**。判据：25 个单元里只有 6 个名字带 `Frame`（24%），`frame` 只描述了少数内容，
  还让 `frame::FrameArena` 这类限定名重复 6 次；`backend` 点的是**读者**（写后端的人），与一直用的措辞
  （“只有后端需要的那一半”）一致。**否决**：`detail`/`internal`/`impl`（**假契约**——这半边是明确支持的扩展面，
  标成“内部”等于对作者说“别依赖”）、`core`（说反“哪半是核心”，且与 `base/core` 撞名）、`support`/`kit`（没有判据）。
  改动一次性做完：50 个文件 `git mv` + 243 处 include + 560 处限定名 + 104 处命名空间声明 + 插件迁移桥 1 行；
  标识符同时正名：`CORE_LAYER_*` → `BACKEND_HALF_*`、`core_layer_findings()` → `backend_half_findings()`、
  `frame_audience_findings()` → `host_audience_findings()`。

## 2026-09-27 诊断路线合并：两个类型上 A 段，引擎持有唯一一条（`ReportOnce` + `Diagnostics`）

- **判据（可复用）**：提到 A 段的理由永远是"**宿主自己具名使用它**"，不是"它看起来通用"。按这条看，
  `ReportOnce` 的证据是硬的：引擎侧本来就有 **6 组**手写的"本帧见过 / 已报过"集合对
  （`unpublishable_passes_seen_this_frame_`/`_reported_`、`unresolved_inputs_reported_`、
  `output_collisions_reported_`、`mismatched_promises_reported_`、`unusable_inputs_reported_`、
  `unproduced_inputs_seen_this_frame_`/`_reported_`）——**同一条规则的第二份实现**，而 `ReportOnce` 的注释
  第一句就是"分两处写一条规则、谁也检查不了"。
- **粘连头按 §1.5 拆开**：`Diagnostics.hpp` 原先同时装着规则与收集器，现拆成 `vine/graphics/ReportOnce.hpp`
  （零依赖）+ `vine/graphics/Diagnostics.hpp`（路线：计数 + 转发），两者都进 A 段、命名空间 `vn::graphics`。
- **路线合并**：`RenderEngine` 持有唯一的 `Diagnostics`（原来的 `diagnostic_sink_` + `Wiring::engine_diagnostic_count_`
  一起消失）；`RenderBackend` 新增 `virtual void setDiagnosticsRoute(Diagnostics&)`，引擎在 `setBackend()` 里
  把同一实例交过去，`RenderBackend::setDiagnosticSink/diagnosticSink/diagnosticCount/reportDiagnostic` 全部改成
  转发到"**当前活跃路线**"（`owned_route_` ↔ `route_`）。宿主因此读**一个**数：
  `RenderEngine::diagnosticCount()` / `diagnosticCount(cat)` / `diagnosticsClean()`；
  旧的 `backendDiagnosticCount()` + `engineDiagnosticCount()`（注释里写着"要总数就自己加"）删除。
- **没被引擎驱动的后端不变**：`route_` 默认指向自己的 `owned_route_`，所以 `test_vsg` 直接构造 `VsgBackend`
  并 `setDiagnosticSink()`/`diagnosticCount()` 的用法一字未改。
- **插件零改动**：`VsgBackend` 的 session/recorder/compiler/executor 本来就报进插件自己那条路线，其 sink 转发
  `reportDiagnostic()` ⇒ 现在落到**引擎那条活跃路线**，所以宿主的 sink 和引擎的计数都看得到，没有重复上报。
  这也是本轮的判据：**先看管道是不是已经在转，再决定要不要动代码**（预期要大改插件，实测一行都不用改）。
- **行为语义变了的一处**（测试跟着改）：`engine.setBackend(nullptr)` 后计数**不再归零**——计数属于引擎的路线，
  后端走了不会把已经说过的话收回。钉子：`DiagnosticsTest.EngineForwardsSinkToCurrentAndFutureBackend`
  新增 `later->diagnosticCount() == engine.diagnosticCount()`（"一条路线就是一个数，两端看到同一个"）。

## 2026-09-27 帧时钟查询面：`FrameProgress`（`FrameTimeline` 的宿主面）

- 按"谁在签名里命名它"逐个量了 backend 半 **24 个头**：宿主半的引用**全部为 0** ⇒ 没有一个是硬需求（R2 不红）。
  于是只剩"值不值得开面"：结论是 **1 个该做、2 个等触发器、21 个不做**。
- **该做的那 1 个**：`FrameProgress`（`sdk/vine/graphics/FrameStats.hpp`，与 `FrameCounters`/`RetentionStats` 同形）
  ＝ `submitted`/`completed` + `inFlight()`；通路 `RenderBackend::frameProgress()`（默认 **false ＝“不记账”**，  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  别拿零当"没有在飞行"）＋ `RenderEngine::frameProgress()`；vsg 从 `Session::timeline()` 的两个水位回答。
  行业对照：`VkFence`/`vkWaitForFences`、swapchain images-in-flight、`ID3D12Fence::GetCompletedValue`、
  Unity `GraphicsFence`——**同步与计时是公开面**，而编译/图/缓存/竞技场/家族细节一律私有。
- **`FrameToken` 不提**（原计划要提，看了生产者后改）：它由 `Session::beginFrame()`（插件内部）铸出，
  **宿主拿不到**；提一个拿不到的值类型 = 凭空开一个面。触发器：哪天 `frame()` 之类把 token 交回宿主。
- **等触发器**：`PhaseTable` 只读快照（触发 = 宿主开始读相位耗时）；`TargetShape` 描述（触发 = 宿主自己表达借深度/复用）。
  **不做**：`Protocol`/`FrameCompiler`/`FrameGraph`/`FrameRecorder`/`Keys`/`StateRegistry`/`VariantPool`/
  `FrameArena`/`FrameRing`/`MaterialArena`/`Streams`/`DeviceRequirements`/`SessionMove`/`SlotProbe`/
  `AllocationGate`/`DepthProbe`/`PixelProbe`/`RetirementQueue`（机制、家族词汇或测试工具）。
  另外 `Readback` 不必再开面——宿主面（`ReadbackResult` + `readColorBuffer/readDepthBuffer`）早就在 `RenderBackend.hpp`。
- **变异反证（两处，都要红）**：引擎 `frameProgress` 直接 `return false` ⇒ `test_graphics` 红；  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  vsg 后端 `return false` ⇒ `test_vsg` 红。恢复后 292 / 453 全绿。
- **坑（本轮）**：搬走/新增 `src/*.cpp` 后，**Debug 树的 CMake 源文件表是陈旧的**（glob 不含 `CONFIGURE_DEPENDS`）⇒
  `ninja` 报 "`<搬动前的路径>.cpp` missing and no known rule to make it"。修法：`cmake -S . -B build` 重新配置（14.7 s），
  然后用 `grep -c ReportOnce.cpp build/build.ninja` 确认新文件真的进了表。Release 树没报错，不代表它也对——照检一遍。
- 三条机器规则：**R1** backend 半不许 include 任何后端；**R2** 除 `gfx_backend_*`、`test_vsg`、backend 半自身外，
  任何地方不许 include `vine/graphics/backend/`（R1/R2 都在 `check_include_hygiene.py`，双向都做过变异验证）；
  **R3** backend 半里凡有 out-of-line 定义的类型/自由函数必须带 `VN_GRAPHICS_API`
  （`scripts/check_export_annotations.py`，37/37；类型与自由函数各做过一次变异，都会变红）。
- 插件侧 `vsg_global.hpp` 的 `namespace core = ::vn::graphics::backend;` 是**迁移桥**，保住 1800+ 处
  `core::X` / `vsg::core::X` 拼写；清理它是独立的一步。
- 本轮踩到的三个坑：①`graphics_global.hpp` 的**块注释**里写 `frame/*.hpp` 会触发 195 条 `-Wcomment`；
  ②注入导出宏后，用 `^class\s+(\w+)` 抓名字会把宏本身抓成名字（检查脚本的 `DECL` 要允许 `VN_GRAPHICS_API`）；
  ③模块 global 头的约定是“**每模块一个**”，嵌套子命名空间的宏由父宏组合（照 `appfw_global.hpp` 的
  `VN_APPFWGUI_NS_BEGIN = VN_APPFW_NS_BEGIN + namespace gui {`），给子命名空间单开 global 头是跑偏。
- 验证：全量 `ninja -C build` 0 error/0 warning；`test_vsg` 452、`test_graphics` 291 全绿；
  三个卫生脚本干净；gate 见 `.ai/design/vsg-reimplementation.md` §11 的实施记录。

### 2026-09-27 续：放置判据（用户纠正）与 `frame/` 目录三分

- **放置判据只有一条：是否跨 DLL（是否导出）**。导出的 ⇒ 必须在 `sdk/`（`install(DIRECTORY sdk/)`
  就是"导出面 = 安装面"）；只在本 DLL 内用的 ⇒ 私有头与 cpp 同目录放 `src/`（appfw 的
  `src/fw/appfw/src/gui/{SurfaceWindow,RenderControl}.hpp`；`src/` 不在任何 include 根上 ⇒ 外部**物理上**
  include 不到）。**"给谁用"是规则判据（R2），不是放置判据**——我曾用"frame 不是宿主的 API"把它挪到
  `include/`，那是错的：`include/` 是**插件的**目录（插件整体不安装头），库不用它
  （`base/core` 只有 `sdk/` + `src/`）。
- 已按此回退：`frame/**` 回 `sdk/vine/graphics/backend/**`（include 拼写不变 ⇒ 0 处源码改动）。
- `frame/` 目录三分（只分组，**命名空间仍是 `vn::graphics::backend`** ⇒ 调用点零改动）：
  `device/`（DeviceRequirements SessionMove SlotProbe Readback DepthProbe）、
  `storage/`（FrameArena FrameRing MaterialArena Streams）、
  `evidence/`（Observe PhaseTable PixelProbe AllocationGate Diagnostics）；根留 11 个
  （"一帧怎么走" + 后端身份与寿命）。
- **四个"粘连"单元要拆**：`ClearPlan`/`TargetPlan`（策略 vs 应用）、`FrameTimeline`（查询 vs 账本）、
  `Diagnostics`（`ReportOnce` 工具 vs 收集器）。清单、判据与顺序写在
  `.ai/design/graphics-layering.md` §1.5（并已把 §0 的"三区"改成"两层放置 + 三条规则"）。
- 坑：**检查脚本对"搬走的路径"会报 `0/0` 假绿**（`check_export_annotations.py` 已改成"半区不存在 = finding"，
  并把 `glob` 换成 `rglob` 以支持子目录）。变异：把 src 路径写错 ⇒ 立刻 1 条 finding。

#### 同日：A 段提级（帧计数进宿主 API）

- `FrameCounters` / `RetentionStats` → **`vine/graphics/FrameStats.hpp`**（模块公共、随 SDK 安装）；
  通路 = `RenderBackend::frameCounters()/retentionStats()`（**默认返回 false ＝“不保留计数”**，与“空帧”区分）  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  ＋ `RenderEngine::frameCounters()/retentionStats()`；vsg 后端从 `frame/evidence/Observe` 回答**同一批数字**
  （不再是第二处拼写）。
- 钉子：`RenderEngineTest.FrameCountersForwardTheBackendsAnswerAndSaySoWhenThereIsNone`（无后端 / 不保留 /  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  转发各一条），`VsgBackendTest.TheHostReadsTheBackendsOwnFrameCounters`（真设备：空帧全零 → 画一帧
  `passes=1 draws=1`；**变异**：把 `counters = d->observe.counters();` 改成 `{}` ⇒ 红 ⇒ 证明测的是**值**）。
- **A 段未做**：`ReportOnce`（接进引擎要“每站点一个 episode”，且会改诊断流量 ⇒ 独立一轮）；
  `FrameToken` / `TargetShape` / 计划数据（与 S3/S4/S5 一起）。**B/C 段（Keys/设备/存储）不可提级**——
  它们是家族形状与设备机制，见 `.ai/design/graphics-layering.md` §4。

  > 对账注（2026-10-13）：上面"A 段未做"这几项**已被后续片用别的路走完，别再按当时的形状理解**：`ReportOnce`
  > 现在就是各站的 episode 工具（后端 `ContentPass`/`HostTargets`/`VsgBackend`/`VsgExecutor` 各持一枚，引擎侧
  > 由"计划问题按句子比较 episode"承担 —— 见 `.ai/design/graphics-api-review-2026-10-12.md` 与顶条）；
  > `FrameToken` 已存在（`backend/FrameTimeline.hpp`）；"计划数据"就是 S3/S5 之后的
  > `FrameDescription` / `FramePlan`。**B/C 段那条约束仍然成立**，那一半不用再对账。

#### 同日：`frame/` 最终形态 = **扁平一个目录**（判据记在 layering §1.5）

- 结论：**25 个单元平铺在 `frame/` 下**，不分组、也不提为 `frame/` 的同级。四条判据逐条否决拆分：
  ①放置看"是否导出"（都导出 ⇒ 都必须在 `sdk/vine/graphics/**`，在哪个目录是组织问题）；
  ②**同级不改变权限**（要让宿主/引擎用只能提到 `graphics/*.hpp`）；
  ③**同级不降低深度**（`frame/X.hpp` 与 `device/X.hpp` 同深度——"层级太深"用同级解决不了）；
  ④**同级不区分读者**（frame/device/storage/evidence 的读者都是后端）。
- 唯一站得住的反对理由只有"目录名说实话"（`frame/DeviceRequirements.hpp` 会误导），代价是多一个顶层名字 ⇒ 不值得。
- 弯路记录：我先做成 `frame/{device,storage,evidence}/`（子目录），又抬成同级——**两次都不是问题本身**；
  问题只是"要不要拆"，答案是不要。分类仍然成立（设备/存储/证据/一帧怎么走），但只写在文档里、不落成目录。


> 2026-09-26 **FPS 读数改成窗口真平均（设计 graphics-overlay.md 新节）**
> · 旧形：每帧瞬时 1/dt 的 EMA(0.2) + 0.15 s 刷新 ⇒ 报“样本的平滑值”：时间常数 1/(0.2·fps)（60fps≈83ms、
>   200fps≈25ms，依赖被显示的数）；真实 60→30 阶跃印 **40/33**（没发生过的值）；拖动流 10 s 改值 75 次、单步
>   最大 17 fps。
> · 新形：**窗口内数帧/窗口墙钟**、关窗即发布、**零混合**；同阶跃印 60,60,60,60,60,**54**,30,30（54=真的前60后30
>   那个半秒的真均值）；50 ms 卡顿如实 56.25。旋钮= `Sampler::window_seconds`（默认 0.5 s、2 Hz、整数死区）。
>   实测取舍：0.5 s ±3.0 fps / 响应 0.5 s；≥2 s 摊平真实短事件；<0.25 s 噪声 ≥±5。
> · 行业（PresentMon 一手）：主量=帧时间 ms，FPS 派生；capture=逐帧 CSV+摘要（avg/min/max + 90/95/99th 分位）；
>   overlay 画图。⇒ 一个数字做不到既可读又不撒谎，我们选可读那半并保证不撒谎。
> · 门禁：`FpsOverlayTest` 5 例 + 变异 2/2 红（每帧瞬时 / 跨窗混合）；test_graphics 287→**291**；两棵树门禁
>   cases=451、应用判图逐字不变。

> 2026-09-26 **ASan 余量收口（设计 §11.16dg）——单一共享运行时 + 判词分层**
> · `asan_check.sh` 旗标改 `-shared-libasan`（+`-Wl,-rpath,$(clang -print-runtime-dir)`，clang 不加 rpath）⇒
>   插件侧分配与主程序同账本，报告可读；整树重建一次，默认电池与 test_vsg 复验绿。
> · 读明白后的结论：严格 test_vsg 剩 **零 Direct、全 Indirect** ⇒ 直接根全被豁免，其余是同一保留的子树
>   （分配帧= glslang `spv::Builder` 构造里的扩展类型指令，~12 条/672 B 每 builder）。判词分层：Direct=永远红；
>   Indirect 条目 PASS 并把计数+豁免表打印（不隐藏）；新增 `leak:spv::Builder::` 带理由豁免（解出符号时命中）。
> · 实测：严格 test_vsg exit 0（间接 106 块/5,936 B/28 条）；`test_gui '*' + 泄漏` 仍红=其自身 13 个 Direct 根
>   （GuiTest::SetUp/buildDock），非本线。边界：插件帧符号化只部分（豁免根据=LSan 的结构性 INDIRECT 标记）。

> 2026-09-25 **ASan 全量 test_vsg 首次跑通（设计 §11.16df）——一雷一 UAF 一卫生**
> · volk 静态进插件**所有树都没有 -fPIC**（Debug/Release 靠历史对象在绿；ASan 树 ld.bfd 直接拒链）⇒
>   `POSITION_INDEPENDENT_CODE ON`，三棵树重链验证；ASan 树重生成要 `env -u http_proxy`（FetchContent 更新步）。
> · **真 UAF**：`ContentStore::tablesFor` 查表前读 `geometry->revision()`——停靠契约（"窗口内旧计划仍应答"）与同文件
>   三处的"先查后读、持 share 才准读"违背 ⇒ 成员检查移进 `liveGeometry`；修前 ASan 红、修后 0 ASan 错。
> · 泄漏主体 = 22 个设备用例不 `xcb_disconnect`（685,560B/395 处）⇒ `TestXConnection` RAII + 22 处插入 ⇒ **6,048B/108 处**；
>   分配门禁 mallinfo2 用例 ASan 下 GTEST_SKIP（量具对 ASan 分配器不可见）。余 6KB 栈穿插件、符号错乱（插件与主程序
>   各带一份静态 ASan 运行时）⇒ 收口（-shared-libasan / 逐条豁免）**登记待决**。
> · 提交 `28f1332` + `6dc23d1`；两棵树 451/451、门禁 cases=451 vuid=0、应用行逐字不变。

> 2026-09-25 **B9 在飞槽数地板 + 学到更深即重布局（收口）**
> · ① 构造地板：`BlockStorage::create` 以 `layoutForInFlight(layout, kAssumedInFlightSlots)` **上举**五个 per-frame
>   形态（只举不缩）；`hasSlabsForInFlight` 是判断面。旧字面量 `slots{3}` 从此造不出存储——**上举而非拒绝**
>   （create 的 null 是设备语言，太浅的存储没有正确的服务方式）。
> · ② `VsgBackend::growBlockStorageIfNeeded` 每帧读 `Session::slots()` 并把“槽数太浅”与“预算增长”**合并**一次
>   `create`+`adoptBlockStorage`——分开做会丢另半请求（替换身 `growthNeeded` 从零起）。Info 三个文案变体；
>   预算变体前缀照旧含 "grew"。测试接缝 `BackendContentAccess::assumeInFlightSlots`（本机框架报的恰=假设 3）。
> · 门禁四条：无设备策略、设备建造地板、接缝触发重布局（4 ⇒ 五形态 5、恰一条 Info、次帧不再换）、原增长用例。
>   变异 4/4 红：地板移除 / 上举失效（策略+地板+接缝；批量跑的插件黑帧单跑绿=环境模式）/ 接线断 / 谓词过宽（+套件崩）。
>   test_vsg 448→**451**、门禁 cases=451 vuid=0 hazard=0、应用阶段逐字不变。
> · 诚实边界：本机学到的在飞数恰=假设 3；“真 N≥4”经接缝可走同一条分支，但本机未自然观测到（真机首现时复核文案与内存）。

> 2026-09-25 **B8 跨帧差异相位（部分）：可读回子类守住，F−N 洞记实**
> · 新相位 `VsgBackendTest.TheLastFrameOfAMovingSequenceKeepsItsOwnViewBlockValue`（test_vsg 447→**448**；门禁 cases=448/vuid=0、
>   应用阶段逐字不变）：每帧动相机（0.95..0.75）、**末帧 0.1**；片元 `step(0.75,|cam_pos.x|)` 把红通道变成 0/1 两类
>   （linear/sRGB 同字节，判据与色彩空间无关）⇒ 末帧只要读到别帧数据（绑定冻结/过期、错 slab）就整片翻 255。
>   变异（`ContentPass::recordCommand` 读侧绑定落后一 slab）**2/2 红**；写侧"落后一 slab"变异自洽、抓不到（读写共用同一 `view.offset`）。
> · **为什么抓不到 §11.16dd 本体**：受害帧 = 最旧在飞帧（F−N），readback 只见最新帧；探针实测（`writeView` 帧号/slab/偏移/时间戳、
>   `Session` 学到槽数 = 3）本机框架逐帧节流 ≈ 1 GPU 帧（submit/回拷阻塞），F−N 总在覆盖写之前收尾；负载压到 4096²×64 遍
>   （≈1G 像素/帧）仍无重叠。⇒ 该洞继续由结构性旋转用例（`BlockStorageTest.TheSlabsRotate…`）承担；登记 B8 行已改写。
> · 方法记：临时探针跑完即撤；"读是活的"用两次判别实验钉的（全 0.9 ⇒ 255；0.9×5+0.1 ⇒ 0 ⇒ 读自己帧的 slab）。

> 2026-09-25 **M11ah：节点四种变更行为钉住（换父 / 换材质 / 换着色器 / 移除；设计 §10.2）**
> · 新套件 `SceneMutationTest.*`（4 例，无设备，收集层）：①换父 = 单父结构 + 命令世界矩阵跟随 + **新旧两链都重算**
>   （`boundsRecomputeCount()` 证人）+ 旧父 box 缩回；②换父跨越 `StateNode` ⇒ 材质/程序/不透明度按新链整体切换
>   （移回即还原）；③同一 `Geometry` 换材质/换程序 ⇒ **下一次收集**带新身份，清空回继承；④`removeChild` ⇒
>   命令消失、旧链重算、重加回仍可画（移除 ≠ 销毁）。
> · **修正过的判断**：漏公告后果**不对称**——漏入库侧（新父 `addChild`）才是"物体消失"（陈旧空盒剔掉内容）；
>   漏出库侧（旧父 `removeChild`）只是陈旧**超集**盒（多走空子树，性能级，画面仍对）⇒ 抓出库侧必须用重算计数
>   证人；`scene.boundingBox()` 是新鲜走线，照不出漏公告（第一版用例被这点骗过：M1「removeChild 不失效」跑绿）。
>   变异 3/3 红（两侧静默 / 换父不摘旧父），恢复绿。
> · `test_graphics` 283→**287** 两树；ctest 23/23 两树；门禁两树全阶段干净（cases=446、app 行逐字同）；无源改动。

> 2026-09-25 **M11ag：逐 draw compare op 进动态层（收掉 M3d-2 的登记待办）**
> · 背景（旧登记）：`core::DynamicState` 没有 compare 字段，M2c 的 `ContentPipeline` 把 `GREATER` 烘死
>   （§11.11 的 reverse-Z 约定），而旧实现按 `DepthState.compare` 逐 draw 映射 ⇒ **内容自定比较算子**的
>   画面新旧不同。落地：`DynamicState` 加 `compare`（默认 `Less`，距离语义）；`resolveDynamicState` 直接
>   拷贝 `state.depth.compare`（pass 早退分支也拷 —— 比较与深度策略正交）；`makeDynamicStateCommand` 以
>   `RenderStateMapper::mapCompareOp` 做边界反转（`Less→GREATER` / `LessEqual→GREATER_OR_EQUAL` / `Greater→LESS`…）。
>   默认 `Less→GREATER` 与烘死时逐字同值 ⇒ **无 StateNode 的场景管线与画面不变**（门禁 app 判据逐字复现）。
>   `Keys.hpp` 审计表 `DynamicState` 行补 `+ compare`；文件头 "deliberately NOT resolved" 段改写为距离语义。
> · **另修一处静默**：`DynamicState::operator==` 补上 compare ⇒ 比较算子变了现在会被 variant 的"动态变了"
>   看见（此前改了它不会重发 set 命令，画面停旧值）。
> · 用例：`ContentDrawTest.TheDynamicMappingFollowsTheEngineConventions`（默认与自定义两向）+ 
>   `FrameCompilerTest.TheAuthoredCompareOpTravelsIntoTheDynamicState`；变异 3/3 红（烘死回归 / 解析不拷 / 翻转表破）。
>   `test_vsg` 两棵树全绿；门禁 cases **445→446**、vuid=0、app 画面逐字同。

> 2026-09-25 **M11ae/M11af：首帧叠层收口 + 应用启动尺寸定下来（设计 §11.16db/dc）**
> · **首帧叠层**（旧登记"一帧没有工具叠层"）：探针（`AxisGizmo::onSurfaceResized/execute` 前 8 次，跑完即撤）把
>   旧读法"表面尺寸未知"改写了——预热帧（160×160）叠层**有效**（vp 96×96）；第一个**上屏**帧画在 Qt 布局**瞬态**
>   尺寸 **100×30** 上（`Startup frame going away` 之后、`-> Presenting` 之前），角落盒放不下 ⇒ 旧算式
>   `dev_h-2*margin` = **-2** 给出**负视口**（-2×-2），后端空矩形守卫跳过并记一条 Info（日志里那条
>   "its rectangle is empty" 就是它）。修：`side = fitted>0 ? min(size_px_,fitted) : 0`——零面积=说出来的"画不下"。
>   `FpsOverlay` 同类边界**保持原样**（放不下留上一矩形、被夹进目标；两拼法未统一，此句即登记）。
>   钉子 `AxisGizmoTest.ASurfaceTooSmallForTheBoxNeverYieldsANegativeViewport`；变异 1/1 红（放回旧算式）。
>   `test_graphics` 两棵树 **283**。
> · **启动尺寸**（旧登记"窗口随日志长大 = 产品决定"）：`MainWindow` 在 `setMinimumSize(800,600)` 旁加
>   `resize(800×600)`（= 最小值 = 文档默认 = 门禁判图尺寸），首 show 不再走"按布局 sizeHint 定尺寸"。
>   **诚实记录：今天本机没能复现漂移**——修前后各一批 + 两次强制宽控制台对照都读 **378×247**（顶层链：
>   渲染区 378×247 ← 容器 800×600 ← 顶层 864×664）；§11.16cs 那组"六次启动六个渲染区（最大 3418×1110）"
>   仍是漂移存在的记录，改动按**构造**去机制。钉子 `MainWindowTest.TheOpeningSizeIsStatedInsteadOfInheritedFromTheLayout`
>   （构造后 min==size==800×600 且 `WA_Resized` 已置；hint 跟随在本机两平台都不可复现，所以只钉构造决定）；
>   变异 1/1 红（删 resize 行 ⇒ 回 Qt 默认 640×480）。`test_gui` 两棵树 **208**。
> · **顺带的既有红**：`build-release` 跑 `test_gui` 全量露出 `PluginLifecycleTest.HandwrittenRegistrationCanDisableForAllUsers`
>   一直红（断言写死 Debug 后缀 `test_plugind`，`808bbd2` 起；Release 库叫 `test_plugin.so`）⇒ 改成"注册路径必须点名
>   沙箱拷贝的文件名"（期望值从拷贝自身推出）。教训：**测试里写死构建后缀 = 另一棵树必红**。

> 2026-09-25 **M11x：B5 的触发规模做成配方（设计 §11.16cu）**
> · `VsgBackendTest.TheDocumentedScaleGrowsTheDrawBudgetOnceAndThenServesEveryCommand`：一次 `render()` 带
>   **2000 条命令**（文档点名的规模），默认预算。帧 1 拒 **976 个 draw 块**（首条 + 汇总两条诊断）；同 call 的
>   light/shadow 各 1 块装得下 ⇒ 撞顶只在 draw region。帧 2 换存储（draws 1024→2048）后全部装下、帧 3 稳态，
>   全书 3 条诊断、**一次**增长事件。record ≈2–5 ms、commit ≈84–179 ms（打印）。
> · 口径教训：一次 `render()` = **一个 drawing call**（灯/影每 call 一块），命令只是它的 draw 块 —— 我按"每条命令一个 call"
>   预测成三帧三次增长，实测全相反。套件 **440**、两棵树门禁 vuid=0；本片只加配方与记录（无源改动）。

> 2026-09-25 **M11w：块预算按需增长（收掉 B5 后半；设计 §11.16ct）**
> · 某帧写超预算仍**按帧**拒写（有报告 + 计数），但 storage 记住"最坏一帧试了多少"（`growthNeeded()`，max over
>   frames、不重置）；下一次 `beginFrame()` 用**新 buffer**（预算 `max(need, 2×budget)`）整体替换，旧 storage 与
>   **被换下的描述符集**都进退役队列停放，并以一条 Info/`ContentSkipped` 报"拒绝为什么停了"。
> · **踩到并修掉的真缺陷（03047）**：`BlockDescriptors::repoint` 换 set 时若不停放旧的，池会把已被释放 set 的
>   `VkDescriptorSet` 句柄发回，新 set 在下一帧编译时 `vkUpdateDescriptorSets` 一个挂起命令缓冲仍在用的句柄 ——
>   用例全绿、验证层 1 条（M4 变异专门钉它）。命令缓冲对 vsg 对象的引用**保不住句柄**。
> · 判据：`test_vsg` 437→**439**（两个 `BlockStorageTest` 用例 + `VsgBackendTest.AFrameThatRanOutOfBudgetGrows
>   TheStorageBeforeTheNextFrame`，端到端两层像素）；变异 **4/4 红**；两棵树门禁 `cases=439 vuid=0 hazard=0`、
>   应用行与历史逐字相同。测试接缝：`BackendContentAccess::{storage, useBlockStorage}`。

> 2026-09-24 **M11i：租约的顺序有了设备相位（设计 §11.16ce，收掉 M10c/M10d/M10e/M10f 反复登记的一条）**
> · 新相位 `runLeasedTargetOrderPhase`（`DevicePhaseTest` 第七行）：出借方 8×4 + 借方（自己的颜色、出借方的深度），
>   计划把**借方那趟放第一位**（走计划顺序 = 走错顺序），事实里两个都要求长到 16×12 ⇒ 两趟 `ResizeInPlace`；
>   判据：`resized == 2`、`refused+failed == 0`、两边 extent 16×12、`queue.pending() == 2`、两个目标在新 extent 上
>   各自的清屏色、诊断干净。第一帧是 bootstrap（`Repair` 那一臂压过 resize，所以先按原尺寸渲染一帧）。
> · **写它当场抓到的**：执行者的排序**由事实驱动** —— 手工拼 `TargetFacts` 时漏了租约（`depth.borrowed/source`）
>   ⇒ 它按计划顺序走、借方被拒（`resized==1`）。填法照 `OffscreenTarget::depth()`：`has_depth/borrowed/source/
>   promotion/any_pass_preserves_depth`。任何自己拼 facts 的调用方都要照这条。
> · `counters.parked` / `counters.plan_applied` 是**相位体自己加的**（不是读队列）⇒ 新相位要加；表尾合计跟着更新
>   （frames 12→14、targets_built 8→10、resizes 2→4、plan_applied 2→4、parked 4→6）。
> · 证据：两棵树门禁 `cases=431 failed=0 vuid=0 hazard=0 skipped=0`、**相位 12 行 / 2 次运行**（新行
>   `[selftest] leased targets: … lender first …`）、应用 `vuid=0 warnings=0`。诚实记录：应用画面参数这次 Debug 跑
>   是 `86.53%/242`、Release 是 `87.04%/244`（历史各跑都是后者）⇒ 那几个数字跑与跑之间有 ~0.5% 抖动（FPS 数字 /
>   抓帧时机），门禁判的是阈值。
> · 变异 1/1：`indexOf(lenderOf(...))` 改成"没有出借方"（按计划顺序走）⇒ 相位红（`resized==1`、借方停 8×4、
>   行文本 FAILED）；恢复后 7 行全绿。

> 2026-09-25 **M11o：拒绝报告"先说一遍，再说一共几条"（B5 前半；设计 §11.16ck）**
> · 老问题：拒绝路径**逐命令**上报（~35 个 `reportRefused` 调用点），场景里有坏内容时刷屏；登记的形状
>   = "每 pass 每原因一次 + 后续只计数"。
> · 改法：`ContentPass` 一张**本趟账本**（`RefusalRow{what,why,count}`）——首次照旧完整报（点名那条命令），
>   同 `what` 后续只计数；pass 结束时（`record` 里 **RAII 守卫**，任何 return 都跑）每个 count≥2 的
>   原因报一行 "N drawing call(s) were not drawn for one reason - …"；诊断计数跟着消息走。
> · 证据：新真设备用例 `ManyRefusalsForOneReasonAreOneLinePlusACount`（3 条同因被拒 ⇒ **恰好 2 条**消息 +
>   计数==2）。变异 1/1 红（`noteRefusal` 恒 true ⇒ 4 条消息）。两棵树门禁
>   `cases=438 failed=0 vuid=0 hazard=0 skipped=0`。
> · **B5 后半（块预算 1024/256）不改**：`BlockStorage.hpp` 的文件注记写明这是**有意**的（"growing the buffer
>   would move bytes a submitted command buffer still names"）；登记里"按需增长"的方向与实现冲突，若要真的
>   增长要走"新 buffer + retirement"（同 `MaterialArena` 轮转），是另一片的量级。

> 2026-09-25 **M11n：高光强度真的接上了（收掉 D2；设计 §11.16cj）**
> · 登记：`Material::specular()` 文档写"A 是强度"，但**没有着色器读它**（前向与 G-buffer 都只读 `.rgb`）。
>   选**接线**（改文档会留一个哑字段）：前向 `... * material.specular.rgb * clamp(material.specular.a,0,1) * ndl`；
>   G-buffer `out_specular = vec4(rgb * a, 1.0)`（把强度折进彩色 ⇒ 光照侧一行不改，两条路按构造同乘积）。
> · 文档同步：`Material.hpp` 的 `@brief`、`docs/guides/graphics.md` 的规范 G-buffer 清单。
> · 证据：新真设备用例 `ContentPassTest.ASpecularIntensityScalesWhatTheSurfaceReflects` —— 材质选成
>   "答案只差一次乘法"（albedo 全黑、高光白、太阳在镜头后方 ⇒ `dot(n,h)=1`、`pow=1`），四趟里三档读数
>   （前向 α=0 ⇒ 0、前向 α=0.25 ⇒ 0.125、G-buffer→光照 α=0.25 ⇒ 0.125）。变异 2/2 红（两条路各去掉强度 ⇒
>   读回 0.5）。两棵树门禁 `cases=437 failed=0 vuid=0 hazard=0 skipped=0`、`vine_shader_check.sh` PASS。
> · **顺手量到**：演示证据行**逐字不变**（87.04%/85.14%, preview 244）——demo 的材质全用 `alpha=1.0`
>   ⇒ 这缺陷能活到现在的原因就是"没人用过分数 alpha"。

> 2026-09-25 **M11m：一句话只说一次——半分片的插话归半片所有（设计 §11.16ci）**
> · 登记过两次的老问题：`ContentPass` **每帧新建** ⇒ 它的 `ReportOnce` 插话每帧重置（`shadow_map` 那条
>   警告每帧重复）。`Scope` 的注释早就写着契约"episode 的结束是**调用方**的选择"，缺的是有人真给一个
>   活得更久的 scope。
> · 三处插话各归各处：半片的两种句子（content half 不能服务 / `reportShadowNotSampled`）归 **half**
>   （`ContentHalves::Data::Half` 的两个 `ReportOnce`，`halvesFor` 填进 `Scope::Entry`）；光掉落归
>   **每 pass**（`ContentAssembly::Data::lights_dropped`，按 `PassId` 一行，id 不回收 ⇒ 随 pass 数长、
>   不随帧长）；空矩形归**会话**。
> · 契约写进类型且**向后兼容**：`Entry` 尾部追加 `reported/shadow_reported`，`Scope` 追加
>   `lights_dropped_episode/empty_rectangle_episode`——**空 = 旧行为**（一帧一插话），手工拼 Entry 的
>   老用例一行不改。
> · 证据：新**无设备**用例 `ContentHalvesTest.TheHalvesEpisodeStateIsTheHalvesOwn`（跨帧同一个对象、
>   第一帧说过第二帧静默、另一个 pass 也不再说、`rearm()` 后再说、两种句子互不干扰）+ 真设备用例扩展
>   （第二帧静默、重臂后再报）。变异 2/2 红（记录器忽略调用方状态 / halves 不再交出状态）。
> · 门禁两棵树 `cases=436 failed=0 vuid=0 hazard=0 skipped=0`、应用 `vuid=0 warnings=0`。
> · 未做（登记）：`lights_dropped_episode` / `empty_rectangle_episode` 的管线只有代码审查，没有自己的
>   用例（触发要真设备 + 一次"灯装不下"/"矩形为空"的计划）。
>
>   对账注（2026-10-13）：**这条已过期** —— `ContentPassTest.cpp` 里 `recordOneFrame(frame, empty_rectangle,
>   too_many_lights)` 那一条就是它（真设备、按相位驱动、"第二帧静默 / 重臂后再报"都断言了）。

> 2026-09-25 **M11l：门禁的应用阶段有了第三条画面判据——"整片背景灰"（设计 §11.16ch）**
> · 起因 = M11k 里那条 M3 登记：D4 契约的另一半（G-buffer 写 `w = 1`）没有用例看着 —— 把 `w` 改成 0，
>   演示窗口**看得见**（14 440 px 平色 `(69,69,69)`、平均色 (101,112,121)→(84,91,99)），但门禁两条判据
>   （非黑占比 ≥30%、预览条 max ≥64）**都通过**。
> · 量出来的分离度：健康画面里平背景色 **2 px**（±4）/ **1 px**（线性拼法）；变异画面 **13 565 px = 14.53%**。
> · 改法：`scripts/ppmprobe.py` 多一个**可选**颜色参数（+容差，默认 ±4），给了颜色就多打一行
>   `share r,g,b +-t: N pixel(s) (P%)`（原输出不变）；应用阶段多一条判据 = **预览条以下"视口带"**里平背景色的
>   占比（两种编码取大者）≤ `VINE_GATE_APP_MAX_BACKGROUND`（默认 **5%**），两个样本都判、写进证据行。
> · 证据：健康时两棵树 `background 0%`；变异（G-buffer `w = 0`）⇒ 应用阶段红
>   （`before … background 25.64% … the picture is one flat surface`）。
> · **限度（诚实记录）**：resize **之后**那个样本不敏感（698×132 里四个预览槽盖住视口大部分 ⇒ 同一次失败只读
>   3.97%）⇒ 敏感的是 resize **前**那个样本；整窗版 15.47% vs 视口带版 25.64% ⇒ 判据选视口带。
> · 顺带登记：Debug 门禁**首跑**有一次 `BlockStorageTest.TheRegionsAreLaidOutOnceAndDoNotOverlap` 红、重跑绿
>   （门禁把首跑失败具名写进证据行）；此前不在已知偶发名单 ⇒ 第二次出现就按 §11.16cf 的办法先分环境/真缺陷。

> 2026-09-25 **M11k：延迟光照的"背景"改看写掩码（收掉 D4；设计 §11.16cg）**
> · 旧判据 `dot(pos,pos) < 1e-6`（`builtin_deferred_lighting.frag`）在**相机贴住几何**时把视图位置 ≈(0,0,0)
>   的**写过的**像素当成背景 ⇒ 固定的 0.06 色洞。新判据 = G-buffer 位置附件的 **w**：写过的像素
>   `w = 1`（`builtin_gbuffer.frag` 本来就这么写），没写过的保持**透明黑**（后端 `core::planClearValues`
>   Rule 4：只有附件 0 收到 pass 颜色，其余彩色附件保持构造值）⇒ `if (pos4.a < 0.5) 背景;`。
> · **契约的两半**写进两个 shader 的注释 + `docs/guides/graphics.md` §3.4 手搭延迟管线清单（位置的 w 不是备用通道）。
> · 证据：新真设备用例 `ContentPassTest.TheLightingBackgroundIsDecidedByWhetherTheGbufferWasWritten` ——
>   **手写** G-buffer writer 覆盖左半、位置**精确**写成 `vec4(0,0,0,1)`（光栅化几何不可能可靠落在
>   `|pos|² < 1e-6` 上，所以必须手写值），右半保留清除；两探针：写过的贴脸像素 = `albedo × ambient` =
>   (0.5,0.125,0.125)，没写过的 = 0.06 平背景。变异 2/2 红（判据改回距离 ⇒ 红，消息自带 "(15,15,15) 说明
>   判据又在读距离"；writer 写 w=0 ⇒ 红 ⇒ 用例读的是写掩码）。
> · **M3 诚实记录**：把**引擎 G-buffer** 的 `w` 改成 0 —— 演示窗口**看得见**（天空渐变消失、14 440 px 平色
>   `(69,69,69)` = sRGB 编码的 0.06、平均色 (101,112,121)→(84,91,99)），但**门禁判据看不见**（content 两次
>   都 87.04%，阈值 30%）⇒ 登记"应用阶段画面判据对'整片天空变平背景'不敏感"，方向 = 加平色/颜色种类判据。
> · 门禁两棵树 `cases=435 failed=0 vuid=0 hazard=0 skipped=0`、相位 12 行 2 次、应用与修前**逐字相同**
>   （87.04%/85.14%, preview 244）—— 演示里没有一个可见像素的视图位置接近 0，这就是缺陷活到现在的原因。
>   `scripts/vine_shader_check.sh` PASS（9 shader 编译 + 内嵌副本逐字节同步）。

> 2026-09-24 **M11j：读窗口的偶发红 = "窗口还没被服务端报可用就呈递"（测试宿主加了同步点；设计 §11.16cf）**
> · 先把黑拆成两种事实：`TestHostWindow::readError()` 记下每次读自己的 X 错误 ⇒ `read-error 8`（`BadMatch`，
>   服务端**拒绝**了读）与 `read-error 0`（读被服务、窗口里就是没画面）。改前两者都是 `{0,0,0}`，
>   所以被当成"画面错误"读了好几轮。设备无关的新用例组 `HostWindowReadTest`（纯 X，不碰设备）钉住两半 +
>   有界等待的两种行为。
> · 20 次探针的时间线：`@501ms read-error=8`（窗口还没 VIEWABLE）→ 4 次 `read-error=0` 仍黑（≈1.3 s）→
>   **再呈递一次自己的图**后立刻 `(0,137,0)`；另一跑 5 s 内始终没到 ⇒ ①**窗口被报 VIEWABLE 之前的呈递永远
>   到不了**（等多久都没用）②同一张画面再呈递一次能落地。
> · **plan-free 帧不是 settle**：会话空帧呈递的是**会话自己的图**，会把窗口重画成它那张（清屏色）⇒
>   "驱动两帧空计划再读"是在**盖掉**画面。三处 settle 改成再呈递同一张画面（`VsgBackendTest` 的
>   `settle()` 直接再 `drive()`；两个会话套件在循环里重新 `assignFrameGraphs`——分配只供**下一次**
>   `commitFrame` 用，必须进循环）。
> · 根因（设备无关）：`TestHostWindow` 构造先等"服务端报 VIEWABLE"（原 2 s 期限），**只跑 X 的
>   `HostWindowReadTest` 20 次红 2 次**，红的就是 `wasViewableAtCreation()` ⇒ **服务端有时 >2 s 才把刚 map
>   的窗口报成 viewable**；用例在那之前呈递的帧全作废。抖动**成片**出现（另一时段 30 次一次不迟）。
>   ⇒ 期限放宽到 10 s（迟到会打 `[host-window] … after N ms`），花销只在服务端迟到的那几跑。
> · 证据：两棵树门禁 `cases=434 failed=0 vuid=0 hazard=0 skipped=0`、hygiene `0/793`、相位 12 行 2 次、应用
>   `vuid=0 warnings=0`（`87.04%/85.14%, preview 244`）。速率：单跑 4/12 红 → 0/12、三套件组 2/3 红 → 3/3 绿、
>   全量 4 跑（3 跑 3~7 条红）→ 4 跑全绿 434。
> · 变异：M3（去掉构造的等待）⇒ `wasViewableAtCreation` 用例确定性红，恢复即绿；M4 **没证成**（`kViewDeadline=0`
>   重跑 12 次不红——那时服务端已不迟到）⇒ 速率对比是**跨时段**量的，机制由探针直接测到。
> · 未做（登记）：产品侧"画面已落地"的事实（会话 presented/displayed 计数或 present 完成事件）；构造的等待只
>   保证"呈递发生在窗口可用之后"，不保证"读的时候画面已落地"。
>
>   核对（2026-10-13）：**仍然成立，且这次核过前提** —— 本仓的 `framesPresented()` 是"我们调了几次 present"的计数
>   （`VsgBackend` → `Session`），而 pinned 的 vsg 1.1.16 不暴露"真的上屏了"的计数/完成事件（`Window.h` 里没有），
>   所以这个事实今天仍无处可取，等它真需要时再选"我们自己数 `present` 完成"还是"上游暴露"。

> 2026-09-24 **M11h：空绘制调用不是绘制调用 —— 应用日志最后一条 warning 消失（设计 §11.16cd）**
> · 定位手法：在 `ContentAssembly::record` / `ContentHalves::halvesFor` / `ContentPass::record` 的拒绝点插 `[probe]`，
>   第一帧第 7 趟 pass 是 `content=1 entries=0` 且**没有任何"事实缺失"的 skip 输出** ⇒ 那条 draw 本身
>   `commands=0`；再在 `beginPass` 打印身份 ⇒ `id=7 name=`（无名 = `applyOverlays` 的 gizmo/fps 叠层），
>   `AxisGizmo` 在 `surface_w_ <= 0` 时不设视口 ⇒ 那一帧的场景收集是空的（引擎侧行为，合法）。
> · 缺陷链（三跳，只有第二跳是错的）：空调用合法 → **录制器把调用记成绘制**（计划描述一件不存在的工作）→
>   内容层看到"内容绘制"却没有半片 ⇒ 拒绝**整趟**（连清屏）+ 一条 warning。修生产者，不给消费者加容忍。
> · 修法：`FrameRecorder::render` 只在 `commands` 非空时记绘制；调用仍消费 viewport/lights 公告（一条公告一次
>   调用不变）；`FrameCounters::draws` 是"draw calls **recorded**" ⇒ 空调用不计数；既没画也没清屏的 pass 照旧不存在。
>   契约注记补在 `FrameRecorder.hpp` 的消费规则第二条。
> · 守卫（无设备）`FrameRecorderTest.ADrawingCallWithNothingToDrawIsNotADrawAtAll`：空调用不留 draw、公告被它
>   消费、计数器 == 1、诊断干净；空调用 + 无清屏 ⇒ 计划里没有这趟 pass。变异 1/1：`if (true || !commands.empty())`
>   ⇒ 守卫红 **且** 应用 warning 回到 1；恢复后两处都绿。
> · 证据：`build` 与 `build-release` 同一道门禁 —— **431 用例 0 failed / 0 VUID / 0 SYNC-HAZARD / skipped=0**、
>   hygiene `0/792`、相位 11 行 2 次收尾、应用 **`vuid=0 warnings=0`**（修前 1），画面参数与修前**逐字相同**
>   （`87.04% / 85.14%, preview 244`）；gizmo 从第二帧起照旧可见（首帧它本来就没有命令可画）。

> 2026-09-24 **M11g：计数分配器把 4 对齐的分配变成 `bad_alloc` ⇒ 设备电池全灭（已修；设计 §11.16cc）**
> · 症状：**带设备**的 `test_vsg` 在第一个设备用例上 abort（`LLVM ERROR: out of memory / Buffer allocation failed`）；
>   其实是"分配被拒"而不是"没内存"（0.03 s、峰值 RSS 85 MB）。gdb 断 `posix_memalign` ⇒ 最后一次请求
>   **`align=4 size=512`**；栈 `llvm::allocate_buffer` ← `EngineBuilder::selectTarget` ← lavapipe 的 JIT。
> · 根因：`tests/test_vsg/AllocationCounter.cpp`（§11.16bx 加的**计数分配器**）把**每个** aligned 分配转给
>   `posix_memalign`，而它对 `alignment < sizeof(void*)` 回 EINVAL ⇒ 我们按契约抛 `bad_alloc`，libLLVM 不接 ⇒ abort。
> · 代价：**M11b–M11f（六片）的证据行（"passed / 24 skipped"）都是不带设备的那一半** —— 设备电池事实上停了几片，
>   没人发现。规则：**证据行里的 skipped 条数就是设备电池的缺席**（"跑过了、跳过几条"是误读）；每次收尾都要
>   `VK_ICD_FILENAMES=<lvp_icd.json>` 跑套件并断言 `skipped == 0`（门禁脚本本来就这么要求，前提是人去跑它）。
> · 修法：对齐 **≤ `alignof(std::max_align_t)` 用 `std::malloc`**（"aligned"不等于"更大"）；分支只放 POSIX 半，
>   因为 MSVC 的 `_aligned_malloc` 内存必须由 `_aligned_free` 释放（在 Windows 上返回 `malloc` 内存 = 堆损坏）。
>   守卫 `CoreAllocationGateTest.EveryAlignmentTheLanguageAllowsIsServed`（1..128 逐对齐 + 计数窗口；
>   **检查必须在窗口外** —— gtest 的消息自己会分配，实测给窗口加了 160 B 噪声）。
> · 同一族的**第二处**（只有跑 Release 才露出）：`-O2` 会**省略**对可替换全局分配函数的调用（`[expr.new]` 的省略规则）
>   ⇒ `TheCountedHalfSeesChurnTheHeapReadingCannot` 在 Release 里数到 **0/64**，也就是说这条"正对照"只在 Debug 成立。
>   修法：新 `keepAllocation()`（本文件匿名命名空间的 `noinline` 函数：POSIX `asm volatile("" : : "r"(block) : "memory")`、
>   MSVC `__declspec(noinline)` + volatile 存储）把块地址交给优化器看不穿的代码（**volatile 存储不够**：语言不要求地址值互不相同）。
>   变异：删那一行调用 ⇒ **Release 红（0/64）、Debug 绿（64/64）**。
> · 变异 2/2：删掉那一支 ⇒ 守卫红（exit 1）**且**第一个设备用例 abort（exit -6）；恢复后两处都绿。
> · 现状（2026-09-25，lavapipe + `DISPLAY=:0`）：`test_vsg` **435 passed / 0 skipped / 0 failed**（Debug 与
>   Release 两棵树都是）；两棵树的整道门禁阶段行与画面参数**逐字相同**（0 VUID / 0 SYNC-HAZARD /
>   hygiene `0/793` / 相位 **12** 行 2 次收尾 / 应用 `vuid=0 warnings=0`）。**Release 树别忘了建插件**
>   （`ninja -C build-release gfx_backend_vsg`：没有它三条 `VsgBackendPluginTest` 报空后端，与 M10h 的教训同源）。
>   应用那条 warning（首帧 "no compiled content half"）已由 M11h 收掉；读窗口的偶发红由 M11j 收掉大半
>   （**计数器**那半仍未做，见 M11j 最后一条）。

> 2026-09-17 **V3/V4 收口：把"等上游"从假设变成有门禁的事实（附一个表格形状的坑）**
> 停在"被上游阻塞"上的条目，危险的不是它没做，而是**前提悄悄失效后没人再看它**。
> · **事实（现抓，不是回忆）**：pinned v1.1.16 与**上游 master** 都是 `vkCreateGraphicsPipelines(*device, **VK_NULL_HANDLE**, …)`
> ——`src/vsg/state/GraphicsPipeline.cpp:260`，compute `ComputePipeline.cpp:110`、rt `RayTracingPipeline.cpp:168` 同样；
> vsg 自己的代码里除 Vulkan 头文件（`include/vsg/vk/vulkan.h` 的 typedef）外**不出现** `VkPipelineCache`，也没有
> `PipelineCache` 类型 ⇒ **升到 master 也拿不到**。V4 同理：`src/` 全树无 `vkCmdBeginRendering`，只有
> `vkCmdBeginRenderPass`（`src/vsg/app/RenderGraph.cpp`）。
> · **门禁**：新增 `scripts/check_vsg_upstream_capabilities.py`（7 条断言；无 vsg 树则 SKIP 退 0，照 `check_vsg_window_surface_state.py`
> 的样子）。**变异 4 条全红**：让 vsg 传一个真 cache 句柄 / 在 vsg 头里加 `VkPipelineCache` / 往 `RenderGraph.cpp` 塞
> `vkCmdBeginRendering` / 造一个 `PipelineCache.h`。变异后 vsg 树**逐字节复原**（`diff -q` 验过）。
> · **推上游的最小形状**照 vsg 自己的先例：`Context` 已有 `getOrCreateShaderCompiler()`（`include/vsg/vk/Context.h:88`）
> ⇒ 要一个 `getOrCreatePipelineCache()`（默认 `VK_NULL_HANDLE`，零破坏），`Implementation` 里把那第三实参换成它。
> · **坑**：backlog 里 V3/V4 两行**原本就多带一个空的尾单元格**（9 parts），我按"6 列 = 8 parts"断言，替换后才发现行变成 8 个 `|`。
> ⇒ **表格行替换要在替换后复查 pipe 数**（或者按"目标形状"重建整行），不要只断言替换前。

> 2026-09-17 **P10 结案：缺陷已修且已钉，剩下的 B2 触发条件**未到**（顺手纠正本行一句陈旧话 + 给设计提个醒）**
> 先查"剩余"是什么：本行说`内建路径仍用载体（vsg phong 读 `vine_Color.a`）`，查下去发现这句话**两重陈旧** ——
> vsg 内建 set 这条路径 **2026-09-13 已删**（现在只有引擎自己的 set）；剩下的"白载体"是**静态兜底**，给没写 loc2 色的
> 几何一个合法的 `vine_Color` 属性（乘 albedo 等于没乘），**不承载 opacity**。opacity 今天的唯一来源是每 drawable 的
> `draw.params.x`（渐变片 `alpha = draw.params.x`，顶点色只出 `.rgb`），并被三条**具名**测试钉住：
> `OpacityIsNotPartOfTheVariantIdentity`、`OpacityDoesNotRideTheVertexColour`、`OpacityEditRebuildsNothing`。
> · **B2 按自己的触发条件暂缓**（§12.6："要等第二个标量（材质值或用户参数）出现，否则会把一个 float 包装成一整套 API"），
> 实测条件**未到**：`VineDrawBlock.params` 只有一个活标量（`.x`），`.yzw` 在渐变片里明写 reserved；阴影块的
> `VineShadowBlock` 属于**另一个块**（它自己的 `params.w` 自 2026-09-18 起 = 所属灯的槽位），不算第二个标量。
> · **给将来的 B2 留一句**：§12.6 把"材质值进 `VineDrawBlock.params`"当首个候选消费者，但 `params` 是**每 drawable** 的，
> 材质是**每材质**的（按地址键、跨 drawable 共享、已有 `set0/b0` 材质块）—— 搬进去等于按 drawable 复制材质值，还和材质身份
> 键打架。已在 `.ai/design/graphics-shader.md` §12.6 加一条日期注记。
> · 本次**无代码改动**（缺陷已修、API 按设计暂缓），只改了 backlog 一行 + 设计文档一条注记。

> 2026-09-16 **R5 结案：appfw 插件注册表的 LSan 报告 —— 抑制，但把“为什么是有意的”一并写进去**
> 先查证它到底是不是缺陷：`~DynamicLibraryLoader` 的 `d.release()` **是有意的**（注释写明：若按静态析构序在退出时 dlclose 掉插件代码，而 `CommandManager` / `RenderBackendRegistry` 还持着指进插件的 callable / 工厂指针 ⇒ SIGSEGV）。所以这是**保留决定**，不是忘记释放 —— 而 `asan_leaks.supp` 原来的规则（“框架自己的分配一律不许抑制”）恰好把这种情况也堵死了。
> · 处置：给那份文件加一条**带条件的例外**（“有意保留 **且** 决定写在代码里，可以入表；‘只有几个字节’不是理由”），并新增 `leak:vn::runtime::DynamicLibrary` + 实测数字。
> · 收益：后端两条跑法（`test_vsg` **292** 全绿 + 设备自检 55 行证据全跑完）现在都用**全或无**泄漏判据 PASS —— 比 R2 当时的 `VINE_ASAN_LEAK_SCOPE` **更强**（scope 会把同一轮里别的泄漏一起放过）。scope 开关保留，但降级为“还没 excuse 的泄漏”的备用手段。
> · **证据**：`LSAN_OPTIONS=…:print_suppressions=1` ⇒ `Suppressions used: count 16 bytes 808 template vn::runtime::DynamicLibrary`（**只有**这一类被匹配）。

> 2026-09-14 **着色器内的插值变量前缀 `vn_` → `vine_`**（补上 2026-09-13「着色器内标识符前缀统一 `vine_`」的尾巴）
> - `src/viz/graphics/shaders/` 下 9 个源文件全改：`vn_uv` → `vine_uv`、`vn_view_pos` → `vine_view_pos`、
>   `vn_view_normal` → `vine_view_normal`、`vn_color` → `vine_color`、`vn_texcoord` → `vine_texcoord`、`vn_dir` → `vine_dir`。
>   顶点输入本来就是 `vine_Vertex` / `vine_Normal` / `vine_Color` / `vine_TexCoord0`，现在着色器里的标识符
>   不分输入 / 输出 / 插值，一律 `vine_`。
> - 同步改：**ABI 文档**（`BuiltinShaders::fullscreenVertexProgram`、`ScreenPass::setProgram`、
>   `VsgPipelineFactory` 的用户 program 段）与**钉住成品文本的门禁**（`EmbeddedShadersTest`、
>   `ForwardShaderSetTest`、`OverlayStagesTest`，以及 `GraphicsTest` 里照全屏 ABI 写的宿主 program 样例）。
> - **行为中性**：只换标识符，location / 绑定 / 取值范围 / Y 方向一个不动 ⇒ 画面不变（任何 shading 的 ABI 都没变）；
>   `vine_shader_check.sh` 9 shader × 各 define 组合照旧全编。
> - 教训：**插值名虽然只在顶点/片元两段之间可见，仍是 ABI 的一部分**——文档与单测都会钉住它，改名要一起改；
>   漏改一处不是编译错，而是某个门禁按旧名 find 不到（前者喧哗，后者难查）。

> 2026-09-14 **texcoord 的 kind 显式化：`VINE_TEXCOORD_CUBE`（缺省 = UV）→ `VINE_TEXCOORD_UV` / `VINE_TEXCOORD_CUBE`**（用户口径：“不要隐式，要显式”）
> - **两个轴，别再混**：`VINE_DIFFUSE_MAP` 是**门**（有没有贴图要采）——它由 vsg 的赋值门控置位（`ArrayConfigurator::assignArray`：一赋数组/描述符就置位），
>   并且决定 pipeline layout 里那个属性/描述符**存不存在**（`ShaderSet::createDescriptorSetLayout` 按 `defines.count(binding.define)` 过滤）。**kind** 是“槽是哪一种”，
>   由后端按**数据宽度**手插（三标量 ⇒ CUBE，否则 UV）。一个 binding 只能挂一个 define、`getAttributeBinding` 是首个匹配 ⇒ 门不可能被 kind 名取代。
> - 四个内容 shader（forward / gbuffer 两对）的 kind 链改成 `#if CUBE / #elif UV / #else #error`：**采样了槽却没说 kind 的变体现在编译不过**，
>   不再静默当 UV。
> - 后端 `SceneBridgePipeline`：**每个 variant 一定插恰好一个 kind**；自定义 program 的 opt-in 判据改成“问它被给的那个 kind”（SDK 的 gbuffer 两个名字都声明）。
>   原来只问 `VINE_TEXCOORD_CUBE`，于是“声明 CUBE”的程序在 2-wide 槽上也套用了槽规则——现在按 kind 分别生效。
> - **天空盒例外**：`builtin_skybox.*` 没有门，而 `VsgPipelineFactory` 的**程序级编译是零 define**（`compiledProgramStages`；variant 由 `ShaderSet::getShaderStages(scs)` 按 defines 重编）
>   ⇒ 对它“没有 kind”是真实构建状态，`#error` 会把整个 skybox program 变成 declined。所以它保留“配对”分支，并在注释里点名 `VINE_TEXCOORD_UV`。
> - 为什么不反过来（采样器 → 坐标）：顶点侧宽度只能由**数据**决定（vsg 取数组自己的 format 建顶点输入，Vulkan 要求 `in` 类型与之兼容），
>   不一致时只能让**纹理**让步（白 fallback + 报告）；让数据让步就得更凭空造分量。反方向还要两个 define（采样器 kind + 通道宽度），名字更多。
> - 门禁：`vine_shader_check.sh` 矩阵 = on/off define 组合 × **恰好一个 kind**（每 shader 16 组）；`ForwardShaderSetTest.ASampledTexcoordSlotMustStateItsKind`
>   用 glslang 直接钉（带门无 kind ⇒ 必须失败；UV / CUBE / 零 define ⇒ 必须过）；`ProgramSamplingTest` 钉 per-kind opt-in。
> - 判据：build 0 error（仅 `OverlayStagesTest` 两条**既有**的 `const const` 警告）；`vine_shader_check.sh` PASS（9 shader × 16 组合 + 嵌入字节一致）；
>   test_vsg **250**、test_graphics 259 全过；`gfx_lavapipe_check.sh` PASS —— selftest 证据基线 **55 行逐字节不变** + app 阶段 validation clean（只换分支名，画面没动）。

> 2026-09-13 **shader 文件命名规则：目录里每个文件都是 `builtin_<角色>.<阶段>`**（本条覆盖此前两次取名）
> - 现名：`src/viz/graphics/shaders/` 下 `builtin_forward.{vert,frag}`（原 `std_forward.*`，更早 `vine_forward.*`）、
>   `builtin_gbuffer.{vert,frag}`（原 `gbuffer_geometry.*`）、`builtin_deferred_lighting.frag`（原 `deferred_light.frag`）、
>   `builtin_skybox.{vert,frag}`、`builtin_screen_copy.frag`、`builtin_fullscreen.vert`。
> - 规则：`builtin_` 划分归属（引擎文本 vs 宿主 shader）；角色是**渲染器**的词，不重复阶段或目标（`gbuffer` 不必写 `_geometry`）
>   且不带产品名或 C++ 的词（`std_forward` 读起来是 `std::forward`，弃用）；常量按文件名推导
>   （`kBuiltinForwardVert`）；**program 名 = 文件名去后缀**（`builtin_forward` / `builtin_gbuffer` / `builtin_deferred_lighting`，
>   多 program 加后缀 `_flat` / `_shadowed` / `_<N>`）；**pass 名不带前缀**（`gbuffer` / `deferred_lighting`，
>   因为 pass 画的是宿主可以替换的 program，见 `RenderPipelineBuilder` 的 `gbuffer_program` / `lighting_program`）。
> - 门禁：`test_graphics::EmbeddedShadersTest.EveryShaderNameFollowsTheInventorysRule`（规则 + 反例都测）。
> - 行为中性：证据基线 55 行逐字节不变，`vine_shader_check.sh` 9 shader PASS，test_graphics 258 → **259**。
> - 历史章节沿用当时的名字。

> 2026-09-13 **P0.C1：L1 数据块布局（SDK）+ 命名定案**：L1 块统一命名 **`Vine<Role>Block`**（与既有 `VineLightsBlock` 一致；
> `Block` 明示内存布局、与 `FrameContext`/`RenderCommand` 区分），草稿的 `VineFrame` 改成 **`VineViewBlock`**（per-view 语义，避开 `FrameContext`）。
> `ShaderAbi.hpp` 新增 `VineViewBlock`(288B) / `VineDrawBlock`(80B)（16B 对齐、全 mat4/vec4 ⇒ std140 与 D3D cbuffer 同布局）+ `static_assert`；
> `tests/test_graphics/ShaderAbiTest.cpp` 钉 sizeof/offsetof。口径：行为中性；test_graphics 236 → **239**。下一步 C2（vsg 标注 push ≡ 子集）。

> 2026-09-13 **P0.B1：SDK 显式属性 location 表**：新增 `sdk/vine/graphics/ShaderAbi.hpp`（`VertexAttribute{Position,Normal,Color,TexCoord0}`
> + `attributeLocation()`，值 **0/1/2/8**）；vsg 的 `buildVineShaderSet` / `assembleProgramShaderSet` 用它替代字面量。
> 契约与 DX 映射写在 `.ai/design/graphics-shader.md` §11（L1/L2/L3 + B1..B4 分期）。
> 口径：两条证据基线 47 行不变；test_graphics 235→**236**（+1：表值 ↔ shader 文本声明的 location 一致）；lavapipe PASS。
> 下一步 B2：`ShaderProgram` 参数表 + 命名槽声明。**口径决策见 `graphics-shader.md` §12**：B3 采用选项 C
> （L1 声明式块 + vsg push 作内部优化；C1 SDK 块布局 / C2 标注等价），L2 shim 等第二个后端，**B2 暂缓**（无消费者前不加 `addParam`/`addInputSlot`）。

> 2026-09-13 **P0.A：内建前向着色归 SDK（行为中性）**：`std_forward.{vert,frag}` 从 `gfx_backend_vsg/shaders/` 搬到
> `src/viz/graphics/shaders/`，清单随之移动（嵌入数 graphics 3→5、vsg 4→2）。新增 SDK `BuiltinShaders.hpp/.cpp`：
> `builtinProgram(ShaderPreset)`（**已被 `forwardProgram()` / `flatForwardProgram()` 取代**）+ `gbufferGeometryProgram`/`deferredLightProgram`
> （从 `RenderPipelineBuilder` 搬来，builder 的两个静态工厂改转发，公开 API 不变）。
> 后端 `VsgPipelineFactory::compiledStages(preset)` 取 SDK program 编译（每 preset 缓存一次），`buildVineShaderSet` 不再自带 GLSL。
> 口径：两条证据基线 47 行逐字节不变；`vine_shader_check` PASS（7 shader）；test_graphics 234→**235**、test_vsg 237→**238**；lavapipe PASS。
> 边界：SDK 拥有**着色文本**（L3），后端拥有**编译 + ABI + 管线**（L2）；`ShaderSet` 仍只属 vsg 后端。下一步 P0.B（ABI 契约移入 SDK）。

> 2026-09-13 **着色器文件化 + 构建期嵌入（P12）**：产品 shader 从 C++ 字符串搬进真文件，构建期嵌进二进制；死文件
> `flat.*`（含两个提交进仓库的 `.spv`）删除。清单在**顶层** `cmake/VineShaders.cmake`（生成规则必须在顶层：`tests/test_vsg`
> 直接编译插件源码，要能依赖同一个生成头文件）→ 机制 `cmake/VineShaderHelper.cmake`（`vn_declare_embedded_shaders` /
> `vn_use_embedded_shaders`）→ 生成器 `cmake/VineEmbedShaders.cmake`（`cmake -P`，写 `inline constexpr std::u8string_view`
> + `Entry{name,hash,bytes}` 表）。为什么不用 `file(READ)` + `CMAKE_CONFIGURE_DEPENDS`：那样每次改 shader 都整包
> reconfigure（实测 ~15s），而 `-P` + `add_custom_command` 拿的是 ninja 原生依赖追踪（改 `.glsl` 只重编依赖它的 TU，
> 改生成器本身也会重新生成），且内容没变就不重写头文件（否则 touch 一下 `.glsl` 引发一串重编）。
> 类型口径：`ShaderStage::source` 是 `vn::String`（内部 `std::u8string`）⇒ `String(kX)`；vsg 侧要 `std::string`
> ⇒ `asShaderSource(kX)`（`vine/vsg/support/VsgUtils.hpp`，GLSL 是 ASCII 的逐字节视图）。
> **新门禁** `scripts/vine_shader_check.sh`：每个 shader × 4 种 define 变体（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP`）
> 过 glslangValidator + 嵌入副本的 SHA-256 前缀与字节数必须与磁盘一致 + 每个 `*/shaders/*` 文件必须在清单里。
> 生成器两条**构建期**守卫（都做过 mutation）：源里出现 CR ⇒ 报错（`file(READ)` 会**静默**把 CRLF 归一化成 LF，所以要
> 用 `HEX` 读原始字节判）；源里出现 `)VINE_GLSL"` ⇒ 报错（否则 raw string 提前结束）。环境事实：本机 CMake 4.2.3 的
> `string(SHA256 …)` 恒返回空串 ⇒ 必须用 `file(SHA256 <path>)`；`glslangValidator -V` 不带 `-o` 会把 `vert.spv`/`frag.spv`
> 写进**当前目录**（门禁曾自己污染工作区，已加 `-o <tmp>`）。
> 判据：迁移**行为中性** —— selftest 证据 47 行逐字节相同、lavapipe 0 VUID、test_graphics 230 → **234**、test_vsg 220 → **224**、
> 诊断格式 0 suspicious；四条 mutation（非法 GLSL / 只改不重建 / 未入清单 / hash 截断）各自恰好目标门禁红。
> 设计与"加一个 shader 的步骤"见 `.ai/design/vsg-custom-shader.md` §10。

## 后端契约（规范性在 `RenderBackend.hpp` 类注释；预算见设计文档 §14）
- **调用序**：`beginFrame` → 每启用 pass（order 升序）`beginPass`→`setPassOrder`→可选逐
  pass 状态→绘制→`endPass` → `endFrame` → `swapBuffers`（**唯一 present 点**）。首帧前有
  warm-up（启用且非清屏的 pass 先各跑一遍），所以"首帧前创建的东西"也要能摆对位置。
- **借用**：camera / commands / lights / target / program 只在当次调用内有效（commands 是本
  帧临时量），`beginPass` 的 pass 只在作用域内有效；**不得保留**，需要就拷贝/上传。
  * `releasePass()` / `releaseRenderTarget()` **同时宣布宿主可以立刻销毁该对象** ⇒ 队列里的“宣布”（
    `request.pass` / `request.target`）必须一起作废：直接驱动路径的请求**跨帧存活**，留着就是悬垂指针；
    接下来那一次画不了 ⇒ **拒画 + 分集报一次**（`PassProtocolViolation`，消息带哪个调用点被跳过），
    **不得静默改画到窗口**。同理：0 尺寸的 target 建不了 ⇒ `TargetBuildFailed` 分集报一次，
    不静默什么都不画。
- **保留**：保留 GPU 状态必须按被服务对象寿命定键、由对应 `release*` 释放、**不随帧数增长**；
  宿主不必为正确性调 `release*`。
- **线程**：串行、不可重入、无需加锁；不得假设跨 `initialize`/`shutdown` 同线程；**诊断 sink
  是同步回调**，只能记录返回，不得回调后端。
- **声明**：输出的三种声明（`setRenderTarget` 画到哪 / `setOutput`·`setOutputTarget` 对象声明（校验 + 对象侧解析）/ `setOutputName` 查表键）**互不覆盖**，一个错误只报一次；**发不出去的必须报** —— 名字表只能交付可采样 target，所以"渲染进窗口却声明发布名"和 `publish(name, nullptr)` 都**分集上报**（过去是静默丢弃，消费者随后被告知"本帧没人产出它"，指向错的一方）。
- **pass 的前置条件（接线期一次性检查）**：`ScreenPass` 必须有输入（否则永远不画）、只声明深度的 texture 路径会被**静默**换成彩色 0、**有 program 就必须有相机**（program 路径要先建 view）——这三条都在 `validateWiring()` 报一次（分集），因为 host 侧看到的是"这个 pass 从来不出现"。
- **声明的 `ImageRef` 必须 `bind` 到一张 target**：未绑定的身份**没有地址** ⇒ 消费者解析不到、承诺校验也无从比较（旧注释写着"接线期已报"其实没报）⇒ 整根线静默。现按**每张图**报一次（分集），且**已被冲突报过的不再报**（一个错误一条消息）。
- **失败**：接口内不抛异常；`false` ≠ 部分生效；**`initialize()` 返回 false 必须自己收拾残局**
  （引擎只在 initialize 成功后才调 `shutdown()`，见 `RenderEngine::shutdown`）。
- **保留预算**（本后端数字）：退役环 4（提交帧）、几何条目按“外侧是否仍持有”回收（无时间窗，2026-09-14 删）、材质 256 条、
  变体/ShaderSet 超限整表清空、pass/target 槽靠 `releasePass`/`releaseRenderTarget`。

> 2026-09-11 **后端诊断通道（失败不再静默）**：新增 `vine/graphics/RenderDiagnostic.hpp`
> （`DiagnosticSeverity` / `DiagnosticCategory`（枚举、按后果分类）/ `RenderDiagnostic` /
> `DiagnosticSink`）+ `RenderBackend::setDiagnosticSink/diagnosticSink/diagnosticCount(category)`
> 与 `protected reportDiagnostic`（默认实现齐全 → 现有后端零改动）；`RenderEngine::setDiagnosticSink`
> 是宿主入口（引擎保存并应用到当前与之后的后端）。**vsg 后端：`VsgRenderer::reportFailure` 是唯一上报
> 权威**（stderr 追踪 + 计数器 + 宿主 sink），槽内 `SceneBridge` 经 `installDiagnosticRoute` 把发现
> 转发给它（保证 `diagnosticCount()` 诚实、无双份追踪）；自由函数 helper 改为**返回失败原因**由调用方
> 上报。已接线：几何拒绝 / 通道丢弃 / 着色回退（原 D9 全静默）/ 编译失败 / 离屏 target 失败 / 内容跳过 /
> 初始化失败；上报频次按 **revision**（不刷屏）。宿主侧 `RenderControl` 把诊断写进 `vine/logging`。
> 验证：test_graphics 151 / test_vsg 58 / `vsg_backend_selftest::runDiagnosticsPhase()`（真实设备上
> 1 条 GeometryRejected + 1 条 ShaderFallback 到达 sink）/ lavapipe 门禁 PASS。详见设计文档 §10。

> 2026-09-07 **Design C（RenderEngine 瘦身 + SceneView）**：相机/导航/内容都不再放 engine，引擎
> 纯调度器（零内容零相机）。新增 graphics 概念 `SceneView`（宿主无关，组合**借用** engine）：owns
> Camera + 内容 Scene（`scene()` 返回 owning `intrusive_ptr<Scene>`）+ 默认 OrbitCameraManipulator
> （懒创建）+ 尺寸/aspect 维护；内容一律 per-pass 显式绑定（`addPass(pass, content, order)`）。
> `ensureWindowPass()` 注册 order-0 窗口 pass（3 参显式绑 view scene）除非 engine 已有「携带该
> camera 且 RT==null」的 pass。RenderEngine 删除 `setMasterCamera/masterCamera`、
> `setCameraManipulator/cameraManipulator`、mouse/scroll/key 三路 `pushEvent`、Resize 里的相机
> aspect 维护，**以及 `scene_/setScene/scene()`**（默认内容）；`hasWindowPass()` 无参
> →`hasWindowPass(raw_ptr<Camera>)`（结构化查询）。RenderControl 持 engine+view，输入/尺寸走 view；
> AppShell/test_plugin 改用 `render_control->view()->camera()/scene()`，内容 pass 显式绑
> `view->scene()`（gbuf/deferred/multislot/builder.setContent）；RenderPipelineBuilder 需显式
> setCamera/setContent。CameraManipulator 基类新增虚 fitToScreen()/home()（Orbit override）。
> test_graphics 121/121（SceneViewTest×4），全量构建绿。详见 /memories/repo/vine-sceneview-engine.md
> 与本文件下方旧 Design B 条目（已过时）。**新 .cpp 进构建需重跑 cmake configure**。
> 2026-09-07 布局最终态：engine `resize(w,h)`（由 `pushEvent(ResizeEvent)` 改名，唯一 resize
> 入口；RenderControl 一行 `engine->resize(w,sh)` 紧接 `view->onSurfaceResized(w,sh)`）只做
> swapchain+frame_ctx；RenderTarget 只有 setSize；新增 `Viewport` 值对象由 RenderPass 持有（未设=
> 全幅）；resize 布局走 SceneView::addSurfaceLayout 回调注册（创建方显式，每 effect 自己
> setSize/setViewport）。test_graphics 124/124。
>
> 2026-09-08 **统一主窗管线 preset**：`RenderPipelineBuilder::build(PipelinePreset, PipelineOptions)`
> → `intrusive_ptr<Pipeline>`（新 `Pipeline.hpp/.cpp`，RefCounted）。`Forward`=order0 窗口场景 pass；
> `Deferred`=order-3 gbuffer(MRT albedo/normal+shininess/spec/view-pos+D24, 发布"GBuffer")+order0 全屏延迟
> 光照 ScreenPass（带 camera+绑 content 转发灯光）为窗口 pass；阴影变体 ForwardShadowed/DeferredShadowed
> =占位（暂同无阴影）。Deferred **默认自带临时 shader**（builder 公共静态 `defaultGbufferGeometryProgram()`/
> `defaultDeferredLightProgram()`）与 canonical G-buffer（`defaultGbufferTarget(w,h)`），调用方零 GLSL；options 的
> gbuffer/lighting program 为可选覆盖；缺 camera/content 才返回 null。**SceneView::ensureWindowPass
> 默认 viewer 也改走 build(Forward)**（同一套 recipe），不再手搓 pass；行为不变。test_graphics 128/128。
>
> 2026-09-07 **Scene 收敛为单根**：`Scene` 只持一个根 `Node`（空场景 `root()==nullptr`，不渲染）。
> 删除 `addNode/removeNode/nodes()`；新增 `setRoot(intrusive_ptr<Node>)/root()`；`clear()` 释根
> （灯不受影响，仍用 `clearLights()`）。`findNode/boundingBox/collectRenderCommands` 自单根遍历。
> 消费迁移：`addBox`/demo 改收 `Group*`（单恒等根 Group + `addChild`，删 removeNode 再包 StateNode
> 的写法改为直接 state 包 box 后 addChild 根）；AxisGizmo 3 根并入一个根 Group；RayIntersection
> 两遍历器改单根；test_graphics 增 `setIdentityRoot(Scene&)` helper、SceneTest 改 RootSetClearAndFind/
> RootlessSceneIsEmpty。世界矩阵/bbox/剔除/透明度/状态折叠语义不变（根 Group 恒等）——纯 API 形态
> 收敛，与 osg/vsg "一个 scene = 一棵根子树" 对齐（graphics-scene-graph.md §5）。
> 旧多 root 表述仍留在 graphics-design.md §3.4/§7（已标注过时）。
>
> 2026-09-04 **Material 透明度已移除 + 内建 ShaderSet 契约已归档**：透明只属
> scene/node(叶)/geometry 的 opacity（per-vertex alpha 通道 = vine_Color.a@loc6），
> **Material 纯颜色**。删除 `Material::opacity()/setOpacity()`+`opacity_`（doc 注明
> diffuse alpha 恒 1 忽略）；`RenderCommand` ctor 不再用材质 opacity 播种（Scene
> 收集器重算）；Scene 有效透明度 = clamp(node_opacity)（叶 Geometry 自身即 node，
> 材质项移除）；vsg `VsgRenderer` no-cull 收集器去 `material_opacity` 项；材质默认
> 灰 Phong 在 `VsgMaterialManager` 里 force `diffuse.a=1`。测试改用叶/节点透明度：
> `CollectCommandsSortsTransparentBackToFrontAfterOpaque` 用 MatrixTransform
> setOpacity(0.5)，`CollectCommandsOpacityIncludesMaterial`→改名 `…LeafAndNode`
> (叶 0.5×祖先 0.5=0.25)；MaterialTest 删 opacity 断言。**test_graphics 113/113、
> test_vsg 10/10、全量构建、lavapipe 回归全绿**。材质颜色类型保持 `Colorf`(vec4)
> 不改 vec3（全 SDK 统一 + vsg PhongMaterial/UBO 本就 vec4；opaque 约定放渲染映射
> 层 force alpha=1）。内建 ShaderSet 输入契约（attribute loc/format/define +
> descriptor set/binding + pc 128B + "按名查找+define 变体"机制，flat/phong/pbr 共用
> 一张表）已核对进 `.ai/design/vsg-custom-shader.md` §9（供过渡期与 P0 自写对照）。
>
> 2026-09-03 program 槽 P1 · glslang 运行期编译验证：`tests/test_vsg/GlslCompileTest.cpp`
> （+2）——`vsg::ShaderCompiler::supported()==true`，SDK `ShaderProgram` 的 VS/FS GLSL 逐 stage 经
> ShaderCompiler 成功编出 SPIR-V（module->code 非空，72ms，纯 CPU 无需 device）。test_vsg 10/10。
> 这验证了 program 槽后端装配的"编译半环"；"装配半环"（按用户 Program 建 ShaderSet（stages/
> attributeBindings/descriptorBindings）+ SceneBridge buildGeometry 泛化 + Item 重建键含 program +
> lavapipe/真机像素验证）仍待做——注意 vsg 的 phong ShaderSet 是序列化 blob（shaders/phong_ShaderSet.cpp
> 为 io.read_cast 数据），自定义需手搭 addDescriptorBinding(…, coordinateSpace) 等，视图矩阵经
> ViewDependentState 绑定，需 GPU/像素验证，勿盲改。
>
> 2026-09-03 program 槽 P1 第二步（解析链 + 命令携带）落地：`StateNode` 增
> `setProgram/clearProgram/program()`（子树级着色覆盖，独立于 render-state）；新增自由函数
> `effectiveProgram(node)`（叶子 Geometry program 优先 → 最近祖先 StateNode program → null=默认）；
> `Scene::collectRenderCommands` 每叶把 `cmd.program` 折好；`RenderCommand` 增 `ShaderProgramPtr
> program`。test_graphics **113 tests 全绿**（+4），全工程构建过。遗留：vsg 后端按 cmd.program
> 建 ShaderSet 装配（ShaderCompiler 可用）+ Item 重建键含 program。
>
> 2026-09-03 program 槽 P1 第一步（SDK 类型 + 挂点）落地：`ShaderProgram.hpp/.cpp` 新增
> `ShaderStageType{Vertex/Fragment/Compute}` + `ShaderStage{type,source,entryPoint="main"}`
> + `ShaderProgram : Object+RefCounted`（name/addStage/stageCount/stage/stages）；`Geometry` 增
> `program()/setProgram()`（null=引擎默认，零回归）。test_graphics **109 tests 全绿**（+2），
> 全工程构建过。遗留：StateNode 级 program + 解析链、RenderCommand 携带、vsg 后端按用户 Program
> 建 ShaderSet 装配（ShaderCompiler 已可用）。
>
> 2026-09-03 **glslang 已集成**（vsg 运行期 GLSL→SPIR-V 可用）：
> - gfx_backend_vsg 在 VINE_USE_FETCHCONTENT 分支**改源码构建 vsg**（FetchContent v1.1.16，链系统
>   glslang-dev 16.2）→ `VSG_SUPPORTS_ShaderCompiler 1`（build/_deps/vsg-build/include/.../Version.h），
>   ShaderCompiler.cpp 已编译；无 glslang 的本地 `/opt/opensrc/VSG` 仅 VINE_USE_FETCHCONTENT=OFF 用。
> - 依赖注记：本 build 的 `FETCHCONTENT_FULLY_DISCONNECTED` 曾缓存 ON（网络禁用、依赖预取）——
>   vsg 首次拉取须 `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF -DFETCHCONTENT_UPDATES_DISCONNECTED=ON`；
>   vsg 拉取后与其余依赖一样进 _deps。graphics-shader.md 顶部 ⚠ 已把"仅离线"决策更新为
>   "运行期可用（默认）+ 离线/SPIR-V 兜底"。
> - 验证：test_vsg 8/8、test_graphics 107/107、lavapipe 回归脚本 PASS（新 vsg）。
>
> 2026-09-03 后端 renderState 消费（vsg）落地：`include/vine/vsg/support/RenderStateMapper.hpp`（纯、device-free）
> `makeRenderStateObjects(resolved)->{DepthStencil,Rasterization,ColorBlend,InputAssembly}` +
> `applyRenderStateObjects(config,…)`；SceneBridge::buildGeometry 按 cmd.renderState 装配、Item 增
> render_state（状态变→重建）；tests/test_vsg/RenderStateMapperTest 6 用例全绿（直跑 8/8，全工程构建过）。
> **两个历史决策落地**：①深度——vsg 投影是 reverse-Z(1→0)，SDK CompareOp 为"closer/farther"距离语义
> ⇒ 边界反转映射（Less→VK GREATER…），默认=vsg GREATER=现状零回归；②Blend——per-vertex-alpha 使 alpha
> blend 必须常开，blend.enabled 只是"是否用自定义因子"，vsg 后端无法经 StateNode 关闭 alpha blend。
> 映射：cull→cullMode(CCW front,默认 NONE)、polygon→polygonMode、topology→InputAssembly.topology。
> 遗留：cull winding/blend 因子真机视觉验证（无 GPU）。test_vsg 现 8/8（+6 映射）。
>
> 2026-09-03 场景图 R1 补强：新增 `MatrixTransformTest`（5）与 `GroupTest`（1）+ Scene 嵌套 world
> modelMatrix + GeometryTest 拓扑分离用例，**test_graphics 107 tests 全绿**（新增 8）。
> `vertexCount` = 纯数据统计，不随 StateNode Topology 变化（有测试钉住）；`triangleCount` 已于
> 2026-09-04 移除（Geometry 数据面不保证是三角网格；geometry 模块 `Mesh::triangleCount()` 保留）。
>
> 2026-09-03 场景图 R1 核心落地（test_graphics 99 绿、全工程构建过、test_vsg 直跑 2/2 绿）：
> **`Node` 拆成基类**（name/visible/opacity/parent()/虚 boundingBox()/worldMatrix()；无 children/
> 无变换）；**`Group`** 承接 addChild/removeChild/children()/boundingBox=children 并集；
> **`MatrixTransform : Group`** 为唯一变换源（matrix/setMatrix，经 protected 虚 localTransformMatrix()
> 参与 Node::worldMatrix() 父链累积）；`StateNode : Group` 不变。**`Geometry : Node` 叶子**：
> material 并入（name/visible/opacity 继承自 Node），boundingBox = loc0 本地盒 × worldMatrix()。
> **`Drawable.hpp/.cpp` 已删**；`RenderCommand.drawable`→`RenderCommand.geometry`(GeometryPtr)。
> Scene::collect/findNode、RayIntersection 三遍历器、AxisGizmo、SceneBridge、VsgRenderer no-cull
> walker、app_shell::addBox、TestRenderLiveCommand、GraphicsTest 全部迁移；`makeTriangleNode` 现返
> MatrixTransform(子=Geometry)。bbox 语义=**世界空间**（叶子世界盒；Group/MT=子世界盒并集）。
> 关键易错：`Mat4d()` 默认=identity（无 ::identity()）；Mat4d×Point3d 需 include math/Transform3.hpp；
> intrusive_ptr 析构需完整类型→RenderCommand.hpp 直接 include Geometry.hpp；加/删 .cpp 要重跑 cmake。
> 基线无关失败：test_cppstd/test_runtime/test_system 与本重构无关；test_vsg 的 ctest SegFault 为既有
> 退出时序问题（直跑干净）。
> 遗留：program 槽、renderState 后端消费、MatrixTransform 测试用例补强（现靠 Scene/Node 用例覆盖）。
>
> 2026-09-03 拓扑归位修正：`Geometry` 移除 PrimitiveType（曾误放），改为 **渲染状态项 `Topology`**
> （默认 Triangles；StateNode set/clear；与 PolygonMode 分清：点云=Topology::Points，线框=
> PolygonMode::Line）——同一数据可换状态变三角/点/线框，不换几何（vsg/Vulkan 拓扑属管线）。
>
> 2026-09-03 State 切片已落地：`StateNode` + 状态项（Depth/Cull/Blend/PolygonMode/**Topology**）
> + 折叠函数（collect/resolve/effectiveRenderState）；`RenderCommand.renderState` 已由 collect 折叠
> 填入（Scene 集成 3 用例全绿）。未接后端变体键（下一步）。
>
## 边界框类型（Aabbd）

- `Scene/Node/Drawable/Geometry::boundingBox()` / `computeBoundingBox()` 返回
  `vn::math::Aabbd`（`Rect3<double>` 别名，见 `vine/math/Rect3.hpp`）。
  原来的 `vn::graphics::BoundingBox` 已移除。
- **语义**：`Rect3` 默认构造为零点在原点的合法零盒；累积式构建必须用
  `Aabbd::empty()`（反转哨兵）起步，再用 `expandBy(Point3/Vector3/Rect3)`。
  空盒 `isEmpty()==true`、`isValid()==false`。
- **注意**：`Rect3.hpp` 只前向声明 `Point3/Vector3`，`min()/max()/center()/size()`
  的调用方需自行 include `vine/math/Point3.hpp`/`Vector3.hpp`；
  `vn::math::Point3d` 别名仅定义于 `Point3.hpp`。

## 模块文档（面向使用者）

`docs/guides/graphics.md`（2026-10-11 起面向使用者的文档统一在 `docs/guides/`；此前是各模块自带的 `docs/` 目录）：分层架构、属性沿树折叠的四条规则、每帧数据流与"顶点被别名而非复制"、生命周期与变更契约（**数据变更一律 `Geometry::setRevision()` 公告**）、最小宿主用法，以及现成例子的**构建与运行方式**（`VINE_PIPELINE=forward|deferred|forward_shadowed|deferred_shadowed`（默认 deferred）、`VINE_VSG_GBUFFER` / `VINE_VSG_DEFERRED` / `VINE_VSG_OFFSCREEN_MULTISLOT` / `VINE_VSG_SLOT_DEMO` / `VINE_SHADER_PRESET`、无头门禁）。
文档如实标注 **forward_shadowed / deferred_shadowed 目前是占位（等同无阴影预设）**：阴影切片（order<0 深度 pass + 阴影光照）未实现，只有 `Light::castShadow()/shadow()`、`ShadowSettings`、`ShadowFilter` 已就位，计划见 `.ai/design/graphics-shadow.md`。
**2026-09-13**：占位不再默不作声——`RenderPipelineBuilder::build` 会报一条 `DiagnosticCategory::UnsupportedRequest`；`ShaderPreset`（含 `ShadowedPhong` 那个“保留项”）已删除，内容着色只能显式命名 program（见本文件顶端条目）。
- **着色模式（2026-10-11）**：`SceneView::Options::Mode{Lit, Flat, Unlit, Normals, Wireframe}` 落在**pass 属性**上
  —— 前四种是**画面那条 pass** 的 **program override**（`RenderPass::setProgramOverride`），第五种再多一个**光栅化覆盖**
  （`RenderPass::setPolygonModeOverride(PolygonMode::Line)`）⇒ 同一条链条上换模式**零重建**（同一条管线、同一张 pass 表）。
  四种着色模式是**同一段** `builtin_forward.frag` 文本 + 一个 `#define`：`VINE_FLAT`（已有的逐面法线）/
  `VINE_UNLIT`（只写 albedo × opacity，光照前 return）/ `VINE_NORMALS`（写 `n*0.5+0.5`，alpha 1）；线框用无光照
  程序 + 线填充（wireframe 的惯例：看结构不看明暗）。
  **模式作用在几何阶段（M1，2026-10-11 已落地）**：延迟配方的画面是"全屏光照"那条 pass 产出的（没有几何可着色），
  所以非 Lit 档期间 `SceneView` 把链条按**前向**建（`Options::path` 字段不动 —— 请求归请求、生效归生效；`effectivePath()`），
  于是四档在两条配方上都能画出来。旧行为（延迟路径报一条 `UnsupportedRequest` 并保持 Lit）与那条诊断一起删除。
  代价：延迟配方**进出**非 Lit 档是一次重建（链条变了），链条内换档仍零重建；判据
  `test_graphics` 的 `SceneViewViewportTest.ANonLitModeOnADeferredRecipeBuildsTheForwardChain` +
  `SwitchingBetweenNonLitModesOnADeferredRecipeKeepsTheChain`（变异：让 `effectivePath()` 恒等于配方 ⇒ 第一条失败）。
  门禁：`test_vsg/GlslCompileTest.EveryBuiltinForwardProgramCompilesToSpirv` **真的用 glslang 编这四个程序**
  （define 里写坏的 GLSL 会被后端当成"这个预设没有 SDK 程序"而静默退化——flat 一开始就是这么错的），
  `ProgramAbiTest.EveryModeOfTheForwardProgramDeclaresTheSameAbi` 钉住四者绑定一致（内容集是按 program 绑的）。
- **pass 级光栅化覆盖（2026-10-11）**：`PolygonMode{ Fill, Line, Point }` 从 `StateNode.hpp` 抽到
  **`PolygonMode.hpp`**（两个作用域共用同一个类型：逐子树的 `StateNode::setPolygonMode` 与逐 pass 的
  `RenderPass::setPolygonModeOverride`/`clearPolygonModeOverride`/`polygonModeOverride()`）。`std::optional` 表达"没有覆盖"≠"强制 Fill"：
  清掉覆盖 = 回到内容自己的状态。应用点就在 `RenderPass::execute()` 里那次"需要自己的命令表"的拷贝（与
  program override 同一处，一次拷贝满足两者）。**零后端改动**：后端早就把多边形模式当 EDS3 **动态状态**
  （`VK_DYNAMIC_STATE_POLYGON_MODE_EXT` + `vkCmdSetPolygonModeEXT`，`fillModeNonSolid` 本来就在 `DeviceFeatures` 里请求）。
  **命名/优先级规则**（`graphics-state.md` §4.1）：同一个状态两处出现时，pass 级里**赢了内容**的名字带 `Override`
  （`setProgramOverride` / `setPolygonModeOverride`），pass 级里**会被内容压过**的不带（`setDepthMode`，靠
  `RenderCommand::depthExplicit`）。线框必须是覆盖：否则内容自己写过的 Line 会把"回实面"这个开关永久钉住。
- **「谁呈现这块表面」有了 owner（M2a，2026-10-11 已落地）**：新类 `vn::graphics::RenderSurface`
  （`sdk/vine/graphics/RenderSurface.hpp`，`RefCounted`，**借用**引擎 ⇒ 必须晚于引擎析构）持一块表面的
  **呈现表**：`present(SceneView&)`（顺带 `ensureWindowPass()`）/ `present(raw_ptr<RenderPass>)` / `dismiss` /
  `hasPresentation()` / `presentations()` —— 后者返回 `Presentation{pass, view}`，按**引擎执行序**排序
  （`std::stable_sort`，同序保持声明序；引擎不认识的 pass 落选，所以答案描述的是"现在这一帧谁在画"）。
  视图那条呈现**按需解析** `view->pipeline()->windowPass()`（每次读），所以视图重建后表自动跟着走
  （钉子 `RenderSurfaceTest.TheTableFollowsARebuiltPipeline`）。
  **同时删掉 `RenderEngine::hasWindowPass(camera)`**：那条"任何携带这台相机、RT==null 的 pass 都算有人占了"
  的全局查询是 M2a 要消掉的推断本身（顺序敏感，且会静默跳过共用一台相机的第二个视图）；`RenderEngine` 新增
  `passOrder(raw_ptr<RenderPass>) const -> std::optional<int>`（顺序住在引擎的槽里，这条查询同时是"这条 pass
  还在不在"）。`SceneView::ensureWindowPass()` 从此不问引擎任何事。
  一块表面**恰好一口清屏**、矩形归呈现条目都还是设计（`graphics-render-next.md` §3.2 的 M2b/M2d）——
  **M2b 见下条，M2c 见"aspect 出相机"那条**。
- **清屏归表面（M2b，2026-10-11 已落地）**：`RenderSurface` 决定**谁**清 —— 呈现表（按引擎执行序）**第一条**保留
  自己的清屏，其余条 `setClearEnabled(false)`（`applyClear()`，私有）。从前是"谁先公告谁赢 + 后端只应用第一条公告"，
  于是第二条的 `clearColor()` 与画面**不符**却没人被告知。**没有** `Surface::setClear(ClearOptions)`：那三样
  （enabled/color/depth）已经住在 `RenderPass` 上且是**一份真相**（`announceClear()` 从那儿读），搬一份上去就是
  "同一个状态两处"+ 三态优先级；表面只搬**决定**，值留在 pass（延迟配方那条 `present` 屏 pass 自己说不清 ⇒
  照样不清，不会被"第一条"逼出一次多余清屏）。
  每条呈现自己的"ask"（清不清）被记住（`Entry::asked_of` + `Entry::clears`），所以①第一条被 `dismiss` ⇒ 下一条
  恢复自己的 ask；②视图**重建**（`setOptions` 换了管线）⇒ 新窗口 pass 又是 enabled，靠视图回调
  `surface_->applyClear()`（`SceneView` ↔ `RenderSurface` 互为 `friend`）把决定落回去；③同一个 pass 被两条呈现
  指到（视图那条 + 直接声明那条）只算一次决定。
  **生命周期**：视图与表面互持 `raw_ptr`，两边都自己解绑 —— `~SceneView` 先 `surface_->dismiss(this)`（此时它的
  `default_pipeline_` 还握着引擎，`dismiss` 里要读引擎的执行序）；`~RenderSurface` 把每个视图的 `surface_` 清空。
  任一先死都不留悬垂（用例 `APresentedViewThatDiesLeavesTheSurface` / `AViewThatOutlivesItsSurfaceDoesNotCallIntoIt`）。
  全部判据在 `tests/test_graphics/RenderSurfaceTest.cpp`（变异验证：去掉 `applyClear()` 调用 ⇒ 4 条失败；
  去掉 `entry.clears` / 去掉视图重建回调 ⇒ 各 1 条失败；去掉 `~SceneView` 的自解绑 ⇒ 1 条失败）。
- **aspect 出相机（M2c，2026-10-11 已落地；⛔ 同日已回退 —— 见下面 2026-10-11 A 那条，本节只作历史记录）**：`Camera` 只持**观看**（eye/up/target）+ **投影参数**（fov / near / far /
  类型 / 正交盒），**不持 aspect、也不缓存投影矩阵** —— `setPerspective(fovy, near, far)`
  （少一个形参）、`projectionMatrix(double aspect)` 现算（正交**忽略**该参数：盒自带比例）、`aspectRatio()` 删除、
  `screenToWorldRay(screenPos, aspect)` 多收一个（**提案漏算的第五个读者**）。为什么：view matrix 只依赖眼睛、
  可跨矩形复用，projection 依赖 fov **和 aspect**、不可 —— 差别就在 aspect，于是"矩形变了投影没跟上"这一整类
  静默错（`.ai/bugs/vsg-resize-distortion.md` 记的那次）在结构上消失，同一台相机可进两块不同形状的矩形
  （镜像 / PiP / 立体 / 编辑器两个窗格）。
  **唯一一处推导**：新契约 `RenderBackend::viewAspect() const`（纯虚）—— pass 的 viewport 优先，否则它画进的
  目标尺寸，再否则表面尺寸，什么都没有 ⇒ 1（"还没有画面"，不是猜一个）；vsg 后端 `beginPass` 清掉上一条 pass 的
  矩形，`setRenderTarget`/`setViewport` 填它，`render()` 前 `recorder.setViewAspect(viewAspect())`。
  **两类读者读同一个数**：①画面 —— `RenderPass::execute()` 在收集命令**之前**问一次（后端刚被告知这条 pass 的
  目标与矩形），转给 `Scene::collectRenderCommands*(camera, aspect)`；②view block —— `FrameRecorder::setViewAspect()`
  （scope 属性，像清屏与深度模式；`beginPass` 复位成 1.0；非正 / 非有限拒收）让 `snapshotCamera()` 用
  `camera->projectionMatrix(pass_aspect_)`。剔除的视锥与画面的投影因此是**同一个矩形**推出来的。
  `Scene` 的帧内 memo 以 **view-projection** 为键 ⇒ 同一台相机两种形状 = 两条 memo 项、两次走树、互不冒充。
  `OrbitCameraManipulator` 自持 `viewportAspect()`（`onResize` 不再往相机里塞 aspect）、`SceneView::applySurfaceGeometry`
  里那段"推 aspect 进相机"删除、`FpsOverlay`/`AxisGizmo` 不再设（它们的 HUD pass 的 viewport 就是形状）、
  方向光阴影照旧 `projectionMatrix(1.0)`（正交）。
  判据（`test_graphics` 330 绿、`test_vsg` 405 绿、其余套件计数不变）：`CameraTest.TheProjectionIsBuiltForTheShapeTheCallerNames`、
  `SceneTest.OneCameraInTwoRectanglesIsTwoPicturesAndTwoWalks`、`RenderEngineTest.EachPassCullsWithTheShapeOfItsOwnRectangle`
  （同一台相机：1600×600 的那条 pass 画出画面外侧那颗、600×600 的剔掉它）、
  `FrameRecorderTest.TheViewBlockIsProjectedForTheShapeTheScopeAnnounced`（公告 2.0 ⇒ 快照投影按 2.0；下一条 pass
  回到默认；0 / NaN 被拒）。变异验证：`execute()` 写死 1.0 ⇒ 1 条失败；memo 去掉 view-proj 项 ⇒ 1 条失败；
  `projectionMatrix` 忽略参数 ⇒ 2 条失败；recorder 固定 1.0 / `beginPass` 不复位 ⇒ 各 1 条失败。
  **回退后的现状**：`Camera::setAspect/aspectRatio/projectionMatrix()/screenToWorldRay(pos)`、
  `collectRenderCommands*(camera)`、`RenderBackend` **没有** `viewAspect()`、`FrameRecorder` **没有** `setViewAspect`、
  `SceneView` 把窗格形状 push 给相机、HUD 相机自己声明形状、矩形与相机不一致由
  `RenderEngine::collectPictureShapeMismatches()` 报出来（详见上面 2026-10-11 A 那条）。
  本节以下提到的用例名 `TheProjectionIsBuiltForTheShapeTheCallerNames` /
  `EachPassCullsWithTheShapeOfItsOwnRectangle` / `TheViewBlockIsProjectedForTheShapeTheScopeAnnounced` 已在 A 里改名或反转
  （`TheProjectionIsBuiltForTheCamerasOwnShape` / `OneCameraCullsWithItsOwnShapeInEveryRectangle` /
  `TheViewBlockIsProjectedForTheCamerasOwnShape`）。
- **状态解析单点（M3，2026-10-11 已落地）**：**一个函数**是优先级表的唯一实现 ——
  `ResolvedCommandState resolveRenderState(const RenderState& authored, const PassRenderState& pass)`
  （`StateNode.hpp`）→ `{state, origin}`，`origin` 逐项回答"这一项是谁给的"（`StateOrigin::Default/Content/Pass`）。
  命令侧只带**内容作者写的那份**（`RenderCommand::authoredState`，每项 optional；`renderState` 的解析值 +
  `depthExplicit` 旗标都删了），pass 侧只带**它自己那份**（`RenderPass::stateOverrides()` →
  `RenderBackend::setPassState()`）。**合并规则**：光栅化覆盖（pass）> 内容 > 默认；**depth 不对称** —— pass 给
  test/write **策略**（`DepthMode` 没有 compare），内容写过就整项归内容（compare 一起），compare 否则跟
  内容/默认走。`program` **不在状态里**（换程序是"用另一套程序画这份内容"，引擎仍旧逐命令改写 ⇒ 后端
  "每条命令一个管线键"的口径不动）；光栅化覆盖**不再改写命令**（跟着 pass 状态走、按命令解析，于是"只切线框"
  的 pass 一次拷贝都不付）。后端侧：`resolveDynamicState(state)` 只剩纯映射，
  `CollectedPass::state` / `CollectedCommand::authoredState` 记的就是这两份真相。
  **行为不变的证据**：既有用例**一条没改语义**就跑绿（`test_graphics` 339、`test_vsg` 405、`test_gui` 253…），
  含 M1 那三条光栅化覆盖用例与 `FrameCompilerTest.ThePassDepthAppliesOnlyWhereTheContentAuthoredNone`；
  新增 `RenderStateTest.ThePriorityTableIsOnePlaceAndSaysWhoDecidedEachItem`（五项逐一 + depth 不对称 + 空内容
  ⇒ 全 default 且 origin 说 default）、`RenderEngineTest.APassAnnouncesItsOwnStateForTheContentItDraws`
  （公告 == `stateOverrides()`，且命令**没**被改写）。变异验证：不公告 pass 状态 ⇒ 2 条失败；解析忽略光栅化
  覆盖 ⇒ 2 条失败；pass 深度反过来压过内容 ⇒ 2 条失败。
- **输入按矩形走 + 焦点视图（M5a，2026-10-11 已落地）**：分屏的两半都落在这张**表面**的表上 ——
  * **焦点**：`RenderSurface::focus(view)` / `focusedView()`。它和矩形、清屏住在一起，因为三件事问的是同一个问题
    （"这块表面被分成什么、谁在画、用户在哪儿"）。默认（没人按过）= **第一条呈现**，所以一个表面永远答得出"当前
    视图"；`focus` 只接受**确实呈现这块表面**的视图（不在表里 = 没有矩形可聚焦）；`focusedView()` 还要**验活**：
    撤掉的呈现、或 pass 已经不在引擎帧里的呈现都不算（否则"当前视图"会答成一张没人画的画面）。
    `dismiss` 顺手清掉 `focused_` —— 对*结果*冗余（存活检查给同样答案），挡的是"指针留着、被后来同地址的视图继承"。
  * **输入路由**：`SurfaceWindow::routedView` 没装 router 时问表面（`viewAt`，逻辑像素 ÷ 表面尺寸），
    **按下即焦点**且**按下总是捕获**（分屏拖拽越界不断线），**键盘**去焦点（没有位置可路由）；
    `setInputRouter` 降级成覆盖。`RenderControl::focusedView()` = 表面焦点，表面没有呈现视图时回退控件自己的视图。
  * **命令**：五条显示模式命令改 `control->focusedView()` —— `setPrimaryRenderControl` 只回答"窗口的 3D 区在哪块
    控件上"。钉子：`RenderControlTest.TheSurfacesRectanglesRouteInputWithNoRouterAtAll`、
    `ModelViewerTest.TheShadingModeCommandsDriveTheCurrentModelView`（追加一段：焦点在第二格 ⇒ 命令改第二格、
    主视图不动；转回来再改一次）。六处变异全部被杀（见 `graphics-render-next.md` §5 的 M5a 行）。
- **矩形放呈现条目（M2d，2026-10-11 已落地）**：**分屏**是这张表的下一层 —— 每条呈现带自己的**归一化**矩形
  （`Presentation::rect`，`optional`），矩形由**表面**持有、**视图读**它：
  `RenderSurface::present(view, rect)` / `present(view)`（沿用视图当前那块）/ `setViewport(view, rect)`；
  `SceneView::setViewport` 在**绑了表面时转发**到表上（没绑时留在自己身上 —— 一个视图单独用不需要表面），
  `SceneView::viewport()` 由 `const NormalizedViewport&` 改成**按值**返回并按需读表（`viewportOf`）：
  一份矩形，两处拼写。`dismiss` / `~RenderSurface` 把矩形**还给**视图（它接着画同一块地方，只是从"表面的声明"
  变成"视图自己的声明"），`~RenderSurface` 的顺序仍是"先解绑再交矩形"（否则视图会去一张已经没有它条目的表里读）。
  `present(view, rect)` 里的矩形经 `applySurfaceGeometry()` 落成设备像素（窗口 pass 的 viewport、targets、HUD 盒、
  layout 步）—— 形状（aspect）不用另说：后端从那条 pass 的 viewport 推（M2c）。
  **同时**：`RenderSurface::viewAt(nx, ny)`（点归哪条呈现；**归一化**坐标，反序扫描 ⇒ 重叠时**最后画的**赢，
  `NormalizedViewport::contains` 让东/南邻拥有共享边；宿主自建的 pass 不中选）；"一个视图呈现两块表面"被拒
  （一个视图画一张画，而它的矩形是向它呈现的那块表读的）—— 与"跨引擎"同一条诊断。
  **边界（本片不做）**：表里**没有表面尺寸**（只有归一化矩形）⇒ 宿主自建的 pass 没有归一化矩形（它自己给
  `pass->setViewport()` 设备像素），`viewAt` 用归一化坐标。把尺寸也搬进表要先回答"尺寸在表面与视图哪一份"——
  两份正是本设计要消的病，而没有验收判据需要它。
  判据（`test_graphics` 336 绿）：`RenderSurfaceTest.TheSurfaceOwnsTheRectangleAndTheViewReadsIt`（两种拼写写同一块；
  invalid ⇒ 铺满；没呈现的视图 `setViewport` 返回 false；`dismiss` 之后视图接着画同一块 —— 用的值 0.7 视图自己
  从没存过）、`PresentingAViewWithARectangleStatesTheSplitInOneCall`、`AViewIsFoundByThePointItsRectangleHolds`、
  `ThePointGoesToThePresentationDrawnLast`、`AViewThatAlreadyPresentsAnotherSurfaceIsRefused`、
  `SceneViewViewportTest.ASurfaceSplitStatesEachViewsRectangleAndNothingIsRebuilt`（半宽 = 640×720、右上四分之一 =
  640×360：两块形状不同；改矩形不重建（句柄 + pass 数不变））。变异验证：视图不读表 ⇒ 2 条失败；`viewAt` 不看矩形
  ⇒ 2 条失败；`viewAt` 正序 ⇒ 1 条失败；`dismiss` 不还矩形 ⇒ 1 条失败；去掉"已在别的表面"的拒绝 ⇒ 1 条失败；
  `SceneView::setViewport` 不转发 ⇒ 1 条失败。

## 帧计划：一眼看出这一帧画了什么（M4，2026-10-11）

- **新单元 `FramePlan.hpp/.cpp`**：`struct PassPlan`（`pass` 指针 + `order`/`name`/`content`/`camera`/`target`/
  `enabled`/`clears`/`clear_color`/`clears_depth`/`viewport`（`optional`，空 = 铺满）/`inputs`/`output`/`record_micros`）
  + `struct FramePlan{ passes, describe() }` —— 全是**值的拷贝**（两个指针是借用的），也**不回答**"这条 pass 是不是
  某块表面的呈现"（那是 `RenderSurface` 的事，引擎不认识表面）。
- **`RenderEngine::framePlan() const`**：按执行序读一遍注册表，只读、不建任何东西。它是"测试别再数 `passCount()`、
  别比指针身份"的替代品，也是打印给人看的那份。
- **`Slot::record_micros`**：`frame()` 里每个 pass 的整个 scope（`beginPass` → `endPass`）前后各读一次
  `steady_clock`（每 pass 一次，不是每次 draw）；**没跑的 pass 清零**。计划讲的是"下一帧会发生什么"，唯一一个讲过去的
  数就是它，所以它旁边写着 `last=` 而不是混在别的列里。
- **`describe()`**：一行一 pass 的 key=value 表（`target=window` = 画到窗口，`clear=-` = 它不清屏，
  `viewport=full` = 铺满）。真机实测（demo 默认 Deferred，`VINE_FRAME_PLAN=1`）11 条：
  `shadow`(-100) → `gbuffer`(-40) → `deferred_lighting`(0) → `forward_transparent`(20) → `present`(40，唯一画到窗口的那条)
  → `axis_gizmo`(70) → `fps_overlay`(90) → 四个 `gbuffer_preview`(120..123)；每条的上一帧记录耗时 4~436 µs 同表可见。
- **顺手给两条内建 HUD pass 起名**（`axis_gizmo` / `fps_overlay`，与 `main` / `gbuffer` / `present` 一致）：没有名字的表里
  那两行只写得出 `(unnamed)`，也就没法拿"名字序列"当结构断言。
- 钉子：`RenderEngineTest.FramePlanDescribesThePassesInTheOrderTheyWillRun`（每项事实 + `describe()` 的文本）、
  `TheFramePlanIsWhatTheFrameActuallyRan`（**plan 里 enabled 的序列 == 记录型后端被开过 scope 的序列** —— 这条让 dump
  不可能变成"另一个故事"）、`TheFramePlanCarriesWhatEachPassLastCost`（2 ms 的慢 pass ≥ 1000 µs、没跑的 = 0）；
  另两条老用例改成结构断言：`SceneViewViewportTest.TheRecipeDecidesWhichOverlayPassesExist`（断言 pass 名字序列）、
  `RenderPipelineBuilderTest.ForwardPathBuildsWindowPass`（断言 plan 那一条的全部事实）。
- **边界**：M4 只读**当前注册表** —— 不编译图、不分配 transient、不做依赖排序（那是 M7 的 FramePlan 编译）；
  `describe()` 是文本，结构性断言请直接读 `passes`。
