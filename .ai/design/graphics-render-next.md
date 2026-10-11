# graphics 渲染栈的重新设计提案（next，2026-10-11）

**状态：M1 已落地（2026-10-11）；M2 的 M2a/M2b/M2c 与观测片 M4 已落地（2026-10-11）；其余是提案，未落地** ——
本文回答"如果重新设计这一套会怎么切"，并把每条切成能独立验收的迁移片。落地的部分按本文自己的规矩**以现状文档为准**
（`appfw-render-view.md` §4.1、`.ai/memory/graphics.md`），这里只留下"原稿哪里说错了"的记录。**本文允许大改**：
§3.2 是按行业口径重写的（`Surface` + 呈现表 / `Camera` + viewport 矩形 / `View` 配方），不迁就当前实现。

**一句话**：骨架保留（分层 API + 第 0 层逃生口、配方/几何/开关三分、托管与借用分开），**重切四件事**；
其中三件便宜、立刻消掉真实混乱，第四件是唯一的大改、等触发条件。

**与既有文档的关系**（现状以它们为准，本文不重复）：

| 想知道 | 看 |
| --- | --- |
| 视图层今天长什么样（配方/几何/开关、初始化时机、多视图立场、所有权 R1/R2/R3） | `appfw-render-view.md` |
| 管线对象（`Pipeline` 的形状、效果与生命周期） | `graphics-pipeline.md` |
| 状态与"两处作用域"（`StateNode` vs pass 级覆盖，谁赢谁叫 Override） | `graphics-state.md` |
| 表面生命周期（attach 状态机、跟句柄重建、三态与其不变量） | `appfw-render-surface.md` |
| 文档 / 视图 / 控件链 | `appfw-document-model.md` |
| 管线配方层（preset → 管线） | `graphics-pipeline-builder.md` |

## 0. 一页纸

**保留（这是这套设计真正的本事，重设计也不动）**：

| 保留 | 为什么 |
| --- | --- |
| 四层 API + 第 0 层逃生口（`engine->addPass`、自写 shader、自建 builder） | "三五行出画面"和"完全自己接"能同时存在，靠的就是每层都能单独用 |
| **配方 / 几何 / 开关** 三分（`SceneView::Options` / `setViewport`+`onSurfaceResized` / `setEnabled`） | 换设置才重建、开关永不重建、几何只改取值 —— 这是"配置窗口"能被信任的前提 |
| "**赢了内容才叫 Override**"的命名规则 | 优先级住在类型名字里，比藏在文档里强（`graphics-state.md` §4.1） |
| 默认窗口管线**让位**给宿主自建主 pass（**M2a 已落地**：意图保留，机制从"读引擎的 pass 表"换成"宿主显式声明"—— 宿主 `present()` 自己的 pass，视图就不建默认管线） | 兜底而非竞争，宿主永远赢 |
| 文档 → 视图 → 控件 → 面 的 1:1 链 + `DocumentView::deactivate()` 的收回约定 | 隔离干净、视角按实例保住、切文档回默认态天然正确 |

**重切四条**（编号即 §3 的小节号）：

| # | 一句话 | 代价 | 立刻消掉什么 |
| --- | --- | --- | --- |
| 3.3 | 调试覆盖（线框/法线/无光照/平面）挪到**几何阶段**，不贴在窗口 pass | 小 | Deferred 上四档全部可用（今天报 `UnsupportedRequest`）；窗口 pass 变成纯合成 |
| 3.2 | **呈现与分屏**：`Surface`（唯一清屏 + 呈现表 + 矩形）/ `Camera`（观看 + 投影参数，**不含 aspect**）/ `View`（配方 + 管线）；`SceneView` 退成单矩形门面 —— **M2a（呈现表的 owner）+ M2b（清屏归表面）+ M2c（aspect 出相机）已落地 2026-10-11** | 大（唯一动公开形态的一刀） | 全局让位规则 `hasWindowPass(camera)`（已删）、靠发言顺序的清屏（已归表面）、`SceneView` 三件事捏一起、**aspect 锁在相机里**（相机整台不可复用 —— 已出相机） |
| 3.1 | **状态解析单点** `resolveRenderState(...)` | 中 | "同一个设置两处"这类争论从命名问题变成类型问题 |
| 3.4 | Pass 从"子类 + 就地声明"变成**描述符 + 每帧 FramePlan** | 大 | `wiringRevision()` + `validateWiring()` 这套"谁动了声明"的事后重查 |

**横切三条**（§2 的契约表）：所有权（Device → Session → Lease，取代 R2 的"操作期间临时加引用"）、
**能力查询**（取代"跑一遍再看日志"）、**观测**（FramePlan dump 取代 `passCount()`/指针身份做测试替身）。

## 1. 现状的摩擦与证据

每条只写"现象 + 证据 + 根因一句话"；机制细节在 §3 对应小节。

- **F1 状态优先级住在名字和文档里**。同一件事有三个来源：内容自己的 `StateNode`、pass 级覆盖
  （`setProgramOverride` / `setPolygonModeOverride`）、视图配方（`Options::mode`）；靠 `graphics-state.md` §4.1
  那张"谁赢谁叫 Override"的表 + `RenderCommand::depthExplicit` 这类旗标对齐。`RenderPass::execute()` 的
  拷贝路径就是三个来源的汇总点，但它不做优先级决策 —— 决策分散在各自的 setter 里。
  *根因*：优先级没有落成一个可测函数。
- **F2 呈现是运行时争夺**（**M2a 已落地删除，见 §3.2.1 落地记**）。`SceneView::ensureWindowPass()` 用
  `engine_->hasWindowPass(camera_)` 决定"我要不要
  建默认窗口 pass"；`RenderPipelineBuilder` 里还得写注释解释"render target 为空的 pass 也算占位"。
  于是"谁负责最后一步"是**发现**出来的，不是声明的；配套补丁就是"重建之后要把使用者关掉的档再关回去"
  （`applyOverlayIntent()`）。*根因*：视图既描述"场景长什么样"，又偷偷拥有"最后一步"。
- **F3 调试覆盖贴在最后一步**（**M1 已落地，见 §3.3**）。`applyModeToWindowPass()`（M1 已改名
  `applyModeToGeometryPass()`）把"内容怎么着色"写在**窗口 pass** 上，于是
  Deferred 上四档天生兑现不了（G-buffer 出颜色 + 全屏光照合成）⇒ 只能报 `UnsupportedRequest` 并保持 Lit。
  *根因*：调试覆盖的作用域选错了阶段（应该是几何阶段：两条路径都从那儿开始）。
- **F4 声明就地可变 ⇒ 引擎只能事后重查**。`RenderPass` 同时是"声明"（目标/输入输出/程序/启用）、"状态"
  （清屏/深度/视口/覆盖）和"执行"（`execute(scene, backend)` 收命令并下发）；`FpsOverlay`、`AxisGizmo`、
  `ScreenPass` 都是它的子类，注册后就地改。引擎因此需要 `wiringRevision()` 计数 + `validateWiring()` 重查。
  *根因*：缺一层"每帧重建的声明式图"。
- **F5 借用与托管混在一起**。视图借用引擎（`raw_ptr`），但它的默认管线**强持有**引擎；于是"操作期间临时加
  引用"成了必须记住的约定（`intrusive_ptr<RenderEngine>(engine_, true)`，`appfw-render-view.md` §13 的 R2）。
  *根因*：所有权不是从父到子的单向链，而是靠"谁恰好还活着"。
- **F6 "当前视图"是可变全局槽**。命令要拿到"要改的那个 3D 视图"，靠 `MainWindow::setPrimaryRenderControl()`
  这一个槽 + `activate()/deactivate()` 成对发布；单窗口成立，多窗口/多分屏含糊（§12 已记）。
  *根因*：命令缺一个**显式上下文**，于是用全局槽代替。
