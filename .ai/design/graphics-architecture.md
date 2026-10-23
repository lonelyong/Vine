# graphics 架构：设备、引擎、链、一帧

> 单一来源：分层、所有权、一帧的机制。API 细节看头文件（`RenderEngine.hpp` / `PassChain.hpp` /
> `PassRegistry.hpp` / `SceneView.hpp` / `RenderSurface.hpp` / `RenderBackend.hpp`），
> 不在这里复制。待办见 `graphics-followups.md`。

## 1. 分层

```
graphics SDK            src/viz/graphics/sdk/vine/graphics
  backend/              可移植后端契约（FrameDescription / TargetPlan / Keys / Streams …）
  core/                 graphics 自己的策略（引擎编译、调度、框架）
  appfw / model_viewer  宿主
vsg 后端                src/plugins/gfx_backend_vsg
  core/                 纯策略、无 vsg 类型（协议、帧环、退役、键、观测、证据夹具）
  api/                  vsg 适配（设备、会话、目标、管线、录制）
  support/              宿主窗口、规则、工具
```

依赖方向：`core` 不依赖 vsg；`api` 依赖 `core`；SDK 不依赖插件。规则与机器检查见仓库脚本
`check_include_hygiene.py`。

## 2. 设备 / 窗口 / 区 / 视图 / 引擎

- **`RenderDevice`**（`RenderBackend`）：拥有的 GPU 设备与队列；可服务多个窗口；**不是窗口**
  （`device-is-not-a-window` 的结论）。
- **`RenderEngine`**：生命周期（`initialize` / `shutdown` / `frame`）、借用设备、尺寸、能力与报告。
  它**不是** pass 表本身：三个门面 `passes()` / `outputs()` / `diagnostics()` 是同一实现的三个窄视图。
- **`RenderSurface`**：一块可呈现表面（尺寸 + 呈现表）；由引擎创建，引擎持有一个区；`present(view)` /
  `present(pass)` 是声明，帧编译时解析。
- **`SceneView`**：相机 + 场景 + 操纵器 + 采纳的 `PassChain`；它回答“哪条 pass 画我”（`presentingPass()`）。
- **`PassChain`**：一张画面的配方 + 注册归属；`PassChain::build(engine, ChainDesc)` 建链并注册 pass，
  析构把自己的 pass 交还引擎。链只借引擎（`PassRegistry` / `DiagnosticsRoute` 是 lease），不拥有它。

## 3. 引擎的门面

| 门面 | 职责 | 关键操作 |
|---|---|---|
| `PassRegistry` | 唯一的 pass 登记表 | `add(PassDesc, role)` / `remove` / `size` / `orderOf` / `contentOf` / `bindContent` / `passes()` |
| `OutputRegistry` | 命名产出与 transient | `publish` / `resolve` / `unpublish` / `transientTarget` |
| `DiagnosticsRoute` | 诊断路线 | `setSink` / 计数 / `clean` / report |

`PassChain` 不存第二份 pass 清单：role / owner / group 都在引擎 `Slot` 上。

## 4. 一帧

```
RenderEngine::frame()
  compileFrame()      ← 读登记表，解析输入，建边，调度，生成 FrameDescription / FramePlan
  beginFrame()
  per pass: beginPass → setPassOrder → setPassInputs → execute → endPass
  endFrame()
  present             ← RenderSurface 的呈现表解析到具体 pass
```

- **调度只在引擎**：`detail::FrameScheduler`（迭代 Tarjan + 堆就绪队列，稳态零分配）。
  `EngineCompile.cpp::scheduleRows()` 建行/建边（`PassDesc::inputs` / `output` / `depth_view` /
  借用深度）+ 稳定拓扑排序；`PassStage` 只做同优先级 tie-break；环组件整块跳过并由引擎报一次。
- **后端只物化**：按交来的 `FrameDescription.passes` 建目标/subpass/barrier/clear，记录命令，报告无法服务的项；
  不再重排、不报环。
- `FramePlan` / `FrameDescription` 是值；`FrameReport` / `DiagnosticsRoute` 是观测出口。

## 5. 画面与视图

- 一区一幅画：`RenderSurface` 只呈现一条 pass；多格布局归应用层。
- 相机自带 aspect 与投影；pass 的矩形与相机的画面形状不一致会在编译期报出来。一台相机一张画面，
  要复用就复制相机。
- HUD（`AxisGizmo` / `FpsOverlay`）是内容对象 + 一条 pass 声明，不是引擎级叠加层。
- 呈递端两层：`PresentTarget`（尺寸 + present，帧的契约）与 `WindowPresentTarget`（窗口句柄）。

## 6. 设计立场

- 机制进 SDK，组合归应用层（分屏布局、多 transient 互借深度、便利对象的边界）。
- 声明与执行分开：pass 是声明，帧是值，后端只消费值。
- 一个事实一个家：不新增第二套登记、第二套调度、第二套呈递知识。
- 拾取是机制，选择是业务（见 `graphics-scene.md` §7）。

## 7. 模块文件

- `graphics-scene.md` —— 场景图、状态、收集、层、拾取。
- `graphics-geometry.md` —— `Geometry` / `Primitive` / 实例化 / revision。
- `graphics-rendering.md` —— 目标、MSAA、图源、颜色、着色、阴影。
- `graphics-content-lifecycle.md` —— 内容释放与缓存。
- `vsg-backend.md` —— vsg 后端的契约、平台事实与坑。
