# appfw UserIO 设计（2026-09-11 审查轮）

代码：`src/fw/appfw/sdk/vine/appfw/UserIO.hpp` + `src/fw/appfw/src/UserIO.cpp`（基类），
`src/fw/appfw/src/ConsoleUserIO.hpp/.cpp`（无头）与
`src/fw/appfw/sdk/vine/appfw/gui/VisualUserIO.hpp` + `src/fw/appfw/src/gui/VisualUserIO.cpp`（GUI，2026-09-21 起公开），
宿主接线在 `Application`（`createUserIO()`/`setupUserIO()`、`ApplicationData::user_io`）、
`VisualUserIO::setConsolePanel()`（由宿主自己绑，见下）与 `CommandManager`（`reportToUser()`、命令集事件）。
测试：`tests/test_gui/test_gui.cpp`（`UserIOTest.*` 6 例、`ConsoleUserIOReadTest.*` 4 例 + `GuiTest.CommandManager_PendingUserInput*`）。

## 结构

| 类 | 何时创建 | 输出 | 读入 |
| --- | --- | --- | --- |
| `UserIO`（抽象基类） | — | `putString`/`clear` | 四个 `getXxxAsync` |
| `ConsoleUserIO` | `Application::createUserIO()` 默认（无 GUI） | `std::cout`（互斥） | 后台读线程 + 行缓冲，`std::getline` 永不在等待者线程上 |
| `VisualUserIO`（**公开 SDK 类**，实现为私有 PImpl） | `GuiApplication::createUserIO()` | `ConsolePanel`（编组） | `ConsolePanel` 的 `lineEntered`/`escapePressed` + `AsyncEvent` |

拥有者：`ApplicationData::user_io`（`unique_ptr`，先于 dispatcher 拆）；`Application::shutdown()` 先
`cancelPendingInput()` 再排空命令链。

### 面板接线（2026-09-21）

- **面板归调用方，绑定在 UserIO 上**：`VisualUserIO::setConsolePanel(ConsolePanel*)`（`nullptr` = 解绑，重绑先摘旧面板的
  handler）。插件建面板、挂 dock，再把它交出去；这个类不销毁面板。
- **`VisualUserIO` 是公开类，但实现是私有的**（`struct Impl` + `unique_ptr<Impl> d`）：它只有 `setConsolePanel` 加基类
  上的 override，没有暴露任何私有成员/方法，所以它的布局不因实现细节变化而变。以前它在 `src/` 里私有、
  公开门是 `GuiApplication::setConsolePanel()`；现在需要面板的宿主不必再是 `GuiApplication`，所以那道门没了意义。
- **`GuiApplication::setConsolePanel()` 已删**（2026-09-21，用户要求）：宿主自己 `obj_cast<VisualUserIO>(app->userIO())`
  再绑，`app_shell` 就是这样做（找不到可视 IO 时它自己记一条 warning）。
  代价是“绑不上”不再集中在框架里报：每个调用方都得自己判空（app_shell 的 `load()` 里那 4 行）。
- **没有面板时**：输出被丢弃（`putString`/`clear` 直接返回），而 `waitForInput` 会跳过显示提示后
  `co_await done` —— 能唤醒它的只有面板的 `lineEntered`/`escapePressed` 或关机的 `cancelPendingInput()`，
  所以一个 `getXxxAsync` 会挂到关机（不崩、不报错）。要改语义就得先在 `UserIO` 层决定“没有交互面”算什么。

## 契约（本轮固化）

### 线程

- **所有入口都可以从任意线程调用。** 命令在它恢复时所在的线程上继续（定时器线程、IO 线程），
  所以 `putString`/`clear` 是"任意线程"的输出路径：实现负责编组，调用方不管线程。
  `VisualUserIO` 用 `MainThreadDispatcher` 编组（`onApplicationThread`），`ConsoleUserIO` 用一把
  stdout 互斥锁。
- **编组之后还要防存活**：投递出去的回调可能排在事件循环上，而面板（或绑定它的窗口/测试）已经
  销毁。`VisualUserIO` 的 `onConsolePanel()` 用 `QPointer<QWidget>` 守卫，面板没了就是空操作
  （这个坑是实测出来的：测试结束时未投递的输出会让下一次 `deliverPostedCalls()` 写已释放的面板）。
- **`cancelPendingInput()` 可以从任意线程调用**，包括事件循环已经停掉的时候（`Application::shutdown()`
  正是如此）——所以它只能改原子标志并 `set()` 事件，绝不能碰 UI。

### 交互

