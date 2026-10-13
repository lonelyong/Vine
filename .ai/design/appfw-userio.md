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
- **补全的 `registry_mutex` 读热点（④B，2026-09-25 核实为不成立）**：每键路径
  （`ConsolePanel::Impl::onTextChanged` → `CommandCompleter::complete()`）只过滤**本地快照**
  （`setEntries` 建好的候选表，纯内存前缀匹配），不碰注册表锁；注册表读（`commandInfos()`，O(N) 拷贝）
  只发生在 `refreshCompletion()`——绑定面板与 `commandsChanged`（注册/卸载/启用/别名）时，
  量级 = 命令集变化次数（全仓导出命令 ≈18 个；启动期插件加载会有连续几十次，
  每次 O(N) 拷贝 + 一次编组 UI 更新）。没有测量支持它是瓶颈；若未来剖析真的看到启动期刷新成堆，
  再考虑把连续刷新合并成一次 posted 刷新——当前不动。

## 文件 / 目录读取（2026-10-10 落地）

> 状态：**已落地**（W1–W4）。三条定稿（用户 2026-10-10）：**对外四个分开的接口**（文件/文件夹 × 单个/多个）、
> **多个路径写在一行里用 `;` 分隔**、**浏览按钮（打开文件对话框）**。理由分别在 §1 / §2 / §5。
>
| 落地时**刻意没做**的：拖放（§5.4 只留设计）、路径的 Tab 补全、自动提交。
> `VN_APPFW_PLUGIN_ABI_VERSION` **不用 +1**：四个新虚函数追加在 vtable 末尾，`ConsolePanel` 新增的状态全在 `Impl` 里，
> 一个 `Signal` 成员都没加（§5）。
>
> 测试：`tests/test_gui/test_gui.cpp` 的 `ConsoleUserIOReadTest.*`（无头）与 `UserIOTest.*`（GUI，含一个真的点按钮、
> 真的开对话框、由用例自己关掉对话框的端到端用例）。

### 0. 为什么合理

今天命令想拿一个文件路径只有 `getStringAsync()`：提示与取值都在框架里，但**"这个路径能不能用"和"再问一次"调用方做不到** ——
重提示是实现的内部状态机（`VisualUserIO::Impl::parseAndComplete()` + `repromptError()`），调用方在
`getStringAsync()` 之上自己循环会造出第二套语义（无头侧**不该**重提示，见
`ConsoleUserIOReadTest.ParsesIntsAndFailsOnBadInputWithoutReprompt`）。

所以"路径"和"整数"是同一类东西：**一类值 = 一种提问**（提示文本 + 解析 + 校验 + 失败后的处理），
而四个 `getXxxAsync` 存在的理由正是"把这几件事收进实现"。加路径读取不扩大这个职责边界。

三条具体理由：

1. **校验只能在实现里做**（重提示是实现的交互模型），所以选项必须随请求下发，不能留在调用方。
2. **存在性/类型/后缀是共享逻辑**，与 `parseInt()` 同层：写在基类，两个后端给出**一致**的判定与文案。
3. **GUI 的"浏览…"需要一个挂点**：只有 IO 知道"当下这次读的是文件还是目录、后缀是这些、允许多条"。
   挂在 `getStringAsync()` 上是挂不上去的（IO 不知道这次问的是不是文件）。

代价（认下来）：`UserIO` 追加四个虚函数 + 两个选项结构；`ConsolePanel` 后期多一个按钮。

### 1. 接口（SDK 面）

