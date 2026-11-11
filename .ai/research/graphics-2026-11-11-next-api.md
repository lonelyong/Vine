# graphics-next：现有概念 / 抽象 / API 的问题与更好的做法

> **状态**：调研提案的一部分（**非现行设计**；未落地、未决策）
> **日期**：2026-11-11
> **属于**：`graphics-2026-11-11-next.md`（总纲）的 §3 展开；与
> `graphics-2026-11-11-next-requirements.md`（要什么）、`graphics-2026-11-11-next-design.md`（怎么做）并列。
> **范围**：**审现有 API 集** —— 概念、命名、API 形状、抽象层次、所有权与错误处理、缺失的能力。
> 每条给：现象 → 证据 → 判据（`G#`）→ 更好的做法 → 迁移成本。
> **出口**：采纳 → 按 §10 分批改；否决单条 → 在对应 ADR 记理由。本文件在采纳后删除。

---

## 1. 怎么读

编号 `P#` 是"问题"，`M#` 是"缺失的能力"。每条都有**证据**（文件:行，或数出来的个数），
没有证据的不算问题，只算偏好。判据引 `G1–G13`。

---

## 2. 概念层（一个概念被说了两遍 / 说不清）

### P1 一个 pass 有两种表示（`G1`）

`PassDesc`（值）与 `RenderPass`（约 25 个 setter 的可变对象）并存，`RenderPass::setDesc()` 在两者间拷贝
（`RenderPass.hpp:92`）。`PassDesc` 的注释自己承认："a descriptor IS a hand-built pass by the time the engine
sees it"，且手搓 pass 是 "escape hatch"。

**更好**：只有 `Pass` 一个值；`RenderPass` 作为**构建器**存在（`RenderPass::desc() → Pass`），不进 SDK 的
执行路径。**成本**：中（宿主改写法，语义不变）。

### P2 声明里有回调（`G4`）

`PassDesc::before_draw` 是 `std::function<void()>`（`PassDesc.hpp:201`）。带回调的值
**不能比较、不能差分、不能安全过线程**，而"计划是值"的一切收益都建立在比较与差分上。

**更好**：删掉。宿主若要"在某 pass 前做点事"，那是会话的一个**已知位置的钩子**（且只在 inline 模式成立），
不是声明的一部分。**成本**：小（今天没有生产消费者）。

### P3 一个 pass 的目标有四种说法（`G1` / `G4`）

`writes`（`RenderTarget` 指针）/ `writes_image`（字符串身份）/ `TransientShape`（尺寸+格式）/
"null = 画到窗口"，再加上引擎侧 `transientTarget(name)` 与 `transientTargetCount()` 去**取回**那个目标
（`RenderEngine.hpp:523`、`:540`）。

这是"声明 + 按名字取回"的隐式两阶段协议：宿主写了声明，还得再去引擎问"你给我建的 target 在哪"，
并且跨尺寸变化时那个句柄**故意失效**。

**更好**：目标只有一种表达 —— `Target` 值（`Window | Offscreen{name,shape} | External{Handle}`），
派生它的地方就是它的 owner；宿主按**名字**取（`Session::target(name)` 返回值视图），而不是取回一个会失效的指针。
**成本**：中。

### P4 `PassKind` 与投影重复（`G1`）

`PassKind{Scene, Screen}`（`PassDesc.hpp:43`）× `ProjectionKind`。屏幕 pass 只是"投影=屏幕"的视图，
不是一个种类，也不需要 `ScreenPass` 这个类。

**更好**：`Pass{ name, view, target, clear, state, inputs, order }`，投影是 `view.projection` 的取值。
**成本**：中（`ScreenPass` 的消费者是 HUD / 后期，用例齐）。

### P5 顺序概念有四套（`G1` / `G6`）

1. `PassStage`（枚举阶段）+ 2. `PassDesc::order`（显式覆盖）+ 3. 节点 `draw_order` + 4. 排序键里的层与深度。
每一套都有"谁覆盖谁"的规则，但要读三处文档才知道。

