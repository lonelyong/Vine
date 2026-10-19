# graphics 抽象层优化清单（2026-10-19）

> **结论**：本轮只收口“契约文字”，不改公开形态。结构性条目按触发条件登记，等下一次公开面变更或真实负载命中再做。
> 依据：graphics SDK 公开头与 `graphics-redesign.md` / `graphics-layering.md` /
> `graphics-object-model-vs-3d-frameworks.md`。

## 0. 判断口径

优化的目标不是“更像某个引擎”，而是让**概念边界**自洽：

- 一个事实只有一个权威；
- 声明、解析、编译、执行、证据分开；
- 缺席（没人说）与零（说了零）分开；
- 机制在 SDK，策略在应用层。

## 1. 本轮已收口

| # | 问题 | 落点 |
|---|---|---|
| A1 | `FrameDescription` 被称作 value，实际是 borrowed snapshot | `FrameDescription.hpp` 类注释；`RenderEngine::frameDescription()` |
| A2 | 顺序有两个权威：announced order vs dependency schedule | `FrameDescription.hpp`、`RenderFrameExecutor.hpp`、`FramePlan.hpp`、`RenderEngine::framePlan()` |
| B1 | `PassKind` 封闭集 vs `RenderPass::describe()` 逃生口 | `PassDesc.hpp` 的 `PassKind` 注释 |
| C1 | `Pipeline` 与 PSO 的命名碰撞 | `RenderPipeline.hpp` 类注释 |
| C2 | `FramePlan` 与 `FrameDescription` 两个帧值的读者不同 | `FramePlan.hpp` 类注释 |
| D1 | `program` 不在 state 里 | 已由 `StateNode.hpp` 说明；本轮不改 |
| E1 | `ImageRef`（pass 产出）与 `Texture`（导入）是两个图源根 | `Texture.hpp` 与 `ImageRef.hpp` 互指 |
| F1 | 设备像素/逻辑像素的边界 | `Viewport.hpp` 注释（并指向 `SceneView::resize` / `Pipeline::resize`） |
| F2a | 逻辑→设备像素换算分散在三处 | 私有 `PixelGeometry.hpp`（`toDeviceSize`）；`Pipeline` / `AxisGizmo` / `FpsOverlay` 共用（见 §1.2） |
| G1 | `RenderEngine` 角色过载（`publish`/`resolve` + transient） | 拆出私有 `FrameResources`（见 §1.1）；`RenderEngine` 保留 pass/顺序/编译/诊断 |

### 1.1 G1 落地（2026-10-19）

- **新私有类型** `FrameResources`（`FrameResources.hpp` / `FrameResources.cpp`，不导出）：拥有三类“关于图像”的表——宿主 standing binding（`publish`/`unpublish`）、当前帧的 production/publication 表、transient target 表。`RenderEngine` 仍然拥有 pass 注册、顺序、编译、诊断路线和 episode 规则。
- **公开面不变**：`RenderEngine.hpp` 只多了一个前置声明和一个 `std::unique_ptr<detail::FrameResources>`；`publish`/`resolve`/`unpublish`/`transientTargetCount`/`transientTarget` 的签名与行为不变。
- **顺带修掉两个真实的生命周期缺陷**：
  - `clearPasses()` 原来先 `slots_.clear()`，再用 `raw_ptr<RenderPass>` 去 `releasePass` / 读 `renderTarget()`；现在先持有 `intrusive_ptr<RenderPass>`，pass 在释放期间保证存活。
  - `VsgBackend::~VsgBackend()` 原来是 `= default`，`Data` 的成员顺序会先销毁 `DeviceClock` / `RetirementQueue` / device，再销毁仍借用它们的 `OffscreenSession`；ASan 抓到 `OffscreenSession::shutdown -> DeviceClock::removeWindow` 的 heap-use-after-free。析构现在调用幂等的 `shutdown()`，由它决定销毁顺序。
