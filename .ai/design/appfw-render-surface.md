# appfw 渲染表面生命周期

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`RenderControl` / `SurfaceWindow` 的原生表面、attach、状态与平台回退；图形层的表面/目标/设备契约见
> `graphics-architecture.md`。
> **权威来源**：`RenderControl.hpp`、`src/gui/SurfaceWindow.{hpp,cpp}`、`tests/test_gui/RenderControlTest.cpp`。
> **待办**：`../backlog/appfw-startup.md`

## 1. 概要设计

### 1.1 问题与背景

一块 3D 区要同时回答“原生窗口什么时候有句柄”“什么时候能 attach”“什么时候能显示”“窗口重建后怎么办”。
过去把这些时机交给定时器/重试梯子，结果是空窗、黑洞和猜时机。

### 1.2 目标

- 构造只组装，attach 有明确入口；宿主能在自己的顺序里决定什么时候 attach。
- attach 不要求表面可见；首帧由 attach 自己渲出来，区域在首帧之前保持隐藏。
- 原生窗口重建后会话保留，宿主不需要自己做任何事。
- 失败有状态和原因，宿主可重试；控件不排定时器。

### 1.3 非目标

- 不在控件里做布局策略；尺寸由容器/resize 事件给。
- 不替宿主重试；没有 100 ms × N 的 attach 梯子。
- 不保证所有平台都能在未显示窗口上 attach（Wayland 回退为“先显示再 attach”）。

### 1.4 一句话结论

`RenderControl` 构造只组装；`init()` / `initAsync()` / 第一拍兜底做一次同步 attach；状态只有
`Pending → Presenting`（失败是 `Failed`），容器在首帧前隐藏，窗口重建由控件自己跟。

## 2. 设计要点

### 2.1 文件划分

| 文件 | 职责 |
| --- | --- |
| `sdk/vine/appfw/gui/RenderControl.hpp` / `src/gui/RenderControl.cpp` | 公开控件：转发 `init()/initAsync()`、`state()`、`failureReason()`、尺寸/比例、`viewer()`、`view()`、`engine()`、`renderFrame()`、`fitToScreen()`、`setContinuousRendering()` |
| `src/gui/SurfaceWindow.{hpp,cpp}` | 私有实现：原生 `QWindow`、`RenderEngine`、`SceneView`、`Viewer`、attach 规则、延迟 resize、Qt 事件 → 视图输入 |
| `SurfaceWindow` 的容器 | `QWidget::createWindowContainer` 包住原生 `QWindow`；容器在首帧前隐藏 |

### 2.2 构造只组装

`new RenderControl()` 返回时：建好引擎、视图、查看器、诊断路由；**不选后端、不 attach**；
`state()==Pending`、`engine()->device()==nullptr`、没有 pass/链。这样 `engine()->setDevice()`、
`viewer()->setViewerOptions()`（或宿主自己的 `view()->setChain()`）和内容装配都在第一次 attach 之前同步跑完，
**零重建**。

### 2.3 两个 attach 入口

- `init()`：同步、幂等；会话已绑在当前句柄上就直接 `true`，否则走完整 attach。
- `initAsync()`：`co_return init()`，**不挂起**，也不把任何一段丢到池上；只是给协程调用方的拼法。
  判据：`RenderControlTest.InitAsyncCompletesWithoutSuspending`。

没有调时，第一拍事件循环的 `QTimer::singleShot(0)` 兜底会 attach；兜底用宿主刚装上的后端，不是注册表默认值。

### 2.4 attach 的次序

1. **先保证这块表面有呈现**：视图没有链就按 `ViewerOptions` 建一条默认链并采纳，然后在 `RenderSurface` 上声明；
   宿主已用自己的 pass 声明过 ⇒ 这一步不动。
2. **补默认后端**：`useDefaultBackend()` 只在引擎没有后端时取注册表第一个；晚注册的后端仍会被下一次 attach 捡起。
3. **没有后端** ⇒ `failAttach("no render backend is registered")`，状态 `Failed`。
4. **句柄空 / 尺寸退化** ⇒ 记一行日志、保持 `Pending`，等 show/resize/下一次 `activate()`；控件不排定时器。
5. **attach**：`setWindowHandle` → 设备/会话/管线 `initialize()` → `resize(w,h)` + 把尺寸交给表面
   → `renderFrame()`（这一帧付掉 pass 图 / program slot / 编译，状态进 `Presenting`）。

attach 不要求表面可见；只有 present 才要求窗口在屏上。平台不同意时 attach 失败，控件回退到“先显示表面再 attach”。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 构造只组装，attach 有入口 | `init()` / `initAsync()` / 第一拍兜底 | 已落地 |
| F2 | 状态与可见性 | `Pending` / `Presenting` / `Failed`；首帧前容器隐藏 | 已落地 |
| F3 | 原生窗口重建 | 新句柄重新公告，会话保留 | 已落地 |
| F4 | 失败与重试 | 拒绝 ⇒ false + `Pending`；无后端 ⇒ `Failed`；宿主可重试 | 已落地 |
| F5 | 平台回退 | X11/WSLg 未显示可 attach；Wayland 先显示再 attach | 已落地（Windows 未验） |