**更好**：**两套**且各司其职 —— pass 之间一个 `order`（整数，阶段枚举降级为便捷常量），
绘制项之间一个**全序键**（`draw_order, layer, transparent, depth`，见 `graphics-2026-11-11-next-design.md` §4.4）。
**成本**：小（语义等价，去掉一个枚举）。

### P6 值里有装饰字段（`G8`）

`PassDesc::tag`：注释写着 "A label … No behaviour."。值里的死字段要参与比较、拷贝、dump，却不决定任何行为。

**更好**：删；要给人读的标签就用 `name`（本来也要人读）。**成本**：小。

### P7 内容层会产出渲染命令（`G2` 回边）

`Scene::collectRenderCommands(...)` 直接在 Scene 上产生 `RenderCommand` 列表。于是**内容层知道**
`DrawList` / `RenderCommand` / 排序，而执行层又反过来读内容的活对象（这正是材质那一类 bug 的来源）。

**更好**：`Scene` 只产出**快照**；派生是独立纯函数 `derive(snapshot, view) → DrawList`。
**成本**：中（内部重组，公开 API 只变一处）。

### P8 一个默认程序是环境态（`G6`）

`setDefaultContentProgram()` / `defaultContentProgram()`（`RenderEngine.hpp:557`）是**引擎级**的默认着色，
对每一处内容 draw 生效。它是"别处的一个开关改变这里的结果"的典型环境态。

**更好**：默认值属于**计划**（`FramePlan::default_program`）或视图（`View::program_override`），
作用域与生命随值走。**成本**：小。

### P9 内容可以被两个入口改（`G4`）

pass 的 content 既写在声明里（`PassDesc::content`），又能经 `bindPassContent(pass, scene)` 改，
还能用 `contentOf(pass)` 读（`RenderEngine.hpp:683`、`:690`）。同一件事两个写者。

**更好**：内容只属于声明；引擎没有 binder，只有"提交新计划"。**成本**：小。

---

## 3. API 形状（入口、命名、观测）

### P10 一个引擎 49 个公开方法，画一帧只要 6 个（`G4` / `G8`）

实测 `RenderEngine.hpp` **48–49 个 public 方法**（`public:` 到第一个 `private:` 之间）。其中：

| 类别 | 例子 | 数 |
|---|---|---|
| 真正驱动 | `setDevice` / `initialize` / `addPass` / `frame` / `waitForFrame` / `shutdown` | 6 |
| 观测计数 | `contentCollectCount` / `contentCollectReuseCount` / `diagnosticCount`×2 / `diagnosticsClean` / `transientTargetCount` / `passCount` / `surfaceWidth`/`Height` | 9 |
| 内部缝 | `frameDescription` / `framePlan` / `passOrder` / `transientTarget` / `contentOf` / `bindPassContent` / `reportEngineProblem` | 7 |
| 边跑边改的输出协议 | `publish` / `resolve` / `unpublish` / `createSurface` | 4 |
| 开关 | `setFrameThreading` / `frameThreading` / `setDefaultContentProgram` / `setDiagnosticSink` | 4 |
| 结果 | `frame`(outcome) / `presentRecord` / `presentedCamera` / `capabilities` / `frameReport` | 5 |

**更好**：会话 7 个方法（`open/submit/present/wait/report/capabilities/close`）；
所有观测收成一个 `FrameReport` 值；内部缝全部私有化（今天 `reportEngineProblem` 是 public，
注释说"因为引擎自己的 builder 也用它" —— 那是内部调用，不是公开理由）。**成本**：大（宿主全改，见 §9）。

### P11 观测散成 9 个函数，而不是一个值（`G11`）

`builds`、`draws`、`passes`、`record_micros`、`execute_micros`、`presented_view`、`diagnosticCount` 分居
8 个 accessor，没有一个可以整体 dump / 整体断言 / 跨帧保存。

