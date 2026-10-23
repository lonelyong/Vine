# graphics 架构：设备、引擎、链、一帧

> **一句话**：设备不是窗口；引擎负责生命周期与一帧；`PassChain` 是一张画面的配方和注册归属；
> 调度只在引擎一处，后端只物化。
>
> **权威来源**：`RenderEngine.hpp` / `PassChain.hpp` / `PassRegistry.hpp` / `OutputRegistry.hpp` /
> `DiagnosticsRoute.hpp` / `SceneView.hpp` / `RenderSurface.hpp` / `RenderBackend.hpp`。
> **待办**：`graphics-followups.md`。

## 1. 设计要点

1.1 **机制进 SDK，组合归应用层**：分屏布局、多 transient 互借深度、便利对象的边界都不做成引擎政策。
1.2 **声明与执行分离**：pass 是声明，帧编译成值（`FrameDescription` / `FramePlan`），后端只消费值。
1.3 **一个事实一个家**：pass 登记只有 `PassRegistry`；命名产出只有 `OutputRegistry`；诊断只有
    `DiagnosticsRoute`；呈递只有 `RenderSurface` 的呈现表。
1.4 **设备不是窗口**：`RenderDevice` 拥有 GPU 设备与队列，可服务多个窗口；窗口目标由设备发放。
1.5 **调度只有一个实现**：引擎的 `detail::FrameScheduler` 排序；后端不再按依赖重排、不再报环。

## 2. 功能点

### 2.1 设备、窗口、区、视图、引擎

| # | 功能 | 说明 |
|---|---|---|
| F1 | 设备 | `RenderDevice`（`RenderBackend`）：设备 + 队列；不拥有窗口。 |
| F2 | 窗口目标 | `WindowPresentTarget`：窗口句柄 + 交换链；一个窗口一个目标。 |
| F3 | 区 | `RenderSurface`：尺寸 + 呈现表；由 `RenderEngine::createSurface()` 创建；一区一幅画。 |
| F4 | 视图 | `SceneView`：相机 + 场景 + 操纵器 + 采纳的 `PassChain`；回答“哪条 pass 画我”。 |
| F5 | 引擎 | `RenderEngine`：`initialize` / `shutdown` / `frame`、设备借用、尺寸、能力、报告。 |

### 2.2 引擎的三个门面

| # | 功能 | 说明 |
|---|---|---|
| F6 | `PassRegistry` | 唯一的 pass 登记表；`add` / `remove` / `count` / `orderOf` / `contentOf` / `bindContent`。 |
| F7 | `OutputRegistry` | 命名产出与 transient：`publish` / `resolve` / `unpublish` / `transientTarget`。 |
| F8 | `DiagnosticsRoute` | 诊断：`setSink` / 计数 / `isClean`；引擎先死 ⇒ `isValid()` false。 |

### 2.3 链（配方 + 注册归属）

| # | 功能 | 说明 |
|---|---|---|
| F9 | 建链 | `PassChain::build(engine, ChainDesc)`：建 pass、注册、返回句柄。 |
| F10 | 注销 | 链析构把注册的 pass 交还引擎；引擎先死 ⇒ 句柄失效，空转。 |
| F11 | 画面事实 | `draws(scene)` / `presentingPass()` / `offscreenTarget()` / `compositeTarget()`。 |
| F12 | 运行时开关 | `setShadingMode` / `setGizmoVisible` / `setStatsVisible` / `resize`。 |
| F13 | overlay | role + 内容 + pass；`overlayPass(role)` / `overlayContent(role)`；第三种 overlay 不改类。 |

### 2.4 一帧

| # | 步骤 | 说明 |
|---|---|---|
| F14 | 编译 | `compileFrame()`：解析输入 → 建边 → 稳定拓扑排序 → 分配 transient → `FrameDescription`。 |
| F15 | 记录 | `beginFrame` → 每 pass（`beginPass` / `setPassOrder` / `setPassInputs` / `execute` / `endPass`）→ `endFrame`。 |
| F16 | 物化 | 后端按 `FrameDescription.passes` 建目标/subpass/barrier/clear、记录命令、报告无法服务项。 |
| F17 | 呈递 | `RenderSurface` 的呈现表解析到具体 pass；`PresentTarget` / `WindowPresentTarget` 两层。 |

### 2.5 呈现与多窗口

