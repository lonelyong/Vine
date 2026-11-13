# graphics 架构：设备、引擎、链、一帧

> **状态**：现行
> **最后核对**：2026-11-12
> **范围**：设备/窗口/区/视图/引擎的分层，一帧的编译与调度。
> **权威来源**：`RenderEngine.hpp` / `PassChain.hpp` / `PassRegistry.hpp` / `OutputRegistry.hpp` /
> `DiagnosticsRoute.hpp` / `SceneView.hpp` / `RenderSurface.hpp` / `RenderBackend.hpp`。
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
- 5b-2c 收口前**不再新增公共入口**：`applyPlan` / `plan` / `bindSnapshot` / `prepare` 是迁移 API，尚未成为宿主
  契约（`W2`，`graphics-frame-pipeline.md` §4）；新能力先写设计，不先加签名。

### 1.4 一句话结论

> 设备不是窗口；引擎负责生命周期与一帧；`PassChain` 是一张画面的配方和注册归属； 调度只在引擎一处，后端只物化。

## 2. 设计要点

2.1 **机制进 SDK，组合归应用层**：分屏布局、多 transient 互借深度、便利对象的边界都不做成引擎政策。
2.2 **声明与执行分离**：pass 是声明，帧编译成值（`FrameDescription` / `FramePlan`），后端只消费值。
2.3 **一个事实一个家**：pass 登记只有 `PassRegistry`；命名产出只有 `OutputRegistry`；诊断只有
    `DiagnosticsRoute`；呈递只有 `RenderSurface` 的呈现表。
2.4 **设备不是窗口**：`RenderDevice` 拥有 GPU 设备与队列，可服务多个窗口；窗口目标由设备发放。
2.5 **调度只有一个实现**：引擎的 `detail::FrameScheduler` 排序；后端不再按依赖重排、不再报环。

2.6 **内容属于 pass 的声明**：`PassDesc::content` 是唯一存储；引擎的 slot 只存 order / owner / role / transient / 计时。
为什么：一个 pass 就是一条声明（`RenderPass.hpp` 的 “ONE FACT, ONE STORAGE”），内容由声明者写、编译者读；引擎另存一份
会立刻变成两份可写的值。边界：同一个 pass 实例被两个引擎注册时共享内容——这是“一条声明”的直接后果，要两幅画就用两个 pass。
`applyPlan(plan, owner)` 只是写声明；清空用 `bindPassContent(pass, nullptr)`（或计划里不再命名那一行）。
满足 `G1` / `G4`；为 `G1` 放弃“同实例多内容”。

2.7 **可失败的变更从返回值看得出**：`applyPlan` 返回问题表（空即应用），`bindPassContent` / `PassRegistry::bindContent`
返回是否命中已注册的 pass，`frame()` 返回 `FrameOutcome`（Ran / NotInitialized / NoDevice）。
为什么：静默 no-op 让宿主把“没生效”当成“生效”；返回值是调用点唯一能判断的答案。边界：`removePass` 保持幂等、不返回——
移除未注册的 pass 与移除已移除的 pass 是同一个结果。满足 `G4` / `G6`；代价是每个调用点多一个可忽略的返回值。

2.8 **`RenderEngine` 是组合根，不是功能的家**：它拥有三样东西 —— 帧泵（`frame` / `waitForFrame` /
    `setFrameThreading` / `frameNumber`）、会话形状（pass 声明、`plan` / `applyPlan` 与帧编译）、设备借用
    （`initialize` / `shutdown` / `device` / `createSurface`）与报告（`frameReport` / `presentRecord` /
    `presentedCamera`）。为什么：`RenderEngine.hpp` 1462 行、56 个公共签名，一个类里住着 8 类职责；线程策略的
    **机制**只有 `detail::FrameRunner`（123 行）与 5 个调用点，但**语义契约**外溢到 9 个文件——不写下来，下一次
    加功能会继续默认往里塞（`G5` / `G12`）。边界：新的**机制**属于 `detail::`，不属于 `RenderEngine`；它只组合
    现有机制。`addPass` 家族已删（W9d），`slots_` / `applyPlanRows` 若要继续下沉，目标仍是 `detail::SessionShape`——那次拆分
    有真实消费者，不是今天预建的抽象（`G8`）。
    附带一条名词纪律：`PlanProblemCode::SessionBusy` 的字符串保留（测试钉住），但它的文档含义是“**引擎里有帧在录**”；
    SDK 里今天**没有** `Session` 类型，该类型随 F8（一 Session 多 Surface，`graphics-frame-pipeline.md` §3.8）落地——
    在那之前，文档与新接口都不再引用一个不存在的 `Session`。