- **判据**：构建 0 error / 0 warning；`test_vsg` 461 通过；`ctest` 24/24；8 条 hygiene 门禁 0 finding。

### 1.2 F2a 落地：逻辑→设备像素换算（2026-10-19）

- **新私有头** `PixelGeometry.hpp`：`LogicalSize` / `DeviceSize` / `PixelRatio` / `toDeviceSize`；换算规则只有一处（截断，与宿主自己的缩放一致），`EngineCompile.cpp` 的 `kShapeTolerance` 说明改为指向它。
- **落点**：`Pipeline::resize`（离屏目标）、`AxisGizmo::onSurfaceResized`、`FpsOverlay::onSurfaceResized`——三处原来各自 `static_cast<int>(logical * ratio)`，现在都调用 `toDeviceSize`。
- **公开面不变**：`Viewport`、`SceneView::resize`、`Pipeline::resize` 的签名不动；这次只把“内部换算”收成一个有名字的类型边界。
- **未做**：色彩空间（linear/sRGB）与数值域（0–255 / 0–1）仍在注释里，按 F2b 的触发条件。
- **判据**：构建 0 error / 0 warning；`ctest` 24/24；8 条 hygiene 门禁 0 finding。

### 1.3 H1 / L1 / I1 / J1 提前落地（2026-10-19，用户要求）

> 四条原本都挂"触发条件"。本轮按用户要求提前做，**先核前提**（见 §2.1），再按核实后的范围落地；
> 每条都带"先红后绿"级别的判据，且不改变已有行为。

- **H1 能力查询**：`Capabilities::Feature{GpuTimings, Wireframe, AnisotropicFiltering}` + `features` 位集 +
  `supports()` / `bit()`，以及**数值上限** `max_anisotropy` / `max_color_attachments`（vsg 从 physical device
  回答，anisotropy 用与 sampler 同一条夹紧）。与 `backend/DeviceRequirements.hpp` 的 `DeviceFeature`
  分工明确：**后者是"不做就不跑"的地板，前者是"能做什么"的宿主词汇**。
  判据：`CapabilitiesTest.OneFeatureIsOneBitAndAnUnansweredFeatureIsFalse`、`CapabilitiesTest.TheLimitsTravelBesideTheFeatureBits`；
  `VsgBackendTest.TheCapabilitiesAnswerTheFeaturesTheDeviceActuallyHas`（线框/各向异性由设备地板保证为 true，
  `supports(AnisotropicFiltering) == max_anisotropy > 1`，至少 4 个彩色附件）。
- **I1 深度边**：新 `DepthView` 值（命名一张 target 的深度图并保活它，可带 `layer()`）；
  `RenderTarget::shareDepth(DepthView)` / `sharedDepth()` 是**机制**，`PassDesc::depth_view` +
  `RenderPass::setDepthView()` 让**画它的 pass 声明边**，引擎在 `addPass` 时应用到 target，`setRenderTarget`
  把边带给新 target。**两种拒绝**也是这条边的一部分：layered 声明（target 只有一张深度图）报一次并拒绝；
  同一 target 的第二个、不同声明也拒绝（首个立住），按 `(target, image)` 只报一次。
  `RenderPipelineBuilder` 的两条合成 pass 改为声明 `depthViewOf(gbuffer)`，不再有人替它们写 target 字段。
  判据：`DepthViewTest.APassStatesTheDepthEdgeAndItsTargetGetsTheMechanism`、
  `DepthViewTest.ALayeredDepthViewIsRefusedRatherThanPointingAtTheWholeImage`、
  `DepthViewTest.TwoDifferentDepthViewsForOneTargetAreRefusedOnce`；既有 `RenderPipelineBuilderTest` 的
  `composite->depthSource() == gbuffer` 与 `SharedDepthTest` 全部不回归。
