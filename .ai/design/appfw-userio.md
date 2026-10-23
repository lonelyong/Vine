# appfw UserIO 设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`UserIO` 契约、控制台/可视两个后端、文件读取与输入协议。
> **权威来源**：`UserIO.hpp`、`ConsoleUserIO.hpp`、`VisualUserIO.hpp`、`tests/test_gui`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

命令在任意线程恢复，却要输出到控制台、向用户提问、读文件/目录路径。输出若直接写 QWidget 会跨线程；
阻塞读若发生在等待者线程上，关机会卡住；两个并发读会互相覆盖提示与结果。

### 1.2 目标

- 所有 `UserIO` 入口可从任意线程调用；实现负责编组。
- 同一时刻只允许一个交互等待；取消必须真的解开等待。
- 无头与 GUI 后端共享解析/校验逻辑，但交互模型可以不同。
- 无头模式也有进度输出（`LongRunning` 命令不再默默跑）。

### 1.3 非目标

- 不做拖放、路径 Tab 补全、自动提交。
- 不把 `ConsoleUserIO` 的读改成非阻塞 `poll`。
- 不给 `ConsoleUserIO::clear()` 加 TTY 判断。

### 1.4 一句话结论

`UserIO` 把“输出 / 提问 / 取值 / 文件路径”收进实现：任意线程可调、实现负责编组；单交互槽位 + 可唤醒取消；
GUI 用 `ConsolePanel`，无头用后台读线程 + 行缓冲。

## 2. 设计要点

### 2.1 两个后端

| 类 | 创建 | 输出 | 读入 |
| --- | --- | --- | --- |
| `UserIO`（抽象基类） | — | `putString`/`clear` | 四个 `getXxxAsync` + 四个文件/目录读 |
| `ConsoleUserIO` | `Application::createUserIO()` 默认（无 GUI） | `std::cout`（互斥） | 后台读线程 + 行缓冲 |
| `VisualUserIO`（公开 SDK 类，私有 PImpl） | `GuiApplication::createUserIO()` | `ConsolePanel`（编组） | `lineEntered`/`escapePressed` + `AsyncEvent` |

拥有者：`ApplicationData::user_io`（`unique_ptr`，先于 dispatcher 拆）；`Application::shutdown()` 先
`cancelPendingInput()` 再排空命令链。

### 2.2 线程

- 所有入口都可以从任意线程调用；`VisualUserIO` 用 `MainThreadDispatcher` 编组，`ConsoleUserIO` 用 stdout 互斥锁。
- 编组后还要防存活：`VisualUserIO` 用 `QPointer<QWidget>` 守卫面板；面板没了就是空操作。
- `cancelPendingInput()` 可从任意线程调用，包括事件循环已停时；它只改原子标志并 `set()` 事件，绝不碰 UI。

### 2.3 交互

- 同一时刻只允许一个交互等待；第二个 `getXxxAsync` 立即 `nullopt` + warning。
- 槽位由 `ReadScope` 释放：正常、取消、协程帧提前销毁都会释放。
- `ConsoleUserIO` 用后台线程读 `std::cin`；取消设置共享状态并唤醒等待者，未消费的行留给下一个读。

### 2.4 取值

- `getIntAsync()` 产出 `int`；解析走 `UserIO::parseInt()`（`String::toInt()` 会静默回绕，不能直接用于用户输入）。
- `toDouble` 结果必须 `std::isfinite`。
- 提示文本只在应用线程读写；写点在“显示提示”的编组调用里，结果字段由应用线程写、等待者线程在 `done_` 之后读。

### 2.5 输出与补全

- `putString` 一行一条、`clear` 清屏；`ConsoleUserIO::clear()` 无条件输出 ANSI 序列（重定向时会落控制字符，留档不改）。
- `VisualUserIO` 的补全列表是快照：绑定控制台时刷新，并订阅 `CommandManager::commandsChanged` 跟上命令集变化。
- 重绑面板先摘旧 handler；同一个面板绑两次不会执行两次。

### 2.6 无头进度