- **同一时刻只允许一个交互等待**：控制台只显示一个提示，两个等待中的读会共享同一个完成事件和结果
  字段（旧实现里第二次读会覆盖第一次的 `pending_`/结果，一次输入喂给两个读）。第二个 `getXxxAsync`
  立即以 `std::nullopt` 收尾并记 warning。
- **槽位由 `ReadScope` 释放**：无论读怎么结束（正常、取消，甚至协程帧在恢复前就被销毁）都会释放，
  否则一次被丢弃的 task 会让 IO 永远拒绝后续读。
- **取消必须真的解开等待**：`ConsoleUserIO` 用后台线程读 `std::cin`（阻塞读无法被中断），
  取消时设置共享状态并唤醒等待者，未消费的行留在缓冲里给下一个读。

### 取值

- `getIntAsync()` 产出 **`int`**（旧签名是 `int8_t`：输入 1000 会变成 -24）。
- 解析走 `UserIO::parseInt()`：`String::toInt()` 经 `strtol` 再转 `int`，超出范围会静默回绕，
  不能用于用户输入。`toDouble` 的结果还必须 `std::isfinite`。
- 提示文本（`PromptState::current`）只在**应用线程**读写：写入挪进了"显示提示"那条编组调用
  （`onConsolePanel`），读点是应用线程上的 `repromptError()`。它不再是普通成员而是 `shared_ptr` 持有的小状态，
  这样 posted 回调可以写它而**不必捕获 UserIO 自身**（后者可能在回调真正执行前就被拆了）。
  结果字段由应用线程写入、由等待者线程在 `done_` 之后读取（`AsyncEvent` 的互斥建立 happens-before），
  `cancelled_`/`pending_` 是原子的。
  ⚠️ 旧文档曾写"提示文本只在应用线程读写"，但当时的写点在 `beginRead()`——它跑在**读发起线程**上，
  而 `getXxxAsync` 可以由在定时器/IO 线程上恢复的命令调用；已于 2026-09-17 修正（见
  `tests/test_gui` 的 `UserIOTest.ReadStartedOnAWorkerThreadIsMarshalledAndReprompts`，该用例先断言"读确实
  在非应用线程发起"再验证整条链路）。
- `onApplicationThread()` 带一条断言：有事件循环时，inline 路径必须在应用线程（否则就是拿控件在错线程上写）；
  这是后续改动的保险丝。

### 输出与列表

- `putString` 一行一条、`clear` 清屏；`ConsoleUserIO::clear()` 无条件输出 ANSI 序列（重定向到文件时
  会落进控制字符，留档不改）。
- `VisualUserIO` 的补全列表是快照：绑定控制台时刷新，并订阅 `CommandManager::commandsChanged`
  以跟上之后注册/卸载/改名的命令（旧实现只在绑定时刷一次，app_shell 之后加载的插件命令永远不进补全）。
- 重新绑定面板会先摘掉旧面板上的两个 handler（旧实现每次都挂新的，同一个面板绑两次 ⇒ 一行输入
  被执行两次）。

### 无头进度（2026-09-18）

`LongRunning` 命令会建一个 ambient `ProgressHost`，GUI 侧由 `ProgressPresenter` 显示；无头侧过去
**什么都不显示**。现在 `ConsoleUserIO` 自带一个 `ConsoleProgressReporter`
（`sdk/vine/appfw/ConsoleProgressReporter.hpp`，构造时 `start()`）：

- 语义是**一行一条**（`[进度] 42% 阶段名`，不覆盖已在屏幕上的行），不是往下堆日志；宿主结束后补一行
  `[进度] 已结束`，`stop()` 返回之后不再有任何输出。
- 节流三件套（`ConsoleProgressOptions`）：`show_delay{500ms}`（这么短就结束的操作干脆不显示，避免
  "闪一下就没了"）、`interval{200ms}`（**最小行距**）、`step_percent{5}`（百分比变化不足 5% 不重画）。
  标签变化或进度回退（新宿主/换阶段）会立即重画。
- 它**订阅** `ProgressHost::changed()`，不轮询：唯一需要时钟的是 show_delay（跑到一段时间而没有任何
  进度报告也该出一行），用 async 共享定时服务的一次性唤醒；空闲时没有待发唤醒。
- `poll()` 是公开的：宿主自己的循环可以驱动它（不需要先 `start()`），用例也靠它做确定性断言
  （把 `interval` 设成 0 即"不受最小行距限制"）。
- 生命周期：消费者是 `ConsoleUserIO` 的**最后一个成员**（先析构，早于 `output_mutex_`），sink 走非虚的
  `writeLine`——析构期间不能再碰虚函数 `putString`（对象已在销毁中）。