**更好**：`FrameReport` 值（`graphics-2026-11-11-next-design.md` §3.7），一帧一个，可存可 diff。**成本**：小。

### P12 七个名字指"画东西的那个东西"（`G1` / `G6`）

`RenderEngine` / `RenderDevice` / `RenderBackend` / `RenderFrameExecutor` / `RenderSurface` /
`WindowPresentTarget` / `PresentTarget`。

**更好**：三个 —— `Device`（后端实现）、`Session`（驱动 + 线程 + 表）、`Surface`（present 目的地）。
**成本**：小（改名 + 合并两个纯接口）。

### P13 五个名字指"一次画的描述 / 结果"（`G1`）

`FrameDescription` / `EngineFrame` / `FrameValue` / `FrameOutcome` / `PresentOutcome`。

**更好**：`Frame`（值）+ `PresentOutcome`（一个枚举）。**成本**：内部为主。

### P14 `CollectView` 是视图，却叫收集（`G4`）

`CollectView{view_proj, eye, viewport, layer_mask}` 是一个**视图值**；名字泄露了它的历史用途（收集时传参）。

**更好**：`View`（并把投影种类并进来，见 P4）。**成本**：小。

### P15 `ShaderProgram` 不是程序，是阶段集合（`G4`）

`ShaderProgram` 有 `addStage/clearStages/setStage/stageCount/stage(i)`（`ShaderProgram.hpp:76–133`），
是**可变的阶段列表**；`ShaderStageType::Compute` 注释写着 "reserved"（一个不能用的取值，`G8`）。

**更好**：`Program`（不可变值，`Program::fromStages`）+ `ProgramStage`；reserved 的枚举值删掉，
等真有 compute pass 再加（加的时候是一次有消费者的改动）。
**成本**：小（消费者少，改写机械）。

### P16 所有权词汇混用（`G4` / `G10`）

同一个概念位置，有时是 `intrusive_ptr`（`RenderCommand::material`），有时是 `raw_ptr`
（`Geometry::material()`、`StateNode::material()`）。规则存在（"借的用 raw，持的用 intrusive"）但没有写在
任何一个公共头里。

**更好**：在 SDK 顶部写一条规则并在类型名上体现（`Borrowed<T>` / `Owned<T>` 之类的别名，或至少在所有
getter 的 `@return` 里点明"借"），并把命令改成持有时明确标出。**成本**：小。

### P17 魔法值代替 `optional`（`G4`）

`TransientShape::width/height` 的注释："0 for the size of what this pass' frame is presenting"
（`PassDesc.hpp:56`）。用 0 表达"未指定"，于是 0 不能是合法尺寸，读代码的人必须知道这条约定。

**更好**：`std::optional<int>`（仓库里 `PassDesc::order` 已经这么做了 —— 同一文件里两种风格更糟）。
**成本**：小。

### P18 三层创建、顺序靠注释（`G3`）

`setDevice()` → `initialize()` → `addPass()` → `frame()`，顺序错了只会得到后面的 `false` 或空帧。
没有工厂、没有"已初始化"的类型状态。

**更好**：`Session::open(config, sink)` 一次把不变量建立起来，失败**只有一种**表达（见 P27 的 `Result`）。
**成本**：中。

---

## 4. 抽象层次

### P19 声明里出现后端概念（`G2`）

`PassDesc::TransientShape` 的单位写的是 **DEVICE pixels**，还有 `DepthView`（别的 pass 的深度）、
`RenderTarget::ColorFormat/DepthFormat`、`depth_promotion`。这些是**后端/设备的语言**，却出现在"pass 是
什么"的声明里。

**更好**：声明只说"我要一张 2048×2048、带深度的图"（尺寸用逻辑单位 + 缩放策略），格式归**能力协商**或
目标工厂；`DepthView` 这种跨 pass 的结构关系改成"输入引用"（按名字）。**成本**：中。

### P20 包装层转发而不是吸收（`G5` / `G7`）

