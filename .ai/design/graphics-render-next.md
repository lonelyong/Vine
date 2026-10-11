# graphics 渲染栈的重新设计提案（next，2026-10-11）

**状态：M1 已落地（2026-10-11）；其余是提案，未落地** —— 本文回答"如果重新设计这一套会怎么切"，并把每条切成能
独立验收的迁移片。落地的部分按本文自己的规矩**以现状文档为准**（`appfw-render-view.md` §4.1、`.ai/memory/graphics.md`），
这里只留下"原稿哪里说错了"的记录。

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
| 3.2 | **呈现契约**：视图声明 (camera, pane, clear)，最后一步归 presenter | 中 | `hasWindowPass(camera)` 那条"谁先到谁说话"的取消规则；重建后补状态的补丁 |
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
L3  视图 / 面板     Camera + Pane（矩形 + 清屏 + 呈现目标）+ 视图级覆盖（调试档）+ 交互器
L4  呈现 / 宿主     Surface/Presenter（拥有交换链与最后一步）+ 输入路由 + HUD 作为带 tag 的普通 pass
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
appfw/src/gui/
    Pane.cpp                    (camera, 矩形, 清屏) 的声明对象
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

### 3.2 呈现契约（Pane + Presenter）

**目标**：视图只声明 `Pane{ camera, 归一化矩形, clear 策略, 呈现目标(null = 交换链) }`；
**最后一步归 surface/presenter**（它拥有交换链，也只有它知道这一帧要画哪些 pane）。
默认窗口管线从"视图自己变出来的东西"变成"presenter 在没有 pane 主张时用的兜底配方"。

**影响面**：`SceneView::ensureWindowPass()` / `ensureWindowPass` 的取消判断、`applyOverlayIntent()`、
`RenderPipelineBuilder` 里"render target 为空的 pass 也算占位"的注释、`appfw-render-view.md` §12.2 的
"恰好一个视图清屏"约定（变成 presenter 的职责，反而更简单）。
**风险**：多 pane 的清屏/顺序第一次有了唯一所有者，需要一张明确的"这一帧画谁"的规则表。
**行业对照**：OSG（`Camera` + `GraphicsContext`）、VTK（`Renderer` + `RenderWindow` + 交互器）、
Unity / three.js（`Camera` + target） —— 视图都不自己抢最后一步。

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
| **M2** | §3.2 呈现契约：`Pane` 声明 + presenter 决定最后一步 | ①默认窗口管线仍自动出现（不配置就是今天的样子）；②宿主自建主 pass 时**不再需要**"同一个相机已有窗口 pass"的取消规则（该判断删除，`hasWindowPass` 用例改写）；③重建之后关掉的 HUD 档仍然关着（`applyOverlayIntent` 若被删则该用例必须仍过）；④分屏：两个 pane 各画各的、恰好一次清屏（presenter 断言） | M1 可并行 | 中 |
| **M3** | §3.1 `resolveRenderState` 单点 | ①逐项状态快照：内容/pass/视图三层各自设过同一项时，最终值 = 优先级表；②`depthExplicit` 类旗标删除后行为不变；③诊断能说出"这一项来自哪一层"；④`graphics-state.md` §4.1 的表从"规则"改成"命名习惯"（文档验收） | 无 | 中 |
| **M4** | 观测：FramePlan dump | ①一条命令/API 打印这一帧的表格（pass 顺序、目标、输入、clear、状态、耗时）；②`test_graphics` 里至少两条用"结构断言"替换 `passCount()`/指针身份；③dump 与真实执行一致（同一帧的 pass 序列相等） | 无（越早越好，M2/M3 的验收会更硬） | 小 |
| **M5** | pane 树 + `ViewContext` 命令目标 | ①一个窗口 split 成两个 pane：各自相机/输入/HUD 互不干扰（用例）；②"显示模式"五条命令在多 pane 下作用于**焦点 pane**（同一命令两个 pane 各来一遍）；③`setPrimaryRenderControl` 仍可用但不参与 pane 目标解析 | M2 | 中 |
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
- M1–M3 不需要触发条件：它们消掉的是**今天就在付**的混乱（一条诊断 + 一条取消规则 + 一张命名表）。
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
| 显式 Pane + presenter 拥有最后一步 | OSG / VTK / Unity / three.js | 我们今天的"视图变出窗口管线"是过渡形态 |
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
