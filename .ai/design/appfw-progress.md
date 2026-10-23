# appfw 环境进度（`ProgressHost`）设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`ProgressHost` 的归属、推送模型、两个消费者与契约。
> **权威来源**：`ProgressHost.hpp`、`tests/test_gui`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

“应用此刻在干什么”是应用状态，需要跨线程上报、多个呈现者订阅（状态栏、控制台、启动框），
并且运行期可能没有前台宿主。宿主归 appfw，因为它需要 Core 的 `Signal`；base 的 `vn::Progress`
只提供零依赖的进度原语。

### 1.2 目标

- `ProgressHost`：进程级注册表、前台栈、label、取消源共享、变更通知。
- 两个消费者：`gui::ProgressPresenter`（状态栏）与 `ConsoleProgressReporter`（无头）。
- 空闲零唤醒：没有待办截止时间时不臂定时器。

### 1.3 非目标

- 不做轮询模型；通知由状态变化驱动。
- 不把整个 progress 模块搬进 appfw（`ProgressIndicator`/Range/Scope 保持零依赖）。
- 不在构造函数里建前台宿主；不跑启动的进程不应被判成“忙”。

### 1.4 一句话结论

`ProgressHost` 是 appfw 的进程级前台栈 + `Signal<>` 变更通知；呈现者订阅并自己编组，
base 的 `ProgressIndicator`/Range/Scope 继续做零依赖的进度算术。

## 2. 设计要点

### 2.1 三层分工

- **base / `vn::Progress`**：`ProgressIndicator`（[0,1] 标尺、原子位置、外部取消 token）、
  `ProgressRange` / `ProgressScope`（把标尺切成区间）。零外部依赖。
- **appfw / `vn::Appfw`**：`ProgressHost`——进程级注册表、前台栈、label、取消源共享、变更通知。
- **呈现层**：`gui::ProgressPresenter`、`ConsoleProgressReporter`。

依赖方向：appfw → `vn::Progress`；`ProgressHost.hpp` 按值返回 range/scope，所以 appfw 里
`vn::Progress` 是 **PUBLIC** 链接。`VN_APPFW_PLUGIN_ABI_VERSION` 当前 **8u**。

### 2.2 推送模型

- `ProgressHost::changed()` 返回进程级 `Signal<>`，在宿主注册/注销、前台栈压入/弹出、label 变化
  （设成相同值不发）、指示器位置跨过整百分点（含到终点、含被重置）时发火。
- 信号**不带载荷**：注销通知在宿主析构体内发出，带指针等于交给观察者一个正在死的对象；
  观察者重采样 `current()` / `activeHosts()`。
- 发火线程 = 改状态的线程（可能是工作线程）：呈现层自己编组；发火在锁外，绝不与注册表锁嵌套。

### 2.3 源头合流

`ProgressIndicator::setPositionCallback(std::function<void()>)` 只在“跨整百分点 / 到终点 /
位置被重置”时回调一次，而 `increment()` 是每条目一次的热路径。实测（-O3，2000 万条目）：
`next(1)` 由 10.0 → 9.9 ns/item，2000 万条目只发 **101** 次通知。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 前台栈 | 宿主注册/注销、`setForeground`、嵌套压栈/恢复 | 已落地 |
| F2 | 变更通知 | `changed()` 在状态变化/整百分点时发火 | 已落地 |
| F3 | 状态栏呈现 | `ProgressPresenter` 订阅 + 400/300 ms 单发定时器 | 已落地 |
| F4 | 无头呈现 | `ConsoleProgressReporter` 订阅 + 一次性唤醒 | 已落地 |
| F5 | 取消 | `ProgressIndicator` 的外部取消 token 共享 | 已落地 |

### 3.1.1 F1 前台栈

- **行为**：`ProgressHost` 持进程级注册表与前台栈；`setForeground(true)` 提升，嵌套子命令压栈，
  析构自动恢复父宿主。
- **约束**（C1）：没有前台宿主时 `current()` 为 nullptr；启动阶段自己也是一个前台宿主。
- **要求**（R1）：构造函数不建前台宿主；不跑启动的进程 `isBusy()` 必须为 false。
- **验收**（T1）：`ProgressHostTest` 的注册表/前台栈用例。
- **注意**（N1）：`isBusy()` = 有前台宿主。

### 3.1.2 F2 变更通知

- **行为**：`changed()` 在注册/注销、压栈/弹出、label 变化、整百分点/到终点/重置时发火。
- **约束**（C2）：信号不带载荷；发火在锁外；注销通知在宿主析构体内发出。
- **要求**（R2）：label 设成相同值不发；观察者重采样 `current()`/`activeHosts()`。
- **验收**（T2）：`ProgressHostTest` 的 changed() 合流用例（1 万条目约 100 次）。
- **注意**（N2）：发火线程可能是工作线程；呈现层自己 `QMetaObject::invokeMethod` 编组。

### 3.1.3 F3 状态栏呈现

- **行为**：`gui::ProgressPresenter` 订阅 `changed()` 后不再轮询；只为“出现延迟 400 ms”和
  “隐藏延迟 300 ms”各臂一个单发 `QTimer`。
