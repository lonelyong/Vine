# graphics 渲染栈的重新设计提案（next，2026-10-11）

**状态：M1 已落地（2026-10-11）；M2（呈现与分屏）已按三维规范细化为可执行的四步；其余是提案，未落地** ——
本文回答"如果重新设计这一套会怎么切"，并把每条切成能独立验收的迁移片。落地的部分按本文自己的规矩**以现状文档为准**
（`appfw-render-view.md` §4.1、`.ai/memory/graphics.md`），这里只留下"原稿哪里说错了"的记录。**本文允许大改**：
§3.2 是按行业口径重写的（`Surface` / `Pane` / `View` 三分），不迁就当前实现。

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
| 默认窗口管线**让位**给宿主自建主 pass | 兜底而非竞争，宿主永远赢 |
| 文档 → 视图 → 控件 → 面 的 1:1 链 + `DocumentView::deactivate()` 的收回约定 | 隔离干净、视角按实例保住、切文档回默认态天然正确 |

**重切四条**（编号即 §3 的小节号）：

| # | 一句话 | 代价 | 立刻消掉什么 |
| --- | --- | --- | --- |
| 3.3 | 调试覆盖（线框/法线/无光照/平面）挪到**几何阶段**，不贴在窗口 pass | 小 | Deferred 上四档全部可用（今天报 `UnsupportedRequest`）；窗口 pass 变成纯合成 |
| 3.2 | **呈现与分屏**：`Surface`（唯一清屏 + 呈现）→ `Pane`（矩形 + 相机 + 内容）→ `View`（配方/管线）；`SceneView` 退成单 pane 门面 | 大（唯一动公开形态的一刀） | 全局让位规则 `hasWindowPass(camera)`、靠发言顺序的清屏、`SceneView` 三件事捏一起 |
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
- **F2 呈现是运行时争夺**。`SceneView::ensureWindowPass()` 用 `engine_->hasWindowPass(camera_)` 决定"我要不要
  建默认窗口 pass"；`RenderPipelineBuilder` 里还得写注释解释"render target 为空的 pass 也算占位"。
  于是"谁负责最后一步"是**发现**出来的，不是声明的；配套补丁就是"重建之后要把使用者关掉的档再关回去"
  （`applyOverlayIntent()`）。*根因*：视图既描述"场景长什么样"，又偷偷拥有"最后一步"。
- **F3 调试覆盖贴在最后一步**。`applyModeToWindowPass()` 把"内容怎么着色"写在**窗口 pass** 上，于是
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
  这一个槽 + `activate()/deactivate()` 成对发布；单窗口成立，多窗口/多 pane 含糊（§12 已记）。
  *根因*：命令缺一个**显式上下文**，于是用全局槽代替。
- **F7 生命周期靠被动发现**。`QTimer::singleShot(0, …)` 兜底 + `activate()` 里重试 + `Pending/Presenting/Failed`
  三态，本质是"没有任何一方声明自己 ready"的补丁；能力（能不能画线？能不能 MRT？）只能靠
  `reportEngineProblem(UnsupportedRequest)` + 人看日志。*根因*：宿主与会话之间没有事件协议，也没有能力查询面。

## 2. 目标形态

```
L0  设备 / 会话     RenderDevice（能力查询 + 资源缓存）→ Session（surface + swapchain）；一设备 N 会话
L1  图 / 帧         PassDesc（描述符）→ FramePlan（每帧编译：顺序、目标、输入、clear/load、barrier、transient 分配）
L2  配方           buildRecipe(graph, options)：forward / deferred / shadowed …（今天 RenderPipelineBuilder 的位置）
L3  视图 / 面板     Pane（矩形 + 相机 + 内容 + 清屏开关）+ View（配方 path/mode/HUD + 默认管线）+ 交互器
L4  呈现 / 宿主     Surface（交换链 + **唯一一口清屏** + present）+ 输入路由 + HUD 作为带 tag 的普通 pass
L5  应用 / 文档     文档 / 视图 / 控件链；命令带 ViewContext；pane 布局归宿主
```

拟新增的单元（**提案，未落地**；不进任何门禁的引用检查）：