- **F7 生命周期靠被动发现**。`QTimer::singleShot(0, …)` 兜底 + `activate()` 里重试 + `Pending/Presenting/Failed`
  三态，本质是"没有任何一方声明自己 ready"的补丁；能力（能不能画线？能不能 MRT？）只能靠
  `reportEngineProblem(UnsupportedRequest)` + 人看日志。*根因*：宿主与会话之间没有事件协议，也没有能力查询面。

## 2. 目标形态

```
L0  设备 / 会话     RenderDevice（能力查询 + 资源缓存）→ Session（surface + swapchain）；一设备 N 会话
L1  图 / 帧         PassDesc（描述符）→ FramePlan（每帧编译：顺序、目标、输入、clear/load、barrier、transient 分配）
L2  配方           buildRecipe(graph, options)：forward / deferred / shadowed …（今天 RenderPipelineBuilder 的位置）
L3  相机 / 视图    Camera（投影 + 变换 + **viewport 矩形**）+ View（配方 path/mode/HUD + 默认管线）+ 交互器
L4  呈现 / 宿主     Surface（交换链 + **唯一一口清屏** + present）+ 输入路由 + HUD 作为带 tag 的普通 pass
L5  应用 / 文档     文档 / 视图 / 控件链；命令带 ViewContext；分屏布局归宿主
```

拟新增的单元（**提案，未落地**；不进任何门禁的引用检查）：

```
viz/graphics/src/
    RenderDevice.cpp            设备：能力查询、资源缓存、会话创建
    RenderSession.cpp           会话：surface/swapchain 生命周期，Lease 发放
    PassDesc.cpp                描述符 → 图结点（声明式，每帧可重建）
    FramePlan.cpp               编译后的帧计划 + dump（pass 顺序/目标/输入/状态/耗时）
    RenderStateResolver.cpp     resolveRenderState：内容 → pass → 视图覆盖的单点优先级
    RenderSurface.cpp           表面：呈现表 + 唯一清屏 + present/dismiss（§3.2）
    Camera.cpp                  (改) 多一个 viewport 矩形（归一化）：唯一矩形真相
appfw/src/gui/
    ViewContext.cpp             命令的显式目标（window + 焦点视图 + selection）
```

**四条横切契约**（每一条都对应一个 F）：

| 契约 | 目标 | 取代 |
| --- | --- | --- |
| 所有权 | 一律"父持有子"：Device → Session → 资源/pass；借用一律 `Lease`/句柄，且**会话必须晚于它的租约销毁**（在类型上表达，不靠约定） | R2 的手工加引用 |
| 状态 | 一个函数决定一个 draw 的最终状态，输入是 (命令, pass, 视图覆盖)，优先级是一张表 | 分散的 setter + 命名规则 |
| 能力 | `session->supports(feature)` / `featureLevel()`，可查询、可在建管线之前问 | 事后 `reportEngineProblem` + 文档 |
| 观测 | 每帧一份 FramePlan（可 dump、可断言结构），替代 `passCount()`/指针身份当替身 —— **M4 已落地（2026-10-11）**：`RenderEngine::framePlan()` 返回 `FramePlan{passes}`，`describe()` 出一张表 | 隐式结构 |

## 3. 四条重切

### 3.1 状态解析单点（`resolveRenderState`）

**目标 API**（提案）：

```
RenderState resolveRenderState(const RenderCommand& cmd, const RenderPass& pass, const ViewOverrides& view);
// 优先级（一张表，一处实现，一处测试）：
//   view 调试覆盖  >  pass 级覆盖  >  内容自己（StateNode 折叠后）  >  引擎默认
// 返回值自带"这一项是谁给的"（来源枚举），诊断与测试因此能说清"为什么是 Line"
```

**影响面**：`graphics-state.md` §4.1 的命名表从"实现规则"降级成"API 命名习惯"；`RenderCommand` 的
`depthExplicit` 这类旗标被来源枚举取代；后端的 `SetDynamicState` 路径不变（它收的还是最终状态）。
**风险**：一次性改动所有状态消费点，容易出现"看起来一样但少了一项"；因此验收必须包括逐项的状态快照对照。
**行业对照**：Unreal 的 material override / base pass 覆盖、Unity 的 per-pass `RenderStateBlock`、
Filament 的 `MaterialInstance` 覆盖链 —— 都是"一个解析函数 + 一张优先级表"。

### 3.2 呈现与分屏：**Surface / Camera·viewport / View**（三维规范口径，允许大改）

这一条是四刀里最大的一刀，也是唯一"要动公开形态"的一刀。**先摆事实**（每条都带证据），再给行业口径与目标 API。

#### 3.2.1 今天的两条隐式规则（事实）

**A1「谁呈现」是全局发现式 —— M2a 已落地删除（2026-10-11）**。视图建默认窗口管线之前先问引擎："有人已经用我这台相机呈现了吗？"

```
// SceneView::ensureWindowPass()（M2a 之前的实现）
if (engine_->hasWindowPass(camera_.get())) { return; }   // 有人占了 ⇒ 我让位
```

| 后果 | 证据 |
| --- | --- |
| 判断是**全局**的：任何"这台相机 + render target 为空"的 pass 都算占位，没人记"是**谁**占了这块表面" | `RenderPipelineBuilder.cpp` 里专门写了注释解释"null target 也算，所以 `hasWindowPass(camera)` 为真" |
| **顺序敏感**：`addPass()` 的先后决定谁不建 | 规则读的是当时的引擎 pass 表，没有"谁声明要呈现"这一层 |
| 两台视图共用一台相机（镜像 / 概览 / VR 双眼）⇒ 第二个被**静默跳过** | 同上；`hasWindowPass()` 只是查询，不回答"这块表面上的这一块是谁" |

它同时是一条成文承诺（`SceneView.hpp` 里"an application that built its own main pass…"那段 —— **M2a 把这句承诺
搬到了 `RenderSurface`**：声明自己的 pass 仍然"永远赢"，但赢的方式是说出来，不是视图去让位）和 demo 的依赖：
`DemoScene::install()` 建自己的管线（用视图那台相机），靠这条让位规则让宿主那次 attach 不重复建 pass。
即：**"谁呈现"今天没有 owner，只有一条全局查询 + 一个巧合**。

> **M2a 落地（2026-10-11）**：这条规则与 `RenderEngine::hasWindowPass(camera)` 一起删除。取而代之是
> **`RenderSurface`**（一块表面的呈现表）：`present(SceneView&)` / `present(raw_ptr<RenderPass>)` /
> `dismiss` / `hasPresentation()` / `presentations()`（按引擎执行序的 `{pass, view}`）；`SceneView::ensureWindowPass()`
> 不再问引擎任何事，是否该建由**表面**说（`SurfaceWindow::init()` 的 `if (!hasPresentation()) present(*view)`，
> 仍在后端检查之前）；宿主自建主 pass 的写法变成 `surface->present(pipeline->windowPass())`（`demo_plugin`
> 就这么改的）。`RenderEngine` 新增 `passOrder(pass) -> std::optional<int>`（顺序住在引擎的槽里，同时是"这条
> pass 还在不在"）。钉子：`tests/test_graphics/RenderSurfaceTest.cpp`（六条）+ `test_gui`
> 的 `TheControlsSurfacePresentsItsOwnViewByDefault`。

**A2「谁清屏」靠发言顺序，而且静默失效。** `RenderPass::announceClear()` 是唯一公告点，后端按 attachment
**只应用第一条**公告：

> `tests/test_vsg/WindowCompositionTest.cpp` —— "announced, and NOT applied: **the window clears once**"

于是 §12.2 的"恰好一个视图清"今天不是被保证的，而是**被后端吞掉**的：两块视图都清时第一条的颜色赢、
第二条静默丢弃 ⇒ 第二条 pass 的 `clearColor()` 与实际画面**不符**，换一下 pass 顺序就换背景色，没有任何一方被告知。