**对外是四个分开的名字**：文件与文件夹各一个单个、一个多个，调用方谁也不传 "kind"（用户要求）。
选项也**分成两个结构**，因为字段集合本来就不一样：目录读没有"后缀"这一项，
所以那个组合在类型上就不存在。两个选项类型对外的名字都在 `UserIO` 里（`UserIO::FileReadOptions`、`UserIO::DirReadOptions`），
定义则放在 `vn::appfw::detail`、类里是同名别名：四个读的 `options = {}` 默认参数在类作用域就要用到这个类型的
默认成员初始化器，而 clang/gcc 都拒绝“使用词法嵌套在同一个类里的类的默认成员初始化器”（只有 MSVC 接受），
别名是保住 `UserIO::FileReadOptions` 写法与聚合布局的代价最小形式（嵌套形式已在 clang/gcc 上编译失败过一次）。
名字仍然只服务这个类的四个读，不占用 `vn::appfw`。

```cpp
namespace detail {
    /// 文件读取的要求：实现按它校验，GUI 的浏览对话框按它设过滤器。
    struct FileReadOptions {
        /// 允许的后缀：**规则**，不是标题 —— 每个实现都拿它校验（无头侧根本没有选择器，照样拒错后缀），
        /// 所以它是一张结构化的表，而不是一条 Qt filter 串（"只留一个 filter 字段"为什么不做的理由见 §6）。
        /// 大小写不敏感，"stl" / ".stl" / "*.stl" 都认；支持多段后缀（"tar.gz"）；空 = 不限。
        std::vector<String> extensions;
        /// 是否要求目标已存在。取反读的是"另存为"：`require_existing = false` = "不必已存在"，
        /// 而 `must_exist = false` 会被读成"**必须不**存在" —— 这就是它不叫 must_exist 的原因。
        bool                  require_existing{ true };
        /// 建议路径：GUI 预填输入行，无头只当提示的一部分。是**路径**（与返回值同类型），不是拼好的文本。
        std::filesystem::path suggested_path;
    };

    /// 目录读取的要求。
    struct DirReadOptions {
        /// 是否要求目标已存在（false = "还不存在的目标目录"，用户要新建的那种）。
        bool                  require_existing{ true };
        /// 建议路径，语义同 FileReadOptions::suggested_path。
        std::filesystem::path suggested_path;
    };
} // namespace detail

class UserIO : public Object {
  public:
    /// 一次回答里交过来的路径表（';' 切分后的结果）。
    using PathList = std::vector<std::filesystem::path>;

    /// 对外的名字（定义在 detail，原因见正文）。
    using FileReadOptions = detail::FileReadOptions;
    using DirReadOptions  = detail::DirReadOptions;
```

```cpp
  public:
    // …四个 getXxxAsync 与 cancelPendingInput() 不动，新条目**追加在 vtable 末尾**…

    /// @brief 请用户给一个文件；@param options 后缀过滤与存在性要求；@return 路径，取消/拒绝为 nullopt。
    virtual vn::async::Task<std::optional<std::filesystem::path>> getFileAsync(const String&          prompt,
                                                                              const FileReadOptions& options);

    /// @brief 请用户给一个或多个文件（多个写在一行里，';' 分隔）。
    virtual vn::async::Task<std::optional<PathList>> getFilesAsync(const String& prompt, const FileReadOptions& options);

    /// @brief 请用户给一个目录。
    virtual vn::async::Task<std::optional<std::filesystem::path>> getDirAsync(const String& prompt,
                                                                             const DirReadOptions& options);

    /// @brief 请用户给一个或多个目录（多个写在一行里，';' 分隔）。
    virtual vn::async::Task<std::optional<PathList>> getDirsAsync(const String& prompt, const DirReadOptions& options);
```

- **单个与多个分开**是有代价的：四个槽位、两个后端各四个 override。但每个 override 只有三四行，
  转发到它自己的两个私有助手（单个/多个各一个，带一个**后端私有**的 kind 参数），逻辑不重复。
- `prompt` 与 `options` 都有默认值（`getFileAsync(u8"网格: ")` 这样写就够用），而默认**只在编译期**生效，
  所以它不影响 vtable 里那一格。