```
viz/graphics/src/
    RenderDevice.cpp            设备：能力查询、资源缓存、会话创建
    RenderSession.cpp           会话：surface/swapchain 生命周期，Lease 发放
    PassDesc.cpp                描述符 → 图结点（声明式，每帧可重建）
    FramePlan.cpp               编译后的帧计划 + dump（pass 顺序/目标/输入/状态/耗时）
    RenderStateResolver.cpp     resolveRenderState：内容 → pass → 视图覆盖的单点优先级
    RenderSurface.cpp           表面：pane 表 + 唯一清屏 + present/dismiss（§3.2）
appfw/src/gui/
    Pane.cpp                    (矩形 + 相机 + 内容 + 清屏开关) 的声明对象（§3.2）
    ViewContext.cpp             命令的显式目标（window + pane + selection）
```

**四条横切契约**（每一条都对应一个 F）：

| 契约 | 目标 | 取代 |
| --- | --- | --- |
| 所有权 | 一律"父持有子"：Device → Session → 资源/pass；借用一律 `Lease`/句柄，且**会话必须晚于它的租约销毁**（在类型上表达，不靠约定） | R2 的手工加引用 |
| 状态 | 一个函数决定一个 draw 的最终状态，输入是 (命令, pass, 视图覆盖)，优先级是一张表 | 分散的 setter + 命名规则 |
| 能力 | `session->supports(feature)` / `featureLevel()`，可查询、可在建管线之前问 | 事后 `reportEngineProblem` + 文档 |
| 观测 | 每帧一份 FramePlan（可 dump、可断言结构），替代 `passCount()`/指针身份当替身 | 隐式结构 |

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

### 3.2 呈现与分屏：**Surface / Pane / View**（三维规范口径，允许大改）

这一条是四刀里最大的一刀，也是唯一"要动公开形态"的一刀。**先摆事实**（每条都带证据），再给行业口径与目标 API。

#### 3.2.1 今天的两条隐式规则（事实）

**A1「谁呈现」是全局发现式。** 视图建默认窗口管线之前先问引擎："有人已经用我这台相机呈现了吗？"

```
// SceneView::ensureWindowPass()（当前实现）
if (engine_->hasWindowPass(camera_.get())) { return; }   // 有人占了 ⇒ 我让位
```

| 后果 | 证据 |
| --- | --- |
| 判断是**全局**的：任何"这台相机 + render target 为空"的 pass 都算占位，没人记"是**谁**占了这块表面" | `RenderPipelineBuilder.cpp` 里专门写了注释解释"null target 也算，所以 `hasWindowPass(camera)` 为真" |
| **顺序敏感**：`addPass()` 的先后决定谁不建 | 规则读的是当时的引擎 pass 表，没有"谁声明要呈现"这一层 |
| 两台视图共用一台相机（镜像 / 概览 / VR 双眼）⇒ 第二个被**静默跳过** | 同上；`hasWindowPass()` 只是查询，不回答"这块表面上的这一块是谁" |

它同时是一条成文承诺（`SceneView.hpp` 里"an application that built its own main pass…"那段）和 demo 的依赖：
`DemoScene::install()` 建自己的管线（用视图那台相机），靠这条让位规则让宿主那次 attach 不重复建 pass。
即：**"谁呈现"今天没有 owner，只有一条全局查询 + 一个巧合**。

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

1. **矩形属于"表面上的那一块"（pane / viewport），不属于"怎么画"**。VTK 把它放 Renderer、Unreal 放 Viewport、
   我们的邻居 OSG 放 Camera（因为那儿的 Camera 就是 view）。我们今天放在 `SceneView` 上 ⇒ **"同一视图两块 pane"
   做不到**（矩形是共享状态），而分屏在行业里是**每块一台相机**（VTK/Unreal/Unity 都是）。
2. **清屏属于表面或 pane，不属于"最后一条 pass"**。Unity/three.js/Filament 由 renderer 统一持有 clear，
   VTK 是每 Renderer 一个背景色 + 窗口层的"清一次"。**"谁公告第一谁赢、其余静默丢弃"是我们特有的规则。**
3. **"怎么画"不自己抢最后一步**：VTK 的 renderer 不创建窗口，Unreal/Unity 的 view 只描述画什么，
   呈现由 Viewport / 相机的 target 决定。我们的 `ensureWindowPass()` 是反过来的（视图去认领窗口）。

