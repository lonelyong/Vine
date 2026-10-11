# appfw 渲染视图（最终设计 v2，2026-10-11）

> 状态：**已落地 S0–S4 + 输入路由（S6+②）**（2026-10-11；其余 S6+ 三轴仍为"缝已定、未实现"）。本文**取代此前各稿**（那些是本设计的推演过程，结论以本文为准）；
> 相关文档：`appfw-render-surface.md`（表面生命周期）、`graphics-pipeline-builder.md`（配方层）、`graphics-pipeline.md`（pass 调度）、
> `graphics-render-next.md`（**如果重新设计这一套栈会怎么切**：提案，含 M1–M7 迁移切片与触发条件 —— 本文是"现在是什么"，那份是"要变成什么"）。
>
> **落地记录（实现时发现的三条，值得记住）**：
> 1. **R2 的"临时加引用"必须罩住整个操作**：只在 `ensureWindowPass()` 里持有的话，`rebuildDefaultWindowPass()` 里
>    "先丢旧管线"那一步就会把引擎的最后一个引用放掉 —— 用例 `RebuildingNowToDeathIsSafeAfterTheHostReleasedTheEngine`
>    第一次跑就崩在这儿（SEH）。持有要放在**最外层操作**（`rebuildDefaultWindowPass()` 与 `removeWindowPass()` 各一处）。
> 2. **`ensureWindowPass()` 挪到了"后端检查"之前**（`SurfaceWindow::init()`）：一个视图呈现哪些 pass 是它的**配方**，
>    不是设备的事实 —— 没有后端注册的宿主也该有能检查的 `pipeline()`（也正是这条让用例能在无后端下断言读数）。
> 3. **滚轮事件原本没有坐标**（`vn::window::ScrollEvent` 只有 delta）：多视图按"点"路由在滚轮这一项上无从下手，
>    于是给该事件加了 `x`/`y`（逻辑像素，与鼠标事件同空间），由 Qt 侧的翻译补上。
>
> **v2 相对 v1 的四处升级**（都是"消掉一类记账/陷阱"，不是加功能）：
> ① **初始化时机**：控件构造只组装，attach 走"宿主显式 `init()`"或"第一拍兜底"（§2.3、R1）；
> ② **配方与几何分开**：`SceneView::Options` 变更才重建，`setViewport`（归一化）只改取值（§4.1、J8）；
> ③ **输入路由成缝**：`RenderControl::setInputRouter`，按下时路由、松开前捕获，ABI 安全（§5、§12.2）；
> ④ **借用 + 操作期间临时加引用**：顺序自由、宿主提前释放引擎也安全（§13、R2）。
> 另：`ViewOptions` 更名 **`SceneView::Options`**（嵌套在消费它的类里，与 `UserIO::FileReadOptions` 同惯例）。
>
> **一句话**：**视图**（相机 + 内容 + 配方 + 视口）自己拥有并维护它的窗口管线；**控件**只提供"表面 + 会话 +
> 尺寸/比例 + 输入路由"；**场景**自己拥有一套可替换的照明；这三件事**各有"快速入口"与"底层入口"，
> 且低层出现时高层让位**。

## 0. 判据（这份设计要同时满足的东西）

| # | 判据 | 由谁保证 |
| --- | --- | --- |
| J1 | **零重复**：宿主不再重抄"建管线 + 随窗口 resize + 填 DPR"这套记账 | §3 分层与缝 4、§6 用法 |
| J2 | **默认不变**：不配置 = 今天的行为（观感、`passCount()==1`、既有用例逐条不动） | §7 不变量 1 |
| J3 | **能快速构建**：常见的"有光、有 HUD、会跟窗口缩放"3 行内到位 | §4、§6 |
| J4 | **能底层自建**：完全自定义的 pass 图可用，且**不被高层挡住** | §3 四条缝 |
| J5 | **职责清晰**：每个概念只回答一类问题，没有第二个配置面 | §2 概念模型、§5 |
| J6 | **行业口径一致**：与 Unreal/Unity/Filament/VTK/OSG/vsg/Qt3D/three.js 的通行分工对得上 | §9 对照表 |
| J7 | **不破 ABI**：不改 appfw 公开类的**布局/虚表**（那条有 `VN_APPFW_PLUGIN_ABI_VERSION` 握手 + 拒绝加载）；graphics 没有版本宏、布局改动**允许但必须让插件重建**（§3.1、§11） | §5 的 `Impl` 方案 + §11 风险 |
| J8 | **运行时状态不是配方**：视口 / 分屏 / 比例 / HUD 显示隐藏**只改取值，不重建管线** | §4.1 `setViewport` / `setXxxVisible` + §7 不变量 11、13 |
| J9 | **多视图/多窗口有明确立场**：现状、目标态、触发判据都写在纸上，不靠"以后再说" | §12 |

### 0.1 一页纸（v2 总览）

**四层**（每层都能单独用；底下那层永远是最后答案）：

```
第 3 层  表面   RenderControl        表面 + 会话 + 尺寸/比例 + 状态机（**唯一**知道"窗口"的地方）
第 2 层  配方   SceneView::Options   视图的意图（走哪条路 / 要不要 HUD）⇒ 视图自己拥有并维护窗口管线
第 1 层  构建   RenderPipelineBuilder + Pipeline    手持的配方：pass 图、离屏目标、RAII 注销
第 0 层  原语   RenderEngine::addPass + RenderPass / ScreenPass / RenderTarget    完全自定义
```

**四条缝**（§3）：换设置 / 插一档 / 并存多条 / 归构建者接线。

**两条生命周期规则**（本轮新增，各消掉一类记账）：

- **R1 初始化时机**：控件**构造只组装**（引擎 + 视图 + 诊断路由）；attach 由"宿主显式 `init()`"或"第一拍兜底"承担
  ⇒ **"`new` 之后立刻配置"永远有效**，而且**不需要**给控件加第二个配置面（§2.3）。
- **R2 借用 + 操作期间临时加引用**：视图**借用**引擎（`raw_ptr`），但凡"会去引擎上注销 / 重建"的操作（
  `ensureWindowPass` / `removeWindowPass` / `setOptions`）在入口先 `intrusive_ptr<RenderEngine>(engine_, true)`
  临时持有 ⇒ 顺序自由，宿主提前释放引擎也安全（§13）。
- **R3 三种状态各有回家路**：**配方**（`Options`：走哪条路、有没有 HUD 档）= 变更即**重建**；**几何**
  （`setViewport` / `onSurfaceResized`：画在哪块、多大多密）= 只改取值；**开关与模式**（`setStatsVisible` /
  `setGizmoVisible` / `Options::mode`：现在看不看、表面颜色是什么含义）= 只改 pass 上的取值
  （`setEnabled` / `setProgramOverride`）。后两者**永不重建**（J8）—— 分屏拖动、F9 切读数、切着色模式都不该付构建代价。
  **边界由"变的是 pass 图还是 pass 的属性"划**：`ShadingPath` 换的是**有哪些 pass**（配方，重建）；
  `mode` 换的是**同一个 pass 用哪个程序**（属性，零重建）—— 这一条是实现时才看清的，见 §4.1。

**三种"多"**（§12）：**多控件 = 多设备**（现状，可接受的过渡）→ **目标态：一设备多交换链**（有触发判据）；
**一个表面里多视图 = 一设备多视图**（第 1 层已够：`setViewport` + 恰好一个清屏 + 默认 pass 让位）；
**输入**由控件的**路由回调**按"**按下时路由、松开前捕获**"分发（§12.2）。

**两条"不"**：**不让控件碰管线/设置**（§5，零新增配置 API）；**不让视图偷偷装灯**（§4.2，零灯就是零灯）。

**两轴已补（2026-10-11 第二轮）**：**着色模式**（`Options::mode`：Lit/Flat/Unlit/Normals/Wireframe —— 前四种是同一段
shader 文本 + 一个 define，第五种再加 `RenderPass::setPolygonModeOverride(Line)`，全部落在 pass 属性上 ⇒ **零重建**）；
**连续渲染**（`RenderControl::setContinuousRendering(on, interval_ms)` —— 按需渲染仍是默认，连续是宿主可选的一档，
未 attach 或表面未 exposed 时滴答免费）。

## 1. 现状与问题（为什么需要这份设计）

| 层 | 有什么 | 缺什么 |
| --- | --- | --- |
| graphics | `RenderPipelineBuilder`（一次构建）+ `Pipeline`（RAII 注销、`resize(w,h,ratio)`、句柄可查）+ `SceneView::ensureWindowPass()` 兜底建**默认**窗口管线 | 默认管线**选项写死**（`PipelineOptions{}`：forward、无 HUD）；**没人 resize 它**（今天侥幸：forward 无离屏、无 HUD）；`SceneView` 不知道 DPR |
| appfw | `RenderControl`：表面 + 会话 + 尺寸推送 + `devicePixelRatio()` + 状态机 | 不碰管线（**这是对的**，见 §3） |
| 宿主/插件 | demo 显式路 `addDemoPipeline`（**106 行**）+ 灯 `addDemoLighting`（**69 行**） | 每个视图重抄；model_viewer 干脆放弃默认路：手挂 HUD（**31 行**）+ 手装灯（**25 行**） |

宿主今天必须写的**六步**：取 engine/view → `setCamera/setContent` → 填 `PipelineOptions`（**DPR 要在 gizmo 与 fps 两处各填一次**）
→ `build()` 并留住句柄 → `addSurfaceLayout` 里 `pipeline->resize(w,h,dpr)` → 另外还要装灯、手挂 HUD。
其中只有"内容是什么"和"要哪条路/要不要 HUD"是**本质**，其余是记账。

**本轮实测的三个坑**（设计的由来）：① 默认管线选项不可配 ⇒ 想要帧率读数只能自己造 pass 挂引擎；
② 默认管线没人 resize ⇒ 一旦带 HUD 或走 Deferred 就是错的；③ 没人装灯不是黑，而是 `albedo × 0.15` 的**平色**。

## 2. 概念模型（五个概念，各回答一类问题）
| 概念 | 回答什么 | 不回答什么 | 生命 |
| --- | --- | --- | --- |
| `Scene` | 画什么：根子树 + **灯** | 谁来画、怎么画 | 宿主（"一份文档一份场景"） |
| `SceneView` | 从哪看、画什么内容、**怎么画**、**画在哪块**：相机 + 场景 + `SceneView::Options`（配方）+ 视口（几何）+ 它的窗口管线 | 表面从哪来、设备怎么建 | 随控件（1:1），但**视图属于引擎**：一个引擎可以有 N 个视图（§12.2） |
| `Pipeline` | 一组已注册 pass 的**寿命**（RAII 注销）+ 离屏目标/窗口 pass 的句柄 | 谁该建它、何时重建 | 构建者持有 |
| `RenderEngine` | 怎么执行：pass 表 + 后端 + 帧（**纯执行器**） | 相机、内容、设置 | 随控件（1:1） |
| `RenderControl` | 表面从哪来：原生表面 + 会话 + **尺寸/比例** + 生命周期状态 | 管线配方、场景、灯 | 宿主 |

### 2.1 对应关系与所有权（本轮查清的事实）