- 四个都是**非纯虚**（默认实现 = 记一条 warning + `nullopt`），同 `cancelPendingInput()` 的口径：
  基类不做假承诺（"做不到就别装"），老插件编译出的 `UserIO` 子类不会因为一次没人调用的新槽位出问题。
  真要"不支持就报错"可以在将来把默认实现改成 abort；两个仓内实现都必须 override。
- ABI：四处**追加在已有虚函数之后**（含 `cancelPendingInput()` 之后），老槽位下标不动，
  `VN_APPFW_PLUGIN_ABI_VERSION` **不用 +1**（与启动框那次追加同理）。两个选项结构是插件可见结构：
  以后加字段属于改布局，那时要按 `PluginInfo` 的规矩 +1。
- 返回 `std::filesystem::path` 而不是 `String`：校验（存在/类型/后缀）本来就要构造路径，
  返回文本会把 `String → path` 的转换散给每个调用方（仓库现行做法是 `std::filesystem::path(std::u8string(text))`）。
  "用户选的是挂载 VFS 里的条目"是另一个请求（返回 `Vfs` 概念），不在本节。

调用方长这样：

```cpp
const auto mesh = co_await io->getFileAsync(u8"网格文件: ", FileReadOptions{ { u8".stl", u8".obj" }, u8"网格文件" });
if (!mesh) {
    co_return CommandResult(CommandStatus::Cancelled);
}

const auto inputs = co_await io->getFilesAsync(u8"输入文件: ", FileReadOptions{ { u8"stl" }, u8"网格文件" });
```

### 2. 输入协议：一行一条答案，多个用 `;` 分隔

| 方面 | 单个 | 多个（`getFilesAsync` / `getDirsAsync`） |
| --- | --- | --- |
| 一次输入行 | 整行 = 一个路径（**不切分**） | 整行 = `;` 分隔的路径表 |
| 分段 | — | 每段去首尾空白；**空段忽略**（尾随 `;`、`;;` 无害）；每段各自去包裹引号 |
| 成功条件 | 校验通过 | 段数 ≥ 1 且每段都通过 |
| 失败 | GUI 重提示（**保留原文**）；无头打一行原因后 `nullopt` | 同左，报**第一条**不合法的原因 |
| 取消 | Escape / `cancelPendingInput()` / 无头无输入（EOF） | 同左 |
| 顺序与重复 | — | 保序、**不去重**（重复是调用方的事） |
| 提示文案 | 原文 | 实现补一句 `（多个用 ; 分隔）`（与 `请输入整数` 同类的框架文案），无头侧同样打 |

- **为什么是 `;`**：`;` 在 Windows 文件名里是非法字符，所以"分号分隔"在 Windows 上无歧义；
  路径复制粘贴的常见形态（成对引号）也容易逐段剥掉。
- **边界（认下来）**：POSIX 上 `;` 是合法文件名字符 —— 这种路径**在多值读里无法表达**（会被切成两段）。
  单值读不受影响（整行不切分），要读这种文件就用 `getFileAsync` / `getDirAsync`。
- **重提示保留原文**：一行里三条路径、第二条坏了，清空输入行等于让用户重打三遍。所以路径读的重提示要能把原文填回去 ——
  `ConsolePanel::beginInput(prompt, initial_text = {})`（给现有方法加一个默认参数，追加型改动），
  `repromptError()` 走"提示 + 原文"，用户改掉坏的那一段再回车即可。
- **列表读不需要新协议**：它就是"一次普通读 + 一次切分"，所以槽位、取消、行缓冲、`AsyncEvent` 全都不用动
  —— 这正是选 `;` 而不是"逐行输入到空行"的最大收益（见 §6）。没有终止符、没有"边读边收"的中间态、没有逐轮回显。

### 3. 校验（基类共享）