- **约束**（C3）：跨线程通知投给原生 widget（`Qt::QueuedConnection`）；presenter 随 widget 销毁。
- **要求**（R3）：空闲时不持有任何定时器。
- **验收**（T3）：`ProgressPresenterTest.*`。
- **注意**（N3）：排队调用不会越过 widget 生命周期。

### 3.1.4 F4 无头呈现

- **行为**：`ConsoleProgressReporter` 订阅 `changed()`；show_delay 用 async 共享定时服务的一次性唤醒。
- **约束**（C4）：generation 计数器把 `stop()` 之后到点的唤醒变成空操作；`sleeping_` 保证同时只有一个。
- **要求**（R4）：`poll()` 保留给宿主自驱循环与测试，且不需要先 `start()`。
- **验收**（T4）：`ConsoleProgressReporterTest.*`。
- **注意**（N4）：`ConsoleProgressOptions::interval` 是最小行距，不是轮询周期。

### 3.1.5 F5 取消

- **行为**：`ProgressIndicator` 带外部取消 token；`ProgressHost` 共享取消源。
- **约束**（C5）：取消是请求；宿主不替消费者决定是否取消。
- **要求**（R5）：`stop()` 返回后不再有任何输出（互斥锁栅栏 + generation）。
- **验收**（T5）：`ProgressHostTest` 的取消用例、`ProgressPresenterTest` 的取消按钮。
- **注意**（N5）：注销通知后观察者看到的是“已经恢复的前台”（父宿主或 nullptr）。

## 4. 公开接口

### 4.1 `ProgressHost`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `current()` / `activeHosts()` | 进程级前台栈/注册表 | 观察者重采样 |
| `changed()` | `Signal<>` | 不带载荷 |
| `setForeground(bool)` / `isForeground()` | 前台提升 | 嵌套压栈/恢复 |
| `label()` / `setLabel()` | 按值返回/写入 | 值不变不发通知 |
| `stop()` | 停止上报 | 栅栏 + generation |

### 4.2 呈现者

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `gui::ProgressPresenter` | 订阅 + 单发定时器 | 空闲零定时器 |
| `ConsoleProgressReporter` | 订阅 + 一次性唤醒 | `poll()` 给自驱循环/测试 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 没有前台宿主时 `current()` 为 nullptr；启动阶段自己也是前台宿主。 |
| C2 | `changed()` 不带载荷；发火在锁外；注销通知在宿主析构体内发出。 |
| C3 | 跨线程通知投给原生 widget；presenter 随 widget 销毁。 |
| C4 | `stop()` 后到点的唤醒是空操作；同时只有一个待发唤醒。 |
| C5 | 取消是请求；宿主不替消费者决定。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 构造函数不建前台宿主；`isBusy()` = 有前台宿主。 |
| R2 | label 设成相同值不发；观察者重采样。 |
| R3 | 呈现者空闲时不持有定时器。 |
| R4 | `poll()` 不需要先 `start()`。 |
| R5 | `stop()` 返回后不再有任何输出。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 信号发火不持有注册表锁。 |
| N2 | 注销通知期间宿主对象仍活着，观察者能安全重采样。 |
| N3 | 启动阶段与普通命令共用同一份前台栈语义。 |

## 6. 注意事项与陷阱

### 6.1 注销通知里的指针

- 会怎么错：信号带宿主指针，观察者在宿主析构体里解引用。
- 怎么判：ASan/UAF；`changed()` 带参数。
- 怎么防：信号不带载荷，观察者重采样 `current()`/`activeHosts()`。

### 6.2 呈现者的定时器

- 会怎么错：空闲时仍轮询/常驻定时器，CPU 空转。
- 怎么判：没有待办截止时间仍有定时器。
- 怎么防：只为 400/300 ms 截止时间臂单发定时器；控制台用一次性唤醒。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 保留 base 里的自查回调 + 单观察者 | 层约束逼出来的降级；搬迁后作废。 | — |
| D2 | 把整个 progress 模块搬进 appfw | `ProgressIndicator`/Range/Scope 是零依赖原语。 | — |
| D3 | 控制台常驻轮询循环 | 事件驱动 + 一次性唤醒已足够，空闲零唤醒。 | — |
| D4 | 宿主轮询自己的 indicator | 退回拉模型。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 位置算术/range/scope/回调合流 | `tests/test_progress/ProgressIndicatorTest.cpp`（9 例） |
| T2 | 注册表/前台栈/label/取消/changed() | `tests/test_gui/ProgressHostTest.cpp`（15 例） |
| T3 | 控制台节流/静音/端到端 | `ConsoleProgressReporterTest.*`（3 例） |
| T4 | 状态栏取消/`isBusy()` | `ProgressPresenterTest.*` |

## 9. 状态与演进

### 9.1 当前状态

`ProgressHost`、两个呈现者、取消源共享均已落地；空闲零唤醒。

### 9.2 已知缺口 / 待办

无；新需求按“是否真的需要轮询/常驻定时器”判断。

### 9.3 兼容性 / 迁移

`label()` 改为按值返回；`ConsoleProgressOptions::interval` 语义改为最小行距。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/ProgressHost.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/ProgressPresenter.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConsoleProgressReporter.hpp`
- `src/base/progress/**`

### 10.2 文档与 ADR

- `appfw-startup-splash.md`
- `appfw-startup-phases.md`
- `../backlog/`

### 10.3 外部资料

- Core `Signal` 的线程安全语义。
