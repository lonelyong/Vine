# appfw CommandManager 设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：命令注册/执行链/历史/禁用/结果状态；对话框与输入输出见 `appfw-userio.md`。
> **权威来源**：`CommandManager.hpp`、`Command.hpp`、`tests/test_gui`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

命令有四种入口（异步/同步 × 按名字/按实例），还要支持嵌套子命令、取消、排他、历史、禁用与
“命令不适用”。过去同步入口会阻塞 UI 线程、排他只取消前台链、历史保存命令指针导致 UAF、
`Failed` 与 `NotApplicable` 混为一谈。

### 1.2 目标

- 一条命令链 = 一个栈 + 一个取消源；链间独立。
- 顶层入口统一收口异常，不让异常穿到 UI/DetachedTask。
- 排他命令要么接管成功、要么明确拒绝；不与旧链并发。
- 历史只存值快照；禁用是注册表标记，不是移除。
- `Failed`（错误）与 `NotApplicable`（当下没对象）分开。

### 1.3 非目标

- 不做运行期热卸载命令；禁用只是标记。
- 不让命令内部用同步 `executeCommandAndWait()`（会死锁）。
- 不在框架里按文档类型过滤 ribbon。
- 不做 `Scheduler` 类抽取；调度逻辑留在 `Impl`。

### 1.4 一句话结论

`CommandManager` 用一条链管一次顶层执行：链内嵌套、链间独立；顶层入口统一把异常转成 `Failed`；
排他先停所有活链再接管；历史存值；禁用是标记；`NotApplicable` 不是错误。

## 2. 设计要点

2.0 **命令声明它需要哪一层，框架统一拒绝**：应用 / 文档 / 视图三层都可空，命令用 `CommandFlags::NeedsDocument` / `NeedsView` 声明前提，派发前统一拒绝（`NotApplicable` + 说清缺哪层），命令体读 `CommandExecutionContext::target()` 而不自己判空；控制台与无 GUI 测试是合法情形。目标在**派发时**解析（`gui::resolveCommandTarget()`：当前文档视图 → 它的文档，视图只在有 3D 区时进目标），随执行走，见 `../decisions/ADR-2026-10-24-command-target.md`。

### 2.1 四个公开入口

全部是**顶层入口**（`nested=false`）；只有 `CommandExecutionContext::executeChild()` 走 `nested=true`。

| 入口 | 用途 | 线程/阻塞 |
| --- | --- | --- |
| `executeCommandAsync(Command*)` | 顶层异步 | 立即返回 `Task` |
| `executeCommandAsync(name)` | 顶层异步按名字 | 立即返回 `Task` |
| `executeCommandAndWait(...)` | 工具/测试 | 阻塞当前线程 |
| `executeDetached(...)` | ribbon/fire-and-forget | 不等人等结果 |

命令内部**不要**用同步入口；应 `co_await executeCommandAsync(...)` 或 `executeChild()`。

### 2.2 链模型

```
CommandManager ── Impl ─┬─ shared_ptr<Chain> foreground
                        ├─ vector<weak_ptr<Chain>> live_chains
                        ├─ bool foreground_busy
                        ├─ deque<CommandHistoryEntry> history
                        ├─ map<String, RegisteredCommand> registry
                        ├─ map<String, String> aliases
                        ├─ mutex (foreground/live/history/snapshot)
                        └─ registry_mutex (registry/owner/aliases, 叶子锁)
Chain ── vector<Command*> commands (栈, innermost 在尾)
      ├─ int runs (归零 = 本链收尾)
      └─ stop_source (只停不换)
```

- 一条链 = 一个栈 + 一个取消源；链间不能清空/弹出/取消彼此。
- 取消源只 `request_stop()`，不替换 ⇒ `Context` 发出的 `stop_token` 永远有效。
- 链只有在自身帧彻底收尾后 `runs` 才归零（先弹栈、析构 `ProgressHost`，最后 `leaveChain()`）。

### 2.3 串联门与排他

- 串联门：`foreground_busy || ProgressHost::current() != nullptr`；检查与占用在同一临界区，
  由 `ChainGuard` 析构释放。