`ConsoleUserIO` 自带 `ConsoleProgressReporter`（构造时 `start()`）：一行一条 `[进度] 42% 阶段名`，
结束后补 `[进度] 已结束`；`stop()` 后不再输出。节流：`show_delay{500ms}` / `interval{200ms}`（最小行距）/
`step_percent{5}`；订阅 `ProgressHost::changed()`，空闲零唤醒；`poll()` 给自驱循环与测试。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 输出 | `putString`/`clear` 任意线程可调，实现编组 | 已落地 |
| F2 | 提问/取值 | `getString/Int/Double/BoolAsync`；单槽位 | 已落地 |
| F3 | 文件/目录读 | 四个分开的接口；`;` 分隔多路径；浏览按钮 | 已落地 |
| F4 | 补全 | 命令集变化后刷新快照 | 已落地 |
| F5 | 无头进度 | `ConsoleProgressReporter` 一行一条 | 已落地 |

### 3.1.1 F1 输出

- **行为**：`putString` 一行一条，`clear` 清屏。
- **约束**（C1）：任意线程可调；`VisualUserIO` 编组到应用线程，`ConsoleUserIO` 用 stdout 互斥。
- **要求**（R1）：面板已销毁时输出是空操作。
- **验收**（T1）：`UserIOTest.OutputFromAWorkerThreadIsMarshalledToTheApplicationThread`。
- **注意**（N1）：`ConsoleUserIO::clear()` 无条件输出 ANSI 序列。

### 3.1.2 F2 提问/取值

- **行为**：四个 `getXxxAsync`；同一时刻只允许一个等待。
- **约束**（C2）：第二个读立即 `nullopt` + warning；槽位由 `ReadScope` 释放。
- **要求**（R2）：`getIntAsync()` 只给 `int` 范围值，绝不回绕；`toDouble` 必须 finite。
- **验收**（T2）：`SecondReadIsRefusedWhileOneIsPending`、`IntReadKeepsItsValueAndRepromptsOnOverflow`。
- **注意**（N2）：提示文本只在应用线程读写。

### 3.1.3 F3 文件/目录读

- **行为**：四个分开的接口（文件/文件夹 × 单个/多个）；多个路径一行用 `;` 分隔；GUI 有浏览按钮。
- **约束**（C3）：选项随请求下发（`FileReadOptions` / `DirReadOptions`）；校验在基类共享。
- **要求**（R3）：无头侧不重提示；坏路径返回 `nullopt`。
- **验收**（T3）：`ConsoleUserIOReadTest.*` 与 GUI 的浏览端到端用例。
- **注意**（N3）：`require_existing` 取反读的是“另存为”，不叫 `must_exist`。

### 3.1.4 F4 补全

- **行为**：`VisualUserIO` 绑定 console 时刷新补全，并订阅 `commandsChanged`。
- **约束**（C4）：补全列表是快照；每键路径只过滤本地快照，不碰注册表锁。
- **要求**（R4）：重绑面板先摘旧 handler。
- **验收**（T4）：`CommandsChangedReportsRegistryAndAliasEdits`、`RebindingTheConsoleDoesNotRunALineTwice`。
- **注意**（N4）：注册表 O(N) 拷贝只发生在 `refreshCompletion()`。

### 3.1.5 F5 无头进度

- **行为**：`ConsoleUserIO` 构造时挂 `ConsoleProgressReporter`；一行一条，结束补一行。
- **约束**（C5）：`stop()` 后不再输出；消费者是最后一个成员，早于 `output_mutex_` 析构。
- **要求**（R5）：空闲零唤醒；`poll()` 不需要先 `start()`。
- **验收**（T5）：`ConsoleProgressReporterTest.*`。
- **注意**（N5）：析构期间不能碰虚函数 `putString`，sink 走非虚 `writeLine`。

## 4. 公开接口

### 4.1 `UserIO`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `putString` / `clear` | 任意线程可调 | 实现编组 |
| `getStringAsync` / `getIntAsync` / `getDoubleAsync` / `getBoolAsync` | 单槽位 | `int` 范围检查 |
| `getFileAsync` / `getFilesAsync` / `getDirAsync` / `getDirsAsync` | 文件/目录读 | `;` 分隔多路径 |
| `cancelPendingInput()` | 任意线程可调 | 只改原子标志 + `set()` |
| `FileReadOptions` / `DirReadOptions` | 选项 | `extensions` / `require_existing` / `suggested_path` |