| # | 功能 | 说明 |
|---|---|---|
| F18 | 声明式呈递 | `RenderSurface::present(view)` / `present(pass)`；谁画这块面由视图/表面回答一次。 |
| F19 | 多窗口 | 一个引擎可服务多个窗口；每个窗口一个目标；窗口销毁不带走其他窗口。 |
| F20 | 尺寸 | 表面尺寸唯一 owner 是 `RenderSurface`；视图通过 `resize` / `addSurfaceLayout` 跟随。 |

## 3. 公开接口

### 3.1 `RenderEngine`

| 接口 | 作用 |
|---|---|
| `initialize()` / `shutdown()` | 生命周期 |
| `frame()` | 编译并执行一帧 |
| `passes()` / `outputs()` / `diagnostics()` | 三个门面 |
| `device()` / `setDevice()` | 借用/安装设备 |
| `createSurface()` | 创建唯一的区 |
| `addPass()` / `removePass()` / `clearPasses()` | 直接登记（宿主自建 pass） |
| `publish()` / `resolve()` / `unpublish()` | 命名产出（等价于 `outputs()`） |
| `transientTarget()` | transient 目标 |
| `releaseContent()` | 释放内容的 GPU 驻留 |
| `framePlan()` / `frameDescription()` | 本帧的编译产物 |
| `surfaceWidth()` / `surfaceHeight()` / `capabilities()` / `frameReport()` | 查询 |

### 3.2 注册表与诊断

| 接口 | 作用 |
|---|---|
| `PassRegistry::add(PassDesc)` / `remove()` / `count()` | pass 登记 |
| `PassRegistry::orderOf()` / `contentOf()` / `bindContent()` | pass 的 order / 内容 |
| `OutputRegistry::publish()` / `resolve()` / `unpublish()` | 命名产出 |
| `OutputRegistry::transientTarget()` / `transientCount()` | transient |
| `DiagnosticsRoute::setSink()` / `count()` / `isClean()` | 诊断 |

### 3.3 `PassChain`

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

### 3.4 `SceneView` / `RenderSurface`

| 接口 | 作用 |
|---|---|
| `SceneView::setEngine()` / `setScene()` / `setCamera()` | 绑定 |
| `SceneView::setChain()` / `chain()` | 采纳配方 |
| `SceneView::presentingPass()` | 转发表面的问题 |
| `SceneView::resize()` / `addSurfaceLayout()` | 跟随尺寸 |
| `RenderSurface::present(view)` / `present(pass)` | 声明呈递 |
| `RenderSurface::resize(w, h, ratio)` | 表面尺寸唯一定义 |

## 4. 注意项

| # | 注意项 |
|---|---|
| N1 | `PassChain` 只借引擎（`PassRegistry` / `DiagnosticsRoute` 是 lease），不拥有它；引擎先死 ⇒ `isValid()` false。 |
| N2 | 链的 pass 注册归引擎；链不存第二份 pass 清单。 |
| N3 | `PassDesc::camera` 是拥有式 `intrusive_ptr<Camera>`；不要传栈相机地址。 |
| N4 | 一区一幅画；多格布局归应用层，不要往引擎加第二个区。 |
| N5 | 相机自带 aspect；pass 矩形与相机形状不一致会在编译期报出来。 |
| N6 | 后端只物化 `FrameDescription.passes`；不要在记录路径重排或自建依赖图。 |

## 5. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 引擎内置多格分屏布局 | 组合归应用层；SDK 只保证矩形、输入、尺寸扇出三条机制。 |
| D2 | 一帧跨多窗口 | 只有“一场画面铺满两块屏”才需要；当前一窗口一幅画。 |
| D3 | `PassPipeline` / `ScenePicture` 命名 | 重开 K1 的 PSO 歧义 / 读成“场景图片”；现行名 `PassChain`。 |
| D4 | 引擎同时做 pass 表、产出表、诊断与 UI 门面 | 一个类型多职；拆成三个窄门面。 |
| D5 | 后端自己再排一次 pass | 两套排序、两套帧词汇；调度上移引擎。 |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | `test_graphics` / `test_gui` / `test_core` 全绿。 |
| T2 | `check_ai_docs.py --strict` / `check_doc_symbols.py` / `check_public_docs.py` 0 finding。 |
| T3 | `scripts/vsg_rewrite_gate.sh`：0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。 |

## 7. 相关文档

`graphics-scene.md` · `graphics-geometry.md` · `graphics-rendering.md` ·
`graphics-content-lifecycle.md` · `vsg-backend.md` · `graphics-followups.md`