| 关系 | 基数 | 依据 |
| --- | --- | --- |
| `RenderControl` : 原生表面/会话 | **1 : 1**（窗口重建 = 同一会话换句柄） | `appfw-render-surface.md`、`SurfaceWindow::recreateSurface()` |
| `RenderControl` : `RenderEngine` | **1 : 1** | `SurfaceWindow.cpp:246` 构造里建，之后只 `setBackend`/`initialize` |
| `RenderControl` : `SceneView` | **1 : 1** | `SurfaceWindow.cpp:250` 构造里建并 `setEngine` |
| `SceneView` → `RenderEngine` | **N : 1**（**借用**，可空） | `setEngine(raw_ptr<>)`；类注释"several views may share one engine" |
| `RenderControl` : 后端会话/设备 | **1 : 1**（多控件 = 多设备；共享设备是后端的缝，§12.1） | `useDefaultBackend()` 每个控件 `factory->create()`；`SessionContentAccess::device(session)` |
| `SceneView` : `RenderEngine`（绑定） | **1 : 1**（`setEngine` 是单值：一个视图同时只服务一个引擎；多引擎共享一个视图**不支持**，行业里也不支持） | `setEngine(raw_ptr<>)` 单成员 |
| `SceneView` : 自己的窗口管线 | **1 : 0/1**（懒建；换设置即重建） | `default_pipeline_`、`ensureWindowPass()` |
| `SceneView` : 它的矩形（`viewport_` × 表面尺寸 × 比例） | **1 : 1**（唯一真相；见 §3.2 的三层换算） | `applySurfaceGeometry()` |
| 任意 `Pipeline` : `RenderEngine` | **1 : 1**（强引用引擎）⇒ 一个引擎上可有多条管线 | `Pipeline(intrusive_ptr<RenderEngine>)` |
| 视图的窗口 pass : 表面 | **1 : 1**，默认**铺满**（`setViewport` 可收窄；两条铺满的窗口 pass 会互相擦掉） | `SceneView` 类注释 |
| `SceneView` : `Camera`/`Scene` | 各 **1**（视图持有强引用） | `camera()`/`scene()` 返回 `intrusive_ptr` |

```
宿主/插件 ── 注册 ──> RenderBackendRegistry
RenderControl（Impl = SurfaceWindow）
  ├─ 拥有 ──> RenderEngine ── 拥有 ──> RenderBackend ── 拥有 ──> 设备 / 会话 / 交换链
  └─ 拥有 ──> SceneView ── 借用 ──> RenderEngine
                 └─ 拥有 ──> Pipeline（默认那条）── 强引用 ──> RenderEngine   ⇒ 引擎寿命 ≥ 视图寿命
                               └─ 注册 ──> passes（归引擎的 pass 表 slots_）
```

**所有权方向：控件 → 引擎，不是引擎 → 控件**。引擎没有任何回指（它不认识控件与视图）；它唯一的"上向"知识是
`hasWindowPass(camera)`，而那是查**自己的 pass 表**（`pass->camera() == camera && pass->isEnabled() && pass->renderTarget() == nullptr`
⇒ "这个相机被某个窗口 pass 呈现了"）—— 那是 pass 的属性，不是视图的。后端也不回指引擎（`gfx_backend_vsg` 里没有
`RenderEngine*` 成员）。**引擎是可独立存在的服务**（`test_graphics` 里到处 `new RenderEngine()`，无窗口、无控件）。
逐边的精确分析（谁强引用谁、为什么、有什么顺序约束）见 **§13**。

### 2.2 构造时到底建了什么、什么时候重建（本轮逐行查清）

`new vn::appfw::gui::RenderControl()` → `RenderControl` 构造 → `new SurfaceWindow(host)` → 后者的构造：

| 步骤 | 代码 | 建了吗 |
| --- | --- | --- |
| 引擎 | `d->engine = new RenderEngine()` | **构造即建** |
| 视图 | `d->view = new SceneView()` + `view->setEngine(engine)` | **构造即建** |
| 会话（attach） | `if (d->engine->backend() != nullptr) init();` | **构造里试一次**，但**只有后端已注册时才开始**（后端没注册时退回，窗口显示/尺寸变化时重试） |
| **默认窗口管线** | `init()` 里第 5 步 `d->view->ensureWindowPass();`（**跑在"句柄/尺寸可用性"检查之前**） | **只要后端在，构造里就建**（Forward、无 HUD）；后端不在 ⇒ 不建，且 state 直接 `Failed("no render backend is registered")` |

推论（都是可观察的）：

- 正常应用（插件在启动期已加载 ⇒ 后端已注册）：`new RenderControl()` 返回时**已经有引擎、视图与一条默认窗口管线**，
  并且 attach 已经尝试过（日志里 `surface Pending -> Presenting` 就发生在构造那一刻）。
- 无后端的宿主（`test_gui`）：引擎/视图在、state `Failed`、**默认管线不存在** —— 上一轮探针里 `before ensureWindowPass: 1`
  的那一个 pass 是"我们自己挂的帧率读数"，不是窗口 pass。
- `ensureWindowPass()` 是**幂等**的（`default_pipeline_ != nullptr` 或"该相机已有窗口 pass"就返回），所以重复调用不会叠管线。

**变更设置 = 重建吗？**

| | 今天 | 本设计（S1 之后） |
| --- | --- | --- |
| 改默认管线的选项 | **做不到**：选项写死 `PipelineOptions{}`；`removeWindowPass()` 是 **private**（`SceneView.hpp:226`），宿主连"逼一次重建"都做不到 | `view->setOptions(...)`：**值语义、变更即重建**（内部正是那个私有原语 `removeWindowPass() + ensureWindowPass()`） |
| 想换管线 | **只能替换**：自建一条呈现同一相机的管线，默认那条自动让位（§3 缝 1） | 替换照旧；或者换设置（第 2 层） |
| 代价 | — | 一次管线构建；Forward 便宜，Deferred 会连带重新分配 G-buffer（设备侧）。**不是每帧成本** |
| 零多余重建 | — | **可以**：默认管线是懒建的 ⇒ 若设置发生在**管线还不存在时**（后端还没注册、或还没 attach），`setOptions()` **只记设置**，等 `init()` 时按设置建 |

所以规则写成一句：**设置变了且管线已存在 ⇒ 重建；管线还不存在 ⇒ 只记设置**。

按 §2.3 采纳的初始化时机（构造只组装、attach 排在第一拍或由宿主显式 `init()`），配置**总是**发生在默认管线建起来之前
⇒ "构造后立刻 `setOptions()`"是**零重建**的那条路，上面"唯一代价"那一格随之作废 —— 这也是不必把设置传进控件构造
（§5 已否决的"控件配置面"）的原因。

### 2.3 初始化时机：构造只组装，attach 由 `init()` 或第一拍兜底（本轮结论）

要回答的是"能不能改成：先配置，再初始化"。三个候选：

| | A 现状：构造自驱 | B 强制手动 `initialize()` | **C 构造只组装 + 双路 attach（采纳）** |
| --- | --- | --- | --- |
| 配置窗口 | **靠运气**：注册表里已有后端 ⇒ 构造里就 attach，配置窗口关闭 | 无条件打开 | **无条件打开**：构造之后的同步代码（`setBackend` / `setOptions` / 装内容 / 调相机）总在 attach 之前 |
| 忘记初始化 | 不会（自驱） | **黑窗口**，而且"没后端"这种本该降级的场景也会漏 | 不会（第一拍兜底，与今天同义） |
| 内部状态机（句柄/尺寸/后端就绪） | 需要 | **仍需**：`init()` 自己也会退回，宿主还要自己处理"布局未就绪" | 需要（不变） |
| 与 SDK 写的次序（构造 ⇒ `setBackend()` ⇒ `init()`） | **只在"后端未注册"时成立**（见下面理由 1） | 成立 | **永远成立** |
| 与行业惯例 | 少（构造里做重活） | OSG `realize()`、ImGui 后端 `Init()`（但两者都另有隐式路径） | Qt `initializeGL()` 首次 paint、VTK 首次 `Render()`、three.js 首次 render，**同时** VTK `Initialize()`、OSG `realize()` 作为显式入口 —— **两种入口并存** |
| 公开 API | 现状 | `init()` 语义从"幂等重试"变成"必须调" | **零变化**（`init()` 已是公开、幂等、同步） |

**规则（一句话）**：**构造只组装；attach 由"宿主显式 `init()`"（同步、优先）或"首个事件循环拍的兜底自驱"承担，
谁先到谁算，两者幂等**（兜底不是竞争者 —— 与 `ensureWindowPass()` 同一个哲学）。

**必须改的四条理由**（都是裂痕，不是体感）：

1. **SDK 写的次序在真实应用里不成立**：`gfx_backend_vsg` 在插件注册期就 `registerFactory`（启动期，早于任何控件），
   于是构造里 `engine->backend() != nullptr` ⇒ **构造里就 attach、就 `initialize()`**（启动日志里 `surface Pending -> Presenting`
   就在构造栈上）。宿主之后 `engine()->setBackend(my_backend)` 直接违反
   [`RenderEngine::setBackend`](/i:/projs/Vine/src/viz/graphics/src/RenderEngine.cpp:57) 的 "Call before initialize()"，
   而它**只是一次赋值**（不拆会话）⇒ 引擎指着一个从未 `initialize()` 的新后端，旧后端的 live 会话还挂在句柄上。
2. **"配置好再初始化"今天无法表达**：会话已经建立，`init()` 幂等地直接返回 true ⇐ 没有"还没开始"的状态可用。
3. **后端后注册被钉死**：`useDefaultBackend()` 里的 `backend_selected` 一旦跑过（构造里就跑过）就永不再查注册表 ⇒
   兜底再也捡不起后来注册的后端，state 停在 `Failed`，宿主只能自己 `setBackend()`（或永远没有后端）。
4. **S1 的 `setOptions()` 必然多一次重建**（§2.2）；C 之后天然零重建。

顺带修好一条**今天已经被绕过**的前置条件：`demo_plugin` 在 `DemoView::activate()` 里先 `DemoScene::install()`（登记 pass）
再 `initAsync()`，而 [`DemoScene.hpp:124`](/i:/projs/Vine/src/plugins/demo_plugin/src/DemoScene.hpp:124) 明写
"Must be called before the control is initialized (before the backend attaches), because the pipeline is built from the
passes registered here"。构造自驱在后端已注册时**先 attach 了一次**（占位尺寸 160×160 就够）⇒ 这条前置条件今天是靠
"晚登记的 pass 也被容忍"侥幸成立；C 之后它第一次真的成立（落地时顺手核对 + 把注释改成事实）。

**落点与代价**：

| 落点 | 改法 |
| --- | --- |
| `SurfaceWindow::SurfaceWindow()` | 去掉构造成员末尾的 `if (backend != nullptr) init();`，改排一拍：`QTimer::singleShot(0, this, [this]{ init(); })`（沿用 `armRecreateHatch()` 同一模式） |
| `SurfaceWindow::useDefaultBackend()` | 守卫从 `backend_selected` 改成"**引擎当前没有后端时才补**"；补的时机变成 attach 时 |
| `RenderControl.hpp` 类注释 | "THE CONTROL ATTACHES ITSELF / 构造即试一次" 段改成"构造只组装；attach = 显式 `init()` 或第一拍兜底" |
| `DemoScene.cpp:1547` 的注释 | "attach 是同步的（控件构造时就跑完了）"不再成立；**demo 代码本身不用改**（`DemoView::activate()` 已经先 `install()` 再 `initAsync()`；`installContent()` 末尾有 `control_->renderFrame()`，没有空白帧风险） |
| `model_viewer` | **不用改**：`ModelRenderView::activate()` 已经显式 `control_->init()`，正是"宿主给时机" |
| 测试 | `RenderControlTest` 用 `pumpUntil` / 显式 `init()` ⇒ 预期不动；`InitAsyncCompletesWithoutSuspending`（显式 `initAsync()` ⇒ 仍不挂起）不受影响。落地时要实跑一遍 |