- **L1 owned 帧**：新 `OwnedFrame.hpp`（`OwnedDraw` / `OwnedPassDescription` / `OwnedPass` / `OwnedFrame`）。
  每个句柄都是拥有式（pass/target/camera/program/lights、`shared_ptr` 命令集、`ImageInput` 输入声明），
  `borrowed()` 交出后端消费的借用视图，有效期 = owned 帧的寿命；输入在 `borrowed()` 里按引擎同一规则解析。
  判据：`OwnedFrameTest.TheOwnedFrameKeepsWhatItsProducersReleased`（生产者全部释放后仍可读，并通过
  `MockBackend::executeFrame` 走**同一条**执行入口——owned 不是第二条线）。
- **J1 门面拆分**：`InputRouter`（事件→操纵器调用：无按钮=移动、有按钮=按下/松开、按键=down/up）与
  `LayoutParticipant`（自身尺寸/比例 + 注册步骤 + 固定顺序）成为 `SceneView` 的协作对象；
  `pushEvent` / `resize` / `addSurfaceLayout` / `clearSurfaceLayouts` 仍是门面入口，行为不变。
  判据：`InputRouterTest`、`LayoutParticipantTest`、`SceneViewTest.InputIsRoutedThroughTheRouterToTheManipulatorTheViewOwns`；
  `SceneViewViewportTest.TheAreaSizeHasOneOwnerAndTheViewReadsIt` 与 `RenderSurfaceTest.TheAreaOwnsTheSizeAndTheViewReadsIt` 不回归。
- **整批判据**：全量构建 0 error；`test_graphics` **385**、`test_vsg` **473**；`ctest` **23/24**（唯一红是既有的
  `AsyncDefectRegressionTest.YieldLoopStackIsConstant`，属 async 模块，与本次改动无关）；8 条 hygiene 门禁 0 finding。

### 1.4 B2：`RenderPass::execute` 不再是扩展点（2026-10-19）

- **结论**：`execute()` 从 `virtual` 改为普通成员函数。它本来就只是“`describe()` → `singlePassFrame()` →
  `executeFrame()`”的糖；只要它可覆写，子类就能画出**编译期完全看不到**的东西（一条 pass 两套真相）。
  现在唯一扩展点是 `describe()`（外加声明侧的 `PassDesc::before_draw`）。
- **顺带**：`RenderEngine.cpp` 里 “ScreenPass::execute returns before asking the backend” 的注释已改写
  （`ScreenPass` 本来就没有 execute 覆写，描述为空就什么也不交）。
- **判据**：`RenderPassTest.ExecuteIsSugarOverDescribeAndTheOnlyExtensionPoint`（`describe` 调一次、
  backend 收到一条 pass 条目；描述为空 ⇒ 不交帧）；
  `RenderEngineTest.TheEngineRunsTheFrameValueAndDescribesEachPassOnce`（每帧 `describe` 一次、
  `frames_executed` 每帧 +1，旧的 `CountingPass::execute` 覆写随契约一并退场）。

## 2. 待办（带触发条件）

> **H1 / L1 / I1 / J1 已于 2026-10-19 提前落地**（用户要求；前提是否过时的论证见 §2.1，落地记录见 §1.3）。
> 下表留下 F2b 与 B2。

| # | 优化项 | 抽象问题 | 动作 | 触发条件 |
|---|---|---|---|---|
| F2b | 色彩空间与数值域未进类型 | linear/sRGB、0–255/0–1 靠注释 | 强类型 `LinearColor` / `SrgbColor` / `UnitScalar` | **本轮不做（用户决定 2026-10-19）** |

### 2.1 提前落地的过时性裁决（2026-10-19，用户要求 H1 / L1 / I1 / J1 开干）

> 四条都写着"触发未命中"，所以先做一件比实现更重要的事：**核实前提今天是否还成立**，再决定做多少。
> 结论是两条不过时、一条范围收窄、一条部分过时——照原稿硬做会把"已经解决的问题"再做一遍。