`RenderControl`（appfw）18 个公开方法里，`engine()`、`setFrameThreading`、`frameThreading`、`surface()`、
`state`、`hasPresented`、`failureReason` 都是把引擎/窗口的状态**搬出来**；`viewer()`、`viewerOptions()`、
`setShadingMode()` 又引入第二套策略。

**更好**：`Viewer` 只吸收"窗口 + 输入 + 一帧"这三件事（`setScene` / `run` / `close`），
其余状态一律经 `report()` 一个值。**成本**：中（app_shell 与 test_gui 要改）。

### P21 全局"着色模式"开关（`G6`）

`RenderControl::setShadingMode(mode)` —— 一个视图级的模式枚举（线框/着色/法线…），它改的是**每一处 draw 的
程序**，但存在包装层上，与内容、程序、材质三处解耦。

**更好**：它就是一个**视图级程序覆盖**值（`View::program_override`），或一个计划层的 `state` 覆盖；
不新增枚举。**成本**：小。

### P22 诊断路由的公开缝（`G4`）

`reportEngineProblem` 是 public（`RenderEngine.hpp:287`），理由是"引擎自己的 builder 也用它"。于是宿主也能
往引擎的诊断流里注入任意文本，计数与门禁的语义被污染（`diagnosticCount()` 不再是"引擎发现的问题数"）。

**更好**：内部 `report()`；宿主只有一个方向：**装 sink**。**成本**：小。

### P23 缺少"声明之后能问回来"的东西（`G11`）

今天能读"计划长什么样"（`framePlan` dump）、"上一帧画了什么"（`frameDescription`），但没有一个**稳定的观测值**：
`FrameReport` 应包含 `passes/draws/builds/resident/snapshot_epoch/presented_view/耗时`，并且可序列化。

**更好**：见 P11。**成本**：小。

### P24 生命周期 API 混进内容 API（`G1`）

`releaseContent(Object* content)`（`RenderEngine.hpp:656`）让宿主按对象显式驱逐 GPU 状态。它是**驻留提示**，
不是内容操作，却挂在引擎的公开面上并按对象指针寻址。

**更好**：`Session::forget(ContentId)`（句柄、提示语义），且驻留默认由快照引用决定（见 `graphics-2026-11-11-next-design.md` §4.6）。
**成本**：小。

---

## 5. API 风格：三档与一条线

### 5.0 三种风格，各有各的岗位

| 档 | 形态 | 谁用 | 今天的例子 |
|---|---|---|---|
| **值（描述形）** | 聚合初始化 + `withXxx()` 返回新值；**没有 setter** | 跨线程、进计划、被比较、被序列化 | `CameraSnapshot`、`MaterialParameters`、`RenderState`、`Viewport` |
| **对象（get/set 形）** | Qt 风格访问器；有身份；单线程增量编辑 | 作者层（场景图、材质、相机、几何） | `Scene`、`Node`、`Geometry`、`Material`、`Camera` |
| **构建器** | `build()` 产出一个**值**，自身不进入运行期 | 参数多、有默认值、一次成型 | `RenderPass`（本该如此）、`PassChain`（本该如此）、`Program` 的编译前阶段 |

**判据不是审美，是物理事实**：

> **会跨线程 / 会进计划 / 会被比较 → 值。**
> **有身份 / 单线程 / 增量编辑 → 对象。**
> **只在创建时存在 → 构建器。**

这也是为什么"全值"与"全 setter"都不可行：全值 ⇒ 每次编辑拷整棵树（`G10`）；
全 setter ⇒ 跨层只能靠锁 + 镜像 + 版本（今天 2519 行的由来）。

### P25 描述类型的后缀有 6 种（`G6`）

实测 9 个类型、6 种后缀：`PassDesc` / `DeviceDesc` / `ChainDesc`（Desc）；`ViewerOptions` /
`AxisGizmoOptions` / `FpsOverlayOptions`（Options）；`ShadowSettings`（Settings）；`SplashConfig`（Config）；
`RenderBackendInfo`（Info）；`MaterialParameters`（Params）。