**与历史的关系**：`.ai/memory/appfw.md`（2026-09-19）记的正是"宿主给时机、`RenderControl` 不自己 attach、构造里的首触发已删"
—— 那版把"配置后初始化"做到了，代价是宿主**必须**记得调 `init()`（demo 今天就是靠 `activate()` 里那次 `initAsync()`）。
现在的构造自驱是后来加回来的，memory 那条没跟着更新。C 是两者的合题：**保留自驱（不靠宿主记得），但把它从构造里挪到第一拍**（不靠运气）。

可观察差异（写进注释即可）：`new RenderControl()` 返回时 `state()` 是 **Pending**（今天后端已注册时是 Presenting），
`Presenting` 落在第一拍；"attach 不需要可见性"这条性质不变（兜底挂在事件循环上，不挂在 show 上）。

**C 的已知代价（也就是"显式入口"必须保留的原因）**：兜底晚一拍 ⇒ 设备/管线那几百毫秒会落在事件循环第一拍
（`.ai/memory/appfw.md` 2026-09-18 的实测：不早给时机就可能"启动框关掉后 ~1 s 空屏"）。
**要早建的宿主要显式 `init()`/`initAsync()`** —— 仓库内两个宿主都已经这么做，所以期 S0 的验收要包含
"启动占用与上屏时序不退化"这一条。

## 3. 分层 API 与四条缝（"自定义"与"快速"如何共存）

行业里的成熟做法都是**分层暴露同一件事**（Vulkan 命令 → render graph → 引擎预设；Qt3D framegraph 结点 → `Qt3DWindow`
默认 framegraph；three.js 裸 `WebGLRenderer` → `EffectComposer` → examples 预设）：**每层都能用，底下那层永远是最后答案**。

| 层 | 行业对应 | 我们的 API | 谁维护尺寸/比例 |
| --- | --- | --- | --- |
| **0 原语** | Vulkan/DX12 命令、`vsg::CommandGraph` | `engine->addPass(pass, pipelineStageOrder(stage))` + `RenderPass`/`ScreenPass`/`RenderTarget` | 你 |
| **1 配方** | framegraph / render graph | `RenderPipelineBuilder::build(PipelineOptions)` + `Pipeline`（RAII、`resize`、`windowPass()`/`offscreenTarget()`） | 你（**一行** layout 步） |
| **2 视图** | Unity URP Asset（配方）/ `Camera.rect`（几何） | `SceneView::setOptions(Options)` + `setViewport` + `pipeline()` | **视图**（自动） |
| **3 表面** | `QVTKOpenGLNativeWidget` / `Qt3DWindow` / vsg `Window` | `RenderControl`（表面/会话/尺寸） | **控件**（自动） |

**四条缝**：

1. **替换**：`view->setOptions(...)`（第 2 层）；或自建一条呈现**同一个相机**的管线（第 1 层）。两者不必协调 ——
   `ensureWindowPass()` 只在"该相机没有任何窗口 pass"时才挂默认那条 ⇒ **默认是兜底，不是竞争者**
   （既有用例 `EnsureWindowPassSkipsWhenCameraAlreadyPresented` 已钉住，本轮把它升格成文档口径）。
2. **扩展**：不换整条管线，只往某一档插 pass —— `engine->addPass(pass, pipelineStageOrder(...))`。`PipelineStage`
   的意义就是"按意图插队"，自定义 pass 不必知道别人用了什么序号。
3. **并存**：同引擎多条管线互不知情（默认那条 + 你的 preview/effect 那条）；`view->pipeline()` 只回答默认那条，
   你自己那条自己持有（`Pipeline` 析构即注销）。
4. **维护归构建者**：默认那条由视图自动 resize；自建那条自己接线，一行：
   `view->addSurfaceLayout([p](int w, int h, double dpr) { p->resize(w, h, dpr); });`
   —— 为此 `addSurfaceLayout` 加**三参数重载**（今天 demo 得在 lambda 里反向取 `render_control->devicePixelRatio()`，
   第 1 层本该与 appfw 无关）。

### 3.2 视图几何的实现拓扑（落地时定的，改它要连改三处）

一个视图的矩形由**视图**一处算出，但**消费**它的是三个对象，于是有一条必须一起改的链：

| 层 | 拿到的尺寸 | 谁换算 | 备注 |
| --- | --- | --- | --- |
| 视图自己的相机 / 操纵器 / `addSurfaceLayout` 回调 | **pane**（视图矩形，逻辑像素） | 视图（`applySurfaceGeometry`） | 自建管线的宿主按 pane 报价（第 1 层） |
| `Pipeline::resize(w,h,ratio)` → 离屏目标（G-buffer / composite） | `pane × ratio`（**设备像素**） | `Pipeline`（自己乘） | Deferred 的 G-buffer 因此只有那一块大 |
| 默认窗口 pass 的 `setViewport` + `AxisGizmo/FpsOverlay` 的 `setAreaOrigin` | 装置像素矩形 | 视图（`deviceViewport()`） | HUD 先知道自己在哪块**区域**里，再按区域角摆盒子 |

⇒ 默认窗口 pass 是**唯一**在视图矩形与 `Pipeline` 之间多一层换算的东西（自己算设备矩形：`pane × ratio`）。
"逻辑 / 装置"这条界线因此只有一处交叉，改换算只改 `SceneView::applySurfaceGeometry()` 与 `Pipeline::resize()` 两处。

## 3.1 API 范式（六条，所有层都照这个写）

| 范式 | 规则 | 例子 |
| --- | --- | --- |
| **签名即所有权** | 借用 = `raw_ptr` 参数 + 文档"必须活得比我久"；接管 = `intrusive_ptr` 参数（调用点看得见"我把所有权交出去了"）；返回内部对象 = `raw_ptr` | `SceneView::setEngine(raw_ptr<>)` / `RenderEngine::setBackend(intrusive_ptr<>)` / `view->pipeline()` |
| **操作期间临时加引用** | 借用的成员一旦要"跨操作使用"，入口先 `intrusive_ptr<T>(p, /*add_ref*/ true)`（该 ctor 的第二参数就是这个意思） | R2（视图→引擎，§13）、控件捕获期间→视图（§5）；**同一条通则，两处复用** |
| **意图用值、变更即重建** | 配方（`Options`）按值进出、无 setter 碎片；改它 = 一次重建 | `setOptions(Options)` / `options()` |
| **几何用 setters、零重建** | 每帧会变的东西（视口、可见性、着色模式）只改取值，不碰结构 | `setViewport` / `setStatsVisible` / `Options::mode` |
| **同名状态两处出现时，赢家的名字带 `Override`** | pass 级 setter **赢了内容**的 → `…Override`（`setProgramOverride` / `setPolygonModeOverride`）；pass 给的是**可被内容压过的默认** → 不带（`setDepthMode`）。判断依据与整张表在 `.ai/design/graphics-state.md` §4.1 | 视图的"线框"开关必须是覆盖：否则内容写过的线框会把它永久钉住 |
| **能不能 vs 为什么** | "能不能"用返回值（`bool`/`nullopt`）；"为什么"一律走**诊断**（`RenderEngine::setDiagnosticSink`，控件转日志）；**不许静默失败** | `init()` → `bool` + 诊断；HUD 开关 / 延迟路径上的模式 → 诊断 |
| **一层一个旋钮** | 同一件事不给两个入口；句柄（`pipeline()->gizmo()`）是**微调口**而不是第二配置面，状态仍然只有一份 | 视图不给 `setPipelineOptions()`；`Options` 不含尺寸/偏移 |

## 4. 最终 API 形态（graphics）

### 4.1 视图设置：**一层一个旋钮**（本轮最大的一处推翻）

前几稿在视图上同时给过 `setPipelineOptions()` 与 `setGizmoVisible()/setFpsVisible()`（两个入口指向同一件事），
那会长出"谁覆盖谁"的第二个配置面。定稿改为：**视图只有一个设置类型**，它是**意图**；`PipelineOptions` 是**构建细节**，
留给第 1 层（自己 build）。两者不再相遇。

```cpp
class VN_GRAPHICS_API SceneView : public RefCounted<SceneView> {
  public:
    /// 这个视图的**配方**（意图）：怎么画。全默认 = 今天的行为（Forward、无 HUD）。
    /// 嵌套在消费它的类里 —— 与 `UserIO::FileReadOptions` 同一惯例（选项属于用它的那个类）。
    /// 尺寸/偏移（gizmo 方框、读数框、HUD 档内偏移）**不在这里**：它们是实现细节，
    /// 需要微调时从 `view->pipeline()->gizmo()` / `fpsOverlay()` 拿句柄改。
    struct Options {
        ShadingPath path{ ShadingPath::Forward };  ///< 结构选择：Forward / Deferred（与 Unreal 的 forward/deferred 同词）
        bool        gizmo{ false };                ///< **有没有**左下角坐标轴档（配方；运行时开关见 setGizmoVisible）
        bool        stats{ false };                ///< **有没有**右下角帧率读数档（配方；运行时开关见 setStatsVisible）
        // 预留（已定缝、未实现，见 §8）：Mode mode;   // Lit / Unlit / Wireframe / Normals（配方：换程序=换管线）
    };

  public:
    /// 换配方：内部重建自己的窗口管线（值语义、变更即重建 —— 与 URP Asset / EffectComposer 一致）。
    /// **只有配方走这条路**：几何（视口 / 尺寸 / 比例）走下面两条，**不重建**。
    void setOptions(const Options& options);
    const Options& options() const noexcept;

    /// 运行时开关：现在**看不看得见**帧率 / 坐标轴档（**零重建**：只是 `pass->setEnabled()`；
    /// 引擎本来就会跳过 disabled 的 pass）。档还不存在时**按需补建一次**（每个视图每种档最多一次）。
    void setStatsVisible(bool on);
    void setGizmoVisible(bool on);
    bool isStatsVisible() const noexcept;
    bool isGizmoVisible() const noexcept;

    /// 这个视图画在表面的哪一块，**归一化 [0,1]**（与 Unity `Camera.rect` / VTK `vtkViewport::SetViewport` 同口径）。
    /// 默认 `{0,0,1,1}` = 铺满 = 今天的行为。归一化的好处：宿主**不必**做 DPR 换算，分屏比例天然跟随表面。
    /// **这是该视图矩形的唯一真相**：宽高比、pass 的设备像素视口、清屏范围、layout 步收到的尺寸全部由
    /// "它 × 表面尺寸 × 比例"推出（所以 `onSurfaceResized` 永远只收**表面**尺寸，宿主不必自己算一半 —— §14 F1）。
    void setViewport(const NormalizedViewport& viewport);
    const NormalizedViewport& viewport() const noexcept;

    /// 表面事件：尺寸 **和比例** 一次给全（appfw 已在按窗口尺寸调它）。推给相机操纵器与 layout 步，
    /// 并**顺手 resize 自己的默认管线**（把归一化视口换算成设备像素写进它拥有的 pass）—— 宿主零代码。
    /// 比"另设一个 DPR setter"好的地方：比例与尺寸同生共死，不可能出现"设了比例、尺寸还是旧的"这种静默错位。
    void onSurfaceResized(int width, int height, double pixel_ratio = 1.0);

    /// 布局步：两参数版沿用；三参数版把比例一起给你（第 1 层自建路径用，见 §3 缝 4）。
    void addSurfaceLayout(std::function<void(int width, int height)> layout);
    void addSurfaceLayout(std::function<void(int width, int height, double pixel_ratio)> layout);

    /// 默认窗口管线的句柄（检查 / 微调：`setEnabled()`、`gizmo()`、`offscreenTarget()`）。
    raw_ptr<Pipeline> pipeline() const;
};
```