#### 3.2.3 目标对象模型与所有权（大改版）

```
Surface                        交换链 + present + 这块表面**唯一一口清屏**      ← 今天的 SurfaceWindow 后半 + presenter
  ├ Pane                       归一化矩形 + 清屏开关（画到这块表面的哪一块）      ← 新（VTK Renderer 的位置）
  │   ├ Camera                 投影 + 变换 + aspect（由 pane 的矩形与表面尺寸推）
  │   ├ Content (Scene)        画什么 —— 每 pane 一份（可引用同一份，见"共享"一行）
  │   └ Interactor             输入：每 pane 一个，含"按下路由、松开前捕获"       ← 今天 setInputRouter 的进化
  └ View / Recipe              path / mode / HUD 的配方 + 默认管线（"怎么画"）    ← 今天 SceneView 的另一半
```

**切法（一句话）**：今天 `SceneView` 把三件事捏在一起 —— **画到什么矩形上**（pane）、**怎么画**（配方 + 管线）、
**画什么**（相机 + 内容 + 交互）。行业把它们分给三个 owner，我们也分：

| 归属 | 今天在 | 目标 | 理由（行业） |
| --- | --- | --- | --- |
| 矩形 | `SceneView::setViewport` | `Pane::setViewport` | VTK Renderer / Unreal Viewport / Unity Camera.rect |
| 清屏 | `RenderPass` 的开关（谁先公告谁赢） | `Surface::setClear` + `Pane::setClearEnabled` | Unity / three.js / Filament 都由 renderer 持有 |
| 呈现 | `SceneView::ensureWindowPass()` 的让位 | `Surface::present(...)` | VTK RenderWindow / Qt3D RenderSurfaceSelector |
| 相机 | `SceneView::camera()` | `Pane::setCamera()`（**一个 pane 一台相机**） | aspect 是 pane 的函数 ⇒ 相机不能共享 |
| 内容 | `SceneView::scene()` | `Pane::setContent()`（默认仍是自己的） | VTK 可把同一 actor 加进多个 Renderer |
| 配方 + 管线 | `SceneView` | 留在 View（收成显式对象，或让 `SceneView` 退成"单 pane 便捷门面"） | "怎么画"与"画到哪"无关 |

**两条硬规则**（要大改就得先认下来）：

1. **一个 pane 一台相机**。同一台相机同时给两个 pane ⇒ aspect 变成共享状态（今天的 `SceneView` 就是这么坏掉的）。
   行业一律每块一台相机；共享的是**内容**（同一 Scene 加进两个 pane），不是相机。
2. **一块表面恰好一口清屏**，由 Surface 持有；pane 只能声明"我要不要参与"（默认：第一块清，其余不清）。
   后端按 attachment 只应用第一条公告这一事实，从此变成"由 Surface 保证只有一条"，而不是"靠顺序祈祷"。

#### 3.2.4 API 草案（贴仓库三条既有规矩：Qt 风格 getter / 赢家才叫 Override / 值语义 + 幂等）

```cpp
// graphics：一块表面（交换链 + 唯一一口清屏 + pane 有序表）
class RenderSurface {
  public:
    explicit RenderSurface(intrusive_ptr<RenderEngine> engine);

    // —— 分屏 ——
    /** @brief 加一块 pane（归一化矩形，默认铺满）；同一相机不能进两块 pane（见设计硬规则 1）。 */
    Pane& addPane(intrusive_ptr<Camera> camera);
    bool  removePane(raw_ptr<Pane> pane);
    std::vector<raw_ptr<Pane>> panes() const;          // 顺序 = 绘制序

    // —— 清屏：这块表面唯一的一处（不再散在 pass 上） ——
    void setClear(const ClearOptions& options);        // {enabled, color, depth}
    const ClearOptions& clear() const;

    // —— 呈现：谁把这块表面画出画面 ——
    bool present(intrusive_ptr<RenderPass> pass);      // 宿主自建管线：这条 pass 是最后一步
    bool present(View& view);                          // 视图用自己的默认管线（幂等）
    bool dismiss(raw_ptr<RenderPass> pass);
    raw_ptr<RenderPass> presentation() const;          // 现在是谁在呈现（可空）
};

// graphics：表面上的一块（VTK Renderer 位 / Unreal Viewport 位）
class Pane {
  public:
    void setViewport(const NormalizedViewport& rect);  // 唯一矩形真相（今天在 SceneView 上）
    const NormalizedViewport& viewport() const;
    void setCamera(intrusive_ptr<Camera> camera);
    raw_ptr<Camera> camera() const;
    void setContent(intrusive_ptr<Scene> scene);       // 画什么（默认：视图自己的那份）
    raw_ptr<Scene> content() const;
    void setClearEnabled(bool on);                     // 我这一块清不清（默认随 Surface 的规则）
    bool isClearEnabled() const;
};

// graphics：一条只读查询（3.2.1 A4 缺的那块；同时是"我记的那条 pass 还在不在"的存活查询）
// RenderEngine::passOrder(raw_ptr<RenderPass>) const -> std::optional<int>

// appfw：控件暴露它那块表面（不加任何"配方"API —— §5 的"零新增配置 API"仍成立）
// RenderControl::surface() const -> raw_ptr<RenderSurface>
```