**更好**：**一个后缀**——创建参数一律 `XxxConfig`；能力一律 `Capabilities`；跨层的值**不带后缀**（`Pass`、
`View`、`Frame`、`FramePlan`）；"某一时刻的值"用 `XxxSnapshot`。**成本**：小（纯改名）。

### P26 126 个公开方法返回 `bool`（`G4`）

`bool` 现在同时承担三件事：**成功/失败**、**有/无**、**找到/没找到**。读调用点时无法区分
`if (!engine.initialize())` 与 `if (!scene.hasRootedRoot())` 属于哪一种。

**更好**：谓词叫 `isXxx()` / `hasXxx()` 且只表达真假；会失败的操作返回 `Result`（P27）。**成本**：中。

### P27 错误表达有三套（`G6`）

1. `bool`（126 处）；
2. **出参枚举**：`createSurfaceTarget(handle, SurfaceTargetResult* why)`、`readColorBuffer(..., ReadbackResult* why)`
   （`RenderDevice.hpp:228 / :333 / :361`）；
3. **诊断文本**：`DiagnosticCategory` + message，**没有稳定 code**（于是测试只能匹配文字）。

**更好**：一套 —— `Result<T>`（值，或 `DiagnosticCode` + 结构化 detail），诊断仍走单一路由 + episode。
`SurfaceTargetResult` / `ReadbackResult` 这类枚举改成 `DiagnosticCode` 的取值。**成本**：中（调用点要改）。

### P28 回调散在 8 个 SDK 头（`G4`）

`PassDesc::before_draw`（**值里**）、`PassChain::OverlayLayout`（`std::function<void(RenderPass&, int, int, double)>`）、
`SceneView::addSurfaceLayout`（2 个重载）、`LayoutParticipant`、`RenderDiagnostic`（sink，**合法**），加上两个 appfw UI 头。

**规则**：**值里零回调**；回调只允许出现在**对象**（事件处理器）与**会话配置**（`DiagnosticSink`）上，
并且必须在注释里写明**在哪个线程被调用**。**成本**：小（值里的两处删掉即可）。

### P29 `OverlayLayout` 把可变 pass 交给回调（`G1` / `G7`）

`std::function<void(RenderPass& pass, …)>` —— 回调里改一个正在成型的可变 pass。这是"pass 值化"的**最大障碍**：
值不能逃逸可变引用，而这个回调的存在使得"pass 在创建后仍会被改"合法化。

**更好**：回调只接收**值**并返回新值（`Pass(Pass)`），或者改成"布局产出 `View` 值"。**成本**：中（HUD / overlay 用例要改）。

### P30 访问器与命令共用 `get` 前缀（`G4`）

`VisualUserIO::getStringAsync / getIntAsync / getDoubleAsync / getPoint3dAsync / getFileAsync / getFilesAsync`
（8 处）—— 它们不是属性访问器，是**命令**（"向用户要一个值"）。仓库规则禁 `getXxx` 访问器是对的，
但一刀切会误伤命令。

**更好**：规则按"访问器 vs 命令"分：访问器用 Qt 风格（`name()`）；命令用动词（`askStringAsync`）。
**成本**：小。

### P31 值可以有成员函数，但不能有 setter（`G4`）

`CameraSnapshot::screenRay()` 是纯函数成员 ✓。判据：一旦出现 `set*` 或非 const 成员，它就不是值了，
必须换档（变成对象或构建器）。

### P32 预设 = 返回值的自由函数（已有的好模式，应写成规则）

`unlitForwardProgram()`、`ChainPresets` 一族都是"直接返回值"的预设，没有"配置对象 + apply"。
**规则**：预设一律是返回值的自由函数；不要为预设造 enum + switch（`G8`）。

### 5.1 今天的东西该归哪一档（含放错的）