- 排他：顶层 Exclusive 先取活链快照，对**每一条**活链 `request_stop()`，再 5ms 切片轮询等待
  排空（上界 2s）。成功则绕过串联门；超时 ⇒ `Failed`，不与旧链并发。
- `cancelAll()` 自增 `cancel_generation`，等待中的接管在下一个轮询点以 `Cancelled` 收尾。
- 已知残余：排他是“启动时刻清场”，不是入场锁；运行期被准入的普通顶层命令仍可能与排他并存。

### 2.4 线程安全

- 命令 `execute()`、工厂、快照回调、事件回调、命令虚函数都在锁外。
- `ProgressHost::current()` 在 `mutex` 之外采样。
- `Signal` 本体是线程安全快照（不可变表 + 原子发布）；订阅/发火可并发。
- 禁用偏好的读-改-写不是原子的（只有 UI 线程调；`ConfigManager` 自身加锁），有意不加互斥。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 顶层执行 | 四个入口建新链、写 `foreground`、统一收口异常 | 已落地 |
| F2 | 嵌套子命令 | `executeChild()` 入父链，深度上限 64 | 已落地 |
| F3 | 结果状态 | `Success` / `Failed` / `Cancelled` / `NotApplicable` | 已落地 |
| F4 | 取消/排空 | `cancelCurrent` / `cancelAll` / `cancelAllAndWait` | 已落地 |
| F5 | 历史 | 值快照，容量上限 1024，不存结果载荷 | 已落地 |
| F6 | 禁用 | 注册表标记 + 持久化偏好 + 两个入口把关 | 已落地 |
| F7 | 命令管理器对话框 | 列表/状态/禁用/启用 | 已落地 |

### 3.1.1 F1 顶层执行

- **行为**：四个入口建新链、写 `foreground`；`nested=true` 用父链。
- **约束**（C1）：顶层入口把一切逃逸异常（工厂/分配/命令体）转为 `Failed`。
- **要求**（R1）：快照回调抛错 ⇒ 命令不执行；`executing`/`executed` 回调抛错只记日志。
- **验收**（T1）：`CommandManager_*` 顶层执行用例。
- **注意**（N1）：`DetachedTask` 未捕获异常即 `terminate`，所以必须收口。

### 3.1.2 F2 嵌套子命令

- **行为**：`context->executeChild(name)` 或 `executeChild(unique_ptr<Command>)`；入父链。
- **约束**（C2）：`stackSize() >= maxChainDepth()` 拒绝并返回 `Failed`；实例重载拒 `nullptr` 与禁用实例。
- **要求**（R2）：子命令以自己报告的名字进事件/历史；未注册则不出现在 `commandInfos()`。
- **验收**（T2）：`CommandManager_ExecuteChildRunsACallerSuppliedInstance` 等。
- **注意**（N2）：嵌套必须走 `executeChild()`；manager 无法区分“嵌套”与“新顶层”。

### 3.1.3 F3 结果状态

- **行为**：`Success` / `Failed` / `Cancelled` / `NotApplicable`。
- **约束**（C3）：`NotApplicable` 不是失败；`succeeded()` 仍是 `status == Success`。
- **要求**（R3）：三处用户可见路径分开：`logOutcome()` info、`executeDetached()` 转达、控制台 Warning。
- **验收**（T3）：`CommandManager_DetachedNotApplicableIsExplainedAndNotReportedAsAFailure`、
  `UserIOTest.NotApplicableTypedInTheConsoleIsAHintNotAnError`。
- **注意**（N3）：不要用 `setCommandEnabled()` 表达“不适用”（那是持久化偏好）。

### 3.1.4 F4 取消/排空

- **行为**：`cancelCurrent()` 只停前台链；`cancelAll()` 停所有活链；`cancelAllAndWait(timeout)` 等排空。
- **约束**（C4）：链的 `runs` 归零前完成弹栈与宿主析构；等待用 5ms 轮询，不用 `AsyncEvent`。
- **要求**（R4）：`Application::shutdown()` 先 `UserIO::cancelPendingInput()` 再 `cancelAllAndWait()`。
- **验收**（T4）：`ExclusiveStopsEveryLiveChain`、`CancelAllAbortsWaitingTakeOver`。
- **注意**（N4）：命令不能在自己内部调 `cancelAllAndWait()`；要停应用用 `Application::quit()`。