拟新增单元（**提案，未落地**；不进引用门禁）：

```
viz/graphics/src/
    RenderSurface.cpp          表面：pane 表 + 唯一清屏 + present/dismiss
    Pane.cpp                   表面上的一块：矩形 + 相机 + 内容 + 清屏开关
appfw/src/gui/
    SurfaceWindow.cpp          (改) 用 control->surface() 取代 view->ensureWindowPass()
```

**兼容与退路**（大改也要给老代码一条活路）：`SceneView` 保留成**单 pane 便捷门面** —— `view->setViewport()`
转发给自己那块 pane，`view->ensureWindowPass()` 变成"把这块 pane 加进表面"（或者在无表面时建一条自用的默认管线，
测试与无头宿主仍能直接用）。这样"三五行出画面"的写法**一个字不用改**，而新代码可以下探到 pane 级。

#### 3.2.5 分片与判据（M2 拆四步，每步独立可验收）

| 片 | 内容 | 验收判据 |
| --- | --- | --- |
| **M2a** 谁呈现（不碰几何/清屏） | `RenderSurface`（先是"presenter"，只管"这块表面的最后一步是谁"）+ `RenderEngine::passOrder()` 只读查询 + 删掉 `ensureWindowPass()` 的全局让位规则 + `RenderControl::surface()`；demo 显式 `present(pipeline)` | ①默认路径行为不变（`test_gui` 全绿）②宿主自建主 pass 且**未**声明 ⇒ 视图的默认窗口管线**会**建；显式声明 ⇒ 不建（两条用例）③`applyOverlayIntent` 行为不回退④demo 冒烟零 error/warn |
| **M2b** 清屏归表面 | `Surface::setClear/ClearOptions` + `Pane::setClearEnabled`；后端那条"只应用第一条公告"从此由表面保证唯一 | ①一块表面只有一条呈现 pass 带清屏（另一条 `isClearEnabled()` 为 false）②撤掉第一条 ⇒ 清屏按 pane 声明回到剩下那条③颜色生效（无头像素判据，走 M8 的夹具；在此之前用结构判据） |
| **M2c** 矩形/相机/内容下探到 pane（**破坏性**） | `Pane` 四件套 + `SceneView` 退成单 pane 门面；**一个 pane 一台相机**成为规则（第二次 `addPane` 用同一台相机 ⇒ 拒绝并说明） | ①同一内容两块 pane（两个相机）：各画各的矩形、各自 aspect 正确②同一视图两块 pane 用同一台相机 ⇒ 明确拒绝（不是静默错）③`setViewport` 老写法仍可用（转发）④相机 aspect / pass 视口 / HUD 尺寸仍由"矩形 × 表面尺寸 × 比例"一处推 |
| **M2d**（并入 §5 的 **M5**） | 每 pane 的输入交互器（`setInputRouter` 的进化：pane 树自己回答"这一点归谁"）+ HUD 按 pane 归属 + 命令的焦点 pane | 见 §5 M5 行 | M2c | 中 |