```cpp
  protected:
    struct PathCheck {
        bool                  ok{ false };
        std::filesystem::path path;
        String                message;  ///< 拒绝原因：GUI 重提示用，无头打这一行。
    };
    struct PathLineCheck {
        bool     ok{ false };
        PathList paths;
        String   message;
    };

    /// 整行当一个路径校验（单个读用）。
    static PathCheck     checkPath(const String& text, const FileReadOptions& options);
    static PathCheck     checkPath(const String& text, const DirReadOptions& options);
    /// 按 ';' 切分后逐段走 checkPath（多个读用）；第一段失败即返回。
    static PathLineCheck checkPathLine(const String& text, const FileReadOptions& options);
    static PathLineCheck checkPathLine(const String& text, const DirReadOptions& options);
    /// 选项里的后缀列表 → Qt 风格的 pattern 列表（{u8".stl"} ⇒ {u8"*.stl"}），对话框与文案共用同一份归一化。
    static std::vector<String> filterPatterns(const FileReadOptions& options);
    /// 这次读要什么 ⇒ 一句摘要（"（.stl .obj，多个用 ; 分隔）"），提示与悬浮文案共用。
    static String requirementSummary(const std::vector<String>& extensions, bool multiple);
    /// 把摘要补进提示文本："网格文件: " ⇒ "网格文件（.stl .obj）: "（摘要在末尾的分隔符之前）。
    static String promptWithRequirements(const String& prompt, const String& summary);

  private:
    /// 上面四个转发的共同实现（目录读借它，只是把 extensions 留空）。
    static PathCheck checkPath(const String& text, bool directory, const FileReadOptions& options);
```

判定顺序（先报最具体的原因）：

1. 去首尾空白；去掉成对的包裹引号（从资源管理器复制来的 `"C:\a b.stl"`）。空 ⇒ 拒绝。
2. `require_existing` ⇒ `std::filesystem::exists`；false 则跳过（"另存为"用）。
3. **存在**就核对类型：文件读要 `is_regular_file`、目录读要 `is_directory`（"另存为"给了一个已存在的目录同样算错；
   而尚不存在的路径只看名字，也就是第 4 条）。
4. `extensions`（仅文件读）非空 ⇒ 条目先**归一**（去前导 `*` 与 `.`、转小写），再与**文件名尾部**比对
   （不是 `path.extension()`：多段后缀 `tar.gz` 也写得出来），大小写不敏感；
   另有 `name.size() > ext.size() + 1` 一道：名字就叫 `.stl` 的点文件不算命中。

不做的：**绝对化/规范化**（不改写用户输入，相对路径由调用方按 cwd 解释，与仓库其它 loader 一致）、
可读性/权限检查（打开失败仍是调用方的事）、TOCTOU（检查是**建议性**的，文件随时可能消失）。

文案示例：`文件不存在: <path>`、`不是目录: <path>`、`后缀不支持（仅 .stl / .obj）`。

### 4. 两个后端的实现要点

**`ConsoleUserIO`** —— 每条读都是"`readLineAsync()` 一次 + 校验"：

- 单个：`readLineAsync(提示 + 要求摘要)` ⇒ `checkPath()` ⇒ 通过给路径；不通过打 `message` 后 `nullopt`。
- 多个：同一条路，只是 `checkPathLine()` 一次给出整张列表。
- 槽位协议（`beginRead()`/`ReadScope`）、行缓冲、跨线程取消**一行都不用改** —— 这是"一行一条答案"换来的。

**`VisualUserIO`**：

- `PendingRead` 加四态：`File` / `Files` / `Dir` / `Dirs`；结果复用 `path_result` 与 `paths_result` 两个字段
  （应用线程写、`done` 之后读，与现有结果字段同一套 happens-before）。
- 一次读要显示/要挑什么，打成一个 `ReadUi`（提示文本、预填、面板动作、选项）由 entry point 造好传下来。
- **选项跟着 `prompt_state` 走**（`shared_ptr`，与提示文本同住）：读是命令恢复时所在线程发起的，而答案是应用线程
  解析的，所以选项**不能**是普通成员（那会变成两个线程读写同一个 `std::vector`）；放进共享状态后两边都在应用线程上。
