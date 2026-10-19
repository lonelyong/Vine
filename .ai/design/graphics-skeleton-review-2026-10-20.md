# graphics 骨架复核：对象关系、职责与命名（2026-10-20）

> **这一轮问的是"骨架"**：对象之间谁拥有谁、谁是 1:1、谁在借、一个概念被几个对象同时担着。
> 顺序是**先画图、再逐对象审、最后才改代码**——图不对，改哪里都是猜。
>
> 依据：`graphics-layering.md`（两半与依赖方向）、`graphics-object-model-vs-3d-frameworks.md`
> （逐对象对到 VTK / OSG / three.js / Filament / Qt3D）、`graphics-abstraction-review-2026-10-19.md`
> §0（判断口径）、`graphics-api-simplification-2026-10-20.md`（上一轮瘦身）。
> 本轮**不重开已裁决的题**（§5 逐条列出）。

## 0. 这一轮加的口径

前两轮的口径（一个事实一个权威 / 声明与执行分开 / 缺席与零分开 / 机制在 SDK 策略在应用层 /
同一件事没有第二种写法）+ 本轮两条：

1. **每条借用边要么不可能悬空，要么可查。** `raw_ptr` 只允许出现在"所有者保证活到使用者结束"
   的地方；做不到就必须 `Lease<T>`。一个模块里两种答案并存，等于没有答案。
2. **一条 1:1 关系的 owner 必须是这条边的一端。** 如果 A 与 B 一对一、互相借、由第三方持有，
   那是"三方约定"，不是关系——它会在某个宿主的析构顺序里失效。

## 1. 对象关系图

### 1.1 宿主侧：一扇窗 → 一个区 → 一幅画

```mermaid
graph TD
    RC["RenderControl<br/>(Qt widget)"] --> SW["SurfaceWindow<br/>(宿主会话)"]
    SW -->|owns intrusive_ptr| ENG["RenderEngine<br/>调度器"]
    SW -->|owns intrusive_ptr| SUR["RenderSurface<br/>区：尺寸 + 展示声明"]
    SUR -.->|Lease · 可查| ENG
    ENG -.->|borrow raw_ptr<br/>area_ = 0..1| SUR
    SUR -->|owns intrusive_ptr| PPASS["展示 pass<br/>(窗口 pass)"]
    SUR -.->|borrow raw_ptr| WPT["WindowPresentTarget<br/>(交换链，设备造)"]
    VIEW["SceneView<br/>门面"] -.->|Lease| ENG
    VIEW -.->|borrow raw_ptr| SUR
    SUR -.->|borrow raw_ptr<br/>互指| VIEW
    VIEW -->|owns| PIPE["RenderPipeline<br/>配方结果"]
    PIPE -.->|Lease| ENG
    ENG -->|owns intrusive_ptr| DEV["RenderBackend<br/>device + executor"]
    DEV -->|creates, owns| WPT
```

**读法**：实线 = 拥有（`intrusive_ptr`），虚线 = 借用。三条看似"互相借"的边里，
`区 → 引擎` 与 `视图 → 引擎` 是可查的租约；`引擎 → 区` 与 `区 ↔ 视图` 是裸借用，
由宿主（实例态 `SurfaceWindow`）的析构顺序保证活着。

### 1.2 内容模型：一棵树 + 两处"最近声明优先"

```mermaid
graph TD
    SCENE["Scene<br/>根 + 灯 + 收集 + memo"]
    SCENE -->|owns root| ROOT["Node"]
    SCENE -->|owns 0..N| LIGHT["Light"]
    ROOT --> G["Group"]
    G --> MT["MatrixTransform"]
    G --> SN["StateNode<br/>material / program / state"]
    SN --> GEO["Geometry<br/>通道 + 索引 + material + program"]
    GEO --> MAT["Material"]
    GEO --> TEX["Texture"]
    GEO --> PROG["ShaderProgram"]
    SN -.->|最近声明优先| GEO
    SCENE ==>|collectRenderCommands<br/>每帧一次/每相机| CMD["vector&lt;RenderCommand&gt;<br/>扁平快照"]
```