- 状态共用既非默认选择：`ConsoleProgressReporter` 是唯一用 `shared_ptr<Impl>` 的 appfw PImpl（其余用
  `unique_ptr`），因为一次性唤醒的帧得靠它活下去。
- 直接构造 `ConsoleUserIO` 的用例需要 appfw 私有头：`test_gui` 的 CMake 单列了这个 include 目录，与
  `test_asyncqt` 引私有 Qt 协程胶水同样的做法。

## 本轮审查（11 项）

| 编号 | 缺陷 | 证据 | 修复 |
| --- | --- | --- | --- |
| U1 | 输出路径不编组：`VisualUserIO::putString` 直接写 QWidget | `ConsolePanel::append` 不做编组，而 `CommandManager::reportToUser` 与 `appendOnApplicationThread` 都编组；app_shell 的命令全部 `io->putString(...)` | 编组下沉到 `putString`/`clear`（`onConsolePanel`），并在 `UserIO` 里写明"任意线程可调用、实现负责编组" |
| U2 | `getIntAsync` 返回 `int8_t` 且两个实现无检查地窄化 | `static_cast<int8_t>(1000)` = -24 | 签名改为 `int`（无调用方），解析改走 `UserIO::parseInt`；`VN_APPFW_PLUGIN_ABI_VERSION` 1u → 2u，插件必须重编 |
| U3 | 并发读共享 `done_`/结果字段，互相踩 | 一份 `pending_`/`cancelled_`/四个结果字段 | 单交互槽位 + `beginRead` 的 CAS 拒绝 + warning，`UserIOTest.SecondReadIsRefusedWhileOneIsPending` |
| U4 | `ConsoleUserIO` 阻塞 `std::getline` ⇒ `cancelPendingInput()` 无效，关机可能让命令恢复到已拆的管理器上 | 旧实现直接在等待者线程上 `getline` | 一个后台读线程 + 行缓冲 + 可唤醒的 `AsyncEvent`，取消后读取立即返回 |
| U5 | 补全列表不刷新：绑定 console 之后注册的命令进不了补全 | `refreshCompletion` 只在 `setConsolePanel`/`setCommandManager` 调用；app_shell 在自己的 `load()` 里绑定，命令在加载期注册 | 新增 `CommandManager::commandsChanged` 事件（注册/取消/启用开关/别名，均在锁外触发），`VisualUserIO` 订阅后编组刷新 |
| U6 | 交互状态无同步，而 `cancelPendingInput()` 可能来自别的线程 | `cancelled_`/`pending_` 是普通成员 | 两者改 `std::atomic`；`set()` 待在两个锁之外调用，避免"被唤醒的读又要拿锁"而死锁 |
| U7 | 重复绑定面板会重复挂 handler → 一行输入执行两次 | `setConsolePanel` 只 `connect` | 保存 `Connection` 成员，重绑时先 `disconnect()`（现在是 RAII 句柄，见 command-manager 设计文档）；`UserIOTest.RebindingTheConsoleDoesNotRunALineTwice` |
| U8 | `GuiApplication::setConsolePanel` 用 `static_cast<VisualUserIO*>` | 子类换 `createUserIO()` 即 UB | 改 `obj_cast<VisualUserIO>`（该方法 2026-09-21 已随公开类一起删除：绑定现在直接写在 `VisualUserIO` 上） |
| U9 | `ConsoleUserIO` 细节：stdout 可能交错、非 EOF 失败不区分 | 无锁 `std::cout`；只看 `eof()` | stdout 互斥；`eof`/`bad` 分开记录；两者都让读返回 `nullopt` |
| U10 | `putString`/`clear`/`setCommandManager`/`cancelPendingInput` 缺线程契约 | 头文件没写 | 基类补齐（含"实现负责编组"与"槽位唯一"） |
| U11 | 缺本设计文档 | 其他 appfw 模块都有 | 本文 |

审查顺带发现并修掉的根因：**`String::toInt()` 会静默回绕**（`strtol` 饱和后 `static_cast<int>`）。
它自己的文档承诺"不是有效整数则返回 0 且 ok=false"，所以按契约补上范围检查，并在 `StringTest`
固化（`99999999999` → 0/ok=false，`2147483647` → 保留）。

## 不变量

1. 任何线程调用 `putString`/`clear` 都安全；真正碰面板的动作只在应用线程上发生，且面板已销毁时是空操作。
2. 同一时刻最多一个 `getXxxAsync` 在等待；它的槽位在协程结束（或被销毁）时释放。
3. `cancelPendingInput()` 返回后，等待中的读一定会以 `std::nullopt` 结束（无头实现也不例外）。
4. `getIntAsync()` 只会给出落在 `int` 范围内的值，绝不回绕。
5. 补全列表在命令集变化后与 `CommandManager::commandInfos()` 保持一致。
6. 读可以从任意线程发起，而提示记帐与面板写入只发生在应用线程上。
7. 有前台进度时无头输出里一定会出现进度行（`LongRunning` 命令在无头模式下不再"默默跑"）。