- `parseAndComplete()` 走 `checkPath`/`checkPathLine`：通过 ⇒ 填结果 + `done.set()`；
  不通过 ⇒ 报 `message` 且**把原文填回输入行**（§2 的 `beginInput(prompt, initial_text)`，与整数读的原地重提示同款）。
- 一次读仍然只显示一次提示、只完成一次；取消路径不变（Escape / `cancelPendingInput()` ⇒ `cancelInteraction()`）。
- `endRead()`（每次读结束时都走）顺手把面板的输入动作清掉：按钮不属于面板的状态机，它属于**这一次读**，
  所以撤下与读的收尾在同一处发生（`console == nullptr` 时什么也不做）。

### 5. 输入行的路径交互（浏览 + 高亮；拖放**先不做**）

**一条原则**：手打与点击（浏览）都只是"更快地填那一行"。
所以校验与完成永远只有一条路径（输入行的 `lineEntered`），没有旁路，浏览也不可能绕过 `checkPath`。

pending 时的样子（布局就长这样，不新增行/不新增工具条）：

```
  ready> open_mesh
  网格文件（.stl .obj）:  D:\models\bracket.stl                    [📂]
  ══════════════════════════════════════════════════════════════════════
```

1. **行尾内嵌按钮**：`QLineEdit::addAction(icon, QLineEdit::TrailingPosition)` —— 图标在输入框**框内**，
   不加行高、不加第二行、不形成"表单感"；悬浮/按下反馈由 `QLineEdit` 自己给，不用自己写样式表。
   - 图标：`style()->standardIcon(QStyle::SP_FileIcon / SP_DirOpenIcon)`。appfw 自带**零图标资源**
     （`app_shell` 的 SVG 在插件里，框架够不着），`QStyle` 标准图标跟着平台与调色板走，暗/亮主题都不用管。
   - 悬浮文案（同时作无障碍名）：`选择文件…（.stl .obj，可多选）` —— 由实现拼好交给面板，面板只显示不解析。
2. **提示行带过滤器**：`网格文件（.stl .obj）: ` 由**实现**拼（无头侧打同一串），
   于是"支持什么格式"不只藏在 tooltip 里，键盘用户也看得见。
3. **pending 高亮（2px 下划线）**：颜色取 `ConsoleTheme::prompt`。**不给 `ConsoleTheme` 加字段** ——
   它是公开 SDK 结构体，加字段就是改布局（要 bump ABI 并重编全部插件），而"这次在等输入"用 prompt 色本来最贴切。
   读一结束就撤掉。
4. **拖放**（**先不做**，设计留档）：pending 期间把文件/文件夹拖到面板任意位置都收（输出区大，比只认输入行友好）。
   - 拖入时输入行显示虚线高亮（`setProperty` + `style()->polish()`），离开/落下即撤。
   - 松手只做**一件事**：把路径拼成一行写进输入行（与点击完全同一个动作）。
   - 不匹配的拖入**要说话**（`后缀不支持（仅 .stl / .obj）: <path>`），不做静默无事发生。
   - 坑：`QPlainTextEdit` 默认自己吃文本拖放（会把文件 URL 当文本插进去）⇒ 输出视图要显式 `setAcceptDrops(false)`，
     接受拖放装在 `Impl` 的根 widget 上（子控件不收时事件会冒泡上来）。
5. **键盘路径**：pending 期间 `Ctrl+O` 等价于点按钮（Qt 的行内 action 本身无法 Tab 聚焦，得另给键盘入口）。
6. **多选与追加语义**：`Files` / `Directories` 两次点选**追加**（中间补 `;`，行首不补）——
   "点两次 = 两个路径"，与手打 `a;b` 完全同构；`File` / `Directory` 是整行替换（单值读不接受两条）。