**与其它三条的接口**：M2a 只依赖"引擎能说出 pass 的 order"（3.2.1 A4）；M2b 依赖 M2a 的 pane/present 表；
M2c 与 M1 无冲突（模式换链条是"视图的配方"，pane 只管矩形）；M4（FramePlan dump）应排在 M2c 之前，
好让"这一帧画谁、清在哪"能一眼 dump 出来。

**风险**：

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| M2c 动的是公开形态（`setViewport` 的归属） | 已写下的宿主代码要改 | `SceneView` 保留转发门面 + 老用例逐个核对；破坏性写进 `.ai` 与提交信息 |
| "一个 pane 一台相机"是新约束 | 现有"两台视图共用一台相机"的用法会失效（今天它本来就静默错） | 拒绝时说明原因（不静默）；文档给"共享内容、不共享相机"的替代写法 |
| 清屏从 pass 搬到表面 | 后端 `ClearPolicy` 仍要按 pass 公告 | Surface 只保证"唯一一条公告"，后端契约不动（零后端改动） |
| 改动面比 M1 大 | 回归风险 | 四步各自独立可验收、各自可回退；M2a/M2b 不动几何 |

**行业对照（本片的核心依据）**：VTK `vtkRenderWindow` + `vtkRenderer`（每块一个，矩形在 renderer 上，交互器按
renderer 路由）、OSG `GraphicsWindow` + `Camera`（`setViewport` / `setClearColor`）、Qt3D framegraph 的
`QRenderSurfaceSelector`、Unreal `FViewport` + `FSceneViewport`（输入归 viewport）、Unity `Camera.rect` + 单例
renderer 的 clear、Filament `Renderer::setClearOptions` + `Viewport`。**共同点**：表面/pane 持清屏与矩形，
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

## 4. 上层：命令上下文与 pane

- **命令目标从全局槽换成显式上下文**：`ViewContext{ window, pane, selection }`，由命令参数传入或由
  per-window 的 active-pane 服务提供；`MainWindow::setPrimaryRenderControl()` 降级成"窗口自己的默认 3D 视图"
  （仍可用，但不再是唯一通路）。对照：Blender 的 `context.area/region`、Unreal 的 `GEditor->GetActiveViewport()`。
- **pane 是一等公民**：`PaneTree`（splitter）→ 每个 pane 一个 camera + 一个矩形 + 自己的输入捕获 + 可选 HUD。
  今天 `setViewport` + `setInputRouter` 已经把"一个表面多视图"打通，缺的是**布局与生命周期的所有者**
  （谁 split、谁销毁、焦点在哪个 pane）。输入路由从"宿主写回调"进化为"pane 树自己回答"。

## 5. 迁移切片（每片独立可落地、独立验收）