| # | 原稿前提 | 今天的代码（2026-10-19 核实） | 裁决 |
|---|---|---|---|
| **H1** | `Capabilities` 只放预检事实，缺能力查询 | `Capabilities.hpp` 只有 `render_targets` / `presentation`；后端侧另有 `backend/DeviceRequirements.hpp` 的 `DeviceFeature` / `DeviceFacts`，但那是**要求层**（不满足就拒绝设备），宿主读不到"这台设备能不能给我 GPU 时间/线框/各向异性" | **不过时**。缺的是**宿主可事前查询的能力词汇**，不是要求表。落地：`Capabilities::Feature{GpuTimings, Wireframe, AnisotropicFiltering}` + `features` 位集 + `supports()`；vsg 后端从 physical device 的 `limits.timestampComputeAndGraphics` / `fillModeNonSolid` / `samplerAnisotropy` 回答。与 `DeviceFeature` 刻意的关系：前者是"能做什么"，后者是"不做就不跑"。**已落地，见 §1.3** |
| **I1** | `RenderTarget::shareDepth` 把调度边藏进描述 | **边本身早就在建图里**：`backend/FrameCompiler.cpp:174-184` 读 `target.depth.borrowed` / `depth.source`，并要求"借入方的生产者必须先写"（顺序缺陷会被拒绝）。缺的只是**声明点**：边写在 target 字段上，由 builder 代写 | **前提成立、范围收窄**：不需要新造资源图机制。把声明变成**值**并交给 **pass** 即可——边是"这条 pass 画在谁的深度上"，而 pass 才是画的那个。落地：`DepthView`（持有 target）、`RenderPass::setDepthView` / `PassDesc::depth_view`；引擎在 `addPass` 应用到 target（`RenderTarget::shareDepth(DepthView)` 降为机制），builder 改为给两条合成 pass 声明边。**已落地，见 §1.3** |
| **L1** | 帧 IR 没有 owned 形式 | `RenderFrameExecutor.hpp:44-56` 写明：`executeFrame` 的参数**只在调用期间借用**、引擎**单线程串行**、对象必须在整个 `initialize()`/`shutdown()` 期间留在同一线程。没有任何 owning 形态 | **不过时**：离线/远程/帧录制仍无路。**已落地，见 §1.3** |
| **J1** | `SceneView` 把相机/内容/操纵器/管线/区/输入汇于一身 | 输入**已经**在 `CameraManipulator::onMousePress/onMouseMove/onMouseRelease/onScroll/onKeyDown/onKeyUp`，`SceneView::pushEvent` 只是约 40 行的"按键→方法"映射；配方已归 `RenderPipelineBuilder` / appfw `Viewer`（S7/S8）；**尺寸归 `RenderSurface`**，视图只在"不呈现"时自留一份 | **部分过时**：已经各归其位的是配方与尺寸，真正还压在门面上的只有两小块——"事件→操纵器"的映射、以及"尺寸/比例 + 注册步骤"的布局参与。只拆这两块（`InputRouter` / `LayoutParticipant`），不重做相机/内容/管线的归属。**已落地，见 §1.3** |

## 3. 明确不做

- 不改 `RenderTarget` 的名字（S10a 已裁决：它是描述，不是资源）。
- 不现在拆 `RenderBackend` 聚合（三张契约已各自成头，聚合是过渡）。
- 不引入通用 `std::variant` / `std::any` 的帧图节点（会破坏 `PassKind` 的可读性）。
- 不为“将来可能”加能力标志（触发条件命中再加）。

## 4. 判据

- 契约文字批：全量构建 0 error / 0 warning；8 条 hygiene 门禁 0 finding；不跑测试（comment-only 口径）。
- G1 批：构建 0 error / 0 warning；`test_vsg` 461 通过；`ctest` **24/24**；8 条 hygiene 门禁 0 finding。
- F2a 批：构建 0 error / 0 warning；`ctest` **24/24**；8 条 hygiene 门禁 0 finding。