7. **读一结束就消失，过时结果作废**：实现下发 `kind = None`；而对话框是**嵌套模态事件循环**，期间投递的回调照跑，
   所以返回后先比"当前 kind 还是我开对话框时的那个吗"，不是就**丢掉结果** ——
   对话框选的路径绝不能落到一个已经结束、甚至已经变回"命令输入行"的输入行上。
   这个判断只是 `Impl` 内部一次局部比较，不跨对象、不碰 `pending`。
8. **分隔符归一**：对话框在 Windows 上给 `C:/a/b.stl`（Qt 的内部拼法）⇒ 写进输入行之前过一遍
   `QDir::toNativeSeparators()`，让控制台里看到的是平台自己的写法（`std::filesystem` 两种都吃，所以这只是"看起来对"）。
9. **含 `;` 的路径**：`;` 是分隔符，所以这种路径在这一行里无法表达 ⇒ 打一行警告并跳过（Windows 上不可能出现；
   POSIX 上要读它就用手打单值读，那里整行不切分）。

**面板 API**（只描述"挑什么"，不知道过滤/校验规则）：

```cpp
/// 输入行现在要挑什么：决定图标与对话框形状。
enum class ConsoleInputKind { None, File, Files, Directory, Directories };

struct ConsoleInputAction {
    ConsoleInputKind kind{ ConsoleInputKind::None };
    /// 已拼好的 Qt name filter（"网格文件 (*.stl *.obj);;所有文件 (*)"）；面板**原样**交给对话框，不解析。
    String           filter;
    /// 悬浮文案与无障碍名（"选择文件…（.stl .obj，可多选）"）。
    String           tooltip;
};

void ConsolePanel::setInputAction(const ConsoleInputAction& action);   // kind == None ⇒ 撤掉按钮与高亮
void ConsolePanel::beginInput(const String& prompt, const String& initial_text = {});
```

- 两处都**不动类布局**（状态住在 `Impl`，一个 `Signal` 成员都不加），所以 `VN_APPFW_PLUGIN_ABI_VERSION` 不用再 +1。
- **对话框归面板**（它是 GUI 层），**语义归实现**：面板拿到 filter 串只是当显示串用。

| 读 | 面板弹的对话框 |
| --- | --- |
| `File` | `getOpenFileName` |
| `Files` | `getOpenFileNames` |
| `Directory` | `getExistingDirectory` |
| `Directories` | **原生目录对话框选不了多个** ⇒ 非原生 `QFileDialog`（`FileMode::Directory` + `DontUseNativeDialog`）取 `selectedFiles()` |

- filter 串由实现拼：`filterPatterns(options)` ⇒ `支持的格式 (*.stl *.obj);;所有文件 (*)`（§1/§3）——
  标题是框架的话（调用方只说要什么格式），所以 `FileReadOptions` 里没有"过滤器标题"这个字段。
- **不做**：路径的 Tab 补全（控制台补全现在只认命令名，将来另立"路径补全"再谈）、
  **自动提交**（选完要按一次回车 —— 用户能看见将要提交的确切文本、还能再用 `;` 追加，
  而且"浏览"因此永远不绕过校验）、无头侧没有任何浏览面。

### 6. 备选与未采纳（留档）

- **"逐行输入、空行结束"的列表协议**：不做（用户 2026-10-10 定为 `;`）。理由：要在**一次读里循环**，
  `ConsoleUserIO` 得把槽位协议拆成"外层持槽 + 内层逐行"两层；GUI 侧要逐轮改提示、逐条回显、认两种终止符（空行与 EOF）。
  `;` 版把这些全变成"一次普通读 + 一次切分"。代价是 POSIX 上含 `;` 的路径多值读不了（单值读不受影响，§2）。