| 类型 | 应在 | 今天实际 | 处理 |
|---|---|---|---|
| `Scene` / `Node` / `Geometry` / `Material` / `Camera` | 对象 | 对象 ✓ | 保留 |
| `MaterialParameters` / `RenderState` / `Viewport` / `CameraSnapshot` | 值 | 值 ✓ | 保留（后缀统一） |
| `Pass`（`PassDesc`） | 值 | 值但**带回调 + 四种目标说法** | 删回调，目标合一 |
| `RenderPass` | **构建器** | 可变对象（25 setter） | 退为 `PassBuilder`（`build() → Pass`） |
| `PassChain` | **构建器** | 活到运行期、自己 add/remove | 收成 `FramePlanBuilder`（`build() → FramePlan`） |
| `Program`（`ShaderProgram`） | **值** | 可变对象 + revision | 值 + `ProgramBuilder` |
| `RenderControl` | **适配器**（只吸收） | 转发 18 个方法 | 收成 `Viewer`（4 个方法） |
| `RenderEngine` | 对象（会话） | 对象但 49 个公开方法 | 收口到 7 |
| `Diagnostics` | 对象 ✓ | 对象 ✓ | 加 `DiagnosticCode` |

### 5.2 门禁（已落地：`scripts/check_api_style.py`）

四条纯文本规则，扫 `src/viz/graphics/sdk/vine/graphics/*.hpp`（75 个头）：

**今天的基线：0 条新增、4 条已登记的既有发现**（每条都对应本文件的一个 `P#` 与一个批次，
修完就删掉它那一行）：

| 文件:行 | 符号 | 规则 | 归属 |
|---|---|---|---|
| `PassDesc.hpp:89` | `std::function` | A（值里不许有回调） | P2 / 批 A |
| `PassChain.hpp:27` | `ChainBuilder`（前向声明） | D（构建器必须有 `build()`） | P1 / 批 C |
| `RenderDevice.hpp:333` | `ReadbackResult` | C（bool + 出参） | P27 |
| `RenderDevice.hpp:361` | `ReadbackResult` | C | P27 |

四条规则本身是：

1. 名字以 `Desc|Options|Settings|Info|Config|Params` 结尾的类型：**不得**有 `set*` 方法、**不得**有
   `std::function` 成员（值的定义）。
2. `sdk/` 下 `get[A-Z]\w*` 零容忍，白名单逐个改名（P30）。
3. `bool` 返回 + `XxxResult*` 出参同时出现：零容忍（P27）。
4. `XxxBuilder` 必须有 `build()`，且 `build()` 返回值（不返回引用）。



## 6. 缺失的能力（`M#`）

| # | 缺什么 | 今天的替代 | 判据 |
|---|---|---|---|
| M1 | 能力**协商**（必选/可选 + 拒绝原因 code） | `capabilities()` 只答"有什么"，不答"我不满足你是否还能跑" | `G4` |
| M2 | 管线**预热查询**（给定快照答需要什么） | 首帧现场编译，卡顿不可预算 | `G10` |
| M3 | 异步编译的显式语义（未就绪 ⇒ 跳过 + episode） | 首次出现的卡帧 | `G11` |
| M4 | 帧**节流策略**（深度 / 延迟 / vsync） | 硬编码深度 1 | `G10` |
| M5 | 计划**回读**（我提交的计划下一帧会是什么样） | 只有 `framePlan` dump 文本 | `G11` |
| M6 | replay 后端（录/放帧值） | 无，用户 bug 只能寄日志 | `G11` |
| M7 | 查询吃视图值（pick / box / fit 统一签名） | 各自一套，且部分吃活相机 | `G12` |
| M8 | 内容**句柄化**（陈旧使用可判定） | `const void*` + 兜底 | `G10` |
| M9 | 单位与颜色空间在类型里 | 注释约定 | `G10` |
| M10 | 视图的**多表面**（一设备多窗口） | 一引擎一表面，第二表面被拒 | `G5` |

---

## 7. `RenderEngine` 49 个公开方法的去向