2.10 **迁移 API 与它的收口窗口**：`applyPlan` / `plan` / `bindSnapshot` / `prepare` 今天在 `src/` 里没有宿主调用点
    （只有引擎内部的转发与自测），所以它们是**迁移 API，不是宿主契约**：宿主不应按“已稳定”使用它们，仓库也不在
    5b-2c 前再加同类入口（`G8`）。收口时一次改完三件事：删 `addPass` 家族、把 `FramePlan` 改名为 `FrameTable`
    （它读的是“编译后的帧表”，不是计划）、把 `SessionBusy` 移出 `PlanProblemCode`（它描述运行时前置条件，不是形状
    缺陷；`W7`）。理由：三条入口 0 生产消费者而 `addPass` 家族 218 个调用点——不标状态，宿主会按“已支持”使用；
    分两次改则迁移期三套形状并存。

2.9 **`backend/` 是后端接缝，不是宿主 SDK**：`sdk/vine/graphics/backend/`（24 个头）只被 `gfx_backend_*` 与
    `libGraphics` 自己消费，宿主包含它是编译错误（`BackendHalfGuard.hpp`）。它的 **ABI 随后端变、不随宿主变**，
    所以安装时单独成一个组件（`VineGraphicsBackendSdk`），不在默认 include 树里——“装进 include 但禁止包含”
    是自相矛盾的（`G1` / `G6`）。边界：作为“驻留与帧纪律”，它把复杂性吸收在后端（`G7` 做对了）；`Keys` /
    `ClearPlan` / `TargetPlan` / `FrameCompiler` 里仍是 Vulkan 拼写（`VkFormat` / `ImageLayout` / subpass）——
    第二个后端落地前不再新增这类拼写，把它改成不透明设备格式码是那一轮的入口（`vsg-backend.md` C6）。

## 3. 功能点

### 3.1 设备、窗口、区、视图、引擎

| # | 功能 | 说明 |
|---|---|---|
| F1 | 设备 | `RenderDevice`（`RenderBackend`）：设备 + 队列；不拥有窗口。 |
| F2 | 窗口目标 | `WindowPresentTarget`：窗口句柄 + 交换链；一个窗口一个目标。 |
| F3 | 区 | `RenderSurface`：尺寸 + 呈现表；由 `RenderEngine::createSurface()` 创建；一区一幅画。 |
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
| F17 | 呈递 | `RenderSurface` 的呈现表解析到具体 pass；`PresentTarget` / `WindowPresentTarget` 两层。 |

### 3.5 呈现与多窗口

| # | 功能 | 说明 |
|---|---|---|
| F18 | 声明式呈递 | `RenderSurface::present(view)` / `present(pass)`；谁画这块面由视图/表面回答一次。 |
| F19 | 多窗口 | 一个引擎可服务多个窗口；每个窗口一个目标；窗口销毁不带走其他窗口。 |
| F20 | 尺寸 | 表面尺寸唯一 owner 是 `RenderSurface`；视图通过 `resize` / `addSurfaceLayout` 跟随。 |

## 4. 公开接口

### 4.1 `RenderEngine`

| 接口 | 作用 |
|---|---|
| `initialize()` / `shutdown()` | 生命周期 |
| `frame()` | 编译并执行一帧；返回 `FrameOutcome`（Ran / NotInitialized / NoDevice） |
| `passes()` / `outputs()` / `diagnostics()` | 三个门面 |
| `device()` / `setDevice()` | 借用/安装设备 |
| `createSurface()` | 创建唯一的区 |
| `applyPlan()` / `applyPlan(plan, owner)` / `plan()` / `planFor(owner)` | 宿主与贡献者的形状（W9d：`addPass` 家族已删） |
| `publish()` / `resolve()` / `unpublish()` | 命名产出（等价于 `outputs()`） |
| `transientTarget()` | transient 目标 |
| `releaseContent()` | 释放内容的 GPU 驻留 |
| `framePlan()` / `frameDescription()` | 本帧的编译产物 |
| `surfaceWidth()` / `surfaceHeight()` / `capabilities()` / `frameReport()` | 查询 |

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

### 4.4 `SceneView` / `RenderSurface`

| 接口 | 作用 |
|---|---|
| `SceneView::setEngine()` / `setScene()` / `setCamera()` | 绑定 |
| `SceneView::setChain()` / `chain()` | 采纳配方 |
| `SceneView::presentingPass()` | 转发表面的问题 |
| `SceneView::resize()` / `addSurfaceLayout()` | 跟随尺寸 |
| `RenderSurface::present(view)` / `present(pass)` | 声明呈递 |
| `RenderSurface::resize(w, h, ratio)` | 表面尺寸唯一定义 |

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
| N7 | `frame()` 的 `FrameOutcome` 只说跑没跑；跑了但没画东西读 `frameReport()`，不要从返回值推断绘制结果。 |

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
| D2 | 一帧跨多窗口 | 只有“一场画面铺满两块屏”才需要；当前一窗口一幅画。 |
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