- **两个虚函数（`getPathAsync`/`getPathsAsync`）+ `kind` 参数 + 便利名**：不做。用户要求文件与文件夹接口分开；
  而且 `kind` 会让"目录读 + 后缀过滤"这种无意义组合存在于类型里（现在目录读的选项结构根本没有这个字段）。
  代价：四个槽位、两个后端各四个 override（各自转发到私有助手，不重复逻辑）。
- **`extensions` 写成 `String`（Qt filter 串）或 `map`**：不做。`String` 要把 `label (patterns);;…` 的显示语法
  下沉到 Qt-free 的基类（还得解析 `;;`/`;`/空格/`*`）；`map` 当"后缀 → 标签"会让对话框变成"一个后缀一组过滤器"
  并因按键排序丢掉"第一组默认选中"，当"标签 → 模式"则等于把微语法藏进值里。
- **只留一个 `filter` 字段（后缀从它自动得出）**：不做。两个字段本来就不是重复：`extensions` 是**规则**
  （每个实现都执行，无头侧没有选择器也照样拒错后缀），标题是**显示**。合成一条 Qt filter 串意味着 Qt-free 的基类
  要解析 `标题 (模式);;…`（`;;` / `;` / 空格 / `*` 全是语法，标题里还可能带括号），而两端要的东西本来就不同：
  校验要归一化后的后缀表（`.stl .obj`）、对话框要模式（`*.stl`）、错误文案要后缀表。
  要"少一个旋钮"就砍标题 —— 一个 `extensions` 加框架定的标题已经够用（本轮砍掉 `picker_label` 就是这个理由）。
- **对话框放在实现侧 + 给 `ConsolePanel` 加 `vn::Signal<> browsePressed`（本节早先的写法）**：不做。
  `Signal` 是 `ConsolePanel` 的**数据成员**，加一个就改 `sizeof(ConsolePanel)` —— 插件在 SDK 头里
  `new ConsolePanel(...)`，两边大小不一致就是 ABI 破坏，得 bump 8u 并重编全部插件；
  而"填行"方案让面板复用现成的 `lineEntered`，一个成员都不用加。
  第二层理由：旧方案里对话框结果要**绕过输入行**直接喂 `onLineEntered()`，于是"读是否已经结束"必须在实现侧再判一次；
  填行方案只剩面板内部一次 `kind` 比较，路径根本落不到错误的读上。
- **浏览后自动提交（省一次回车）**：不做。多按一次回车换来的是"提交前看得见确切文本 + 还能用 `;` 追加"，
  并且浏览与手打严格同构（§5 末）。
- **返回 `String` 而不是 `std::filesystem::path`**：不做（理由见 §1）。
- **把"取消"和"校验失败"分开（返回带状态的结果结构）**：不做。现有四个读一律 `optional`
  （`nullopt` = 取消或取值失败，调用方报 Cancelled），路径读跟着走；无头侧原因已打行，GUI 侧已重提示。
  要分就得把四个老读一起改，不是这一轮的事。
- **基类默认实现退化成 `getStringAsync()`（无校验地"能读"）**：不做。默认实现假装能读违反本模块
  `cancelPendingInput()` 立下的口径；默认给 `nullopt` + warning，谁支持谁 override。
- **`suggested_path` 之外的"相对基准目录"**：不做，调用方按 cwd 解释。

### 7. 工作分解与测试