### 3.1.5 F5 历史

- **行为**：`deque` 满 `maxHistoryEntries()`（1024）时 `pop_front()`；`historyAt(0)` 是最早保留的一条。
- **约束**（C5）：历史只存值快照；不存 `CommandResult::data()` 的 `std::any`。
- **要求**（R5）：命令对象协程返回即可销毁；历史不保存指针。
- **验收**（T5）：`CommandManager_*History*` 用例。
- **注意**（N5）：条目数有界时字节数才会随之有界。

### 3.1.6 F6 禁用

- **行为**：`setCommandEnabled()` 先 `resolveName()`，写偏好 + 翻 `RegisteredCommand::enabled`。
- **约束**（C6）：禁用是标记，不移除；禁用项仍在 `names()`/`commandInfos()`。
- **要求**（R6）：按名字与按实例两个入口都把关；`isRegistered()` 只查存在性。
- **验收**（T6）：`CommandManager_DisableIsAFlagAndIsPersisted`、
  `CommandManager_DisableBlocksCallerSuppliedInstance`。
- **注意**（N6）：控制台补全跳过禁用命令；`list_commands` 行尾加 `[已禁用]`。

### 3.1.7 F7 命令管理器对话框

- **行为**：筛选框 → 命令表（名称/状态/别名/来源/分组/描述）→ 刷新/禁用/关闭。
- **约束**（C7）：禁用行灰显斜体，不隐藏；未选中时按钮置灰。
- **要求**（R7）：对话框不提供“卸载命令”；禁用是标记。
- **验收**（T7）：`CommandManagerDialogTest`。
- **注意**（N7）：表格样式与插件管理器共用 `TableStyle.hpp`。

## 4. 公开接口

### 4.1 `CommandManager`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `executeCommandAsync(Command*)` / `(name)` | 顶层异步 | 异常转 `Failed` |
| `executeCommandAndWait(...)` | 阻塞 | 工具/测试 |
| `executeDetached(...)` | fire-and-forget | 失败写 `UserIO` |
| `cancelCurrent()` / `cancelAll()` / `cancelAllAndWait(timeout)` | 取消 | 链独立 |
| `registerCommand<T>()` / `unregisterCommand()` | 注册 | 工厂探测一次 |
| `setCommandEnabled()` / `isCommandEnabled()` / `disabledCommands()` | 禁用 | 持久化 |
| `names()` / `commandInfos()` / `isRegistered()` | 列举/存在性 | 禁用项仍列出 |
| `historySize()` / `historyAt()` / `clearHistory()` | 历史 | 值快照 |

### 4.2 `Command` / `CommandExecutionContext`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `execute(CommandExecutionContext&)` | 命令体 | 协程 |
| `flags()` / `name()` / `group()` | 元数据 | 锁外调用 |
| `executeChild(name)` / `executeChild(unique_ptr<Command>)` | 嵌套 | 入父链 |
| `stopToken()` / `isCancelled()` | 取消 | 只停不换 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 顶层入口把逃逸异常转为 `Failed`。 |
| C2 | 嵌套深度 ≤64；实例重载拒 `nullptr`/禁用实例。 |
| C3 | `NotApplicable` 不是失败；`succeeded()` 仍是 `Success`。 |
| C4 | 链 `runs` 归零前完成弹栈与宿主析构；等待用轮询。 |
| C5 | 历史只存值快照，不存结果载荷。 |
| C6 | 禁用是标记；按名字/按实例两个入口都把关。 |
| C7 | 对话框禁用行灰显，不隐藏。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 快照回调抛错 ⇒ 不执行；事件回调抛错只记日志。 |
| R2 | 子命令以自己报告的名字进事件/历史。 |
| R3 | `NotApplicable` 的日志/控制台/ribbon 三处都分开。 |
| R4 | `Application::shutdown()` 先取消挂起输入再排空命令链。 |
| R5 | 命令对象执行返回即可销毁；历史不保存指针。 |
| R6 | `isRegistered()` 只查存在性；能否执行问 `isCommandEnabled()`。 |
| R7 | 对话框不提供“卸载命令”。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 一条链 = 一个栈 + 一个取消源；链间独立。 |
| N2 | 取消源只停不换。 |
| N3 | 锁内不跑用户回调。 |
| N4 | 前台链唯一；`cancelCurrent()` 只作用于它。 |
| N5 | 排他要么接管成功，要么 `Failed`；不存在两个排他并发。 |
| N6 | 历史/嵌套深度有界。 |