## 测试映射（tests/test_gui/test_gui.cpp）

| 用例 | 覆盖 |
| --- | --- |
| `UserIOTest.OutputFromAWorkerThreadIsMarshalledToTheApplicationThread` | U1 |
| `UserIOTest.SecondReadIsRefusedWhileOneIsPending` | U3 |
| `UserIOTest.IntReadKeepsItsValueAndRepromptsOnOverflow` | U2（1000 原样、超出 `int` 重新提示） |
| `UserIOTest.RebindingTheConsoleDoesNotRunALineTwice` | U7 |
| `UserIOTest.ReadStartedOnAWorkerThreadIsMarshalledAndReprompts` | E4：读在非应用线程发起（先 `sleepFor` 再提示），提示/重新提示/取值照常 |
| `UserIOTest.CommandsChangedReportsRegistryAndAliasEdits` | U5 的机制 |
| `ConsoleUserIOReadTest.ReadsLinesAndReportsEndOfInput` | 无头读路径：行、队列、EOF 收尾、提示上屏 |
| `ConsoleUserIOReadTest.ParsesIntsAndFailsOnBadInputWithoutReprompt` | 无头解析语义：坏行/溢出 ⇒ `nullopt`（重提示是 GUI 的交互模型） |
| `ConsoleUserIOReadTest.SecondReadIsRefusedWhileOneIsPending` | 不变量 2（无头侧） |
| `ConsoleUserIOReadTest.CancelFromAnotherThreadUnblocksAndKeepsArrivedLine` | 不变量 3（无头侧）+ 取消不吞后续输入、不泄漏给下一个读 |
| `GuiTest.CommandManager_PendingUserInputBlocksDrainUntilCancelled` | 取消与排空（原有） |
| `GuiTest.CommandManager_DetachedFailureIsReportedOnTheApplicationThread` | 失败上报编组（原有） |
| `ConsoleProgressReporterTest.WritesThrottledLinesWhileAForegroundOperationRuns` | 无头进度的节流/标签/收尾（确定性，`poll()` 直驱） |
| `ConsoleProgressReporterTest.ChangeNotificationWritesLinesAndStops` | 订阅驱动的一次性唤醒、`stop()` 后无输出 |
| `ConsoleProgressReporterTest.ConsoleUserIOPrintsTheProgressOfAForegroundOperation` | 端到端：无头 IO 构造即挂消费者，进度出现在 stdout |
| `test_core String.NumericConversions` | `String::toInt` 范围检查 |

## 已评估但**未采纳**（留档）

- **不给 `ConsolePanel` 加"当前补全条目"的 getter**：为测试方便扩公共 API 不划算，U5 以机制测试
  （`commandsChanged`）+ 绑定路径覆盖，弹窗行为留给人工检查。
- **不把 `ConsoleUserIO` 的读改成非阻塞 `poll`**：跨平台（Windows 管道/控制台）成本远高于收益，
  单后台线程 + 行缓冲已经满足"可取消"和"不丢行"两条。
- **不为 `ConsoleUserIO::clear()` 加 TTY 判断**：需要 `isatty`/`_isatty` 的平台分支，留档（重定向时
  会出现 ANSI 控制字符）。
- **不在 `UserIO` 上加读取"存储类型"之类的访问器**：本轮的 `parseInt` 是唯一需要共享的解析逻辑。
- **两个实现的"悬挂读"状态机合一提炼（③，2026-09-25 复核后降级为覆盖收口）**：原议是把单飞拒绝/取消/重提示
  收进一个共享内部状态机、两个实现退化为渲染后端。**不做**：等待机制结构不同（`ConsoleUserIO` = 分离读线程 +
  行队列 + 重臂事件循环；`VisualUserIO` = UI 事件 + 按类型结果字段 + 重提示），`ConsoleUserIO` 的取消标志必须
  住在 `StdinReader`（读线程可以活过 IO 对象本身），且解析失败语义刻意不同（无头 ⇒ `nullopt`，GUI ⇒ 原地重提示）
  ——可共享的只有 ~15 行槽位协议，合并反而要跨这两种形状做抽象。改以覆盖收口：新增 `ConsoleUserIOReadTest.*`
  四条用例（行/解析/单飞/跨线程取消），此前无头读路径只有进度侧有覆盖；变异 M1/M2/M3 全红见证
  （槽位不释放 / 取消标志不重置 / 提示不上屏）。