**A3 两条补丁都源于"没有 owner"。** `applyOverlayIntent()`（重建后把关掉的档关回去）、M1 的"非 Lit 档换链条"，
都是"视图自己造管线"的副作用；owner 一旦明确，这类补丁自然变少。

**A4 一条决定 API 能不能做小的关键事实**：引擎本来就按"**order 升序、同序按注册序**"执行 ——
`RenderEngine.cpp` 的槽表注释写着 *"equal orders preserve insertion order (stable), so ties are resolved by the
addPass() call sequence"*；但**没有任何公开查询**能读出某条 pass 的 order。要"指定唯一那条清屏"就必须先有这条
查询，否则消费者只能猜执行序。

#### 3.2.2 行业口径（三维规范）：三个概念、三条共识

| 概念 | VTK | OSG | Qt3D | Unreal | Unity | three.js | Filament |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **表面**（交换链 / present） | `vtkRenderWindow` | `GraphicsWindow` | `QRenderSurfaceSelector` | `FViewport` / `FRHIViewport` | `Camera.targetTexture`（或默认） | `WebGLRenderer` | `SwapChain` |
| **分屏单元** | `vtkRenderer` | `Camera`（它同时是 view） | framegraph 子树 | `FSceneViewport` + layout | `Camera.rect` | `renderer.setViewport()` | `Viewport` |
| **矩形** | Renderer | Camera | 结点属性 | Viewport | Camera | 调用参数 | Viewport |
| **相机** | `vtkCamera` | `osg::Camera` | `QCamera` | `FSceneView` | `Camera` | `Camera` | `Camera` |
| **清屏** | Renderer 的背景色（窗口按"最后画的 renderer"清） | `Camera::setClearColor` | `QClearBuffers` 结点 | Viewport 的 clear color | `renderer.setClearColor` | renderer | `Renderer::setClearOptions` |
| **交互** | `vtkRenderWindowInteractor` 按 renderer 路由 | `osgGA`，每相机一套 | 事件过滤器 | Viewport 拥有输入 | —— | —— | —— |
| **图 / 管线** | `vtkRenderPass` | render stage | framegraph | RDG | SRP | —— | FrameGraph |

**三条被行业反复确认的规则**（也正是我们今天的偏差）：

1. **矩形属于"表面上的那一块"，不属于"怎么画"**。VTK 把它放 Renderer、Unreal 放 Viewport、Qt3D 放一个
   `QViewport` 结点，而 Unity 放 `Camera.rect`、OSG 放 `Camera::setViewport`（"矩形放相机"那一派）。
   **本文选"矩形放表面那一侧的呈现条目"**，理由见 3.2.3 的 aspect 一节：矩形一旦进相机，**aspect 就一起锁进相机**，
   于是那台相机整台不可复用（Unity 的相机就是这样，想复用只能复制）。我们今天放在 `SceneView` 上 ⇒ 矩形与
   "相机 + 场景 + 配方 + 管线"绑死，**同一视图两块矩形做不到**。
2. **清屏属于表面，不属于"最后一条 pass"**。Unity/three.js/Filament 由 renderer 统一持有 clear，
   VTK 是每 Renderer 一个背景色 + 窗口层的"清一次"。**"谁公告第一谁赢、其余静默丢弃"是我们特有的规则。**
3. **"怎么画"不自己抢最后一步**：VTK 的 renderer 不创建窗口，Unreal/Unity 的 view 只描述画什么，
   呈现由 Viewport / 相机的 target 决定。我们的 `ensureWindowPass()` 是反过来的（视图去认领窗口）。

#### 3.2.3 目标对象模型与所有权（大改版）

> **术语：viewport 就是矩形，本设计不新增"格子"类**（先消歧，仓库里 viewport/"pane" 都已经被用过）
>
> - **viewport = 那块矩形**，三个层次今天已经这么叫：`RenderBackend::setViewport(x,y,w,h)`（设备像素）、
>   `RenderPass::setViewport(Viewport)`（设备像素）、`SceneView::setViewport(NormalizedViewport)`（归一化 [0,1]），
>   类型在 `Viewport.hpp`（`Viewport` / `NormalizedViewport`）。**不新增同义类型。**
> - **矩形放在"呈现条目"上**（表面那一侧的绑定：`Surface::present(view, rect)`），**不放在相机上** ——
>   理由见下面的 aspect 一节：相机一旦持有矩形推出来的 aspect，就整台不可复用。
> - **不引入 `Pane`/`View` 这类"格子"对象**。行业里确实有人这么做（Filament 的 `View` =
>   camera + viewport + scene + clear；VTK 的 `vtkRenderer`；Unreal 的 `FSceneViewport`），但那个单元的职责在我们这儿
>   各自已经有家：**顺序**归 Surface 的呈现表、**清屏**归 Surface（3.2.2 第 2 条）、**输入**可以按矩形自动推
>   （"哪条呈现的矩形含这一点" = 今天 `setInputRouter` 回调的默认实现）、**HUD** 归 View 的配方。
>   将来编辑器真需要"格子"当**布局单元**（splitter 的结点）时，它再以布局对象的身份出现，而不是现在按猜测建一个。
> - **它不是**：OS 窗口（`MainWindow` / `SurfaceWindow`）、控件容器（`RenderControl`）、文档视图（`DocumentView`）、
>   pass（`RenderPass`；一块矩形上的画面可以由多条 pass 产出）。
> - **与 appfw 的 "pane" 无关**：`DockPanel` 的文档里 "pane" 指**停靠面板**（`DockingPaneManager`、"the pane
>   content"）—— 本设计不再使用这个词。
> - **仓库里还有第三种用法**：`SceneView::paneSize()`、`pane × ratio`、`onSurfaceResized(pane_w, pane_h)` 里的
>   "pane" 指的就是**这块矩形**（"this view's rectangle in the host's logical pixels"）。矩形挪到呈现条目之后，
>   这些说法一律读作"这条呈现的矩形推出来的那块"，字面不用改。

**aspect 为什么必须进投影矩阵，以及为什么它不该是相机的状态（本片的判据）**

投影矩阵的唯一工作是把视锥映射到 **NDC**（`[-1,1]³`），而 NDC 是**方的**、屏幕上的矩形一般不是：viewport 变换把
`x_ndc ∈ [-1,1]` 铺满宽、`y_ndc ∈ [-1,1]` 铺满高。矩阵里 aspect 只出现在一处（`Transform3.hpp` 的 `perspective`）：

```
f = 1/tan(fovy/2)
m(0,0) = f / aspect      ← 唯一的 aspect            x_ndc = (f/aspect)·x_view / (-z_view)
m(1,1) = f                                          y_ndc =  f       ·y_view / (-z_view)
```

所以"矩阵里必须有 aspect"是硬约束（`gluPerspective` / `glm::perspective` / 我们的
`Camera::setProjectionMatrixAsPerspective(fovy, aspect, near, far)` 都把它当必填参数）。真正要决定的是另一件事：

| | view matrix | projection matrix |
| --- | --- | --- |
| 依赖 | 只依赖**眼睛**（eye / up / target） | 依赖 fov / near / far **和 aspect**（正交还含 box） |
| 跨矩形/跨表面复用 | **可以** | **只要 aspect 不同就不行** |

⇒ **"相机能不能复用"完全取决于 aspect 在不在相机里。** 我们今天在（`Camera.cpp`：`aspect_ratio_ = aspect;`
并重建缓存矩阵）⇒ 一台相机 = 一个投影 ⇒ "同一台相机两块矩形"必然**静默**画错。行业三种排法都真实存在：