**为什么"配方"与"几何"必须分开**（本轮新增，判据 J8）：`setOptions()` 的代价是一次管线构建（Deferred 还要重分配
G-buffer），而视口、尺寸、比例是**每帧都可能变**的东西（分屏拖动、窗口缩放、拖到另一块屏、跨 DPR 显示器）——
把最频繁的操作挂上最贵的代价，就是把"分屏拖动"变成"每帧重建管线"。行业里这条线画得很清楚：
**Unity 的 `Camera.rect` 随手改，`URP Asset` 改了要重建**；**Unreal 的 `FSceneView::ViewRect` 每帧给，
ShowFlags / FeatureLevel 才决定管线**；**VTK 的 `SetViewport` 是 viewport 的状态，与 rendering passes 无关**。
所以定稿：`Options` = 配方（变更即重建），`setViewport` / `onSurfaceResized` = 几何（只改取值，永不重建）。

**为什么撤掉视图上的 `setPipelineOptions()`**：`PipelineOptions` 里有程序覆盖、pass 偏移、离屏尺寸这些**构建细节** ——
把它们与意图放在同一个对象上，就等于让宿主猜"哪些字段会与设置打架"。要构建细节 ⇒ 你就是在做第 1 层（自己 `build()`，
视图的默认管线自动让位）。这就是 J5（没有第二个配置面）。

**运行时开关：显示 / 隐藏永不重建**（本轮新增，J8 的同族）。HUD 是**随时会切**的东西（菜单项、快捷键、F9/F10/FPS），
把最轻的操作挂上最重的代价是最容易犯的错。行业口径一致：Unreal 的 ShowFlags 是**每帧**值，VTK 用
`actor->SetVisibility()` / 渲染器开关，Qt3D framegraph 结点 `setEnabled(bool)`，Blender 的 overlay 开关，
three.js 的 `helper.visible` —— **保留式管线（VTK / OSG / Qt3D / 我们）一律把可见性做成持久对象上的运行时标志**；
Unreal 之所以"感觉免费"，是因为它**每帧重建 pass 列表**（我们是保留式，所以"档在不在"是配方、"现在看不看"是开关）。
代码里本来就够：`RenderPass::setEnabled()` / `isEnabled()`，而引擎在**六处**都跳过 disabled 的 pass。

| 问题 | 归谁 | 代价 |
| --- | --- | --- |
| 这个视图**有没有**帧率 / 坐标轴档 | `Options::stats` / `Options::gizmo`（配方） | 变更 = **一次重建** |
| 现在**看不看得见** | `setStatsVisible()` / `setGizmoVisible()`（开关） | **零重建**（`setEnabled`） |
| 想频繁切 | 在配置窗口里把两个档都设 `true`（R1 ⇒ 零重建），之后随便切 | **0** |
| 宿主**自建了主 pass**（第 1 层） | 视图的默认管线**根本不存在**（让位规则）⇒ HUD 开关没有档可切：`setStatsVisible` 记一条**诊断**并保持不可见（不许静默），HUD 归宿主的 `builder` 配（`PipelineOptions::fps/gizmo` + 句柄） | — |

- `setStatsVisible(true)` 而档还不存在（`Options::stats == false`）⇒ **按需补建一次**（复用 `setOptions` 那条路，
  不引入第二套构建逻辑）；之后 `options().stats` 如实变成 `true` —— 事实就是"这个档现在存在"。**每个视图每种档最多付这一次**。
- `options().stats` 回答"**有没有这个档**"，`isStatsVisible()` 回答"**现在看得见吗**" —— 两个问题两个答案，
  不合并成一个含糊的 bool（Unreal 的 show flag 与 `stat fps` 也是两件事）。
- 反过来：**"用什么程序画"**：`ShadingPath`（Forward/Deferred）是**配方** —— 它换的是**有哪些 pass**，所以变更 = 一次构建；
  但 `Mode`（Lit/Flat/Unlit/Normals/Wireframe）**不是配方**：前四种是**同一个**窗口 pass 上的 `setProgramOverride()`
  （四者出自同一段 `builtin_forward.frag` 文本 + 一个 `define`，见 `BuiltinShaders`），第五种再多一个**光栅化模式**
  （`RenderPass::setPolygonModeOverride(PolygonMode::Line)`），pass 图完全不变 ⇒ 换模式**零重建**，下一帧就是另一种画法。
  这条边界（"变的是 pass 图还是 pass 的属性"）是实现时才看清的：第一稿把 `Mode` 归进配方，落地时发现
  `RenderPass::setProgramOverride` 正是为这件事存在的（它的文档原文就举了"re-render the same scene with a different
  shader (e.g. a wireframe or alternate-shading pass)"），而**线框**走的 `setPolygonModeOverride` 与它是同一个形状
  （pass 级覆盖 → 命令的渲染状态；引擎本来就有逐子树的 `StateNode::setPolygonMode`，后端早就把多边形模式当**动态状态**
  发命令，所以线框不需要任何后端改动）。
- 非 Lit 档**作用在几何阶段**（M1，2026-10-11）：延迟配方配非 Lit 档时，视图把链条按**前向**建（`Options::path` 字段
  不动 ⇒ "请求"与"生效"分开），于是四档在两条配方上都能画出来 —— 旧行为（延迟路径报一条 `UnsupportedRequest`
  并保持 Lit）已删除。代价是"进出非 Lit（延迟配方）"是一次**重建**（链条变了），同一条链条上换档仍是零重建。
- 可见性状态**归视图**，不进 `RenderControl`（控件不碰管线与设置，§5）；菜单/快捷键就是调这两个 setter。

### 4.2 场景侧的照明：**成套配方（light rig）**

```cpp
/// 一套成套的照明配方（VTK 叫 `vtkLightKit`，DCC/摄影棚叫 "light rig"；Unity `RenderSettings`、
/// Godot `Environment`、three.js `Scene.environment` 是同一位置上的东西：**场景级**照明设置）。
struct VN_GRAPHICS_API LightRig {
    std::vector<LightPtr> lights;   ///< 这套配方包含的灯（装进场景后归场景持有）

    /// 环境光 0.35 + 世界固定主方向光 + 背面弱补光（= 今天 demo/viewer 手抄的那套；方向世界固定 ⇒ 明暗只是几何的函数）
    static LightRig studio();
    /// 跟着相机的头灯（VTK `AutomaticLightCreation` 的那盏）：没有可用方向时最保险
    static LightRig headlight(vn::math::Vec3d direction = vn::math::Vec3d(0.0, 0.0, -1.0));

    void addTo(vn::graphics::Scene& scene) const;   ///< 追加，不动场景里已有的灯
};

class VN_GRAPHICS_API Scene {
  public:
    void addLighting(const LightRig& rig);   ///< 追加一套（= rig.addTo(*this)）
    void setLighting(const LightRig& rig);   ///< 先 clearLights() 再加（显式覆盖，别猜）
};
```

- 入口放 `Scene` 上是**发现性**（补全里就有；自由函数要靠文档与搜索才知道存在 —— 本轮 model_viewer 的"平色"正是因为
  没人知道要装灯）；取值为参数让容器只做机制（装灯），策略在 `LightRig` 的预设里（加"户外/摄影棚"是新预设，不是新 API）。
  与 `ConsolePanel::setTheme(ConsoleTheme::dark())` 同构。
- **不做** VTK 式"零灯自动造头灯"（`AutomaticLightCreation` 默认开）：隐式状态让"为什么是亮的"变成谜，还会与后端的
  `ambient = 0.15` 兜底互相掩盖。要"打开就有光"就在**创建场景那一行**写 `scene.addLighting(LightRig::studio())`。

## 5. 最终 API 形态（appfw）：`RenderControl` **零新增配置 API**（只加一条输入路由缝）

| 问题 | 定稿 |
| --- | --- |
| 控件要不要 `Config`/`setConfig()` | **不要**。它造第二个配置面，还越过"表面 + 会话"的边界 |
| 控件要不要 `pipeline()` 转发 | **不要**。`control->view()->pipeline()` 已经拿得到 |
| 控件要不要 `Signal`（如 `pipeline_changed`） | **不要**。加数据成员就是 ABI 断；`state_changed` 已覆盖"什么时候可以配置" |
| 控件替宿主装灯/拥有场景 | **不要**（场景归宿主） |
| 控件要不要一行工厂 `create(...)` | **不要**。它模糊所有权，而第 3 行本来就属于场景；3 行已经够短（§7） |
| 控件**表面侧的改动** | 把 `devicePixelRatio()` 与尺寸**一起**推给视图：`view()->onSurfaceResized(w, h, devicePixelRatio())`（两个调用点），并在比例变化时重推 |
| 控件要不要**输入路由** | **要一个，但只是一个"这一点归谁"的回调**：`setInputRouter(std::function<SceneView*(int x, int y)>)`，默认（未设）= 一切都给自己的 `view()` ⇒ 单视图逐字不变。**放进 `Impl`**（`std::function`，不是 `Signal`）⇒ 公开面零新数据成员、ABI 不动。理由：多视图的**布局是宿主定的**，控件手里只有"表面 + 尺寸"，没有命中知识。**已落地** |
| 控件要不要**连续渲染** | **要，一档而已**：`setContinuousRendering(on, interval_ms = 16)`。按需渲染仍是默认（attach / resize / 输入 / `renderFrame()` 才出帧），连续是给动画、仿真、要能读的读数用的。它是**帧驱动**不是帧率保证（滴答"要一帧"，一帧该多久多久）；未 attach 或表面未 exposed 时滴答**免费**（写明，不是静默）。**已落地** |
| 控件的**初始化时机** | **构造只组装**（引擎 + 视图 + 诊断路由），不选后端、不 attach；attach 由宿主显式 `init()`（同步、优先）或第一拍兜底（§2.3、期 S0）。所以"`new` 之后立刻配置"永远有效，控件不新增任何配置 API |

**路由的语义：按下时路由、松开前捕获**（与 VTK `vtkRenderWindowInteractor` 按 viewport 挑 renderer、Blender/Unity 的手柄一致）：

- **按下**（pointer down / 滚轮）→ 调一次 router 决定目标视图；滚轮即刻结束，指针按下则**记住**该视图直到
  release（否则"按住旋转"时鼠标一飞出那一块就断线）。
- **契约三条**（一条都不能少，否则是"沉默的坏行为"）：
  1. 返回 `nullptr` = **回退到控件自己的视图**（不丢事件 —— 宿主一个 bug 不该让整个表面没反应）；
  2. 返回的视图必须是**本控件引擎上**的视图（输入坐标只对这个表面有意义）；违反是契约错误（debug 断言 + 文档）；
  3. **捕获期间控件持一个强引用**（`intrusive_ptr<SceneView>`，与 R2 同一条通则）⇒ 宿主在拖拽中途销毁自己的视图
     引用也不会让控件手里悬空；release（或视图不再属于本引擎）时释放。