**读法**：`==>` 是从"结构"到"快照"的唯一一次跃迁；之后的一切都只读 `RenderCommand`
（`FrameDescription` 持 `shared_ptr<const vector<RenderCommand>>`）。材质/程序在
`StateNode` 与 `Geometry` 都能声明、就近者胜，这是 OSG / vsg 的 `StateSet` 语义（刻意保留）。

### 1.3 一帧怎么走：声明 → 编译 → 值 → 后端

```mermaid
sequenceDiagram
    participant H as 宿主
    participant E as RenderEngine
    participant P as RenderPass/ScreenPass
    participant F as FrameDescription
    participant B as RenderBackend
    participant X as FrameCompiler
    H->>E: frame(dt)
    E->>E: compileFrame()（每帧，读声明）
    E->>P: describe(PassDescription&) 每个 enabled pass 一次
    P-->>E: 画什么（DrawDescription）
    E->>F: buildFrameValue()（借用快照 + 解析后的输入）
    E->>B: executeFrame(FrameDescription)
    B->>X: compile（FrameGraph：依赖/别名/时序）
    X-->>B: 执行计划
    B->>B: 记录并提交，present 经 PresentTarget
    E->>B: endFrame / swap
```

**读法**：`FrameDescription` 与 `FramePlan` 是**同一次编译的两个投影**（后端读者 / 宿主读者），
这一点 2026-10-19 已裁定（§5），本轮不重开。

### 1.4 配方层：谁造 pass

```mermaid
graph LR
    OPTS["RenderPipelineOptions"] --> BLD["RenderPipelineBuilder"]
    BLD -->|addPass(PassDesc)| ENG["RenderEngine"]
    BLD -->|产出| PIPE["RenderPipeline<br/>持有 passes/targets/overlays"]
    PIPE -->|注册其 passes| ENG
    VIEW["SceneView::setPipeline"] -->|采纳| PIPE
    SUR["RenderSurface::present(view)"] -->|解析回 view 的窗口 pass| PIPE
```

## 2. 所有权矩阵（照代码核过）

| 边 | 机制 | 基数 | 谁销毁 | 判据/备注 |
|---|---|---|---|---|
| 宿主 → Engine | `intrusive_ptr` | 1 | 宿主 | `SurfaceWindow::Data::engine` |
| 宿主 → RenderSurface | `intrusive_ptr` | 1 | 宿主 | 必须**先于**引擎销毁 |
| Engine → RenderSurface | `raw_ptr` | 0..1 | **不是引擎** | 与"父持有子"的 F5 裁决相反 |
| RenderSurface → Engine | `Lease` | 1 | — | **本轮改的**（原来 `raw_ptr`，析构里解引用） |
| Engine → RenderBackend | `intrusive_ptr` | 0..1 | 引擎（生命周期归创建者） | 借用语义写进注释 |
| RenderBackend → WindowPresentTarget | 设备造 | 1:N | 设备 | 区只借 |
| SceneView → Engine | `Lease` | 1 | — | 2026-10-19 F5 |
| RenderPipeline → Engine | `Lease` | 1 | — | 2026-10-19 F5 |
| SceneView → Surface | `raw_ptr` | 0..1 | 宿主 | 与 `区 → 视图` 互指 |
| SceneView → Camera / Scene / Manipulator / Pipeline | `intrusive_ptr` | 1 / 1 / 1 / 0..1 | 视图 | |
| RenderPipeline → passes / targets / overlays | `intrusive_ptr` | 1:N | 管线 | 引擎注册时另持一份 |
| Scene → root / lights | `intrusive_ptr` | 1 / N | 场景 | |
| RenderPass → camera / outputs / inputs | `intrusive_ptr` | | pass | **与 `PassDesc` 逐字段重复** |
| RenderTarget → Light | `intrusive_ptr` | 0..1 | 目标 | `shadowOf()` |
| FrameDescription → 全部 | `raw_ptr` + 一个 `shared_ptr` | | 生产者（借用） | 见 SK2 |