| 工作项 | 状态 | 内容 |
| --- | --- | --- |
| W1 SDK | **已落地** | `UserIO::FileReadOptions` / `UserIO::DirReadOptions` / `UserIO::PathList` / 四个虚函数（默认实现 = warning + `nullopt`，追加在 vtable 末尾）/ `checkPath()`、`checkPathLine()`、`filterPatterns()`、`requirementSummary()`、`promptWithRequirements()` |
| W2 无头 | **已落地** | 四个 override = `readLineAsync` + 校验 + 原因行 + 取消（**槽位协议一行都没动**，所以才没走"逐行到空行"那条路） |
| W3 GUI | **已落地** | `PendingRead` 四态、`path_result`/`paths_result`、选项经 `prompt_state` 共享状态送到应用线程、重提示填回原文（`beginInput(prompt, initial_text)`） |
| W4 输入行交互 | **已落地（拖放除外）** | `ConsoleInputKind`/`ConsoleInputAction`/`setInputAction`、行尾内嵌按钮、`Ctrl+O`、pending 高亮、四种对话框与"填行/追加"、结束时撤销 + 过期结果作废、分隔符归一 |
| W5 首个调用点 | **已落地** | `model_viewer` 的 `open_mesh_file`：`getFileAsync` 问用户要一个网格文件，再让打开器用 `MeshLoader` 读回来。过滤后缀取自 `MeshLoader::supportedExtensions()`（为此把它从 `MeshLoader.cpp` 的匿名命名空间提成静态成员 —— 过滤器与"加载器真能读什么"只能有一个答案）。命令本体不在 `test_gui` 里编译（那是插件的类，编译进来会与沙箱加载插件时的元数据撞车），所以这条链的自动化覆盖到"路径读 + 打开器"为止，命令那 20 行胶水靠编译 + 插件真加载 + 手工点一次 |
| W6 文档 | **已落地** | 本节 + `.ai/memory/appfw.md` 的 UserIO 段 |

| 用例（`tests/test_gui/test_gui.cpp`） | 覆盖 |
| --- | --- |
| `UserIOTest.PathCheckTable` | 共享校验的表：后缀大小写、多段后缀、三种后缀写法、`require_existing=false`、已存在目录当目标文件、成对引号、空文本、目录读不吃后缀、`;` 切分与保序、提示/过滤器的拼法 |
| `UserIOTest.FileReadValidatesAndKeepsTheRejectedTextInTheLine` | 提示带后缀；拒绝 ⇒ 重提示且**原文还在输入行**，读仍在等；合法 ⇒ 原样给出 |
| `UserIOTest.MultipleFileReadTakesOneSemicolonSeparatedLine` | 一行两条 + 顺序 + 一次输入完成整条读 |
| `UserIOTest.InputActionIsOfferedOnlyWhileTheReadWaits` | 按钮的装/换（文件↔目录 图标与文案）/撤；命令行的普通状态没有按钮 |
| `UserIOTest.PickingFilesFillsTheLineWithoutSubmitting` | **端到端**：点按钮 ⇒ 真对话框（offscreen 上是 Qt 自己的，由用例一次性定时器关掉）⇒ 只填行不提交；再点一次 ⇒ `;` 追加；取消 ⇒ 两样都不动；回车 ⇒ 与手打同一条校验路径；读结束 ⇒ 按钮撤下 |
| `ConsoleUserIOReadTest.FileReadTakesAnExistingFileAndReportsWhyItRefuses` | 无头：提示带后缀、合法原样、后缀不符/不存在 ⇒ 一行原因 + `nullopt`（不重提示） |
| `ConsoleUserIOReadTest.MultiplePathsComeFromOneSemicolonSeparatedLine` | 无头：`;` 切分、空白段忽略、保序；一段坏 ⇒ 整条失败（不给部分成功） |
| `ConsoleUserIOReadTest.DirReadRejectsAFileAndAcceptsADirectory` | 目录读拒文件（"不是目录"）、文件读拒目录（"不是文件"） |

### 8. 不变量（新增 8–11）

8. 无头侧一次读**从不重提示**：非法输入只报一次原因，`nullopt` 随之而来（列表读也一样）。
9. 路径读只在**校验通过**时给出路径；`require_existing` 时它在判定时刻存在，此后可能消失（TOCTOU 由调用方承担）。
10. 手打与点击（浏览）都只**填那一行**：没有第二条提交路径，浏览也过 `checkPath`。
11. GUI 侧"浏览"只是**更快地输入**：重提示保留用户已打的原文；读结束时按钮与高亮一并撤销，过期对话框结果被丢弃。