| 方法（行号） | 去向 | 说明 |
|---|---|---|
| `device@184` / `setDevice@200` / `initialize@304` | **合并** → `Session::open` | 创建一次把不变量建好 |
| `shutdown@316` | 保留 → `close()` | 幂等相位 |
| `frame@336` | 保留 → `present(request)` | 返回 `FrameResult` |
| `waitForFrame@375/382` | 合并 → `wait(frame_id)` | 一个重载（无参 = 最后一帧） |
| `addPass@587/605/624` / `removePass@640` / `clearPasses@664` / `passCount@670` / `passOrder@474` | **删除** | 计划是值（`submit`） |
| `bindPassContent@683` / `contentOf@690` | **删除** | 内容只属于声明（P9） |
| `framePlan@492` / `frameDescription@512` | **合并** → `FrameReport` / dump | 观测是值 |
| `presentRecord@403` / `presentedCamera@419` | **合并** → `FrameReport.presented_view` | 一个值 |
| `frameNumber@360` | **并入** `FrameResult.frame_id` | |
| `surfaceWidth@430` / `surfaceHeight@436` | **并入** `FrameReport.target` | |
| `contentCollectCount@237` / `contentCollectReuseCount@244` | **并入** `FrameReport` | 且新设计里 memo 概念消失 |
| `diagnosticCount@256/263` / `diagnosticsClean@273` | **并入** `FrameReport.diagnostics` + `Session::diagnostics()` | 保留一个计数口 |
| `diagnostics@177` / `setDiagnosticSink@214` / `diagnosticSink@220` | 保留（收成一个 `setSink`） | 单路由不变 |
| `reportEngineProblem@287` | **私有化** | 见 P22 |
| `capabilities@448` | 保留 | + 必选/可选（M1） |
| `frameReport@459` | 保留并扩展 | 它的雏形就是 `FrameReport` |
| `transientTargetCount@523` / `transientTarget@540` | **删除** | 目标按名字取值视图（P3） |
| `setDefaultContentProgram@557` / `defaultContentProgram@563` | **移入计划/视图** | 见 P8 |
| `releaseContent@656` | 改成 `forget(ContentId)` | 见 P24 |
| `publish@708` / `resolve@719` / `unpublish@725` | **内部化** | 输出协议的实现细节 |
| `createSurface@743` | **并入** `Session` + `Surface` 值 | 见 M10 |
| `setFrameThreading@349` / `frameThreading@352` | **合并** → `SessionConfig.execution` | 一个轴 |
| `lease@144` / `passes@157` / `outputs@167` | 保留但收进 `FrameReport` | 读的入口少而稳 |

**净结果**：49 → **7**（`open` / `submit` / `present` / `wait` / `report` / `capabilities` / `close`）+ 诊断 sink。

---

## 8. 其它类的逐项

| 类 | 问题 | 更好的做法 |
|---|---|---|
| `PassDesc`（约 25 字段） | P1/P2/P3/P4/P5/P6/P17/P19 全在它身上 | 拆成 `Pass`（8 字段）+ `Target` + `View` + 能力协商 |
| `RenderPass`（25 setter） | 与 `PassDesc` 重复；`setViewport` 三种状态靠 `clearViewport()` | 退为构建器，可选字段用 `optional` |
| `PassChain`（29 方法） | 又一套 add/remove（按角色名） | 收成 `FramePlan` 的构建器，产出值 |
| `ShaderProgram`（11 方法） | P15 | `Program` 值 + `ProgramStage`；去掉 reserved |
| `Material`（10 setter） | 基本合理；`resolved()` 暴露活值给任意调用者 | 保留 setter，`resolved()` 标注"内容线程"并只给发布用 |
| `Geometry` | `material()` 借出、`description()` 持有 —— 词汇混用（P16） | 统一 `@return` 语义 + 别名 |
| `Scene`（30 方法） | P7（产出渲染命令）；`invalidateContent` 是"我也不知道改了什么"的逃逸口 | 只产出快照；逃逸口收窄成"值脏"标记 |
| `RenderControl`（18 方法） | P20/P21 | 收成 `Viewer`（4 个方法） |
| `Diagnostics` | 好设计（单路由 + episode）；缺 code（M-） | 加 `DiagnosticCode` |