- 键盘事件给"当前捕获的 / 控件自己的视图"。
- 归一化视口与这个回调是**互补**的：宿主自己的命中测试就是两行 —— `view->viewport().contains(nx, ny)`
  （归一化坐标由控件的逻辑尺寸一除即得）。

## 6. 用法（三种规模，各自最短）

```cpp
// ── 快速（第 2/3 层）：表面、会话、尺寸、比例、HUD 摆位全自动
auto* control = new vn::appfw::gui::RenderControl();              // 表面 + 会话（appfw 的事）
control->view()->setOptions({ .gizmo = true, .stats = true });    // 这个视图怎么画（配方，graphics 的事）
control->view()->scene()->addLighting(vn::graphics::LightRig::studio());   // 看得见形状（场景的事）
control->view()->scene()->setRoot(myRoot);                        // 内容仍然是我的
// 上面四行的位置就是"配置窗口"：构造只组装，attach 发生在宿主显式 init() 或事件循环第一拍（§2.3）
// ⇒ 这里改设置是**零重建**；之后再改一次设置 = 一次重建（值语义，§4.1）

// ── 运行时开关（任意时刻，菜单项 / 快捷键；**零重建**）
control->view()->setStatsVisible(false);   // 关掉帧率读数（只是 setEnabled(false)）
control->view()->setGizmoVisible(true);    // 打开坐标轴档；若配置窗口里没建过，这一次会补建一次

// ── 扩展（第 0 层，1 行）：不换整条管线，只加一个自己的 pass
control->engine()->addPass(mySsaoPass, vn::graphics::pipelineStageOrder(vn::graphics::PipelineStage::Effect));

// ── 自定义（第 1 层）：自己的 pass 图；默认管线自动让位；维护自己接线（1 行）；appfw 对这段代码不可见
vn::graphics::RenderPipelineBuilder builder(engine);
builder.setCamera(view->camera());
builder.setContent(view->scene());
auto pipeline = builder.build(myPipelineOptions);
view->addSurfaceLayout([pipeline](int w, int h, double dpr) { pipeline->resize(w, h, dpr); });

// ── 分屏（第 1 层 + 两条新缝）：一个表面、两个视图、**一台设备**
auto* left = control->view();                                     // 控件顺手建的那一个视图（左半）
left->setViewport({ 0.0, 0.0, 0.5, 1.0 });                        // 几何：改它**不重建**
auto right = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
right->setEngine(control->engine());                              // 视图挂在**引擎**上，不挂在控件上
right->setOptions({ .gizmo = true });                             // 各自一条窗口管线（默认让位规则照旧）
right->scene()->setRoot(otherRoot);
right->setViewport({ 0.5, 0.0, 0.5, 1.0 });                       // **唯一**的矩形真相：宽高比/像素/清屏范围都从它推
right->ensureWindowPass();                                        // 宿主的**额外**视图：默认管线要显式要（控件自己的那个由 attach 触发）
right->onSurfaceResized(w, h, dpr);                               // 尺寸永远给**表面**的；"这一块"由视图自己算（§14 F1）
control->setInputRouter([&](int x, int y) {                       // 按下时路由、松开前捕获（§5）
    const double nx = double(x) / double(std::max(1, control->width()));
    return nx < 0.5 ? left : right.get();
});
// 清屏：后端是 attachment loadOp 的**整张清** ⇒ 必须"恰好一个视图清"，且它的 pass 排在最前（§12.2）
```

**落地实测**：**model_viewer 56 行 → 2 行**（手写三盏灯 25 行 → `scene->addLighting(LightRig::studio())`；
自建 FpsOverlay + pixel ratio + 尺寸转接 + `activate()` 补尺寸 31 行 → `view->setOptions({ .stats = true })`）。
**demo 的灯不迁移**（原设计稿写的"69 行 → 1 行"作废）：它带**名字**（`scene_sun`/`scene_sun2`）、**阴影配置**
（主日 `castShadow` + 分辨率/bias）和 `VINE_VSG_EXTRA_SUNS` 开发开关，且世界是 Z-up、朝向是它自己的 —— 那是**第 0 层
的策略**，不是预设。`LightRig` 保持"只出预设"（studio/headlight），有自己策略的宿主直接用 `Scene::addLight()`
（J4：底层永远可用）。新插件 3~5 行得到一个"有光、有 HUD、会跟窗口缩放"的视图。

**把"视图设置"接到命令 / 菜单上**（本轮加的参考实现：model_viewer 的"显示模式"五条命令 + ribbon 一组按钮）：

```cpp
// 命令只认"宿主发布的那个当前 3D 视图"，不认任何具体视图类（谁上屏谁发布：activate() 里发布、deactivate() 里收回）
auto* window  = vn::appfw::gui::MainWindow::current();
auto* control = window != nullptr ? window->primaryRenderControl() : nullptr;
auto* view    = control != nullptr ? control->view() : nullptr;
if (view == nullptr) { co_return NotApplicable(u8"还没有活动的 3D 视图：把这份模型显示出来再试"); }

// 读-改-写**整份配方**：只送一个 mode 会把帧率读数、坐标轴那些关掉（值语义的代价）
auto options = view->options();
options.mode = vn::graphics::SceneView::Options::Mode::Wireframe;  // 零重建：程序覆盖 + 光栅化覆盖（§4.1）
view->setOptions(options);
```

三道规矩：**设置归视图、命令只改视图不改文档**（一份文档一个视图 ⇒ 换文档回默认档）；**守卫说清原因**（当前文档不是
本插件的类型 ⇒ `NotApplicable`，不静默）；**一条命令一个模式**（不做循环切换，也不做勾选态 —— 框架没有单选组概念，
做了就要维护"谁选中"这份第二真相）。

## 7. 不变量

1. **默认不变**：不配置（`SceneView::Options{}`）时仍是 forward、无 HUD、视口铺满 ⇒ 观感与 `passCount()==1`
   一类既有用例不动。
2. **兜底而非竞争**：默认窗口管线只在"该相机没有任何窗口 pass"时存在；宿主自建主 pass 时它自动让位。
3. **谁构建谁接线**：默认那条由视图 resize（含比例）；自建那条由构建者接（一行 layout 步）。
4. **尺寸与比例同生共死**：比例只随 `onSurfaceResized(w,h,ratio)` 走，不存在孤立的 DPR 状态。
5. **无隐式装灯**：零灯就是零灯（后端的 `ambient=0.15` 是可见性兜底，不是照明策略）。
6. **公开类不加数据成员**（尤其 `Signal`）；默认值不改。
7. **一个表面一条窗口 pass（默认铺满 + 清屏）**（多视图/分屏见 §12.2：**底层可做、几何与输入两条缝已设计**，
   但默认那条永远是"自己的视口 + 清屏"，不会替宿主做多视图布局）。
8. 视图的默认管线只服务**它自己的相机**；引擎不因任何视图的默认管线而获得视图的概念。
9. **多窗口 = 多控件 = 多设备**（现状/过渡，§12.1）；**多视图 = 一个控件多条 pass**（§12.2）—— 四条"不合一"的路
   （控件 / 引擎 / 视图 / 后端会话）在任何数量的窗口下都成立；目标态见 §12.1 的判据。
10. **输入先按"点"归视图，再捕获到松开**（§5）：默认路由 = 控件自己那个 `view()`（单视图行为逐字不变）；
    多视图的归属由宿主的路由回调决定 —— 控件**不猜**布局。
11. **配方与几何分开**（J8）：`Options` 变了才重建管线；`setViewport` / `onSurfaceResized` 只改取值（含比例换算）。
12. **借用 + 操作期间临时加引用**（R2，§13）：`ensureWindowPass` / `removeWindowPass` / `setOptions` 入口临时持引擎，
    顺序自由、宿主提前释放也安全。
13. **显示 / 隐藏是运行时开关，永不重建**（§4.1）：`setGizmoVisible` / `setStatsVisible` 只切 `setEnabled`；
    档不存在时**按需补建一次**（每个视图每种档最多一次）。要频繁切就在配置窗口里把档都建好（R1 ⇒ 零重建）。
    着色模式同族：`Options::mode` 只切**画面那条 pass** 的 program override，同一条链条上换档**零重建**；
    延迟配方进出非 Lit 档会换链条（前向），那一次是重建（M1，2026-10-11）。
    **关掉的档不会被一次重建又打开**：重建是从**配方**建的（新 pass 天生 enabled），所以使用者最后一次说的可见性
    由 `applyOverlayIntent()` 在每次建完之后落回去（换路径、补建另一个档、把档从配方里去掉再要回来，都算）。
14. **几何只有一个输入**：`view->viewport()`（归一化）是矩形的**唯一真相**；`onSurfaceResized` 只收**表面**尺寸 + 比例；
    宽高比、pass 的设备像素视口、清屏范围、layout 步收到的尺寸**全部由它推**（宿主不必自己算"一半"）。
15. **状态只有一份**：HUD 的可见性就是 **pass 的 `isEnabled()`**（`isStatsVisible()` 读它，不存第二份 bool）；
    `Options` 是**意图**，"有档 ⇔ 有那条 pass"。允许的唯一不一致：宿主自建了主 pass（那时默认管线不存在，开关记诊断）。
16. **默认管线要显式要**：控件自己的视图由 attach 触发 `ensureWindowPass()`；宿主的额外视图自己调一次（幂等）。

## 8. 路线图（S0–S6+：每一步都能独立落地、独立验收）

| 期 | 内容 | 验收 |
| --- | --- | --- |
| **S0** appfw（**可先落地，独立于 S1**） | 初始化时机（§2.3）：构造只组装、attach 排第一拍或宿主显式 `init()`；`useDefaultBackend()` 守卫改为"引擎没有后端时才补";注释同步 | ①构造返回时 `state()==Pending`，第一拍后 `Presenting`；②构造后立刻 `setBackend()` 生效（不再是"已 initialize 的默认后端"）；③注册表空 → 之后注册后端 ⇒ 兜底能捡起；④`RenderControlTest` 全绿（显式 `init()`/`initAsync()` 的同步语义不变）；⑤启动时序不退化：demo/model_viewer 仍在自己那次 `init()`/`initAsync()` 上 attach（比对启动日志的耗时行，没有新的空屏拍）；⑥核对 `DemoScene.hpp:124` 的前置条件（`install()` 早于 attach）现在真的成立，并把 `DemoScene.cpp:1547` 的注释改成事实 |
| **S1** graphics | `SceneView::Options` + `setOptions()` + `options()` + `pipeline()`；`setStatsVisible/setGizmoVisible/isStatsVisible/isGizmoVisible`（运行时开关，**零重建**）；`setViewport(归一化矩形)`（几何，不重建）；`onSurfaceResized(w,h,ratio)` 自 resize 默认管线；`addSurfaceLayout` 三参数重载 | ①不配置 ⇒ `passCount()==1`（既有用例不动）；②`.stats=true` ⇒ 2 个 pass 且在 HUD 档；③`onSurfaceResized(400,300,2.0)` 后 HUD/gizmo 拿到新尺寸与新比例；④换设置 ⇒ 重建后仍只有一条窗口管线（不重复注册）；⑤宿主已有主 pass ⇒ 默认管不插一脚；⑥空配置下 resize 无害空转；⑦三参数 layout 收到比例；⑧**管线还没建时 `setOptions()` ⇒ 不建管线、只记设置，随后 `ensureWindowPass()` 按新设置建**（§2.2 的零重建路径）；⑨默认窗 pass **内部按"视口矩形"取值**（默认 = 铺满），给 §12.2 的分屏留门；⑩**注销/重建在操作期间临时加引用**（R2，§13 后果 3）—— 用例里放一个"宿主把引擎也释放了"的变体钉住它；⑪`setViewport({0.25,0,0.5,1})` 后：默认窗 pass 的设备像素视口跟着变、**且不重建**（`passCount()` 不变、`pipeline()` 句柄不变）；**同一时刻**只喂表面尺寸（`onSurfaceResized(1280,720,1.0)`）也够 —— 相机宽高比与 layout 步收到的尺寸都等于"这一块"（640×720），证明几何**只有一个输入**（不变量 14）；⑫运行时开关：`.stats=true` 建好后 `setStatsVisible(false)/(true)` 连续切 10 次 ⇒ 读数档的 `isEnabled()` 跟着变，而 `passCount()` 与 `pipeline()` 句柄**不变**（零重建）；`.stats=false` 时第一次 `setStatsVisible(true)` **恰好付一次**构建（此后同样零重建） |
| **S2** appfw | 两个调用点改成推 `(w,h,ratio)`；比例变化时重推 | 无后端的 `test_gui`：尺寸与比例确实到了 view；比例变化后跟上。**不加任何配置 API** |
| **S3** graphics | `LightRig`（`studio()`/`headlight()` + `addTo()`）+ `Scene::addLighting()/setLighting()` | ①`studio()` 三盏（1 环境 + 2 方向）、方向非零；②追加语义（装两次 = 两套）；③`setLighting()` 覆盖；④`Scene` 原有 `addLight/clearLights` 不动 |
| **S4** 迁移 | `model_viewer`（灯 + HUD 收口）、`demo_plugin`（灯收口，管线保留） | viewer 的灯/读数用例改成断言新入口；demo 观感不变（截图/日志比对） |
| **S5** 文档 | 三份互链 + 本文转正 | `check_ai_docs.py` 0 条 |
| **S6+** 一轴（**缝已定**） | **共享设备 / 一设备多交换链**（§12.1 的目标态，命中判据才做；会动后端抽象 + 注册表） | 独立，不阻塞 |