| 引擎 | 相机里有没有 aspect / 矩形 | 结果 |
| --- | --- | --- |
| **VTK** | **没有**（`vtkCamera` 只持 `ViewAngle` / `ParallelScale` / `ClippingRange` / `WindowCenter` / `ViewShear` / `EyeAngle`…；投影在 `UseExplicitProjectionTransformMatrix` 为假时**按需推导**，aspect 由 renderer 的 viewport 在渲染时提供） | 一台相机能挂多个 renderer（矩形在 renderer 上） |
| **Unity** | 有（`Camera.rect`，aspect 由它推） | 整台不可复用；要复用就**复制相机** |
| **Unreal** | 每块一个 `FSceneView`（含投影）；可共享的是"相机参数"那一半 | 复用发生在参数层 |
| **Filament** | `View::setViewport` 与投影分开，但投影的 aspect 是显式给的 | 只有 aspect 相同时能共用 |

**本设计选 ③，并已落地（M2c，2026-10-11）**：`Camera` = 观看（eye/up/target）+ 投影**参数**
（fov / near / far / 类型 / 正交盒），**不含 aspect、也不缓存投影矩阵**；aspect 是**渲染时的输入**：

```
矩形（归一化）× 表面尺寸 × 像素比  →  aspect  →  projectionMatrix(aspect)  →  view block
                                      ↑
                     RenderBackend::viewAspect()  ← 唯一一处推导
                     （pass 的 viewport，否则目标 / 表面尺寸）
```

好处有两个：**相机可复用**（同一眼睛、多块矩形 = 镜像 / PiP 同视角 / 立体），以及**投影不可能过期**
（"矩形变了投影没跟上"这一整类静默错在结构上消失）。

**落地形态：一处推导，两类读者**。提案写的是"谁填 view block 谁算 aspect，并把它带给所有用同一份投影的人"；
落地把"算"的那一处钉在**后端**身上 —— 只有它被告知"这条 pass 画进哪块矩形"，也只有它持有目标 / 表面的尺寸：

| 项 | 落地形态 |
| --- | --- |
| **唯一推导处** | `RenderBackend::viewAspect()`（新契约，纯虚）：pass 的 viewport 优先，否则它画进的目标尺寸，再否则表面尺寸；什么都没公告 ⇒ 1（"还没有画面"是答案，"猜一个"不是） |
| **读者一：画面** | `RenderPass::execute()` 在收集命令之前问同一个数，转给 `collectRenderCommandsShared(camera, aspect)` —— 看见的和剔掉的是**同一个视锥** |
| **读者二：view block** | `FrameRecorder::setViewAspect()`（scope 属性，像清屏和深度模式），`snapshotCamera()` 用它 `projectionMatrix(aspect)`；vsg 后端在每次 `render()` 前公告**同一个** `viewAspect()` |
| **第五个读者（提案漏算）** | `Camera::screenToWorldRay(screenPos, aspect)`：picking 也要矩形，且必须是同一块（`OrbitCameraManipulator` 改成自己持 `viewportAspect()`，`onResize` 不再往相机里塞 aspect） |
| **memo** | `Scene` 的帧内 memo 以 view-projection 为键 —— aspect 在矩阵里，所以"同一台相机、两种视锥"天然是两条 memo 项、两次走树，谁也答不了谁 |
| **正交** | `projectionMatrix(aspect)` 对正交相机**忽略**该参数（盒自带比例）：方向光阴影照旧（`RenderPipelineBuilder` 传 `1.0`，是"声明一个不用的形状"而非"顺手叠一次比例"） |

**原稿哪里有偏差（落地时修正）**：

1. 提案写"代价四处（view block / 剔除 / 操纵器 / `aspectRatio()`）"，**漏了第五处** `Camera::screenToWorldRay`
   —— 它也是"按矩形把屏幕点翻成世界"的地方，漏了就是"画面按新矩形、picking 按旧矩形"。
2. 提案把"算 aspect"放在**填 view block 的那个后端内部**；落地把它抬成**契约**（`RenderBackend::viewAspect()`），
   因为 SDK 的剔除（`RenderPass::execute`）也要问同一个数 —— 留在后端内部就成了"后端算一份、SDK 另算一份"，
   正是这份设计要杀掉的东西。

```
Surface                        交换链 + present + 这块表面**唯一一口清屏**      ← 今天的 SurfaceWindow 后半 + presenter
  └ 呈现表（有序）               "这块表面由哪几条画出来、各占哪块矩形"          ← 今天的 hasWindowPass 让位规则换成它
      ├ 矩形                   NormalizedViewport（每条呈现一块；换算成设备像素归 Surface）
      ├ Camera                 观看（eye/up/target）+ 投影参数（fov/near/far/类型）—— **不含 aspect**，可跨矩形复用
      ├ View / Recipe           path / mode / HUD 的配方 + 默认管线 + 内容（Scene，可分屏共享）
      └ Interactor             输入：按"哪条呈现的矩形含这一点"路由，按下路由、松开前捕获
```

**切法（一句话）**：今天 `SceneView` 把三件事捏在一起 —— **画到什么矩形上**（viewport）、**怎么画**（配方 + 管线）、
**画什么**（相机 + 内容 + 交互）。三件事各回各家：矩形与顺序归表面、aspect 归渲染时、配方与内容留在视图。

| 归属 | 今天在 | 目标 | 理由（行业） |
| --- | --- | --- | --- |
| 矩形（viewport） | `SceneView::setViewport` | **呈现条目**（`Surface::present(view, rect)`；`SceneView::setViewport` 的转发保留） | 矩形是"表面怎么分"；矩形放相机（Unity）会连 aspect 一起锁进相机，放绑定上（VTK/Filament）才能复用相机 |
| 矩形的**换算**（归一化 → 设备像素 + aspect） | `SceneView::applySurfaceGeometry()` | **`Surface`**：`rect × 表面尺寸 × 像素比` 一处推完（pass 视口、HUD 尺寸、layout 步、**aspect**） | 只有表面知道"这块表面多大、像素比多少"；相机与视图都不该知道 |
| **aspect** | `Camera::aspect_ratio_`（相机状态） | **渲染时输入**（见上表四处） | view matrix 可复用、projection 不可 —— 差别只在 aspect |
| 投影矩阵 | `Camera` 缓存 | `projectionMatrix(aspect)` 现算（缓存要按 aspect 键） | 投影是"参数 + 尺寸"的函数，不是相机的状态 |
| 清屏 | `RenderPass` 的开关（谁先公告谁赢） | `Surface::setClear`（唯一一处） | Unity / three.js / Filament 都由 renderer 持有 |
| 顺序 + 呈现 | `SceneView::ensureWindowPass()` 的让位 | `Surface::present(...)` 的**呈现表** | VTK RenderWindow / Qt3D RenderSurfaceSelector |
| 相机 | `SceneView::camera()` | 仍在 `Camera`（视图默认带一台，**可跨呈现复用**） | 相机就是相机 |
| 内容 | `SceneView::scene()` | 留在 View（**共享内容 = 多块呈现引用同一 Scene**） | VTK 可把同一 actor 加进多个 Renderer |
| 配方 + 管线 | `SceneView` | 留在 View | "怎么画"与"画到哪"无关 |
| 输入 | `RenderControl::setInputRouter`（宿主写回调） | 默认由矩形推（`Surface::viewAt(x, y)`），宿主仍可覆盖 | VTK 的 interactor 按 renderer 的矩形路由 |

**三条硬规则**（要大改就得先认下来）：

1. **相机可复用**：一台相机能进多条呈现（同一眼睛、不同矩形），因为 aspect 不在相机里 —— 这是 ③ 换来的。
   **但操纵器仍每块一个**（它要把鼠标位移换成角度，必须有 aspect）。
2. **一块表面恰好一口清屏**，由 Surface 持有；呈现表里的第一条才有颜色/深度清屏，其余条声明"不参与"
   （默认如此，且由 Surface 保证）。后端按 attachment 只应用第一条公告这一事实，从此变成"由 Surface 保证
   只有一条"，而不是"靠顺序祈祷"。
   **已落地（M2b，2026-10-11）**：Surface 决定**谁**清（第一条），**值**留在那条 pass 上（`clearColor` /
   `shouldClearDepth` 不复制一份 —— 见 §3.2.4 下面"为什么没有 `Surface::setClear`"）。