## 3. 逐对象审核

判据三问：**它担了几件事？业界同位置是谁？这些事会不会因为不同理由而变？**

| 对象 | 担的事（数） | 业界同位置 | 判断 |
|---|---|---|---|
| `RenderEngine` | 注册/排序 pass、每帧编译、诊断路线、发布槽、借用设备、借用区（6） | Filament `Engine`+`Renderer`；O3DE `RPI::Scene`；wgpu `Device`+`Queue` | **保留**。分区清晰（编译在 `EngineCompile.cpp`，图像表在 `FrameResources`）。唯一问题是"区"这条边的方向（SK1） |
| `RenderSurface` | 尺寸唯一 owner、展示声明、窗口目标借用、像素比（4） | wgpu `Surface`；O3DE `WindowContext`；Filament `SwapChain` | **保留名字**：它确实是"可呈现表面"+ 展示声明的主人。但它不拥有自己的引擎却调用它——本轮修掉（SK0） |
| `SceneView` | 相机、场景、操纵器、引擎、管线、尺寸、输入路由、布局参与（8） | O3DE `RPI::View`（只有相机+pipeline）；three.js 无此对象 | **保留为门面**。8 件事里 6 件已经是协作对象（`InputRouter`/`LayoutParticipant`/`CameraManipulator`），它只转发；再拆只会多一层 |
| `RenderPipeline` | 持 8 个角色（window pass / offscreen / composite / gizmo(+pass) / fps(+pass)）+ resize + 模式切换（5） | O3DE Atom `Pipeline`；Unity SRP `RenderPipeline` | **保留**。形状对（构建结果 + 运行时开关）。但 `setShadingMode/setGizmoVisible/setStatsVisible` 返回 `bool` 的 setter 违反仓库口径（setter 返回 void，除链式 builder）——登记 |
| `RenderPass` + `PassDesc` | 同一份声明存两遍，~50 个镜像访问器 | O3DE `RPI::Pass`+`PassData`；wgpu `RenderPassDescriptor`（值）| **结构性缺陷**，见 SK2 |
| `RenderTarget` | 附件、尺寸、深度借用、生产者投影、`shadowOf`、名字（6） | Filament `RenderTarget`；O3DE `RPI::RenderTarget` | **保留**（S10a 已裁决不改名）。6 件事都通向"一张可被画/被读的图"，内聚 |
| `PresentTarget` / `WindowPresentTarget` | 呈递契约 + 窗口面 | Vulkan `VkSwapchainKHR`；Filament `SwapChain` | 名字里 "target" 与 `RenderTarget` 撞车；**建议改 `SwapChain` / `WindowSwapChain`**，独立切片（§6） |
| `Scene` | 根、灯、可见性/不透明度、名字、包围盒、收集+memo（6） | three.js `Scene`（灯是子节点）；Filament `Scene`（实体注册表） | **保留**。灯不在节点树是**能力缺口**（已登记，触发条件=需要灯随节点走） |
| `Node`/`Group`/`MatrixTransform`/`StateNode`/`Geometry` | OSG/vsg 风（1 件事各） | OSG / vsg | **保留**（刻意偏离，`graphics-object-model-vs-3d-frameworks.md` §2） |
| `Camera` | 位置/朝向 + 投影 + **画幅比例**（3） | three.js（aspect 在相机）；Filament（在 viewport） | **保留**（已裁决）。代价已经被框架自己量出来：`collectPictureShapeMismatches` 就是在给"相机的形状"和"pass 的矩形"对账 |
| `Light` / `LightRig` | 场景级光源 + 配方 | three.js `Light`、Filament `LightManager::Builder` | **保留**。`LightRig::addTo` 是配方（它调 `Scene::addLight`），不是第二个门 |
| `FrameDescription` / `OwnedFrame` | 借用快照 / 拥有快照 | wgpu `RenderPassDescriptor`（借用）| **保留**（L1 own 形态已有） |