**进度（2026-10-11，第四轮）**：**S0–S4 + 输入路由 + 着色模式（含 Wireframe）+ 连续渲染已落地**；五个套件全绿
（`test_graphics` 308、`test_gui` 249、`test_appfw` 32、`test_meshio` 34、`test_vsg` 404 通过 / 26 跳过（要设备）），
`check_ai_docs` / `check_include_hygiene` / `check_export_annotations` / `check_doc_symbols` / `check_diagnostic_formats`
全 0 条；S4 的 demo 灯一项按 §6 的结论**不改**（第 0 层策略）。

**线框（Wireframe）落地的路径**（比预估的短）：`StateNode::setPolygonMode` 让 `PolygonMode {Fill, Line, Point}` 早已存在，
后端也早已把多边形模式当 **EDS3 动态状态**（`VK_DYNAMIC_STATE_POLYGON_MODE_EXT` + `vkCmdSetPolygonModeEXT`，且
`fillModeNonSolid` 本来就请求了）⇒ 只需把"pass 级覆盖"这一半补上：`PolygonMode.hpp`（类型从 `StateNode.hpp` 抽出来，
两个作用域共用）、`RenderPass::setPolygonModeOverride()/clearPolygonModeOverride()/polygonModeOverride()`（与 `setProgramOverride` 同形状，
应用在 `RenderPass::execute()` 里那次"需要自己的命令表"的拷贝上），**零后端改动**。`Mode::Wireframe` = 无光照程序 +
`PolygonMode::Line`。

实现落在：`SceneView`（`Options`/`setOptions`/`options`/`pipeline`/`setViewport`/`viewport`/`setStatsVisible`/
`setGizmoVisible`/`isStatsVisible`/`isGizmoVisible`/`onSurfaceResized(w,h,ratio)`/三参数 `addSurfaceLayout`）、
`Viewport.hpp`（`NormalizedViewport`）、`AxisGizmo`/`FpsOverlay`（`setAreaOrigin`）、`Pipeline::resize`（把比例也推给
HUD 档）、`Scene`+`LightRig`、`SurfaceWindow`（S0 的兜底 attach、比例一起推、`ensureWindowPass()` 提到后端检查之前、
输入路由与捕获）、`RenderControl::setInputRouter`、`ScrollEvent`（`x`/`y`）、`ModelRenderView`（迁到新入口）。

## 9. 行业口径对照（本设计的依据）

| 行业 | 代表 | 我们 |
| --- | --- | --- |
| 设备 + 交换链 + 提交 | vsg `Viewer`/`CommandGraph`、three.js `WebGLRenderer`、Unity pipeline 实例 | `RenderEngine` + `RenderBackend`（纯执行，不认识相机/内容） |
| 窗口 + 原生表面 | vsg `Window`、VTK `vtkRenderWindow`、Qt3D `Qt3DWindow`、`QVTKOpenGLNativeWidget` | `RenderControl` |
| 视图：相机 + 内容 + 视口 + 设置 | Unreal `FSceneView`（`ViewRect` + ShowFlags）、vsg `View`、VTK `vtkRenderer`（`SetViewport`） | `SceneView`：相机 + 场景 + `Options`（配方）+ `setViewport`（几何） |
| **配方 vs 几何** | Unity `URP Asset`（改了要重建）/ `Camera.rect`（随手改）；Unreal ShowFlags / `FSceneView::ViewRect` | `SceneView::Options`（重建）/ `setViewport`+`onSurfaceResized`（不重建） |
| 一进程一设备、N 交换链 | Filament（`Engine`+`Renderer`+`SwapChain`）、VTK（一 renderer 多 render window）、OSG（一 `Viewer` 多 `GraphicsWindow`） | **目标态**（§12.1）；现状是"一控件一设备"（Qt / three.js 口径） |
| 多视图的输入分发 | VTK `vtkRenderWindowInteractor` 按 viewport 挑 renderer（按下路由、拖动捕获）；Blender/Unity 手柄同语义 | `RenderControl::setInputRouter`：按下路由、松开前捕获（§5、§12.2） |
| **多窗口** | Unreal 一 RHI 设备 N 窗口；OSG 一 `Viewer` 多 `GraphicsWindow`；VTK 一个 renderer 可挂多个 render window；**Qt / three.js：每窗口一上下文** | **每控件一设备**（Qt/three.js 路线，§12.1）；共享设备是后端的缝 |
| **一表面多视图（分屏）** | Maya/Blender 一窗口四分屏；Unity 每相机 `viewportRect`；Unreal split screen（一个 `FSceneViewFamily`） | 第 1 层已够（`setViewport` + `setClearEnabled` + `hasWindowPass` 让位）；默认路径不做（§12.2） |
| 视图组的配方/设置资源 | Unreal `FSceneViewFamily`、Unity **URP Asset**、Godot `Environment`、three.js `EffectComposer`、Qt3D framegraph | `Pipeline` + `PipelineOptions`（值语义、变更即重建） |
| 场景图 + 灯 | three.js/Godot/OSG：灯在**场景**；Unity `RenderSettings`/Godot `Environment` 是场景级设置；VTK 有 `vtkLightKit` | `Scene` + `LightRig` |
| HUD / 统计 / gizmo | ImGui 叠 pass、Stats.js 在 DOM、Unity gizmo overlay、Unreal **ShowFlags** | `AxisGizmo` / `FpsOverlay`（= `Options` 的两个 show flag） |
| DPI/像素比 | three.js `setPixelRatio()`+`setSize()`（文档要求先设比例）、Qt 逻辑像素×`devicePixelRatio()`、Vulkan extent×scale | `onSurfaceResized(w,h,ratio)`：**合并成一次**，让"先比例后尺寸"这类顺序错误不可能发生 |
| 分层 API | Vulkan 命令 → render graph → 引擎预设；Qt3D framegraph → `Qt3DWindow`；three.js → `EffectComposer` → 预设 | §3 的四层与四条缝 |

**命名对照**（便于按行业词检索）：`RenderControl` ≈ `RenderWindow`/`RenderWidget`/`Qt3DWindow`/`QVTKOpenGLNativeWidget`；
`SceneView` ≈ `FSceneView` / `vsg::View` / `vtkRenderer`；`SceneView::Options` ≈ URP Asset 的"配方面" / Unreal ShowFlags；
`Pipeline` ≈ framegraph/URP 实例。改名会动 ABI 与调用点，本轮不改。

## 10. 备选与否决（含本轮被推翻的写法）

| 备选 | 结论 |
| --- | --- |
| `RenderControl::Config`/`setConfig()` | **不做**（第二个配置面 + 越界；§5） |
| 控件上的一行工厂 `RenderControl::create(...)` | **不做**（模糊所有权；3 行已够短） |
| 视图上同时给 `setPipelineOptions()` 与 show flag setter | **推翻**：一层一个旋钮；构建细节归第 1 层（§4.1） |
| 视图无参的 `setDefaultLighting()`；VTK 式零灯自动造灯 | **不做**（策略焊进容器 / 隐式状态与后端兜底互相掩盖） |
| 把 `SceneView` 合并进 `RenderEngine`（three.js 式"renderer + scene + camera"） | **不做**：会毁掉"引擎是服务、相机无关"，且封死多视图/多相机 |
| 让 `Pipeline` 拥有视图/相机/内容（Unreal `FSceneRenderer` 式） | **不做**：我们是"一次构建、长期存在"，那样会变成状态的第二份副本 |
| 在 appfw 造一个把 Control+视图+灯打包的 `RenderView` | **不做**：与 `SceneView` 重复，且场景归宿主 |
| 给 `RenderControl` 加 `Signal` | **不做**（ABI；`state_changed` 够用） |
| 把表面/窗口搬进 graphics | **不做**：graphics 保持 Qt-free；表面由 appfw + 后端分职 |
| `SceneView::Options` 上加预设工厂（`shaded()`/`withHud()`） | **不做**：聚合 + 指定初始化器已经够（`{ .gizmo = true, .stats = true }`），少一批名字 |
| 把视口 / 尺寸 / 比例**塞进 `Options`**（"一层一个旋钮"顺手统一） | **不做**：那会把**每帧都可能变**的几何挂上"变更即重建"的代价（J8）；配方与几何分开是行业口径（Unity `Camera.rect` vs URP Asset） |
| 让 `RenderControl` 自己按视口做命中测试（控件存一份视图列表） | **不做**：控件的布局知识不如宿主，而且会与引擎的 pass 表重复记账；用 `setInputRouter`（§5，ABI 安全） |
| 给输入路由加 `Signal`（`pointerPicked` 之类） | **不做**：`Signal` 是数据成员 ⇒ ABI 断；`Impl` 里的 `std::function` 已经够，而且"谁定点"是一次同步问答，不是事件广播 |
| 把 HUD 的显示 / 隐藏做成"改 `Options` 再重建" | **不做**：把最轻的操作挂上最重的代价。保留式管线的行业口径是"可见性 = 持久对象上的运行时标志"（VTK `SetVisibility` / Qt3D framegraph `setEnabled` / Blender overlay 开关）（§4.1） |
| 让 `RenderControl` 管 HUD 开关（`control->showFps(true)`） | **不做**：HUD 是**视图**的属性（一块分屏一处读数、多视图各不同），而且控件不碰管线与设置（§5） |
| 给 HUD 可见性加一个进程级/控件级"全局开关" | **不做**：全局可变状态会破 §12.1 的不变量 1；宿主若想要全局开关，遍历自己的视图调 setter 即可 |
| 把 `init()` 改成**必须**手动调用的 `initialize()`（构造不再 attach） | **不做**（§2.3 的 B）：漏调就是黑窗口（连"没后端"这种本该降级的场景也会漏），而且它**消不掉**内部状态机（句柄/尺寸/后端就绪不受宿主控制）。采用的 C 只挪时机：**兜底自驱照旧，只是挪出构造函数** |