3. **投影只有一个算处、一条传递链**：aspect 在**后端**那一处算（`RenderBackend::viewAspect()`），
   画面（view block）与剔除 / picking 都**读同一个数** —— 各自现推就是"看到的不等于剔掉的"那类错。
   **已落地（M2c，2026-10-11）**：`Camera` 不再持 aspect（`aspectRatio()` 删除、`projectionMatrix(aspect)` 现算），
   剔除走 `RenderPass::execute` 问后端、view block 走后端公告给 `FrameRecorder`，picking 走
   `screenToWorldRay(pos, aspect)`（操纵器自己持 `viewportAspect()`）。

#### 3.2.4 API 草案（贴仓库三条既有规矩：Qt 风格 getter / 赢家才叫 Override / 值语义 + 幂等）

```cpp
// graphics：一块表面（交换链 + 唯一一口清屏 + 呈现表）
class RenderSurface {
  public:
    explicit RenderSurface(intrusive_ptr<RenderEngine> engine);

    // —— 呈现（分屏就是多调几次；每条带自己的矩形；顺序 = 绘制序） ——
    /** @brief 让这个视图呈现这块表面（幂等；`rect` 默认铺满 —— 分屏就是每条给一块）。 */
    bool present(SceneView& view, const NormalizedViewport& rect = {});
    /** @brief 宿主自建管线接手：这条 pass 是这块表面的最后一步（视图的默认窗口管线不会被建）。 */
    bool present(intrusive_ptr<RenderPass> pass);
    bool dismiss(raw_ptr<SceneView> view);
    bool dismiss(raw_ptr<RenderPass> pass);
    /** @brief 这条呈现的矩形（改它**不重建任何管线** —— 几何，不是配方）。 */
    bool setViewport(raw_ptr<SceneView> view, const NormalizedViewport& rect);
    std::vector<Presentation> presentations() const;           // 按引擎执行序：{ view, pass, rect }
    Presentation presentation() const;                         // = 第一条（也就是清屏那条）

    // —— 清屏：这块表面唯一的一处（不再散在 pass 上） ——
    // **落地时否决了这组 API**（M2b，2026-10-11），理由见下面那段。
    // void setClear(const ClearOptions& options);             // {enabled, color, depth}
    // const ClearOptions& clear() const;

    // —— 默认输入路由：按矩形自动回答"这一点归谁"（宿主仍可覆盖） ——
    raw_ptr<SceneView> viewAt(int x, int y) const;
};

// graphics：相机 = 观看 + 投影参数，**不含 aspect**（③）—— 因此可跨矩形复用
// **已落地（M2c，2026-10-11）**：
// Camera::setProjectionMatrixAsPerspective(fovy, near, far)   ← 去掉 aspect 形参
// Camera::projectionMatrix(double aspect) const               ← 投影按 aspect 现算（正交忽略该参数）
// Camera::aspectRatio()                                        ← 删除（读者见上表五处）
// Camera::screenToWorldRay(screenPos, aspect)                  ← 多收一个 aspect（提案漏算的第五处）
// Camera::setProjectionMatrixAsOrtho(left, right, bottom, top, near, far) ← 不变（正交盒自带比例）

// graphics：后端回答"这条 pass 画进的那张画面是什么形状"（唯一推导处）
// RenderBackend::viewAspect() const -> double                   ← 剔除与 view block 都读它

// graphics：一条只读查询（3.2.1 A4 缺的那块；同时是"我记的那条 pass 还在不在"的存活查询）
// RenderEngine::passOrder(raw_ptr<RenderPass>) const -> std::optional<int>

// appfw：控件暴露它那块表面（不加任何"配方"API —— §5 的"零新增配置 API"仍成立）
// RenderControl::surface() const -> raw_ptr<RenderSurface>
```

**不再是 API 的东西**（此前两稿有、这一稿取消）：`Pane` / `addPane` / `removePane` / `panes()` /
`Pane::setClearEnabled` / `Camera::setViewport` —— 矩形放"呈现条目"、清屏与顺序放 Surface 之后，它们都没有必要。

**为什么没有 `Surface::setClear(ClearOptions)`（M2b 落地时否决，2026-10-11）**：`{enabled, color, depth}` 这三样
今天住在 `RenderPass` 上，而且是**一份真相**（`announceClear()` 就从那儿读）。在 Surface 上再放一份，立刻就是
"同一个状态两处"——正是 §3.1（`resolveRenderState`）与 `appfw-render-view.md` §7 不变量 15 要消掉的那种病，
而且还得再造一套"表面设了 / 没设过"的三态与"以哪个为准"的优先级。所以 M2b 只把**决定**搬过去：**谁清**由
呈现表的第一条说了算（其余条被 `setClearEnabled(false)` 明确关掉，不再"公告了却被静默丢弃"），**清成什么**
仍由那条 pass 说了算（前向窗口 pass 的默认灰、延迟那条 `present` 屏 pass 的"我压根不清"都原样保留）。
要换背景色的宿主照旧改那条 pass 的 `setClearColor`（自建 pass 时本来就有这个入口），没有新增配置面。

拟新增单元（M1/M2a/M2b/**M2c**/M4 已落地，其余**提案，未落地**；不进引用门禁）：

```
viz/graphics/src/
    RenderSurface.cpp           表面：呈现表（view + 矩形）+ 唯一清屏 + present/dismiss      ← M2a/M2b
    Camera.cpp                  (改) aspect 出相机：投影按 aspect 现算，参数才是状态           ← M2c
    Scene.hpp/.cpp              (改) 剔除/收集多收一个 aspect（帧内 memo 以 view-projection 为键）← M2c
    sdk/.../RenderBackend.hpp   (改) 新增 viewAspect()：唯一一处"这张画面什么形状"            ← M2c
    RenderPass.cpp              (改) 收集命令前问后端的 viewAspect()（看见的 = 剔掉的）        ← M2c
    backend/FrameRecorder.cpp   (改) setViewAspect()：view block 用公告来的形状建投影          ← M2c
    OrbitCameraManipulator.cpp  (改) 自持 viewportAspect()，不再读相机
appfw/src/gui/
    SurfaceWindow.cpp          (改) 用 control->surface() 取代 view->ensureWindowPass()
```

**兼容与退路**（大改也要给老代码一条活路）：`SceneView` 保留成**单矩形便捷门面** —— `view->setViewport()`
转发给"它所在的那条呈现的矩形"（无表面时留在自己身上，测试与无头宿主照旧直接用），`view->camera()` 仍是
"我这台相机"，`view->ensureWindowPass()` 变成"把这条呈现加进表面"。这样"三五行出画面"的写法**一个字不用改**，
而新代码可以显式管多台相机（分屏）。

#### 3.2.5 分片与判据（M2 拆四步，每步独立可验收）

