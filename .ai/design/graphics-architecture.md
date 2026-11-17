# graphics 架构：设备、引擎、链、一帧

> **状态**：现行
> **最后核对**：2026-11-16
> **范围**：设备/窗口/区/视图/引擎的分层，一帧的编译与调度。
> **权威来源**：`RenderEngine.hpp` / `PassChain.hpp` / `PassRegistry.hpp` / `OutputRegistry.hpp` /
> `DiagnosticsRoute.hpp` / `SceneView.hpp` / `RenderArea.hpp` / `RenderBackend.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

本文件定义 graphics 架构：设备、引擎、链、一帧 的现行设计：先给对象与边界，再给功能、接口、约束与验收。

### 1.2 目标

- 把 §2 的设计要点落成 §3 的功能点、§4 的接口与 §8 的验收。
- 让“现在是什么”与“为什么这么定”分开：理由在 §2/§7，历史看 git。

### 1.3 非目标

- 不在本文件重复头文件里的完整签名；接口只给契约与指针。
- 不在本文件保存已关闭的历史或逐轮审查记录。
- 新能力先写设计、再进 backlog、最后加签名（`G8`）；迁移期的“只增不改”窗口已随 5b-2c 关闭。

### 1.4 一句话结论

> 设备不是窗口；引擎负责生命周期与一帧；`PassChain` 是一张画面的配方和注册归属； 调度只在引擎一处，后端只物化。

## 2. 设计要点

2.1 **机制进 SDK，组合归应用层**：分屏布局、多 transient 互借深度、便利对象的边界都不做成引擎政策。
2.2 **声明与执行分离**：pass 是声明，帧编译成值（`FrameDescription` / `FrameTable`），后端只消费值。
2.3 **一个事实一个家**：pass 登记只有 `PassRegistry`；命名产出只有 `OutputRegistry`；诊断只有
    `DiagnosticsRoute`；呈递只有 `RenderArea` 的呈现表。
2.4 **设备不是窗口**：`RenderDevice` 拥有 GPU 设备与队列，可服务多个窗口；窗口目标由设备发放。
2.5 **调度只有一个实现**：引擎的 `detail::FrameScheduler` 排序；后端不再按依赖重排、不再报环。

2.6 **内容属于 pass 的声明**：`PassDesc::content` 是唯一存储；引擎的 slot 只存 order / owner / role / transient / 计时。
为什么：一个 pass 就是一条声明（`RenderPass.hpp` 的 “ONE FACT, ONE STORAGE”），内容由声明者写、编译者读；引擎另存一份
会立刻变成两份可写的值。边界：同一个 pass 实例被两个引擎注册时共享内容——这是“一条声明”的直接后果，要两幅画就用两个 pass。
`applyPlan(plan, owner)` 只是写声明；清空用 `bindPassContent(pass, nullptr)`（或计划里不再命名那一行）。
满足 `G1` / `G4`；为 `G1` 放弃“同实例多内容”。

2.7 **可失败的变更从返回值看得出**：`applyPlan` 返回 `PlanResult` 三态（`Applied` / `Invalid` + 问题表 /
`Refused` + `PlanRefusal::Busy`），`bindPassContent` / `PassRegistry::bindContent` 返回是否命中已注册的 pass，
`frame()` 的 `FrameOutcome` 只答 Ran / Submitted（`NotReady` + `EngineNotReady` 是前置条件，不是帧结局），
`prepare()` 复用同一个 `EngineNotReady`。
为什么：静默 no-op 让宿主把“没生效”当成“生效”；返回值是调用点唯一能判断的答案。边界：`removePass` 保持幂等、不返回——
移除未注册的 pass 与移除已移除的 pass 是同一个结果。满足 `G4` / `G6`；代价是每个调用点多一个可忽略的返回值。

2.8 **`RenderEngine` 是组合根，不是功能的家**：它拥有并组合三样机制 —— 帧泵（`detail::FramePump`：帧号、一帧在飞、
    `waitForFrame`、`presentRecord`）、会话形状（`detail::SessionShape`：pass 声明、`plan` / `applyPlan`、帧编译与接线检查）、
    表面账（`detail::SurfaceLedger`：唯一的 area、公告尺寸、窗口目标）——而它自己只剩组合与生命周期（`initialize` /
    `shutdown` / `device` / `createArea` 与报告的转发）。为什么：这些机制各有自己的状态与失败方式；放在组合根里时，
    改线程策略要动 9 个文件，加一条接线检查要往 1500 行的头里加一段（`G5` / `G12`）。边界：新的**机制**属于 `detail::`，
    不属于 `RenderEngine`；`FramePump` 只答“另一半在哪跑、什么时候答完”，`SessionShape` 只答“这一帧跑什么、顺序如何、
    哪里接错”，`SurfaceLedger` 只答“这一帧落在哪个目标、尺寸是多少”——每一件都有今天的消费者（两个线程模式、两个后端的
    编译对拍、一个引擎一个表面），不是预建的抽象（`G8`）。
    附带一条名词纪律：`PlanRefusal::Busy` 的文档含义是“**引擎里有帧在录**”；SDK 里今天**没有** `Session` 类型，
    也不需要：**一个引擎驱动一个表面**（[ADR-2026-11-13](ADR-2026-11-13-one-engine-one-surface.md)），`Session` 只作为设计
    名词指一个引擎的执行侧。文档与新接口都不再引用一个不存在的类型。

2.10 **迁移 API 已收口**：5b-2c（W9，2026-11-13）删除了 `addPass` 家族，`Plan` 是 pass 声明唯一的形状；
    `applyPlan` / `plan` / `bindSnapshot` / `prepare` 从迁移入口变成宿主契约。`FrameTable` 是编译后的帧表（不是
    计划），`frame()` 只答 Ran / Submitted，`applyPlan` 三态，`SessionBusy` 不再是一个 plan problem code。
    理由：迁移期分两次改会让三套形状并存；现在一套形状，公开签名与文档一致（`G1` / `G6`）。

2.9 **共享机制与宿主 SDK 同住一个目录、一个命名空间**：`sdk/vine/graphics/` 里除宿主类型外，还有 8 个“两个后端都用”
    的机制——`FrameGate`、`DeviceClock`、`FrameTimeline`、`RetirementQueue`、`Readback`、`DrawState`（原 `Keys`）、
    `TargetPlan`、`DiagnosticDraft`。它们曾单独成目录、由一道包含守卫把“宿主包含”变成编译错误、并单独安装成
    一个独立组件；这套“后端半边”已整体删除（
    [ADR-2026-11-16-no-backend-half](ADR-2026-11-16-no-backend-half.md)）。理由：目录、守卫、安装组件与门禁四处维护
    同一条边界，代价超过了它挡住的错误；而真正要保的性质是**依赖方向**，不是路径。
    边界：仍然由门禁钉住的两条是——图形模块（`sdk/vine/graphics` 与 `src/`）不得 include 渲染 API 或任何具体后端
    （`check_include_hygiene.py`、`check_vk_exposure.py` V2），共享机制的导出符号必须有 `VN_GRAPHICS_API`
    （`check_export_annotations.py` 的文件清单）。**接缝的定义仍是“两个后端都用”**：只有 vsg 读的 17 个头与 16 个
    `.cpp` 已在 ADR-2026-11-16 §2 搬进 `gfx_backend_vsg/internal/`，vsg 自己的类型住在 `vn::backend_vsg::core`
    （`G13`）。代价如实说：宿主现在能 include 这些机制头，SDK 的命名空间里也看得见它们（`G7` 的残余）；换来的是
    一条不需要四处同步的规则。`DrawState` / `TargetPlan` 的设备格式码已是**不透明**的
    `DeviceFormat`（0 = 设备的 UNDEFINED），渲染通道结构由后端计算的 `RenderPassKey` 承载——自
    B8（2026-11-17）起 `VkFormat` 值与 subpass 都不在接缝上，第三个后端不必先学 Vulkan 的格式与
    subpass；`ImageLayout` 仍是 SDK 里的 Vulkan 语义，C6 的“不再新增拼写”照旧适用，收掉它属于
    同一个门禁的下一次推进。
2.11 **扩展点的成本是表，不是形容词**（审计 `A12`）：说"易于扩展"而不说"加一个要动哪几处"是无法证伪的。
    所以每个扩展点在这里有一行，列的是**今天的实际文件**——加东西之前先读这张表，改完回来更新它。
    判据：一个扩展点如果开始需要改**核心模型**（`FrameDescription` 的形状、引擎的编译、调度），那它就不是扩展
    而是重构，那一步要走 ADR（`G9` 的反面正是"每加一样东西都动核心"）。

| 扩展点 | 今天要动的地方（实际文件） | 处数 |
|---|---|---|
| 一个 **pass 种类**（如 compute） | `PassDesc.hpp` 的 `PassKind`；`Plan.cpp` 的校验；`RenderPass.cpp` 的 kind → 节点映射；一个新节点类（照 `ScreenPass.cpp`）；`EngineCompile.cpp` 的 describe；每个后端按 `DrawDescription::Kind` 录制 | 6–8 |
| 一个 **像素格式** | `RenderTarget.hpp` 的 `ColorFormat` / `DepthFormat`；vk `TargetObjects.cpp` 的 `colorFormatOf` / `depthFormatOf`；vk `ContentImages.cpp` 的 `toVkFormat`；vsg `VsgSceneRules.cpp` 的格式映射 | 4 |
| 一个 **pass 声明字段** | `PassDesc.hpp`；`Plan.cpp`（差分与校验）；`FrameDescription.hpp` 的 `PassDraw`；每个后端的录制器 | 4+ |
| 一个 **内容通道**（顶点属性） | `Geometry.hpp` 的 `GeometryDescription` 通道表；`ShaderAbi.hpp` 的块布局；vk `ContentDraw.cpp` 的通道 → location 映射与顶点格式；vsg 的绑定 | 4 |
| 一个 **后端** | 新插件（`Plugin.cpp` + `BackendFactory.cpp`）、`RenderBackend` 两半、自己的 `test_*`、`check_*_exposure` 门禁、`RenderBackendRegistry` 自注册 | 约 6 类文件 |

2.12 **`Content*` 只给内容模型与身份**（命名法，`G12`）：`Content*` 前缀属于**内容对象本身**——`ContentId` /
    `ContentRef` / `ContentRegistry` / `ContentDeclarations` / `ContentStore`（设备的"身份 + 行"表）。由内容
    编译或缓存出来的 GPU 对象按**它们是什么**命名，不带 `Content` 前缀：`ProgramHalves`（每张
    (program, revision, layout, variant) 的编译半片）、`BindingSets`（按声明建出的 descriptor 集合）、
    `PushRange`（按名字填一个声明 push 范围的成员）。
    为什么：一个前缀用在"描述符集""push 常量""编译半边"上也就不再说任何事（`G12`），而 `ContentPush` 读起来
    像一个内容对象、实际是 push 字节的填充规则。边界：其余 `Content*` 名字（`ContentStore` / `ContentCaches` /
    `ContentDraw` / `ContentImages` / `ContentPipeline` / `ContentAssembly` / `ContentPass` / `ContentFacts` /
    `ContentSources`）**保留**——它们至少说得清自己处理的是内容（逐项改名是 churn，`G8`）；新名字按这条规则取。
    拼写偏离：研究文档 `E-08` 建议的 `PushConstants` 没有采用——同一批 TU 里已有 `::vsg::PushConstants`
    （实际被录制的那种对象），两个同名会给每个使用点制造"这是谁的"问题（`G1`）；`PushRange` 说的是本单元拥有
    的事实（一个声明范围与它的具名成员）。

    同角色类型在两个后端的名字（对齐是规则，不对齐要能说出理由）：

| 角色 | vk | vsg |
|---|---|---|
| 门面（= `RenderBackend`） | `Device` | `VsgBackend` |
| 帧驱动者（一帧的机器与暂存） | `Device::FrameDriver`（`createSession` 借出） | `VsgDriver`（`createSession` 借出） |
| 帧首解析出的值计划 | `FrameHead` | `FrameHead` |
| 设备的目标表 / 目标对象 | `TargetTable` / `TargetObjects` | `TargetTable` / `TargetObjects` |
| 窗口目标 | `WindowTarget` | `WindowTarget` |
| 窗口 / 离屏会话 | 无独立类型（设备自己的帧机器） | `WindowSession` / `OffscreenSession` |
| 内容事实与行 | `ContentCaches`（含行与命令计划） | `ContentStore` + `ContentFacts` |
| 编译半片（每 (program, rev, layout, variant)） | 无独立类型（`ContentDraw` 的管线键） | `ProgramHalves` |
| 声明的 descriptor 集合 | `ContentCaches` 的 set 缓存 | `BindingSets` |
| 读回 | `ReadbackStaging` | `HostReadback` |
| 灯 | SDK `LightSet` | SDK `LightSet` |

    `Content*` 名字留在两边的差异（`ContentCaches` / `ContentStore` / `ContentFacts`）不是漂移：它们都属于
    “内容模型与身份”那一类（2.12 的第一句），而且一个后端把行与命令计划放在一个对象里、另一个分开，
    名字说得出这件事。

## 3. 功能点

### 3.1 设备、窗口、区、视图、引擎

| # | 功能 | 说明 |
|---|---|---|
| F1 | 设备 | `RenderDevice`（`RenderBackend`）：设备 + 队列；不拥有窗口。 |
| F2 | 窗口目标 | `WindowPresentTarget`：窗口句柄 + 交换链；一个窗口一个目标。 |
| F3 | 区 | `RenderArea`：尺寸 + 呈现表；由 `RenderEngine::createArea()` 创建；一区一幅画。 |
| F4 | 视图 | `SceneView`：相机 + 场景 + 操纵器 + 输入 + 采纳的 `PassChain`；回答“哪条 pass 画我”（输入的归属见 `appfw-render-view.md` §2.1 / §2.4）。 |
| F5 | 引擎 | `RenderEngine`：`initialize` / `shutdown` / `frame() → FrameOutcome`、设备借用、尺寸、能力、报告。 |

### 3.2 引擎的三个门面

| # | 功能 | 说明 |
|---|---|---|
| F6 | `PassRegistry` | 唯一的 pass 登记表；`add` / `remove` / `count` / `orderOf` / `contentOf` / `bindContent(...) → bool`。 |
| F7 | `OutputRegistry` | 命名产出与 transient：`publish` / `resolve` / `unpublish` / `transientTarget`。 |
| F8 | `DiagnosticsRoute` | 诊断：`setSink` / 计数 / `isClean`；引擎先死 ⇒ `isValid()` false。 |

### 3.3 链（配方 + 注册归属）

| # | 功能 | 说明 |
|---|---|---|
| F9 | 建链 | `PassChain::build(engine, ChainDesc)`：建 pass、注册、返回句柄。 |
| F10 | 注销 | 链析构把注册的 pass 交还引擎；引擎先死 ⇒ 句柄失效，空转。 |
| F11 | 画面事实 | `draws(scene)` / `presentingPass()` / `offscreenTarget()` / `compositeTarget()`。 |
| F12 | 运行时开关 | `setShadingMode` / `setGizmoVisible` / `setStatsVisible` / `resize`。 |
| F13 | overlay | role + 内容 + pass；`overlayPass(role)` / `overlayContent(role)`；第三种 overlay 不改类。 |

### 3.4 一帧

| # | 步骤 | 说明 |
|---|---|---|
| F14 | 编译 | `compileFrame()`：解析输入 → 建边 → 稳定拓扑排序 → 分配 transient → `FrameDescription`。 |
| F15 | 记录 | `beginFrame` → 每 pass（`beginPass` / `setPassOrder` / `setPassInputs` / `execute` / `endPass`）→ `endFrame`。 |
| F16 | 物化 | 后端按 `FrameDescription.passes` 建目标/subpass/barrier/clear、记录命令、报告无法服务项。 |
| F17 | 呈递 | `RenderArea` 的呈现表解析到具体 pass；`PresentTarget` / `WindowPresentTarget` 两层。 |

### 3.5 呈现与多窗口

| # | 功能 | 说明 |
|---|---|---|
| F18 | 声明式呈递 | `RenderArea::present(view)` / `present(pass)`；谁画这块面由视图/表面回答一次。 |
| F19 | 多窗口 | **一个设备可服务多个引擎**（每窗口一个引擎 / 一个区 / 一个 `Viewer`）；窗口销毁不带走其他窗口。一个引擎驱动多个窗口已否决（一帧一表面，见 §7 D2 与 ADR-2026-11-13）。 |
| F20 | 尺寸 | 表面尺寸唯一 owner 是 `RenderArea`；视图通过 `resize` / `addSurfaceLayout` 跟随。 |

## 4. 公开接口

### 4.1 `RenderEngine`

| 接口 | 作用 |
|---|---|
| `initialize()` / `shutdown()` | 生命周期 |
| `frame()` | 编译并执行一帧；返回 `FrameOutcome`（Ran / Submitted；`NotReady` + `EngineNotReady`） |
| `passes()` / `outputs()` / `diagnostics()` | 三个门面 |
| `device()` / `setDevice()` | 借用/安装设备 |
| `createArea()` | 创建唯一的区 |
| `applyPlan()` / `applyPlan(plan, owner)` / `plan()` / `planFor(owner)` | 宿主与贡献者的形状（`PlanResult` 三态；`addPass` 家族已删） |
| `publish()` / `resolve()` / `unpublish()` | 命名产出（等价于 `outputs()`） |
| `transientTarget()` | transient 目标 |
| `releaseContent()` | 释放内容的 GPU 驻留 |
| `frameTable()` / `frameDescription()` | 本帧的编译产物 |
| `areaWidth()` / `areaHeight()` / `capabilities()` / `frameReport()` | 查询 |

### 4.2 注册表与诊断

| 接口 | 作用 |
|---|---|
| `PassRegistry::add(PassDesc)` / `remove()` / `count()` | pass 登记 |
| `PassRegistry::orderOf()` / `contentOf()` / `bindContent()` | pass 的 order / 内容；`bindContent` 返回是否命中已注册的 pass |
| `OutputRegistry::publish()` / `resolve()` / `unpublish()` | 命名产出 |
| `OutputRegistry::transientTarget()` / `transientCount()` | transient |
| `DiagnosticsRoute::setSink()` / `count()` / `isClean()` | 诊断 |

### 4.3 `PassChain`

| 接口 | 作用 |
|---|---|
| `static build(engine, ChainDesc)` | 建链并注册 |
| `isValid()` | 引擎是否还活着 |
| `draws(scene)` | 这条链是否画该场景 |
| `presentingPass()` | 呈现到窗口的 pass |
| `offscreenTarget()` / `compositeTarget()` | 离屏目标 |
| `resize(w, h, ratio)` | 目标与 overlay 布局 |
| `setShadingMode()` / `shadingMode()` / `shadingPath()` | 着色模式 |
| `setGizmoVisible()` / `setStatsVisible()` | overlay 开关 |
| `overlayPass(role)` / `overlayContent(role)` / `setOverlayVisible(role, on)` | overlay 查询/开关 |

### 4.4 `SceneView` / `RenderArea`

| 接口 | 作用 |
|---|---|
| `SceneView::setEngine()` / `setScene()` / `setCamera()` | 绑定 |
| `SceneView::setChain()` / `chain()` | 采纳配方 |
| `SceneView::presentingPass()` | 转发表面的问题 |
| `SceneView::resize()` / `addSurfaceLayout()` | 跟随尺寸 |
| `RenderArea::present(view)` / `present(pass)` | 声明呈递 |
| `RenderArea::resize(w, h, ratio)` | 表面尺寸唯一定义 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 设备不是窗口：`RenderDevice` 拥有设备/队列，窗口目标由设备发放。 |
| C2 | pass 登记、命名产出、诊断、呈递各有唯一 owner，不存第二份清单。 |
| C3 | 调度只在引擎；后端只物化 `FrameDescription.passes`。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 后端不得重排 pass、不得自建依赖图、不得报环。 |
| R2 | `PassChain` 只借引擎，不延长引擎寿命；引擎先死 ⇒ `isValid()` false。 |
| R3 | 一区一幅画；多格布局归应用层，不往引擎加第二个区。 |

### 5.3 不变量（N#）

| # | 注意项 |
|---|---|
| N1 | `PassChain` 只借引擎（`PassRegistry` / `DiagnosticsRoute` 是 lease），不拥有它；引擎先死 ⇒ `isValid()` false。 |
| N2 | 链的 pass 注册归引擎；链不存第二份 pass 清单。 |
| N3 | `PassDesc::camera` 是拥有式 `intrusive_ptr<Camera>`；不要传栈相机地址。 |
| N4 | 一区一幅画；多格布局归应用层，不要往引擎加第二个区。 |
| N5 | 相机自带 aspect；pass 矩形与相机形状不一致会在编译期报出来。 |
| N6 | 后端只物化 `FrameDescription.passes`；不要在记录路径重排或自建依赖图。 |
| N7 | `frame()` 的 `FrameOutcome` 只说跑没跑（Ran / Submitted）或引擎没起来（NotReady + reason）；跑了但没画东西读 `frameReport()`，不要从返回值推断绘制结果。 |

## 6. 注意事项与陷阱

### 6.1 一事实一家

- 会怎么错：同一份状态被两处保存，改一处另一处过期。
- 怎么判：出现两份可写的值/两个可写的注册表。
- 怎么防：回到 §5 的 N#；只留一个 owner，其余只读。

### 6.2 后端/平台的边界

- 会怎么错：把平台差异写进引擎/应用层，换平台时到处分叉。
- 怎么判：出现“如果 Windows/X11 就……”的分支。
- 怎么防：平台事实留在后端，引擎只消费编译好的帧值。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 引擎内置多格分屏布局 | 组合归应用层；SDK 只保证矩形、输入、尺寸扇出三条机制。 |
| D2 | 一帧跨多窗口 / 一个引擎驱动多个区 | 只有“一场画面铺满两块屏”才需要；当前一窗口一幅画。多窗口 = 一个设备服务多个引擎（见 [ADR-2026-11-13](ADR-2026-11-13-one-engine-one-surface.md)，它同时否决了 graphics-next 的 F8）。 |
| D3 | `PassPipeline` / `ScenePicture` 命名 | 重开 K1 的 PSO 歧义 / 读成“场景图片”；现行名 `PassChain`。 |
| D4 | 引擎同时做 pass 表、产出表、诊断与 UI 门面 | 一个类型多职；拆成三个窄门面。 |
| D5 | 后端自己再排一次 pass | 两套排序、两套帧词汇；调度上移引擎。详见 [ADR](../decisions/ADR-2026-10-22-engine-schedules-passes.md)。 |
| D6 | 给 `FrameDescription` 加 `FrameBorrowed<T>` 标记类型 | 借用的答案已经是“要留下就拥有”（`OwnedFrame`）；借用窗口只有一个 `executeFrame` 调用宽，标记类型会穿透 `FrameDescription` / `PassDraw` / `ImageSource`，且能被 `auto` 绕过（`G8` / `G10`）。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `test_graphics` / `test_gui` / `test_core` 全绿。 |
| T2 | `check_ai_docs.py --strict` / `check_doc_symbols.py` / `check_public_docs.py` 0 finding。 |
| T3 | `scripts/vsg_rewrite_gate.sh`：0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

旧名/旧路径只在 §7 已否决选项或 git 历史里出现；新代码按 §4 的接口写。

## 10. 参考

`graphics-scene.md` · `graphics-geometry.md` · `graphics-rendering.md` ·
`graphics-content-lifecycle.md` · `vsg-backend.md` · `../backlog/graphics.md`