## 11. 风险与迁移成本

1. **默认值必须逐字不变**（J2）：`SceneView::Options{}` 必须映射到今天的 `PipelineOptions{}`；一旦有人"顺手"把 `stats`
   或 `path` 的默认值改掉，观感与既有用例一起变。S1 的第①条用例专钉这个。
2. **`Pipeline::resize` 从"只有 demo 调"变成"默认路也调"**：forward 且无 HUD 时它必须是**无害空转**（S1 第⑥条）。
3. **`setOptions()` 的生效时机**（构建前 / 构建后）都要定义清楚；不要留"设置了但不生效"的静默窗口。
   判据即 §2.2 那一句：**管线已存在 ⇒ 重建生效；管线还不存在 ⇒ 只记设置、等 attach 时按设置建**。所以要保证
   `setOptions()` 之后的第一个可观察状态（无论是 `passCount()` 还是画出来的第一帧）已经体现新设置。
4. **重建与 `hasWindowPass` 的交互**：宿主有主 pass 时重建会跳过，看起来像"设置了没生效" ⇒ 文档 + S1 第⑤条写明。
5. **迁移面**：graphics 2 个文件（`SceneView` + `Scene`/`LightRig`）、appfw 2 个调用点、`model_viewer` 1 个文件、
   `demo_plugin` 灯那一处；`PipelineOptions`/`Pipeline`/`RenderControl` 的**公开 API 不变**（除新增），故 ABI 不受影响。
6. **S0 改的是可观察时序**：`new RenderControl()` 之后 `state()` 从"可能立刻 Presenting"变成"一定 Pending"。
   依赖"构造即 attach 完成"的代码/注释今天只有 `DemoScene.cpp:1547` 一句注释（行为上有 `renderFrame()` 兜着），
   但**任何第三方宿主**只要按这个时序写过代码就会被影响 ⇒ 必须写进 `RenderControl.hpp` 的注释与 changelog 口吻的说明。
7. **输入路由回调的生命周期与重入**（已落地）：回调在事件派发**栈上**被调用 ⇒ 回调里不要销毁视图（要销毁就
   `deleteLater`）；返回 `nullptr` = 回退自己的视图；返回别的引擎的视图 = 记一条诊断并回退。用例：分屏两块、
   路由命中右块，验证旋转真的发生在**右边那台相机**上（而不是控件自己的那个视图），并且**越界拖动不断线**、
   捕获期间宿主丢掉引用也不崩（控件持着一个引用）。
8. **一次很大的单步拖动会把 orbit 相机算到"看起来没动"或退化**（实现时观察到：把越界移动写成"从 3/4 处跳到
   1/8 处"时，`eye()` 与初值差不到 1e-9）。原因未诊断（可能是大角度累积的灵敏度/回绕），**不在本轮范围**——
   用例因此改成"小幅越界"并加了一条 `isfinite` 断言，把"路由后的相机仍然可用"当作不变量钉住。
8. **归一化视口的像素缝隙**：设备像素 = 归一化矩形 × 表面尺寸 × 比例，必然出现非整数 ⇒ 相邻两块可能差 1 像素
   （亮线/暗线，分屏的老问题）。规则写在文档里：**左/上边界向下取整、右/下边界向上取整**（相邻块恰好相接，
   重叠 1 像素也比露缝好），S1 用一条 1280×720 三分屏的像素级断言钉住。
9. **第一次开启某覆盖层会付一次构建**（§4.1）：`Options` 里没建过的档，第一次 `setXxxVisible(true)` 会补建（一次）。
   要"永远零代价"就在配置窗口里把档都建好（`{ .gizmo = true, .stats = true }`，R1 ⇒ 零重建）。用例钉住"最多一次"
   （切 10 次只构建 1 次）—— 否则宿主会在按 F9 的时候遇到一次莫名的卡顿而查不出原因。
10. **可见性是每视图的状态，不是全局的**：多视图时每块分屏各有自己的 HUD 状态；宿主想做"全局开关"就遍历自己的
    视图（我们不引入全局状态，见 §12.1 不变量 1）。
11. **ABI 边界要说清**（J7）：`VN_APPFW_PLUGIN_ABI_VERSION`（`7u`，见 [Plugin.hpp](/i:/projs/Vine/src/fw/appfw/sdk/vine/appfw/Plugin.hpp:62)）
    管的是**插件**：appfw 公开类加数据成员/`Signal`/虚函数 ⇒ 插件全部要重建 ⇒ 必须**bump 并让加载被拒**。
    所以 S0 的 `setInputRouter` 一定进 `Impl`（这是"为什么用 `std::function` 而不是 `Signal`"的**硬理由**：
    加 `Signal` = 加数据成员 = bump）。**graphics 没有版本宏**（`SceneView` 加 `options_`/`viewport_` 属于这一类）
    ⇒ 改动可行，但要在变更说明里写"插件需重建"，别假装无事发生。

## 12. 多视图与多窗口（本轮补）

三种"多"能力与代价完全不同，先别混为一谈：

| 场景 | 今天能不能 | 基数 / 代价 | 本轮结论 |
| --- | --- | --- | --- |
| ① 多个 widget / 多个窗口（**各一个** `RenderControl`） | **能，已经在工作**（多文档窗口就是这条路） | 一控件 = 一面 = 一引擎 = 一视图 = 一后端会话 = **一个设备**；同一份内容出现在两个窗口 = **两份显存 + 两遍编译** | **不新增任何东西**，只保不变量（§12.1） |
| ② **一个表面里多视图**（四分屏 / 并排对比） | **底层缝已经在**（`SceneView` 类注释明说"several views may share one engine"），**默认路径不做** | 1 设备、N 相机、N 条 pass；显存只一份 | 第 1 层自建 + 两条新设计的入口（归一化视口、输入路由，§12.2） |
| ③ **同一份内容**出现在多个窗口 | 结构上支持 | 图形对象（`Scene`/`Geometry`/`Material`/`ImageRef`）是**后端无关**的 Node；GPU 副本归**后端会话**（`BlockStorage`/`MaterialImages`）⇒ 每个窗口一份 | 支持，但显存 N 份；跨窗口共享设备见 §12.1 的**目标态** |

### 12.1 多窗口：现状是过渡，目标态是"一设备多交换链"

**行业口径**：除 Qt / three.js 系（每窗口一上下文）外，主流引擎都是**一进程一设备 + N 个表面/交换链** ——
Filament（一个 `Engine`/`Renderer`，N 个 `SwapChain`）、VTK（一个 renderer 可挂多个 render window）、
OSG（一个 `Viewer` 多个 `GraphicsWindow`）、Unreal / Unity（一个 RHI 设备，多窗口/多显示器）。

**我们的现状（刻意保留的过渡）**：一控件 = 一面 = 一引擎 = 一后端会话 = **一个设备**。理由是它与本文档所有
所有权结论同源：**隔离最简单、生命周期最干净、一个控件销毁不牵动别人**，而且文档模型本来就是"一个文档一个视图
（连带它自己的面/会话/设备）"。代价必须说清：**每多一个控件就多一个设备**，同一模型开两个窗口就上传两遍。
⇒ **四分屏不要用四个控件**（那是四份设备），用 §12.2 的一条控件 + 四视图。

**目标态**：一设备多交换链。**触发判据**（命中任意一条就该做）：① 同时活着的渲染视图稳定 > 4；② 需要把**同一份
内容**显示在 2 个以上窗口；③ 设备启动开销（首帧那几百毫秒）成了可感知的启动延迟而被乘上窗口数。

**迁移路径（现在就要保证的事）**：它落在**后端 + `RenderBackendRegistry`**（会话带多条交换链 `setSurface(i)`），
**不碰控件与视图的 API** —— 控件对后端的要求只有"引擎 + 句柄 + 尺寸"三样。为此要保三条不变量：
1. 引擎 / 视图 / 控件的公开面里**没有"进程唯一"的假设**（今天确实没有全局可变状态）；
2. 设备与资源的归属始终是**后端会话**，不是控件（今天就是这样：`SessionContentAccess::device(session)`）；
3. `RenderEngine` 可以多实例、可以换后端（今天就是这样）。

### 12.2 单表面多视图：底层已经够，本轮把两个入口设计出来

| 需要 | 现成的东西 / 本轮新增 |
| --- | --- |
| 每个视图自己的相机/场景/宽高比 | 一个 `SceneView` = 自己的 `Camera` + `Scene`；`view->onSurfaceResized(pane_w, pane_h)` |
| 每个视图画在自己那一块 | 现成：`RenderPass::setViewport(Viewport{...})`（**设备像素**）。**新增**：`SceneView::setViewport(归一化矩形)`（§4.1）⇒ 宿主**不必**算 DPR，比例自动跟随表面 |
| 谁清屏 | 现成：`setClearEnabled` / `setClearColor` / `shouldClearDepth`（**恰好一个** pass 清，其余关） |
| 别让默认窗 pass 捣乱 | 现成：`hasWindowPass(camera)` —— 宿主自建的 pass 一旦呈现该相机，`ensureWindowPass()` 就**不插一脚**（缝 1） |
| 操作哪一块 | **新增**：`RenderControl::setInputRouter(fn)` —— **按下时路由、松开前捕获**（§5） |

**两件已设计 + 一件仍然是宿主的策略**：

1. **视口（几何，S1 落地）**：归一化矩形 `{x,y,w,h}` ∈ [0,1]，默认 `{0,0,1,1}` = 铺满 = 今天的行为。
   改它**不重建管线**（J8）；默认窗 pass 只把归一化矩形换算成设备像素写进自己那条 pass。
   代价为零，收益是把"分屏拖动"从"每帧重建"里救出来。
2. **输入路由（已落地）**：控件里一个"这一点归谁"的回调；未设时一切都给自己的 `view()`，单视图行为逐字不变；分屏时宿主两行命中（`view->viewport().contains(nx, ny)`）。语义：**按下时路由、松开前捕获**，捕获期间控件持有那个视图；滚轮按点路由（为此 `ScrollEvent` 补了 `x`/`y`）；键盘给"捕获中的 / 自己的"视图。返回 `nullptr` = 回退自己的视图；返回别的引擎的视图 = 记一条诊断并回退。
3. **清屏策略（宿主的；而且是后端逼出来的）**：后端用 **attachment loadOp 整张清**（`OffscreenTarget.cpp:107`、
   `WindowTarget::prepare(policy)`），**没有按矩形清**（`vkCmdClearAttachments` + rect）⇒ 分屏必须**恰好一个**视图清，
   而且**它的 pass 要排在最前**（清屏发生在其它 pane 画之前；pane 的几何被 scissor 限在自己矩形里，所以
   "一块清、余下各自画"是**正确**的）。这条**不会**进 `Options`（它是多视图之间的**约定**，不是单视图的配方）。
   将来的改进落在后端侧（加按矩形清），SDK 侧最多多一个 `RenderPass::setClearRect`（或让 `setViewport` 兼任清屏范围）。
   默认路径不会"变聪明"：**默认是兜底，不是竞争者**。