| 片 | 内容 | 验收判据 |
| --- | --- | --- |
| **M2a** 谁呈现（**已落地 2026-10-11**） | `RenderSurface`（"这块表面的最后一步是谁"：呈现表 + `present/dismiss/presentations`）+ `RenderEngine::passOrder()` 只读查询 + 删掉 `hasWindowPass(camera)` 与 `ensureWindowPass()` 的全局让位规则 + `RenderControl::surface()` / `SurfaceWindow::surface()`；demo 显式 `present(pipeline->windowPass())` | ①默认路径行为不变（`test_gui` 253 绿）②宿主自建主 pass 且**未**声明 ⇒ 视图的默认窗口管线**会**建；显式声明 ⇒ 不建（`RenderSurfaceTest` 两条 + `SceneViewTest.EnsureWindowPassDoesNotStandDownForSomebodyElsesPass`）③`applyOverlayIntent` 行为不回退（`AnApplicationWindowPassKeepsTheOverlaysToItself` 改写后仍钉住）④demo 冒烟零 error/warn |
| **M2b** 清屏归表面（**已落地 2026-10-11**） | 表面决定**谁**清：呈现表第一条保留自己的清屏，其余条被 `setClearEnabled(false)`（不再是"公告了却被后端静默丢弃"）；每条呈现自己的"ask"（清不清）被记住，撤掉第一条时下一条恢复；视图重建后用友元回调让表面把决定落回新建的 pass 上。**没有** `Surface::setClear` —— 值仍住在 pass 上（一份真相），理由见 §3.2.4 那一段 | ①一块表面里只有一条呈现 pass 带清屏（其余条 `isClearEnabled()` 为 false）②撤掉第一条 ⇒ 清屏回到剩下那条自己的 ask ③延迟配方的 `present` 屏 pass（自己不清）不会被"第一条"逼着清 ④视图重建（`setOptions`）之后仍然只有一条 ⑤第一次呈现 → 下一条挤进来 → 第一条走掉，三种事件下不变式都成立；⑥表面先死/视图先死都不留悬垂 | M2a | 中 |
| **M2c** ③：**aspect 出相机**（**已落地 2026-10-11**）（正确性 + 复用） | `Camera` 只持观看与投影**参数**（`setProjectionMatrixAsPerspective(fovy, near, far)`、`projectionMatrix(aspect)`、`aspectRatio()` 删除、`screenToWorldRay(pos, aspect)`）；**aspect 只有一处推导** —— `RenderBackend::viewAspect()`（pass 的 viewport，否则目标/表面尺寸），剔除（`RenderPass::execute` 收集前问一次）与 view block（`FrameRecorder::setViewAspect`，vsg 在每次 `render()` 前公告同一个数）读同一个；`Scene::collectRenderCommands*(camera, aspect)`（帧内 memo 以 view-projection 为键，天然区分两种视锥）；`OrbitCameraManipulator` 自持 `viewportAspect()`，`onResize` 不再往相机里塞 aspect | ①同一台相机进两块不同矩形：两张画面各自的 aspect 都对（`CameraTest.TheProjectionIsBuiltForTheShapeTheCallerNames`：同一相机两次投影只差水平缩放；`SceneTest.OneCameraInTwoRectanglesIsTwoPicturesAndTwoWalks`：同一帧两种形状 = 两次走树、两份结果）②剔除与画面一致：**同一台相机**、一条 1600×600 的 pass 与一条 600×600 的 pass，前者画出画面外侧那颗、后者剔掉（`RenderEngineTest.EachPassCullsWithTheShapeOfItsOwnRectangle`；变异：`execute()` 里写死 1.0 ⇒ 失败）③view block 与 pass 的 viewport 同源：`FrameRecorderTest.TheViewBlockIsProjectedForTheShapeTheScopeAnnounced`（公告 2.0 ⇒ 快照投影按 2.0；下一条 pass 回到默认，不继承上一条；0/NaN 被拒）④正交（阴影）行为不变：`CameraTest.TheOrthographicWindowIsKeptWhole` + `TheProjectionIsBuiltForTheShapeTheCallerNames` 后半（正交忽略 aspect）⑤`test_vsg` 405 绿（后端只看矩阵，不受影响）⑥全套 `test_graphics` 330 绿（92 处 `collectRenderCommands` 逐处补上形状，原判据不动） | M2a/M2b + M4（先有 dump） | 大 |
| **M2d** ③：**矩形放呈现条目** | `Surface::present(view, rect)` + `Surface::setViewport(view, rect)`；`SceneView::setViewport` 转发（无表面时留在自己身上）；旧的"矩形由视图持有"退成单矩形门面 | ①分屏：两条呈现各占半张、各自 aspect 正确（同一台相机也允许）②`SceneView::setViewport` 老写法仍可用③改矩形**不重建**任何管线（`pipeline()` 句柄不变）④`Surface::viewAt(x,y)` 与矩形一致（默认输入路由） |
| **M2e**（并入 §5 的 **M5**） | 默认输入路由 = 按矩形推（`Surface::viewAt(x, y)`，`setInputRouter` 可覆盖）+ 命令的焦点视图 | 见 §5 M5 行 |

**与其它三条的接口**：M2a 只依赖"引擎能说出 pass 的 order"（3.2.1 A4）；M2b 依赖 M2a 的呈现表；
M2c/M2d 与 M1 无冲突（模式换链条是"视图的配方"）；M4（FramePlan dump）应排在 M2c 之前，
好让"这一帧画谁、清在哪"能一眼 dump 出来。

**风险**：

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| M2c/M2d 动的是公开形态（`setViewport` 的归属、`aspectRatio()` 的消失） | 已写下的宿主代码要改 | `SceneView` 保留转发门面 + 老用例逐个核对；破坏性写进 `.ai` 与提交信息 |
| "aspect 出相机"= 投影从状态变参数 | 直接读 `camera.projectionMatrix()` / `aspectRatio()` 的调用点会编译不过 | 编译期暴露（不留隐式重载）；四处调用点一次改完，且**剔除的帧内 memo 必须把 aspect 进键**，否则同一相机两块矩形会串味 |
| 清屏从 pass 搬到表面 | 后端 `ClearPolicy` 仍要按 pass 公告 | Surface 只保证"唯一一条公告"，后端契约不动（零后端改动） |
| 改动面比 M1 大 | 回归风险 | 四步各自独立可验收、各自可回退；M2a/M2b 不动几何 |

**行业对照（本片的核心依据）**：**VTK `vtkCamera` 不含 aspect**（投影矩阵按 renderer 的 viewport 与窗口尺寸现推 ⇒ 一台
相机可以喂多个 renderer，这是本节 ③ 的直接依据）、Qt3D `QRenderSurfaceSelector` + `QViewport` 结点（矩形是"画到哪"这一轴
上的一环）、Unreal `FViewport`/`FSceneViewport`（对象持输入；投影参数在 `FSceneView` 里现算）、Filament
`Renderer::setClearOptions` + `View`（camera + viewport + scene + clear）、Unity `Camera.rect`（矩形与 aspect 一起锁在
相机里 ⇒ 相机整台不可复用）、OSG `Camera::setViewport`（同 Unity 口径）。**共同点**：清屏归表面/renderer，
矩形属于"画到哪"这一轴。**分歧**：矩形放相机（Unity/OSG）还是放表面那一侧的格子（VTK/Unreal/Filament/Qt3D）。
**本片选后者** —— 理由不是"少一个类"，而是**矩形放相机就会把 aspect 一起锁进相机**：view matrix 只依赖眼睛 ⇒
可复用；projection matrix 还依赖 aspect ⇒ 不可复用。把 aspect 从相机里请出去，矩形才有地方放，相机才真的能复用。

"视图"只描述怎么画 —— 这是本次大改要落到的位置。

### 3.3 调试覆盖在几何阶段 —— **已落地（M1，2026-10-11）**

**目标**：`Options::Mode`（平面/无光照/法线/线框）作用在**几何阶段**，不贴在最后一步。

**落地时改了这条设计的一处（原稿写错的地方）**：原稿说"Deferred 的 G-buffer program 也要吃
`VINE_FLAT/VINE_UNLIT/VINE_NORMALS`"（当时把它记成 M1 的验收判据 ④）—— **做不到也不该做**：
延迟配方的画面是"全屏光照"那条 pass 产出的，G-buffer 里写什么颜色都会被光照再算一遍，所以把四个 define 塞进
G-buffer 程序只会得到"被光照过的无光照画面"。真正的规则是：**非 Lit 档期间，链条按前向建**（`effectivePath()`，
`Options::path` 字段不动），几何直接进画面 —— 于是四档在两条配方上都能画出来，且**不需要任何新的 shader 变体**。
代价（落地时才明确）：延迟配方**进出**非 Lit 档是一次重建（链条变了）；链条内换档仍零重建。