### 4.2 后端

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `ConsoleUserIO` | 无头；后台读线程 + 行缓冲 | stdout 互斥 |
| `VisualUserIO` | GUI；公开类 + 私有 PImpl | `setConsolePanel` 绑定 |
| `ConsoleProgressReporter` | 无头进度 | 订阅 `ProgressHost::changed()` |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 任意线程可调；实现负责编组。 |
| C2 | 同一时刻最多一个交互等待；槽位由 `ReadScope` 释放。 |
| C3 | 文件/目录选项随请求下发；校验在基类共享。 |
| C4 | 补全列表是快照；每键路径不碰注册表锁。 |
| C5 | `stop()` 后不再输出；消费者先于 `output_mutex_` 析构。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 面板已销毁时输出是空操作。 |
| R2 | `getIntAsync` 只给 `int` 范围值；`toDouble` finite。 |
| R3 | 无头侧不重提示；坏路径返回 `nullopt`。 |
| R4 | 重绑面板先摘旧 handler。 |
| R5 | 进度空闲零唤醒；`poll()` 不需要先 `start()`。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | `cancelPendingInput()` 返回后，等待中的读一定以 `nullopt` 结束。 |
| N2 | 读可从任意线程发起；提示记帐与面板写入只在应用线程。 |
| N3 | 补全与 `CommandManager::commandInfos()` 保持一致。 |
| N4 | 有前台进度时无头输出一定有进度行。 |

## 6. 注意事项与陷阱

### 6.1 编组后的面板生命周期

- 会怎么错：posted 回调排在事件循环上，面板已销毁 → 写已释放对象。
- 怎么判：测试结束未投递输出让下一次 `deliverPostedCalls()` 崩。
- 怎么防：`QPointer<QWidget>` 守卫；面板没了就是空操作。

### 6.2 阻塞读与取消

- 会怎么错：`std::getline` 在等待者线程上阻塞，`cancelPendingInput()` 无效，关机卡住。
- 怎么判：取消后读不返回；关机超时。
- 怎么防：后台读线程 + 行缓冲 + 可唤醒 `AsyncEvent`；未消费行留给下一个读。

### 6.3 `String::toInt()` 静默回绕

- 会怎么错：用户输入 1000 变成 -24。
- 怎么判：`static_cast<int8_t>(1000)`；超出 `int` 的值回绕。
- 怎么防：`UserIO::parseInt()` 做范围检查；`toDouble` 检查 finite。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 给 `ConsolePanel` 加“当前补全条目” getter | 为测试扩公共 API 不划算。 | — |
| D2 | `ConsoleUserIO` 读改非阻塞 `poll` | 跨平台成本高；后台线程 + 行缓冲已满足可取消/不丢行。 | — |
| D3 | `ConsoleUserIO::clear()` 加 TTY 判断 | 需要平台分支；留档。 | — |
| D4 | 在 `UserIO` 加“存储类型”访问器 | `parseInt` 是唯一需要共享的解析逻辑。 | — |
| D5 | 两个后端的悬挂读状态机合并 | 等待机制结构不同；可共享的只有 ~15 行槽位协议。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 输出跨线程编组 | `UserIOTest.OutputFromAWorkerThreadIsMarshalledToTheApplicationThread` |
| T2 | 单槽位/int 范围 | `SecondReadIsRefusedWhileOneIsPending`、`IntReadKeepsItsValueAndRepromptsOnOverflow` |
| T3 | 无头读/取消/解析 | `ConsoleUserIOReadTest.*`（4 例） |
| T4 | 补全刷新/重绑 | `CommandsChangedReportsRegistryAndAliasEdits`、`RebindingTheConsoleDoesNotRunALineTwice` |
| T5 | 无头进度 | `ConsoleProgressReporterTest.*`（3 例） |
| T6 | `String::toInt` 范围 | `test_core String.NumericConversions` |

## 9. 状态与演进

### 9.1 当前状态

两个后端、单槽位、文件/目录读、补全刷新、无头进度均已落地。

### 9.2 已知缺口 / 待办

- 拖放、路径 Tab 补全、自动提交不做。
- 没有交互面板时 `getXxxAsync` 会挂到关机；要改语义先在 `UserIO` 层定义“没有交互面”算什么。

### 9.3 兼容性 / 迁移

`getIntAsync` 返回 `int`（旧 `int8_t`）；文件/目录读的四个虚函数追加在 vtable 末尾，ABI 当前 8u。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/UserIO.hpp`
- `src/fw/appfw/src/ConsoleUserIO.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/VisualUserIO.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConsoleProgressReporter.hpp`
- `tests/test_gui/test_gui.cpp`

### 10.2 文档与 ADR

- `appfw-progress.md`
- `appfw-command-manager.md`
- `../backlog/`

### 10.3 外部资料

- `String::toInt()` / `toDouble` 的契约。
- `ConsolePanel` 的 `lineEntered` / `escapePressed`。