| 片 | 内容 | 验收判据 | 依赖 | 代价 |
| --- | --- | --- | --- | --- |
| **M1** | §3.3 调试覆盖挪到几何阶段（**已落地 2026-10-11**） | ①Deferred 配方 + 非 Lit 档 ⇒ 链条按前向建：没有离屏目标、画面那条 pass 带对应 program 覆盖、线框带 Line 覆盖（`test_graphics` `ANonLitModeOnADeferredRecipeBuildsTheForwardChain`；变异：`effectivePath()` 恒等 ⇒ 失败）；②Forward 四档行为逐字不变（既有用例不动）；③回 Lit ⇒ 链条切回延迟（G-buffer 回来了）；④同一链条内换档零重建（`SwitchingBetweenNonLitModesOnADeferredRecipeKeepsTheChain`），三次切换不留残留 pass（与同配置新视图对照）；⑤那条 `UnsupportedRequest` 诊断、它的用例与文档措辞全部删除；⑥真机烟雾：应用 Deferred 默认路径启动照旧 | 无 | 小 |
| **M2a** | §3.2.5 谁呈现（不碰几何/清屏）：`RenderSurface` 只管"这块表面的最后一步是谁" + `RenderEngine::passOrder()` 只读查询 + 删掉全局让位规则 + `RenderControl::surface()`；demo 显式 `present(pipeline)` | ①默认路径行为不变（`test_gui` 全绿）；②宿主自建主 pass 且**未**声明 ⇒ 视图的默认窗口管线**会**建；显式声明 ⇒ 不建（两条用例）；③`applyOverlayIntent` 行为不回退；④demo 冒烟零 error/warn | 无 | 中 |
| **M2b** | §3.2.5 清屏归表面：`Surface::setClear` + `Pane::setClearEnabled`（后端"只应用第一条公告"从此由表面保证唯一） | ①一块表面只有一条呈现 pass 带清屏（另一条 `isClearEnabled()` 为 false）；②撤掉第一条 ⇒ 清屏按 pane 声明回到剩下那条；③颜色生效（像素判据等 M8，之前用结构判据） | M2a | 中 |
| **M2c** | §3.2.5 矩形/相机/内容下探到 `Pane`（**破坏性**）：**一个 pane 一台相机**成为规则；`SceneView` 退成单 pane 门面（老写法转发） | ①同一内容两块 pane（两台相机）各自矩形与 aspect 正确；②同一相机进第二块 pane ⇒ 明确拒绝（不静默错）；③`setViewport` 老写法仍可用；④几何仍由"矩形 × 表面尺寸 × 比例"一处推 | M2a/M2b + M4（先有 dump） | 大 |
| **M3** | §3.1 `resolveRenderState` 单点 | ①逐项状态快照：内容/pass/视图三层各自设过同一项时，最终值 = 优先级表；②`depthExplicit` 类旗标删除后行为不变；③诊断能说出"这一项来自哪一层"；④`graphics-state.md` §4.1 的表从"规则"改成"命名习惯"（文档验收） | 无 | 中 |
| **M4** | 观测：FramePlan dump | ①一条命令/API 打印这一帧的表格（pass 顺序、目标、输入、clear、状态、耗时）；②`test_graphics` 里至少两条用"结构断言"替换 `passCount()`/指针身份；③dump 与真实执行一致（同一帧的 pass 序列相等） | 无（越早越好，M2/M3 的验收会更硬） | 小 |
| **M5** | pane 树 + 每 pane 交互器 + `ViewContext` 命令目标（= §3.2.5 的 M2d） | ①一个窗口 split 成两个 pane：各自相机/输入/HUD 互不干扰（用例）；②"显示模式"五条命令在多 pane 下作用于**焦点 pane**（同一命令两个 pane 各来一遍）；③`setPrimaryRenderControl` 仍可用但不参与 pane 目标解析 | M2c | 中 |
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
- M1 / M2a / M2b / M3 不需要触发条件：它们消掉的是**今天就在付**的混乱（一条诊断 + 一条全局让位规则 + 一口没有
  owner 的清屏 + 一张命名表）。
- M2c / M5：命中"要分屏（两块 pane 以上）/ 编辑器要让命令作用于焦点视图"之一 —— 它们动公开形态与输入路由，
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
| **Surface / Pane / View 三分**（矩形与清屏归 pane 与表面） | VTK（`vtkRenderWindow` + `vtkRenderer`，矩形与背景色都在 renderer 上）、Unreal（`FViewport` 拥有输入与 clear）、Unity（`Camera.rect` + renderer 持 clear）、three.js、Qt3D `QRenderSurfaceSelector`、Filament `Viewport` + `Renderer::setClearOptions` | 我们今天是"视图变出窗口管线、矩形挂在视图上"：**同一视图两块 pane 做不到**，清屏没有 owner（谁先公告谁赢） |
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
- *保留观察：pane 与文档的关系。* 一个文档开两个 pane 是"两个视图看同一份数据"还是"两个视图不同数据"，
  今天按前者设计（视图按文档）；M5 之前不动它。

## 8. 风险登记

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| M3 一次性改所有状态消费点 | 静默少一项 | 逐项快照对照 + 来源枚举进诊断 |
| M2 清屏/顺序的所有者转移 | 分屏出现"半张脸被清掉" | presenter 的规则表 + 无头像素判据（`VINE_VSG_OFFSCREEN_MULTISLOT` 已有先例） |
| M6 触到后端抽象 | 后端全部要改会话语义 | 只在触发条件命中后做，且先做"设备只读 + 会话多实例"两半 |
| M7 触到所有 pass | 大范围回归 | 强制 M4 先落地，用结构 dump 做回归对照 |
| 提案本身腐烂（代码变了提案没变） | 读者被骗 | 本文标记"提案，未落地"；落地一片就把该片移进现状文档并从本文删除 |