---

## 9. 命名对照

| 今天 | 新 | 理由 |
|---|---|---|
| `RenderEngine` | `Session` | 它是一次"会话"（设备 + 线程 + 表），不是引擎 |
| `RenderDevice` + `RenderBackend` + `RenderFrameExecutor` | `Device` | 三者是同一个后端实现的三张脸 |
| `RenderSurface` + `WindowPresentTarget` + `PresentTarget` | `Surface` | 一个目的地 |
| `PassDesc` + `RenderPass` + `ScreenPass` | `Pass` | 一个声明 |
| `PassKind` | 删（`View::projection`） | 屏幕 pass 只是一种视图 |
| `ShaderProgram` + `ShaderStage` | `Program` + `ProgramStage` | 它是一组阶段 |
| `CollectView` | `View` | 它就是一个视图 |
| `FrameDescription` + `EngineFrame` + `FrameValue` | `Frame` | 一次画的输入 |
| `FrameOutcome` + `PresentOutcome` | `PresentOutcome` | 一个结果枚举 |
| `FrameMode` + `RunnerMode` + `FrameThreading` | `ExecutionMode` + `PipelineDepth` | 一个轴一个枚举 |
| `releaseContent` | `forget(ContentId)` | 提示，不是操作 |

---

## 10. 分批（每批可独立评审）

| 批 | 内容 | 语义变化 | 破坏面 | ROI |
|---|---|---|---|---|
| **A. 删死物** | 删 `PassDesc::tag`、`before_draw`、`ShaderStageType::Compute`；`TransientShape` 改 `optional`；`reportEngineProblem` 私有化 | 无 | 小 | **最高**（几小时，纯减法） |
| **B. 改名** | §8 的映射，`using` 别名 + `[[deprecated]]` 过渡 | 无 | 小 | 高（阅读成本立刻下降） |
| **C. 值化 pass** | `Pass`/`FramePlan` 进 SDK；`RenderPass` 退为构建器；`ScreenPass` 删除 | 中 | 中 | 高（P1/P3/P4/P5 一起消） |
| **D. 收口 session** | 49 → 7；观测收成 `FrameReport`；删 `publish/resolve/unpublish/transientTarget*` | 中 | 大 | 高 |
| **E. 缺失能力** | `M1–M10`，各自独立、有消费者才做 | 增 | 小 | 中（每条独立评估） |

**顺序理由**：A 与 B 是"零语义、低风险、立刻见效"；C 是概念收益最大的一批；D 破坏面最大，放在 C 之后
（C 完成后 D 才有值可比）；E 全部是增量，不与前四批冲突。

---

## 11. 明确不做

| 不做 | 理由 |
|---|---|
| fluent builder 全局化 | 会把值变回可变对象，与"声明是值"冲突 |
| 再包一层 manager/control | P20 就是这条的后果 |
| 在这一批里做单位类型化（`Pixels`/`Points`） | 它有自己的收益，混进来会让评审爆炸 |
| 为 `M#` 里没有消费者的项预先设计 | `G8` |
| 保留 `RenderPass` 的双表示以"兼容" | 兼容用别名在最外层做，不留在核心 |

---

## 12. 一句话

**今天这套 API 的问题不是"缺功能"，而是同一个概念被说了两遍（pass 两种表示、目标四种说法、顺序四套、
观测九个函数、引擎七个名字），以及把内部缝和开关放进了公开面。**
更好的做法几乎全是**减法**：一个概念一个表示、观测一个值、会话七个方法、内部缝私有化 —— 所以 A/B 两批
（删死物 + 改名）可以在语义零变化的前提下先做，收益立即可得，风险接近零。
