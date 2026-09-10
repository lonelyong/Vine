# appfw UserIO 设计（2026-09-11 审查轮）

代码：`src/fw/appfw/sdk/vine/appfw/UserIO.hpp` + `src/fw/appfw/src/UserIO.cpp`（基类），
`src/fw/appfw/src/ConsoleUserIO.hpp/.cpp`（无头）与 `src/fw/appfw/src/gui/VisualUserIO.hpp/.cpp`（GUI），
宿主接线在 `Application`（`createUserIO()`/`setupUserIO()`、`ApplicationData::user_io`）、
`GuiApplication::setConsolePanel()` 与 `CommandManager`（`reportToUser()`、命令集事件）。
测试：`tests/test_gui/test_gui.cpp`（`UserIOTest.*` 5 例 + `GuiTest.CommandManager_PendingUserInput*`）。

## 结构

| 类 | 何时创建 | 输出 | 读入 |
| --- | --- | --- | --- |
| `UserIO`（抽象基类） | — | `putString`/`clear` | 四个 `getXxxAsync` |
| `ConsoleUserIO` | `Application::createUserIO()` 默认（无 GUI） | `std::cout`（互斥） | 后台读线程 + 行缓冲，`std::getline` 永不在等待者线程上 |
| `VisualUserIO` | `GuiApplication::createUserIO()` | `ConsolePanel`（编组） | `ConsolePanel` 的 `lineEntered`/`escapePressed` + `AsyncEvent` |

拥有者：`ApplicationData::user_io`（`unique_ptr`，先于 dispatcher 拆）；`Application::shutdown()` 先
`cancelPendingInput()` 再排空命令链。

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
- 提示文本、`currentPrompt_` 与结果字段都只在应用线程上读写（编组之后）——除了
  `cancelled_`/`pending_`，它们是原子的。

### 输出与列表

- `putString` 一行一条、`clear` 清屏；`ConsoleUserIO::clear()` 无条件输出 ANSI 序列（重定向到文件时
  会落进控制字符，留档不改）。
- `VisualUserIO` 的补全列表是快照：绑定控制台时刷新，并订阅 `CommandManager::commandsChanged`
  以跟上之后注册/卸载/改名的命令（旧实现只在绑定时刷一次，app_shell 之后加载的插件命令永远不进补全）。
- 重新绑定面板会先摘掉旧面板上的两个 handler（旧实现每次都挂新的，同一个面板绑两次 ⇒ 一行输入
  被执行两次）。

## 本轮审查（11 项）

| 编号 | 缺陷 | 证据 | 修复 |
| --- | --- | --- | --- |
| U1 | 输出路径不编组：`VisualUserIO::putString` 直接写 QWidget | `ConsolePanel::append` 不做编组，而 `CommandManager::reportToUser` 与 `appendOnApplicationThread` 都编组；app_shell 的命令全部 `io->putString(...)` | 编组下沉到 `putString`/`clear`（`onConsolePanel`），并在 `UserIO` 里写明"任意线程可调用、实现负责编组" |
| U2 | `getIntAsync` 返回 `int8_t` 且两个实现无检查地窄化 | `static_cast<int8_t>(1000)` = -24 | 签名改为 `int`（无调用方），解析改走 `UserIO::parseInt`；`V_APPFW_PLUGIN_ABI_VERSION` 1u → 2u，插件必须重编 |
| U3 | 并发读共享 `done_`/结果字段，互相踩 | 一份 `pending_`/`cancelled_`/四个结果字段 | 单交互槽位 + `beginRead` 的 CAS 拒绝 + warning，`UserIOTest.SecondReadIsRefusedWhileOneIsPending` |
| U4 | `ConsoleUserIO` 阻塞 `std::getline` ⇒ `cancelPendingInput()` 无效，关机可能让命令恢复到已拆的管理器上 | 旧实现直接在等待者线程上 `getline` | 一个后台读线程 + 行缓冲 + 可唤醒的 `AsyncEvent`，取消后读取立即返回 |
| U5 | 补全列表不刷新：绑定 console 之后注册的命令进不了补全 | `refreshCompletion` 只在 `setConsolePanel`/`setCommandManager` 调用；app_shell 在自己的 `load()` 里绑定，命令在加载期注册 | 新增 `CommandManager::commandsChanged` 事件（注册/取消/启用开关/别名，均在锁外触发），`VisualUserIO` 订阅后编组刷新 |
| U6 | 交互状态无同步，而 `cancelPendingInput()` 可能来自别的线程 | `cancelled_`/`pending_` 是普通成员 | 两者改 `std::atomic`；`set()` 待在两个锁之外调用，避免"被唤醒的读又要拿锁"而死锁 |
| U7 | 重复绑定面板会重复挂 handler → 一行输入执行两次 | `setConsolePanel` 只 `addHandler` | 保存 `HandlerId`，重绑时先移除；`UserIOTest.RebindingTheConsoleDoesNotRunALineTwice` |
| U8 | `GuiApplication::setConsolePanel` 用 `static_cast<VisualUserIO*>` | 子类换 `createUserIO()` 即 UB | 改 `obj_cast<VisualUserIO>` |
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

## 测试映射（tests/test_gui/test_gui.cpp）

| 用例 | 覆盖 |
| --- | --- |
| `UserIOTest.OutputFromAWorkerThreadIsMarshalledToTheApplicationThread` | U1 |
| `UserIOTest.SecondReadIsRefusedWhileOneIsPending` | U3 |
| `UserIOTest.IntReadKeepsItsValueAndRepromptsOnOverflow` | U2（1000 原样、超出 `int` 重新提示） |
| `UserIOTest.RebindingTheConsoleDoesNotRunALineTwice` | U7 |
| `UserIOTest.CommandsChangedReportsRegistryAndAliasEdits` | U5 的机制 |
| `GuiTest.CommandManager_PendingUserInputBlocksDrainUntilCancelled` | 取消与排空（原有） |
| `GuiTest.CommandManager_DetachedFailureIsReportedOnTheApplicationThread` | 失败上报编组（原有） |
| `test_core String.NumericConversions` | `String::toInt` 范围检查 |

## 已评估但**未采纳**（留档）

- **不给 `ConsolePanel` 加"当前补全条目"的 getter**：为测试方便扩公共 API 不划算，U5 以机制测试
  （`commandsChanged`）+ 绑定路径覆盖，弹窗行为留给人工检查。
- **不把 `ConsoleUserIO` 的读改成非阻塞 `poll`**：跨平台（Windows 管道/控制台）成本远高于收益，
  单后台线程 + 行缓冲已经满足"可取消"和"不丢行"两条。
- **不为 `ConsoleUserIO::clear()` 加 TTY 判断**：需要 `isatty`/`_isatty` 的平台分支，留档（重定向时
  会出现 ANSI 控制字符）。
- **不在 `UserIO` 上加读取"存储类型"之类的访问器**：本轮的 `parseInt` 是唯一需要共享的解析逻辑。