**多视图的职责划分**（与 VTK / OSG 同口径）：**视图挂在引擎上，不挂在控件上** —— 控件只提供"表面 + 尺寸/比例 +
输入 + 状态"，`control->view()` 只是"控件顺手建的那一个视图"（文档里别叫它 "primary"：`MainWindow::primaryRenderControl`
才是 appfw 那一层的术语）。多视图 = 多个 `SceneView` 各自
`setEngine(control->engine())`、各自一条窗口 pass；`RenderControl` **不为它们加"视图列表"**（那会与引擎的 pass 表
重复记账，也会把"谁清屏"变成控件的事）。

> **目标形态（大改版设计，未落地）**：这一节的"矩形挂在视图上、清屏靠谁先公告、视图去认领窗口"是**过渡形态**。
> 按三维规范重写的目标（`Surface`（唯一清屏 + present）→ `Pane`（矩形 + 相机 + 内容）→ `View`（配方/管线），
> 以及"一个 pane 一台相机"）见 `graphics-render-next.md` §3.2；分四步落地（M2a/M2b/M2c + 并入 M5 的输入与焦点 pane）。
> 本节其余内容在该设计落地前仍然为真。

**额外视图要显式要默认管线**：`ensureWindowPass()` 是"**我要**这条默认管线"的动作，`setOptions()` 只改配方。
控件**自己**那个视图由 attach 触发（§2.2）；宿主新建的视图没有 attach 事件 ⇒ 自己调一次 `ensureWindowPass()`
（一行，幂等）。这也是缝 1 的另一半：**你要主 pass，我就不插一脚；你不要，我说一声就建**。

## 13. 所有权：`SceneView` ↔ `RenderEngine`（本轮补）

三条边，方向与强度都不一样：

```
SurfaceWindow::Impl（宿主 = 控件）
  ├─ intrusive_ptr<RenderEngine> engine    ← 强引用（声明在前）
  └─ intrusive_ptr<SceneView>    view      ← 强引用（声明在后 ⇒ 析构序：view 先、engine 后）
SceneView
  ├─ raw_ptr<RenderEngine>       engine_   ← 借用（接口意图：不延长寿命）
  └─ intrusive_ptr<Pipeline>     default_pipeline_
                       └─ intrusive_ptr<RenderEngine> engine_   ← 强引用（同一条引擎，私有、无访问器）
RenderEngine
  └─ slots_: { intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content }
                       └─ RenderPass: intrusive_ptr<Camera>（"The pass KEEPS the camera alive"）
```

| 边 | 强度 | 依据 |
| --- | --- | --- |
| `SceneView` → `RenderEngine`（直接） | **借用** `raw_ptr` | `SceneView.hpp:229` 注释"The bound engine is borrowed (the host owns it and keeps it alive)" |
| `SceneView` → `RenderEngine`（经默认管线） | **强引用** | `Pipeline::engine_`（`intrusive_ptr`，私有）；`RenderPipeline.hpp` 类注释"It also keeps the engine ALIVE (a strong reference, not a borrow)" |
| `RenderEngine` → `SceneView` | **没有这条边** | 引擎"camera- and content-agnostic"，类注释；不认识视图，也不认识管线（`Pipeline` 注释："There is no cycle — the engine does not know its pipelines"） |
| `RenderEngine` → 相机 / 内容场景（经 pass） | **强引用，但那是 pass 的属性** | `slots_` 强引用 pass 与其内容场景；`RenderPass::setCamera(intrusive_ptr<Camera>)` "The pass KEEPS the camera alive" |
| `RenderEngine` → `SceneView::Camera`（经窗口 pass） | 同上 | 视图的默认窗 pass 携带该相机 ⇒ 引擎间接让"这个相机对象"活着 |

**为什么是"借用 + 一条强引用兜底"而不是单纯借用**：使用权与寿命要分开 —— 视图**不该**替宿主决定引擎活多久
（引擎是服务：可以没有视图、可以被多个视图共享、`test_graphics` 里满地都是无窗口的引擎），但视图**析构时要去引擎上
注销自己的 pass**，这个动作要求引擎此刻还活着。于是用一条不变量把两者焊在一起：**"我的默认管线活着 ⇒ 引擎活着"**。
[`SceneView::~SceneView()`](/i:/projs/Vine/src/viz/graphics/src/SceneView.cpp) 的注释把这条讲透了：
"it holds the engine strongly, so the engine is still there to be unregistered from **even if the host released it first**"。
而一旦视图是被控件持有的，成员声明序（`engine` 在前、`view` 在后）又额外保证了析构时 **view 先走、engine 后走** ——
两层保险，方向一致。

**三个可观察的后果**（这才是"谁持有谁"的实用价值）：

1. **没建默认管线时，视图对引擎一点强引用都没有**（只 `new SceneView()` 还没 `setEngine()`+`ensureWindowPass()`）
   ⇒ 那段时间引擎的寿命**完全**由宿主负责。
2. **默认管线一建起来，视图就能让引擎活到视图死**：宿主提前 `reset()` 引擎不会把它拆掉（既是保护，也意味着
   "释放被推迟到视图/管线消失"）。
3. **顺序自由（本轮升级成 R2）**：凡是"会去引擎上注销 / 重建"的操作（`ensureWindowPass` / `removeWindowPass` /
   `setOptions`）入口先做一次**临时加引用**：`intrusive_ptr<RenderEngine> keep(engine_, /*add_ref*/ true);`
   —— 这才是"借用"这个接口意图的标准用法：**借用不等于不管寿命，而是"我只在操作期间需要它活着"**。
   于是之前那条"必须先建新、再丢旧"的顺序陷阱**消失**：两个顺序都安全，宿主提前 `reset()` 引擎也安全。
   （`SceneView::setEngine()` 仍是"先注销、再换指针" —— 它换的是**借用目标**，那条语义不变。）

## 14. 框架级自评（流程 / 范式 / 协调性 / 可拓展性）

这一节是**对自己设计的批判性复核**：按四条线逐项过，只记"发现的问题"和"结论"，不重复正文。

### 14.1 流程（端到端走一遍）

| 流程 | 走得通吗 | 发现 / 结论 |
| --- | --- | --- |
| 构造 → 配置 → attach → 首帧 | ✅ | R1 保证"配置总在 attach 之前"；首帧仍由 attach 出。**新约束**：完全不用事件循环的宿主必须自己 `init()`（文档已写） |
| 布局落地 → resize → 视图 resize 默认管线 | ✅ | 尺寸/比例一起推（不变量 4）；**几何只有一个输入**（不变量 14，本轮修） |
| 交互 → 命中 → 捕获 → release | ✅（缝） | 契约三条本轮补齐（§5）：`nullptr` 回退自己的视图 / 必须本引擎的视图 / 捕获期持强引用 |
| 换屏（句柄重建）→ 再 attach | ✅ | `ensureWindowPass()` 幂等 ⇒ 管线沿用、后端移动会话；`init()` 之后补一次尺寸 |
| 销毁 | ✅ | 控件 `Impl` 反序（view → engine）；视图 dtor 注销自己的 pass；R2 保"注销那一刻引擎活着" |
| 多视图的 attach | ⚠️→✅ | **本轮修**：宿主的额外视图没有 attach 事件 ⇒ 必须显式 `ensureWindowPass()`（不变量 16、§12.2） |
| 多视图的清屏 | ⚠️（刻意接受） | **后端逼出来的约定**：loadOp 整张清、没有按矩形清 ⇒ "恰好一个清 + 它排最前"（§12.2） |

### 14.2 API 范式

六条范式在 §3.1，逐条都能在正文找到两个以上例子。**只两处不够统一，且都是刻意的**：
①`init()` 用 `bool`、HUD 开关用 `void`+诊断 —— 按"能不能 vs 为什么"判定；②`options()` 是**意图**、`pipeline()` 是**事实**
—— 用不变量 15 收口（不一致的唯一合法原因是宿主自建了主 pass）。

### 14.3 协调性 / 自洽（有没有两份真相）

| 检查 | 结论 |
| --- | --- |
| 矩形 / 尺寸 / 比例 | 本轮收敛成**一个输入**（不变量 14）；同步改了 §4.1 的注释与 §6 的示例（去掉宿主自己算的 `w/2`） |
| HUD 可见性 | 唯一真相 = **pass 的 `isEnabled()`**（不变量 15）；`Options` 只回答"有没有这个档" |
| 清屏 | 唯一真相 = 每个 pass 的清屏档 + "恰好一个清"的约定；**顺序依赖**写进 §12.2 与风险表 |
| 所有权 | §13 五条边 + R2；`Pipeline` 是"引擎活着"的锚；**无环** |
| 术语 | 去掉 "primary view"（与 `MainWindow::primaryRenderControl` 撞词）⇒ 统一叫"**控件自己的视图**" |
| ABI | J7 重述：appfw 公开类加数据成员 = bump `VN_APPFW_PLUGIN_ABI_VERSION` ⇒ `setInputRouter` 一定进 `Impl`；graphics 无版本宏 ⇒ 布局改动允许，但要写明"插件需重建" |

### 14.4 可拓展性（新轴加进来会不会撞车）

| 新轴 | 加它要动什么 | 撞车？ |
| --- | --- | --- |
| `Options::mode`（Lit/Unlit/Wireframe/Normals） | 一个字段 + 一套程序（第 1 层已有 `setProgramOverride`） | 不撞（配方，一次重建） |
| **第 3 个覆盖层**（grid 等） | 现在要 +1 字段 + +1 setter | **阈值规则**：第 3 个出现时改成 `Options::overlays` 位集 + `setOverlayVisible(Overlay, bool)`（一次改名；现在不预抽象 —— Unreal 用位集正是因为它有几十个 flag） |
| 分屏输入路由 | 已在 S6+ ②（缝与语义已定） | 不撞 |
| 连续渲染 | 宿主计时器调 `renderFrame()`；也顺带修"按需渲染下读数只在交互时动" | 不撞 |
| **共享设备** | **会动后端抽象**：今天一个 `RenderBackend` 实例 = 单句柄（`setWindowHandle`）+ 单诊断路由 ⇒ 要引入"**会话（拥有设备）→ 表面（N 条交换链）**"这一层 | 不撞控件/视图 API（诚实成本：动后端 + 注册表） |
| 同一内容多窗口 | 结构已支持（同一 `Scene` 挂两条 pass） | 显存 N 份（目标态解决） |

### 14.5 总评

- **合理**：四层 + 四缝就是行业主流分层（framegraph / render graph / 引擎预设），每层单独可用，且**低层永远能兜住高层**。
- **协调**：全文**只有两处"两份真相"的风险**（矩形、可见性），本轮都收敛掉了；其余不一致都是**刻意划界**
  （`Options` vs `pipeline()`、默认管线 vs 自建管线），而且**有诊断、不静默**。
- **自洽**：三条生命周期规则（R1 初始化 / R2 借用加引用 / R3 配方·几何·开关）各自只回答一类问题、互不重叠；
  16 条不变量覆盖"谁在什么时候做什么"。
- **可拓展**：新轴要么加字段（配方）、要么加 setter（几何/开关）、要么加位集（覆盖层阈值规则）；**唯一会动接口的是
  共享设备**，而它被挡在控件与视图的 API 之外。
- **还不完美的三处（刻意接受）**：①一控件一设备（过渡，有判据与迁移路径）；②多视图清屏靠约定（后端无矩形清）；
  ③"第一次开启某档付一次构建"（换来的是**永不重建**的开关）。