### 3.1 三条"概念重复"的具体位置

1. **"这一幅画画什么内容"声明了三次**：`SceneView::scene()`、`RenderPipelineBuilder` 收到的内容、
   每个 `PassDesc::content`。三者合法（视图看什么 / 配方给谁建 / 哪个 pass 画什么），但**没有任何
   一处校验它们一致**：一个宿主完全可能建出一条"pass 画 A、视图看 B"的管线，视图的
   `fitToScreen` 就会框住 B 而画面是 A。→ 建议加一条一次报的诊断（SK3，只诊断不改结构）。
2. **"这幅画落地在这里"有两个说法**：`RenderSurface::present(view/pass)`（声明）与
   `RenderEngine` 的 pass 表（事实）。设计上已经用"声明 resolve 到在帧里的 pass"把两者绑住
   （`hasPresentation()` 就是那条绑带），**这一点是对的**，不列待办。
3. **"尺寸"三个入口**：`RenderSurface::resize` / `SceneView::resize` / `RenderPipeline::resize`。
   前两个是同一个公告的两种落点（本轮已经统一成同一签名 + 同一默认），第三个是管线自己的重排
   （离屏目标 + HUD 盒），语义不同，保留。

## 4. 改造切片（按"收益 ÷ 风险"排序）

> 前缀 `SK` 是为了不和 `graphics-followups-2026-10-19.md` §3.9 已有的 S1/S2/S3 混：“SK2”就是那里的 S1（同一件事）。

### SK0（**本轮已落地**）区对引擎的借用改成租约

- **问题**：`引擎 → 区` 与 `区 → 引擎` 都是裸借用，一对一，由宿主当第三方保证顺序；
  `~RenderSurface()` 会调用 `engine_->releaseArea(this)`。宿主若先销毁引擎，这是 use-after-free。
  同一个模块里，视图与管线早就是 `Lease`（F5, 2026-10-19）——**唯独区漏了**。
- **改法**：`RenderSurface::engine_` 从 `raw_ptr<RenderEngine>` 改为 `Lease<RenderEngine>`；
  每个用点取一次 `engine_.get()` 并显式处理"引擎已消失"（不展示、不报诊断——诊断路线随引擎走了）。
- **判据**：`RenderSurfaceTest.AnAreaOutlivingItsEngineAnswersNothingInsteadOfCallingIntoIt`
  （引擎先销毁，区仍可查询/重设尺寸/拒绝展示）；`test_graphics` 392 → **393** 全绿。

### SK1（建议下一个做）**区归引擎所有**

- **问题**：这是所有权矩阵里唯一"父不拥有子"的边。一扇窗一个引擎一个区是 1:1，但 owner 是宿主，
  于是"第二个区被拒"只能**在运行期报**，而"区必须先于引擎销毁"只是一句注释。SK0 让它不再 UB，
  但没让它成为关系。
- **目标形**：
  ```cpp
  // 父拥有子，子借父 —— 与 F5 裁决同一条规则
  intrusive_ptr<RenderSurface> RenderEngine::createSurface();   // 第二个返回 null
  // private: intrusive_ptr<RenderSurface> area_;
  ```
  `RenderSurface` 构造转私有（`friend RenderEngine`），宿主 `SurfaceWindow` 改一行。
- **收益**：1:1 由类型保证；"第二个区"从诊断变成构造失败；消灭隐式销毁顺序；与 Filament
  （`Engine` 造并持有交换链）/ O3DE（`WindowContext` 持交换链）同形。
- **成本**：~30 处构造点（多数是测试的 `new RenderSurface(*engine)`）；`~RenderEngine` 要显式
  排顺序（先 `shutdown()` 解绑目标，再析构区，最后放设备）；三条"第二个区"用例判据改。