**影响面（实际）**：`SceneView::applyModeToWindowPass()` → `applyModeToGeometryPass()`（去掉"路径不匹配就报
`UnsupportedRequest`"那一支与 `mode_unavailable_reported_`）；`setOptions()` 的结构判据从"配方里的 path 变了"改成
"**生效的链条**变了"。`GlslCompileTest` / `ProgramAbiTest` 的矩阵**不动**（没有新的 define 变体）。

**判据（已达成）**：见 §5 M1 行。
**收益（已取得）**：Deferred 与 Forward 四档观感一致，"延迟路径兑现不了"这条诊断、它的文档与它的用例整块删除。

### 3.4 Pass 描述符 + 每帧 FramePlan（唯一的大改）

**目标**：pass 是一份描述符（stage / tag / camera / target / inputs / outputs / state），图每帧编译成 FramePlan；
transient 目标分配、clear/load op、依赖排序由编译负责；调试时能 dump 出这一帧的完整表格。

```
graph.add(PassDesc{ .stage = Overlay, .tag = Hud, .camera = cam, .writes = backbuffer, .state = { ... } });
FramePlan plan = graph.compile();
```

**影响面**：`RenderPass` 退化成一个具体的执行结点（或保留为"老式 pass"以兼容 `engine->addPass` 逃生口）；
`FpsOverlay`、`AxisGizmo`、`ScreenPass` 变成"带 tag 的普通 pass"；`wiringRevision()` + `validateWiring()` 整块删除
（声明每帧重建，谁都不需要"通知我你变了"）。
**风险与理由**：这是唯一会碰所有 pass 的改动，收益（自动资源/自动 barrier/可观测）只有在 pass 数、动态图
需求或显存压力上来之后才兑现 —— **所以它等触发条件**（§5 的 M7）。
**行业对照**：Qt3D 的 declarative framegraph、Unreal RDG、Filament FrameGraph。

## 4. 上层：命令上下文与分屏布局

- **命令目标从全局槽换成显式上下文**：`ViewContext{ window, focus_view, selection }`，由命令参数传入或由
  per-window 的"焦点视图"服务提供；`MainWindow::setPrimaryRenderControl()` 降级成"窗口自己的默认 3D 视图"
  （仍可用，但不再是唯一通路）。对照：Blender 的 `context.area/region`、Unreal 的 `GEditor->GetActiveViewport()`。
- **分屏布局是一等公民**：splitter 树（每格一台**相机** + 它自己的 viewport 矩形 + 自己的输入捕获 + 可选 HUD）。
  今天 `setViewport` + `setInputRouter` 已经把"一个表面多视图"打通，缺的是**布局与生命周期的所有者**
  （谁 split、谁销毁、焦点在哪一格）。输入路由从"宿主写回调"进化为"按矩形自动推"（`Surface::viewAt()`），
  宿主只在需要特殊规则时覆盖。

## 5. 迁移切片（每片独立可落地、独立验收）

| 片 | 内容 | 验收判据 | 依赖 | 代价 |
| --- | --- | --- | --- | --- |
| **M1** | §3.3 调试覆盖挪到几何阶段（**已落地 2026-10-11**） | ①Deferred 配方 + 非 Lit 档 ⇒ 链条按前向建：没有离屏目标、画面那条 pass 带对应 program 覆盖、线框带 Line 覆盖（`test_graphics` `ANonLitModeOnADeferredRecipeBuildsTheForwardChain`；变异：`effectivePath()` 恒等 ⇒ 失败）；②Forward 四档行为逐字不变（既有用例不动）；③回 Lit ⇒ 链条切回延迟（G-buffer 回来了）；④同一链条内换档零重建（`SwitchingBetweenNonLitModesOnADeferredRecipeKeepsTheChain`），三次切换不留残留 pass（与同配置新视图对照）；⑤那条 `UnsupportedRequest` 诊断、它的用例与文档措辞全部删除；⑥真机烟雾：应用 Deferred 默认路径启动照旧 | 无 | 小 |
| **M2a** | §3.2.5 谁呈现（**已落地 2026-10-11**）：`RenderSurface` + `RenderEngine::passOrder()` + 删掉 `hasWindowPass(camera)` 与全局让位规则 + `RenderControl::surface()`；demo 显式 `present(pipeline->windowPass())` | ①默认路径行为不变（`test_gui` 253 绿）；②宿主自建主 pass 且**未**声明 ⇒ 视图的默认窗口管线**会**建；显式声明 ⇒ 不建；③`applyOverlayIntent` 行为不回退；④demo 冒烟零 error/warn | 无 | 中 |
| **M2b** | §3.2.5 清屏归表面（**已落地 2026-10-11**）：呈现表第一条保留自己的清屏，其余条关掉（`setClearEnabled(false)`）；每条呈现的 ask 被记住，第一条走掉时下一条恢复；视图重建后由视图回调表面重放（`friend`）。没有 `Surface::setClear`（值仍住那条 pass 上，一份真相） | ①一块表面里只有一条呈现 pass 带清屏；②撤掉第一条 ⇒ 清屏回到剩下那条；③延迟配方那条"自己不清"的 pass 不被逼着清；④视图重建后不变式仍在；⑤视图/表面任一先死都不悬垂 | M2a | 中 |
| **M2c** | §3.2.5 **aspect 出相机**（**已落地 2026-10-11**，**破坏性**）：`Camera` 只留观看 + 投影参数（`projectionMatrix(aspect)` / 删 `aspectRatio()` / `screenToWorldRay(pos, aspect)`）；**一处推导** = `RenderBackend::viewAspect()`，剔除与 view block 读同一个数；`collectRenderCommands*(camera, aspect)`；操纵器自持 `viewportAspect()` | ①同一相机两条呈现两块矩形：两边 aspect 都对（`TheProjectionIsBuiltForTheShapeTheCallerNames` + `OneCameraInTwoRectanglesIsTwoPicturesAndTwoWalks`）；②剔除与画面一致（`EachPassCullsWithTheShapeOfItsOwnRectangle`：宽 pass 画外侧那颗、方 pass 剔掉；变异：写死 1.0 ⇒ 失败）；③view block 与 pass 的 viewport 同源（`TheViewBlockIsProjectedForTheShapeTheScopeAnnounced`；变异：固定 1.0 / 不在 `beginPass` 复位 ⇒ 各自失败）；④正交（阴影）行为不变；⑤`test_vsg` 405 绿、`test_graphics` 330 绿 | M2a/M2b + M4（先有 dump） | 大 |
| **M2d** | §3.2.5 **矩形放呈现条目**：`Surface::present(view, rect)` / `setViewport(view, rect)`，`SceneView::setViewport` 转发 | ①分屏两条呈现各占半张、aspect 各自正确；②`SceneView::setViewport` 老写法仍可用；③改矩形不重建管线；④`viewAt(x,y)` 与矩形一致 | M2a | 中 |
| **M3** | §3.1 `resolveRenderState` 单点 | ①逐项状态快照：内容/pass/视图三层各自设过同一项时，最终值 = 优先级表；②`depthExplicit` 类旗标删除后行为不变；③诊断能说出"这一项来自哪一层"；④`graphics-state.md` §4.1 的表从"规则"改成"命名习惯"（文档验收） | 无 | 中 |
| **M4** 观测：FramePlan dump（**已落地 2026-10-11**） | `FramePlan.hpp`（`PassPlan` 每 pass 的 order/name/content/camera/target/enabled/clear/viewport/inputs/output + 上一帧的 `record_micros`）+ `RenderEngine::framePlan()`（只读、按执行序）+ `FramePlan::describe()`（一行一 pass 的 key=value 表）+ demo 开关 `VINE_FRAME_PLAN=1`（attach 那一帧之后打一行表格） | ①一条 API/开关打印这一帧的表格（`describe()`；demo 真机输出见 `.ai/memory/graphics.md`）②`test_graphics` 两条改用结构断言替换 `passCount()`/指针身份（`SceneViewViewportTest.TheRecipeDecidesWhichOverlayPassesExist` 改断言 pass 名字序列；`RenderPipelineBuilderTest.ForwardPathBuildsWindowPass` 改断言 plan 那一条的全部事实）③dump 与真实执行一致：`RenderEngineTest.TheFramePlanIsWhatTheFrameActuallyRan` 用记录型后端对照"plan 里 enabled 的那些 pass，按序 == 后端被开过 scope 的那些"④耗时是"上一帧"的且说清楚（`TheFramePlanCarriesWhatEachPassLastCost`：慢 pass ≥ 1000 µs、没跑的 pass = 0） | 无 | 小 |
| **M5** | 分屏布局 + 按矩形推的默认输入路由 + `ViewContext` 命令目标（= §3.2.5 的 M2e） | ①一个窗口 split 成两块分屏：各自相机/输入/HUD 互不干扰（用例）；②"显示模式"五条命令在多分屏下作用于**焦点视图**（同一命令两块各来一遍）；③`setPrimaryRenderControl` 仍可用但不参与焦点解析 | M2c/M2d | 中 |
| **M6** | Device / Session 分离：一设备 N 会话 | ①同一模型开两个窗口只上传一份几何（计数/内存断言）；②一个窗口销毁不带走另一个的会话；③`SessionLease` 类型上禁止"会话先死在租约前"；④R2 的"临时加引用"约定删除（该用例改写仍过） | M2 | 大 |
| **M7** | Pass 描述符 + 每帧 FramePlan（§3.4） | ①`wiringRevision()`/`validateWiring()` 删除后全部套件绿；②新增一个 pass = 加一条描述符（无新子类）；③transient 目标由编译分配（显存/目标计数断言）；④FramePlan dump 与 M4 同形 | M4（观测先有） | 大 |
| **M8** | `test_vsg` 的 **SDK 级设备夹具**（能用 SDK 建引擎/视图/管线并在真设备上探像素） | ①至少一条 view-mode 的**像素**断言（Deferred 配方 + 线框 ⇒ 画面里真的有边；用 `PixelProbe`，同一帧的 forward/line 两版对照）；②夹具本身带一条"没有视图也能渲染"的最小用例，证明它不是为某一条用例特制的 | 无 | 中 |