### 3.1.1 F1 构造只组装，attach 有入口

- **行为**：构造建面 + 引擎 + 视图 + 容器；`init()` 同步 attach，`initAsync()` 是协程拼法；没有显式调用时第一拍兜底。
- **约束**（C1）：attach 必须有句柄和尺寸；句柄空/尺寸退化时保持 `Pending`，不猜延时。
- **要求**（R1）：配置写在 attach 之前 = 零重建；控件不排重试定时器。
- **验收**（T1）：`ConstructionOnlyComposesAndTheFirstTurnAttaches`。
- **注意**（N1）：`initAsync()` 不下池；设备/会话/管线/warm-up 都在应用线程。

### 3.1.2 F2 状态与可见性

- **行为**：`Pending`（没有可用表面或还没渲过）/ `Presenting`（帧在表面里）/ `Failed`（后端起不来）。
  容器在 `state_changed` 报到 `Presenting` 之前一直隐藏。
- **约束**（C2）：没有会话就没有帧；`failureReason()` 非空 ⟺ `Failed`。
- **要求**（R2）：`Presenting` 之前露出的是主窗背景，不是洞；`renderFrame()` / `fitToScreen()` 在 attach 前不得推状态。
- **验收**（T2）：`TheFrameTheAttachRendersPutsTheSurfaceOnScreen`、
  `AFrameAskedForBeforeAnySessionDoesNotPutTheSurfaceOnScreen`。
- **注意**（N2）：未呈现的原生窗口在 Qt 里是一个洞（window container 把自己的矩形挖透明），所以“藏着”是正确性。

### 3.1.3 F3 原生窗口重建

- **行为**：Qt 销毁并重建平台窗口（dock 拖拽、换屏、`setCentralWidget()` 重父化）时，新句柄被捡起并重新公告给后端；
  会话、设备、缓存与已编译管线都留，状态重走 `Pending → Presenting`。
- **约束**（C3）：重建只换表面，不换会话；宿主不需要自己重新 `init()`。
- **要求**（R3）：重建期间状态退回 `Pending`，区域隐藏；首帧回来后再显示。
- **验收**（T3）：`FollowsARecreatedSurfaceWithoutTheHost`。
- **注意**（N3）：重父化会重建原生窗口，所以中央区不要每页一个控件，容器内切页。

### 3.1.4 F4 失败与重试

- **行为**：后端拒绝 ⇒ `init()` 返回 false，状态停在 `Pending`；没有注册后端 ⇒ `Failed` + 原因；
  宿主再调 `init()` 就是重试。
- **约束**（C4）：控件没有任何后台重试；失败原因只在 `Failed` 时有效，attach 成功后清掉旧判决。
- **要求**（R4）：拒绝与失败都必须可观察（返回值/状态/原因），不能静默。
- **验收**（T4）：`RefusedAttachReportsFalseAndWaitsForTheHost`、`HostCanRetryAfterTheBackendRefused`、
  `ReportsFailedWhenNoRenderBackendIsRegistered`。
- **注意**（N4）：晚注册的后端会被下一次 attach 捡起，不要在构造时就判死。

### 3.1.5 F5 平台回退

- **行为**：X11/WSLg 上主窗未 show 时容器里的独立 `QWindow` 已有有效句柄，attach 成功；Wayland 未映射 surface
  拿不到 extent，`init()` 返回 false，控件显示表面后宿主再调一次。
- **约束**（C5）：attach 只需要句柄 + 尺寸；present 需要窗口在屏上。
- **要求**（R5）：平台不同意时回退为“先显示再 attach”，不能更差。
- **验收**（T5）：`scripts/xwinmap.py` 读启动期 map_state；Windows 判据见 backlog。
- **注意**（N5）：`map_state` 字段偏移是 92，不是 100；Windows 的 HWND + client rect 仍未实测。

## 4. 公开接口

### 4.1 `RenderControl`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `init()` | 同步、幂等 attach | 会话已绑当前句柄 ⇒ 直接 true |
| `initAsync()` | `co_return init()`，不挂起 | 协程调用方的拼法 |
| `state()` | `Pending` / `Presenting` / `Failed` | 见 F2 |
| `failureReason()` | 非空 ⟺ `Failed` | attach 成功后清掉 |
| `engine()` / `view()` / `viewer()` / `surface()` | 借用，控件拥有 | `view()` 在 `release` 后为 null |
| `renderFrame()` / `fitToScreen()` | 按需出帧 / 对准内容 | attach 前不推 `Presenting` |
| `setContinuousRendering(on, interval_ms)` | 连续出帧 | 默认按需 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | attach 必须有句柄和尺寸；否则保持 `Pending`。 |
| C2 | 没有会话就没有帧；`failureReason()` 非空 ⟺ `Failed`。 |
| C3 | 重建只换表面，不换会话；宿主不需要重新 `init()`。 |
| C4 | 控件不排重试定时器；失败可观察。 |
| C5 | attach 不要求可见；present 要求窗口在屏上。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | attach 之前配置 = 零重建。 |
| R2 | `Presenting` 之前区域保持隐藏，露出主窗背景。 |
| R3 | 重建期间状态退回 `Pending`，首帧后恢复 `Presenting`。 |
| R4 | 拒绝/失败必须可观察；晚注册后端可被下一次 attach 捡起。 |
| R5 | 平台不同意时回退为“先显示再 attach”。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 构造返回时 `state()==Pending`、`device()==nullptr`、没有链。 |
| N2 | `Presenting` ⟺ 帧已经在表面里。 |
| N3 | 原生窗口重建不销毁会话/设备/已编译管线。 |
| N4 | 控件不拥有窗口容器的可见性策略之外的东西；尺寸由 resize 事件给。 |