## 6. 注意事项与陷阱

### 6.1 排他不是入场锁

- 会怎么错：排他启动后新准入的普通顶层命令与它并存。
- 怎么判：`ExclusiveStopsEveryLiveChain` 只覆盖启动时刻；运行期窗口仍在。
- 怎么防：需要“关门”状态才能彻底消除；属单独一轮 API 改造。

### 6.2 handler 内重入/取消

- 会怎么错：`executing`/`executed` 里再跑命令、注销刚跑完的命令、增删 handler。
- 怎么判：Signal 快照语义；新增的本轮不生效，注销的不再调用。
- 怎么防：通知在锁外；不要依赖“本轮新增立即生效”。

### 6.3 关停时活链

- 会怎么错：管理器析构时还有命令帧指向 `Impl`/`CommandManager` → UAF。
- 怎么判：`~CommandManager` 发现活链只告警。
- 怎么防：宿主先 `cancelAllAndWait()` 再销毁；`Application::shutdown()` 已如此。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 命令内部用同步 `executeCommandAndWait()` | 阻塞的线程可能是 UI/定时器线程，恢复需要它转循环 → 死锁。 | — |
| D2 | 排他只取消前台链 | 被顶出前台的 detached 链仍在跑，会并发。 | — |
| D3 | 历史保存 `Command*` | 命令协程返回即析构，历史指针悬垂。 | — |
| D4 | 用 `setCommandEnabled()` 表达“不适用” | 用户偏好会被持久化；不适用是临时事实。 | — |
| D5 | 框架给命令带 `document_types` | 扩 `Command` 虚表/ABI；命令自己判断更准。 | — |
| D6 | `Scheduler` 类抽取 | 审计确认现状与不变量一致，抽取无收益。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 顶层执行/异常收口 | `CommandManager_*` |
| T2 | 嵌套/实例子命令 | `CommandManager_ExecuteChildRunsACallerSuppliedInstance` |
| T3 | `NotApplicable` 三处路径 | `DetachedNotApplicableIsExplained...`、`NotApplicableTypedInTheConsole...` |
| T4 | 排他/取消/排空 | `ExclusiveStopsEveryLiveChain`、`CancelAllAbortsWaitingTakeOver` |
| T5 | 禁用/持久化/实例入口 | `DisableIsAFlagAndIsPersisted`、`DisableBlocksCallerSuppliedInstance` |
| T6 | 对话框 | `CommandManagerDialogTest` |

## 9. 状态与演进

### 9.1 当前状态

四个入口、链模型、排他、取消/排空、历史、禁用、`NotApplicable`、对话框均已落地。

### 9.2 已知缺口 / 待办

- 排他不是入场锁；运行期普通顶层命令仍可能与排他并存。
- 禁用偏好的读-改-写不是原子的；只有 UI 线程调，未加互斥。
- 命令禁用/卸载不改变注册表布局；插件重注册会带着偏好回来。

### 9.3 兼容性 / 迁移

`getIntAsync` 等 UserIO 迁移不在本文；`CommandStatus` 增加 `NotApplicable`，调用方要按状态区分“错误”与“不适用”。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/CommandManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/Command.hpp`
- `src/fw/appfw/src/CommandManager.cpp`
- `src/fw/appfw/src/gui/CommandManagerDialog.cpp`
- `tests/test_gui/test_gui.cpp`

### 10.2 文档与 ADR

- `appfw-userio.md`
- `appfw-eventbus.md`
- `../backlog/`

### 10.3 外部资料

- `Signal` 的线程安全快照语义。
- `ProgressHost` 的前台宿主语义。