**M1 验收判据的边界（诚实记账）**：M1 只做**结构判据**（`test_graphics`，设备无关）+ 应用烟雾，**没有**设备级像素
判据 —— `test_vsg` 今天不碰 SDK 层（`RenderEngine` / `SceneView` / `RenderPipelineBuilder` 一次都没用），它直接对着
vsg 内部测（`PixelProbe` 在它那里是配合 vsg 内部管线用的）；要"像素上真的画出线"得先给它一个 SDK 级设备夹具，
那是独立的一片（M8）。这是 M1 与 M8 的分界：**M1 保证"该画的 pass 在画"，M8 保证"画出来的像素对"**。

**触发条件**（不满足就不做，避免为"将来"付费）：

- M6：命中"同一内容要多窗口 / 设备启动开销被窗口数乘 / 视图数 > 4"之一。
- M7：命中"pass 数或动态图需求上来 / 需要 transient 资源与自动 barrier / M4 的 dump 已经不够用"之一。
- M1 / M2a / M2b / M2c / M3 不需要触发条件：它们消掉的是**今天就在付**的混乱（一条诊断 + 一条全局让位规则 +
  一口没有 owner 的清屏 + 一张命名表 + "相机的 aspect 和它被画进的矩形不一定一致"）。
- M5：命中"要分屏（两块以上）/ 编辑器要让命令作用于焦点视图"之一 —— 它动输入路由与命令目标，
  没有真实需求不该先付。
- M8：命中"观感（view mode / HUD 摆位 / 清屏结果）成为争议点，或要让 CI 守住它"之一 —— M1 因此只到结构层为止。

## 6. 不变量与兼容策略

1. **公开 API 不乱动**：M1–M3 都是"改内部归属 + 加窄口"，`RenderControl` / `SceneView` 的既有入口保持可用；
   新增入口优先（老入口标"便捷/兼容"，不删）。
2. **三层配置语义不变**：配方变更才重建、开关永不重建、几何只改取值（`appfw-render-view.md` §7 不变量 11–16
   继续成立）。
3. **逃生口永远在**：任何一层都能被绕过（`engine->addPass`、自写 shader、自建 builder）；图/描述符层不许
   变成"必须用它"。
4. **每片一次验收，且用现有门禁**：`test_graphics`/`test_gui`/`test_vsg` + 五个 gate；新增能力必须有
   无头（`VINE_VSG_*`）或结构断言，不靠人眼看。
5. **文档同步是验收的一部分**：动到 `appfw-render-view.md` / `graphics-state.md` / `appfw-render-surface.md`
   的结论，就同一提交里改（这三份是"现状为真"的文档）。

## 7. 行业对照与备选

| 本设计的取向 | 行业同款 | 差别 |
| --- | --- | --- |
| **清屏归表面、矩形归呈现条目、aspect 归渲染时**（本片口径） | VTK（`vtkCamera` **不含 aspect**，投影按 renderer 的 viewport 现推 ⇒ 一台相机挂多个 renderer）、Filament（`View` = camera + viewport + scene + clear）、Unreal（`FSceneView` 含投影、参数层可共享）、Unity（`Camera.rect` ⇒ 整台不可复用，要复用就复制相机） | 我们今天是"视图变出窗口管线、矩形挂在视图上、**aspect 存在相机里**"：同一相机两块矩形必然静默画错 aspect，且清屏没有 owner（谁先公告谁赢）。**view matrix 可复用、projection 不可 —— 差别只在 aspect**，所以把 aspect 从相机里请出来，复用与"投影不过期"一起得到 |
| 声明式图（M7） | Qt3D framegraph、Unreal RDG、Filament FrameGraph | 只有它在 pass 数/动态图需求上来后才划算 |
| 声明式图（M7） | Qt3D framegraph、Unreal RDG、Filament FrameGraph | 只有它在 pass 数/动态图需求上来后才划算 |
| 状态解析单点 | Unreal material override、Unity `RenderStateBlock`、Filament `MaterialInstance` | 我们先有"命名规则"，再有函数 |
| 能力查询 + 上下文式命令目标 | Vulkan feature 查询、`GEditor->GetActiveViewport()`、Blender `context.area` | 我们从"读日志"起步 |
| 一设备多交换链 | Filament / VTK / Unreal | 刻意推迟（隔离优先），但缝现在留 |

**备选与否决**：

- *否决：现在就整块重写（图 + 设备 + 视图一起换）。* 理由：四条摩擦里三条能用窄口消掉；整块重写会把
  "已经对的"（配方/几何/开关、让位规则、文档门禁）一起推倒，代价与风险不成比例。
- *否决：把 HUD 抽出 `Pipeline`，改成"引擎级叠加层"。* 理由：HUD 的可见性/尺寸/比例归视图是对的（多视图各不同），
  问题在**归属**（重建后补状态）而不是位置；M2/M4 解决它。
- *保留观察：分屏与文档的关系。* 一个文档开两块分屏是"两套相机看同一份数据"还是"两份不同数据"，今天按前者
  设计（视图按文档、内容可共享）；M5 之前不动它。

## 8. 风险登记

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| M3 一次性改所有状态消费点 | 静默少一项 | 逐项快照对照 + 来源枚举进诊断 |
| M2 清屏/顺序的所有者转移 | 分屏出现"半张脸被清掉" | presenter 的规则表 + 无头像素判据（`VINE_VSG_OFFSCREEN_MULTISLOT` 已有先例） |
| M6 触到后端抽象 | 后端全部要改会话语义 | 只在触发条件命中后做，且先做"设备只读 + 会话多实例"两半 |
| M7 触到所有 pass | 大范围回归 | 强制 M4 先落地，用结构 dump 做回归对照 |
| 提案本身腐烂（代码变了提案没变） | 读者被骗 | 本文标记"提案，未落地"；落地一片就把该片移进现状文档并从本文删除 |