## 6. 注意事项与陷阱

### 6.1 未呈现的原生窗口是洞

- 会怎么错：容器可见但还没有帧，桌面透出来。
- 怎么判：`state()!=Presenting` 时容器必须隐藏。
- 怎么防：`state_changed` 到 `Presenting` 才 `setVisible(true)`。

### 6.2 不要在启动期替渲染组件 pump 事件

- 会怎么错：`processEvents()` 把别的组件的定时器/事件一起跑，嵌入式表面被提前唤醒。
- 怎么判：attach 在布局没稳时发生，拿不到有效尺寸/句柄。
- 怎么防：启动框只 `repaint()`；首帧门用 `Window::hasPainted()` + 300 ms 兜底。

### 6.3 重父化会重建原生窗口

- 会怎么错：每个文档一个中央控件，切页/拖拽就重建会话，画面闪。
- 怎么判：状态反复 `Pending → Presenting`，设备数上涨。
- 怎么防：中央区放一个容器、只装一次、内部 `QStackedWidget` 切页。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 延后创建/插入 `RenderControl` 直到“就绪” | 自锁：控件不在已显示窗口的布局里就没有可用表面。 | — |
| D2 | 先给尺寸再 attach、最后嵌入 | 退化尺寸（1x1）本来就够 attach；先嵌入再 `init()` 更简单。 | — |
| D3 | 延后 show `QWindow` 到引擎就绪之后 | 因果反了：引擎就绪需要窗口存在，present 又要求窗口已映射。 | — |
| D4 | 在 `QWindow` 背后画背景当占位 | 未呈现的原生窗口露出什么取决于平台/合成器。 | — |
| D5 | 100 ms × N 次 attach 重试梯子 / settle 帧 | 首次 attach 是宿主的 `init()`，之后由窗口事件驱动。 | — |
| D6 | 单独的 `Attached` 状态 | 同步 attach 里它永不驻留，没有读者区分它和 `Pending`。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 构造只组装；第一拍 attach 到宿主刚装的后端 | `ConstructionOnlyComposesAndTheFirstTurnAttaches` |
| T2 | attach 那一帧就进 `Presenting`；attach 前不推状态 | `TheFrameTheAttachRendersPutsTheSurfaceOnScreen`、`AFrameAskedForBeforeAnySessionDoesNotPutTheSurfaceOnScreen` |
| T3 | 原生窗口重建后会话保留 | `FollowsARecreatedSurfaceWithoutTheHost` |
| T4 | 拒绝 ⇒ false + `Pending`；无后端 ⇒ `Failed`；可重试 | `RefusedAttachReportsFalseAndWaitsForTheHost`、`HostCanRetryAfterTheBackendRefused`、`ReportsFailedWhenNoRenderBackendIsRegistered` |
| T5 | 嵌入后立刻 `init()`，不等布局 | `HostCanAttachRightAfterEmbeddingBeforeTheLayoutSettles` |

## 9. 状态与演进

### 9.1 当前状态

F1–F5 已落地（Windows 的未显示窗口 attach 仍未实测）。同步 attach，三态状态机，容器隐藏到首帧。

### 9.2 已知缺口 / 待办

- Windows：主窗未 show 时 `VsgHostWindow` 是否拿到有效 HWND；见 `../backlog/appfw-startup.md`。
- 会话 init 仍在应用线程（~184 ms）；搬到池上需要重新设计 attach 与内容装配的并发，暂不做。

### 9.3 兼容性 / 迁移

`SurfaceState::Attached` 已删除；旧代码只看 `Pending` / `Presenting` / `Failed`。`isAttaching()`、
`AttachScope`、settle 帧均已删除。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp`
- `src/fw/appfw/src/gui/SurfaceWindow.{hpp,cpp}`
- `tests/test_gui/RenderControlTest.cpp`

### 10.2 文档与 ADR

- `appfw-render-view.md`
- `appfw-startup-splash.md`
- `graphics-architecture.md`
- `../backlog/appfw-startup.md`

### 10.3 外部资料

- Qt `QWidget::createWindowContainer` 的原生窗口语义。
- X11 `XQueryTree` / `XGetWindowAttributes`（`scripts/xwinmap.py`）。