- **风险**：中（生命周期顺序）。**不要与 SK2 同批做**。

### SK2（最大的一处，单独一轮）`RenderPass` 只存一份声明

- **问题**：`PassDesc`（值）与 `RenderPass`（节点）每个字段各一份，引擎 `addPass` 逐字段抄，
  公开面 ~50 个镜像 getter/setter。漂移风险全压在这张对照表上。
- **目标形**：节点持 `PassDesc desc_` + 一小块编译期事实（`target`/`transient`/解析后的 viewport）；
  公开面 = `desc()` / `setDesc()` / 少量运行期查询。
- **收益**：一个事实一份存储；`RenderPass.hpp` 642 行 → 约 250；"值形式与节点形式不一致"这一类
  缺陷从此不可能。
- **成本**：高（后端与测试的读取点全部改成 `desc().field`）。可分三步：先加 `desc()` 并存，
  再迁移读者，最后删镜像访问器——三步之间每次都要全绿。
- **判据**：`EngineCompile` 里那段逐字段抄的代码消失；`RenderPassTest` 全绿。

### SK3（小）一条"视图看的场景不在管线里"的诊断

`SceneView::setPipeline` 时，若该管线的任何 pass 都不画视图的场景，报一次（诊断类别已有
`PassProtocolViolation`）。**只加诊断，不改结构**——因为"管线画多个场景"是合法的。

## 5. 不重开（已裁决，本轮核过仍然成立）

| 项 | 裁决 | 复核结论 |
|---|---|---|
| `FramePlan` vs `FrameDescription` 两个帧值 | 2026-10-19 关闭：读者不同 | 仍然成立：一个是宿主可读的表（名字/标签/上次耗时），一个是后端消费的借用值 |
| `RenderTarget` 改名 | S10a：它是描述不是资源 | 成立 |
| `Pipeline` 命名（PSO） | K1：公开类已叫 `RenderPipeline` | 成立 |
| `Camera` 带画幅比例 | 刻意偏离 three.js 一侧 | 成立，代价由画幅对账诊断兜底 |
| 多格 / 分屏 | 一块区只画一幅画 | 成立 |
| `Pipeline` 便利入口、零灯自动头灯、`RenderControl::Config` | 否决清单 | 成立 |

## 6. 命名裁定

| 现名 | 业界 | 裁定 | 理由 |
|---|---|---|---|
| 事件/对象 `RenderSurface` | wgpu `Surface`、Filament `SwapChain`、O3DE `WindowContext` | **保留** | 它是"可呈现表面 + 展示声明的主人"，三者里只有 `Surface` 同时指这两件事 |
| `PresentTarget` / `WindowPresentTarget` | Vulkan `VkSwapchainKHR`、Filament `SwapChain` | **建议 `SwapChain` / `WindowSwapChain`** | 与 `RenderTarget` 的 "target" 撞车；业界口径一致 |
| `RenderEngine::device()` 返回 `RenderBackend` | — | **保留，补注释** | "device" 是角色（引擎借用什么来画），`RenderBackend` 是它落在插件上的类型；改名收益小于混淆成本 |
| `RenderPipeline` | O3DE Atom `Pipeline` | **保留** | 与 PSO 已分开（K1） |
| `RenderCommand::program/material` | — | **保留** | 与 `Geometry`/`StateNode` 的"最近声明优先"配套 |

## 7. 判据

| 门 | 结果 |
|---|---|
| 全量构建 | 0 error |
| `test_graphics` | **393 passed**（新增 1 条区/引擎寿命用例） |
| `test_vsg` | **478 passed** |
| 静态门禁 8 条 | 0 finding；公开面基线 `0 new / 0 stale` |

SK0 的"不再解引用已销毁引擎"是**结构上**成立的（租约为空 ⇒ 一个引擎调用都不发生）；
把它变成可复现的变异证据需要 ASan 树（`scripts/asan_check.sh`），本轮未跑，记在下一轮的开场。
