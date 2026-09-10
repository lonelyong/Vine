# appfw CommandManager 设计（2026-09-10 重写）

代码：`src/fw/appfw/sdk/vine/appfw/CommandManager.hpp`、`src/fw/appfw/src/CommandManager.cpp`；
`CommandFlags::Exclusive` 语义见 `src/fw/appfw/sdk/vine/appfw/Command.hpp`。
测试：`tests/test_gui/test_gui.cpp`（`CommandManager_*` 共 22 例）。

## 入口点语义（顶层 vs 嵌套）

四个公开入口全部是**顶层入口**（`executeCommandAsyncImpl` 传 `nested=false`）；只有
`CommandExecutionContext::executeChild()` 走 `nested=true`。

| 入口 | 链 | 前台归属 | 串联门 | 取消源 | 阻塞调用线程 |
| --- | --- | --- | --- | --- | --- |
| `executeCommand(Command*)` / `(name)` | 新建 | 成为前台链 | 受门 | 新源 | 是（`syncWait`） |
| `executeCommandAsync(Command*)` / `(name)` | 新建 | 成为前台链 | 受门 | 新源 | 否（惰性，await 后才跑） |
| `executeDetached(name)` | 新建 | 成为前台链 | 受门 | 新源 | 否（立即跑到首次挂起） |
| `context->executeChild(name)` | **共享父链** | 不变（同一条链） | **绕过** | **共享父链源** | 否（co_await） |

由此产生的实际后果（均已有用例固化）：

- **异步入口的链是“首次 await 时才建立”的**：`Task` 是惰性的（`initial_suspend = suspend_always`），
  所以 `cm->executeCommandAsync(...)` 只是拿到一个未启动的 task，**那一刻不建链、不抢前台**；
  真正 `co_await`（或 `syncWait`）时子协程才进入 `Impl::admit`，此时才新建链并写 `foreground`。
  丢掉不 await = 命令永不执行（帧被析构），也不会留下任何链。
- 在命令内部调顶层入口 = 开一条**独立链**，不是嵌套：它会**抢走前台**
  （`currentCommand()`/`runningCount()` 随后描述新的链，`cancelCurrent()`/Esc 也只能取消它），
  且 `maxChainDepth()`（64 层）**拦不住**这种写法的递归。
- **前台不会“恢复”**：子链跑完后 `foreground` 仍指向那条已 drain 的子链，
  于是父命令还在跑而 `currentCommand()` 返回 nullptr、`runningCount()` 返回 0
  （`cancelAll()` 仍能触及父链，因为它在活跃链注册表里）。
- 父命令是 `LongRunning`（持有门）时，内部调顶层入口会被拒（`Failed("Another operation is in progress")`）
  ⇒ 想嵌住跑子命令必须用 `executeChild()`。
- 被门拒绝现在会记一条 warning（`Command refused by the serialization gate`）：
  `executeDetached` 的调用方拿不到返回值，否则拒绝会完全无声。
- 命令内部**不要**用同步 `executeCommand()`：它阻塞当前线程，而该线程可能是 UI 线程或定时器线程
  （`syncWait` 自己的文档就警告：若该线程的事件循环是任务恢复所必需的，就会死锁）。
  命令内部应 `co_await executeCommandAsync(...)`，或直接用 `executeChild()`。

## 最佳实践整理（第三轮）

1. **日志永不改变行为**：三个级别共用一个 `logNoThrow`（`logInfoNoThrow`/`logWarnNoThrow`/`logErrorNoThrow` 是薄封装），
   调用点传 `std::string_view`（`toUtf8View`，零分配），消息拼装在 noexcept 边界**内部**完成。
   原因：这些调用位于 catch 块、detached 协程（抛出即 `std::terminate`）和命令执行途中，
   之前的 `toUtf8(x) + ": " + e.what()` 会在调用方侧分配，一旦失败就会穿出 noexcept 边界。
2. **异常消息有兜底**：`messageFromException()`（noexcept，失败返空串）+ `failureFromException()`
   在空 `what()` 时填充 `command threw an exception`，UI 不再可能拿到空失败消息。
3. **`snapshot_handler` 纳入锁保护**（修复上一轮遗漏的数据竞争）：锁内取副本、锁外调用（用户代码）。
4. **历史记录改为尽力而为**：写历史失败（如分配失败）只记日志，不得把已成功的命令改成 Failed。
5. **函数分解**：`Impl::takeOverForeground()`（接管等待）/ `Impl::admit()`（准入决策，含临界区）/ 
   `Impl::report()`（结果上报：日志 + executed 事件 + 历史）；协程体只剩主流程与守卫声明。
6. **`hasFlag(CommandFlags, CommandFlags)`** 取代散落的 `static_cast<std::uint32_t>(...)`（README 里的位标志运算）。
7. **头文件与接口**：std include 按字母序；`application()` 标 `noexcept`；
   `registerCommand<T>` 改用 `is_base_of_v` 并加 `is_default_constructible_v` 断言；
   `createCommandByName()` 加 `[[nodiscard]]`。
8. **`Impl` 加显式构造函数** `explicit Impl(Application*)`，消除聚合初始化漏字段警告并明确 `app` 的初始化时机。
9. **严格警告体检**：用 compile_commands 的真实参数 + `-Wall -Wextra -Wpedantic -Wshadow
   -Wnon-virtual-dtor -Woverloaded-virtual -fsyntax-only` 检查本 TU ⇒ **0 警告**。
   剩余仅是全仓 `V_OBJECT_META_DECL;` 多一个分号的 `-Wextra-semi`（宏末尾自带 `;`，且仓库里两种写法并存），
   要统一得单独一轮、全仓改，不在本模块范围（项目并未启用该警告）。

## 第二轮审查（12 点，逐条对照代码核实）

| # | 指控 | 核实结果 | 处理 |
| --- | --- | --- | --- |
| 1 | Exclusive 等待超时后静默放行 ⇒ 双链并发 | **真缺陷**：仅 `V_LOGW` 就继续执行，排他契约被破坏 | 改为 **Fail-Safe 拒绝**：`Failed("Another operation is still stopping")` + 错误日志 |
| 2 | 串联门 `ProgressHost::current()` TOCTOU | **真缺陷**：检查与占用分离，两个顶层长任务可同时通过 | 门检查 + `foreground` 赋值 + 占用标志写入同一临界区 |
| 3 | 链式别名只解析一层 | **真**（功能性缺陷，且环路会死循环） | `resolveName` 迭代解析 + visited 防环；列举按最终目标挂别名 |
| 4 | `executeDetached` 异常导致 `std::terminate` | **真缺陷**（已核实 `DetachedTask::promise_type::unhandled_exception()` 无 handler 时直接 terminate；树内 `VisualUserIO::executeInput` 的 DetachedTask 无 try/catch） | 异常在**顶层入口收口**为 `Failed` 结果（含工厂、快照、事件回调），detached 包装再叠一层兵底 catch |
| 5 | history 无上限 | 真 | `std::deque` + `maxHistoryEntries()`（默认 1024），超出丢最旧 |
| 6 | detached 链被顶出前台后无法取消 | 真 | 新增活跃链注册表 + `cancelAll()`（`cancelCurrent()` 语义不变） |
| 7 | `syncWait` 在协程内可能死锁 | **无法确认为框架缺陷**：`syncWait` 在调用线程上驱动任务，`sleepFor` 在独立定时器线程恢复，因此嵌套同步调用不会自锁（已用 `NestedSyncExecuteCommandDoesNotDeadlock` 验证）。真正的约束是语义性：命令内部嵌套必须走 `context->executeChild()`，否则开新链会被串联门拒绝 | 保留 `syncWait` 语义 + 用例验证 + 文档强调 `executeChild` |
| 8 | 嵌套无深度限制 | 真 | `maxChainDepth()`（默认 64），`executeChild` 超限返回 `Failed("Command nesting is too deep")` |
| 9 | probe 实例化有副作用/可能抛异常 | 部分真：副作用是设计选择（缓存元数据必须实例化一次）；**异常穿出确是真问题** | 探测在锁外 + try/catch；工厂抛/返回 nullptr 仍注册成功，仅无缓存元数据（插件加载期服务未就绪也能注册） |
| 10 | registry/aliases 无线程保护 | 真 | 新增 `registry_mutex`（叶子锁）；`registrationOwner()` 改为返回值（原来返引用，调用方零使用） |
| 11 | flags 缓存不一致 | 真（代码质量） | Undoable 判定改用缓存值 |
| 12 | 事件参数裸指针 | 已在第一轮处理（仅通知期间有效）；接口不变 | 文档已注明 |

## 第一轮审查

| 项 | 结论 | 处理 |
| --- | --- | --- |
| `history` 存 `Command*` | **真缺陷（内存安全）**：`executeCommandAsync(name)` 里命令是局部 `unique_ptr`，协程返回即析构，`historyAt()` 返回悬垂指针 | 改为值记录 `CommandHistoryEntry{name, command_class, result}`，`historyAt()` 返回 `std::optional` |
| `Exclusive` 先 `stack.clear()` 再替换 `stop_source` | **真缺陷**：旧链同时丢栈与取消源，`cancelCurrent()` 再也无法取消它；`stack.size()>1` 的取消传播判据被污染 | 见下"链模型"与"Exclusive 接管" |
| `commandInfos()` 每次列举都实例化命令；工厂返回 nullptr 即解引用崩溃 | **真缺陷** | 注册时探测一次并缓存 `group`/`description`；列举不再实例化 |
| `registerCommand()` 不校验空名/空工厂 | **真缺陷**（登记不可用条目） | 空名或空工厂直接拒绝并告警 |
| 别名挂接 O(n·m) | 真（性能） | 改为 name→index 表，O(n log n) |
| `currentCommand()`/`runningCount()`/`cancelCurrent()` 依赖单个全局栈 | **真缺陷（语义 + 并发）**：并发链互相污染，且读写无同步 | 改为"前台链"语义 + 互斥保护 |
| `CommandExecutedEventArgs::command()` 未说明有效窗口；`Exclusive` 文档与实现不符 | 真（文档） | 两处文档按实现改写 |
| "协程栈应显式建模"、"token 在源被替换后失效" | 真（架构） | 即链模型：源只 `request_stop()`，永不替换 |

## 不变量

1. **一条链 = 一个栈 + 一个取消源**：链内是嵌套关系（父命令 `co_await` 子命令），链间互相独立；
   任何一条链都不能清空、弹出或取消另一条链的成员。
2. **取消源只停不换**：`Chain::stop_source` 创建后只 `request_stop()`，因此 `Context` 发出去的
   `std::stop_token` 永远指向有效的源，不存在"令牌随源被替换而失效"。
3. **历史与命令生命周期解耦**：历史只保存值快照；命令对象在协程返回时即销毁。
4. **锁内不跑用户回调**：命令 `execute()`、工厂、快照回调、事件回调、命令虚函数（`name()`/`getType()`）
   都在所有锁之外；`drained.set()` 恢复等待者也在锁外。锁内只做容器操作与存储值的拷贝
   （`String`/`std::any` 的值拷贝，属数据而非回调）。
5. **链只有在自身帧彻底收尾后才报告 drained**：先弹栈项、再析构 `ProgressHost`，最后才
   `drained.set()`；门标志在信号前释放，所以被唤醒者看到的是干净状态。
6. **前台链是唯一**：`d->foreground` 指向最近一次顶层执行建立的链；`cancelCurrent()` 只作用于它，
   而 `cancelAll()` 作用于活跃链注册表。
7. **异常不外逃**：顶层入口（`executeCommandAsync(Command*)`、`executeCommandAsync(name)`）
   把一切逃逸异常（工厂、分配失败、命令体）转为 `Failed` 结果；快照回调抛错 ⇒ 命令不执行；
   `executing`/`executed` 回调抛错只记日志。原因是调用方是 UI 事件处理器与 `DetachedTask`
   （后者未捕获异常即 `std::terminate()`）。
8. **排他性不可静默丢失**：Exclusive 只要么接管成功，要么以 `Failed` 拒绝；
   不存在的状态是“两个链并发跑”。
9. **资源有界**：历史不超过 `maxHistoryEntries()`，嵌套深度不超过 `maxChainDepth()`。

## 结构

```
CommandManager ── Impl ─┬─ shared_ptr<Chain> foreground   (最近一次顶层链)
        │               ├─ vector<weak_ptr<Chain>> live_chains (供 cancelAll())
        │               ├─ bool foreground_busy           (串联门占用标志)
        │               ├─ deque<CommandHistoryEntry> history (值语义, 有上限)
        │               ├─ map<String, RegisteredCommand> registry (含缓存元数据)
        │               ├─ map<String, String> aliases
        │               ├─ mutable std::mutex mutex          (foreground/live_chains/busy/history/snapshot_handler)
        │               └─ mutable std::mutex registry_mutex (registry/owner/aliases, 叶子锁)
        └─ Context ── shared_ptr<Chain> chain_             (子命令共享父链)

Chain ── vector<Command*> commands    (链内栈，innermost 在尾, mutex 保护)
      ├─ int runs                     (未结束的运行数, mutex 保护)
      ├─ std::stop_source stop_source (只停不换)
      └─ AsyncEvent drained           (runs 归零时 set，供 Exclusive 有界等待)
```

- `Chain` 是 `CommandManager` 的私有嵌套类型，定义在 `.cpp`（`struct CommandManager::Chain`），
  因此头文件只前向声明 `struct Chain;`。
- 顶层入口（`executeCommandAsync(Command*)`、`executeCommandAsync(name)`、`executeDetached`）
  各自建立新链并写 `d->foreground`；`nested=true` 的分支使用 `Context` 传入的父链。
- 每个 `LongRunning` 命令（无论嵌套与否）仍各自持有 `ProgressHost`，绑定**本链**的 `stop_source`。

## 关键机制

- **Exclusive 接管**：顶层 Exclusive 命令先对 `foreground` 链 `request_stop()`，再
  `co_await Impl::waitDrained(*previous)`：等待是**协作式**的（挂起本协程而不是阻塞线程），
  因此被取消的链仍能在原线程继续收尾；等待有上界
  `CommandManager::exclusiveDrainTimeout()`（默认 2s）。
  超时 ⇒ 命令以 `Failed("Another operation is still stopping")` 拒绝，**不会**与旧链并发执行。
  接管成功则在自己的新链上运行并绕过串联门。
- **串联门（原子）**：决定“是否准入”与“占用门”在同一临界区（`d->mutex`）内完成，
  占用标志为 `foreground_busy`，由拿走它的那次运行的 `ChainGuard` 在析构时释放。
  门条件 = `foreground_busy || ProgressHost::current() != nullptr`（后者覆盖外部手动挂的前台宿主）。
  必须这么做的原因：`ProgressHost` 的宿主是在临界区之后才创建的，
  若“检查—占用”分离，两个线程会同时通过。
- **异常策略**：`command->execute()` 抛出的非取消异常在当前层转成 `Failed(what())` 并记入历史、
  触发 `executed`；`TaskCancelledException` 仍按嵌套规则上抛（链起点转为 `Cancelled`）。
  快照回调失败 ⇒ 不执行该 Undoable 命令（否则后续 undo 会回到不存在的状态）。
- **嵌套深度**：`Context::executeChild` 在 `chain_->stackSize() >= maxChainDepth()` 时拒绝并返回 `Failed`，
  自我递归命令最多 64 层。
- **历史容量**：`deque` 满 `maxHistoryEntries()` 时 `pop_front()`，`historyAt(0)` 始终是“仍保留的最早一条”。
- **活跃链注册表**：只在 `mutex` 下维护 `weak_ptr`（不延长链寿命），每次登记/`cancelAll()` 前
  顺带清理已结束项，因此注册表大小 = 活链数。
- **取消传播**：`catch (TaskCancelledException)` 里用 `chain->stackSize() > 1` 判定"我是嵌套子命令"，
  是则 `throw` 继续上抛，否则（链的起点）记录 `Cancelled`。判据取自**本链**，不再被其他链污染。
- **`cancelCurrent()`**：只对 `foreground` 链 `request_stop()`；对已收尾的链再请求是无害的
  （源只停不换，不会影响下一条链的新源）。
- **`currentCommand()` / `runningCount()`**：读前台链的 innermost/深度（加锁），
  用于 UI 展示"用户正在交互的那条链"。
- **注册元数据**：`registerCommand()` 调一次工厂探测以缓存 `group`/`description`；
  工厂返回 nullptr 只告警，条目仍可注册（执行时得到 `Failed`）。

## 必须由调用方保证

- 命令实例的生命周期由调用方持有（`Command*` 重载）；manager 不接管所有权，
  但**历史不再保存该指针**，所以执行返回后即可安全销毁。
- `CommandExecutedEventArgs::command()` 只在通知期间有效（同步触发）。
- 嵌套必须走 `context->executeChild()`：manager 无法区分"嵌套"与"新的顶层执行"。
- 注册工厂会在注册时被调用一次（元数据探测）；它应当廉价、可重入（可能被并发调用），
  但不要求无副作用：探测失败只影响列举元数据。
- 命令运行时不得忽略取消令牌又长时间持有共享状态：框架只能在“拒绝新命令”与“并发执行”中选一个，
  当前选前者（排他性优先）。

## 测试映射（tests/test_gui/test_gui.cpp）

- `CommandManager_HistoryRecordsValueSnapshots`：值快照、越界 `nullopt`、取消注册后仍可读
  （该用例会解引用历史条目里保存的命令信息，旧实现保存的是已析构命令的地址）。
- `CommandManager_ExclusiveTakesOverRunningChain`：合作型旧链被取消且收尾、新命令成功、无前台残留。
- `CommandManager_ExclusiveIsRejectedWhenChainIgnoresCancellation`：旧链无视取消 ⇒ 排他命令
  在上界后返回 `Failed`，探针命令证明它从未与旧链重叠；旧链自己跑完 Success。
- `CommandManager_CancelCurrentStopsOnlyForegroundChain`：取消前台链不影响另一条并发链
  （旧实现共享 `stop_source`，会连带取消）。
- `CommandManager_CancelAllReachesBackgroundChains`：`cancelCurrent()` 够不到的 detached 链由
  `cancelAll()` 收掉。
- `CommandManager_GateAdmitsAtMostOneTopLevelLongRunningCommand`：门被占用时第二个顶层命令被拒；
  两线程同时抢门恰有一个成功（旧实现可能两个都通过）。
- `CommandManager_RegisterValidatesAndCachesMetadata`：空名/空工厂被拒、重名被拒（且不做多余探测）、
  `commandInfos()` 不再实例化命令。
- `CommandManager_ThrowingFactoryDoesNotEscape`：工厂抛异常时注册仍成功、列举/执行都不抛穿。
- `CommandManager_NullFactoryIsTolerated`：工厂返回 nullptr 时列举与执行都不崩溃。
- `CommandManager_CommandInfosAttachesAliases`：别名挂到目标条目、结果有序、目标不存在时忽略。
- `CommandManager_ResolvesAliasChains`：别名→别名→命令可解析；环路不死循环。
- `CommandManager_RejectsTooDeepNesting`：自我递归命令在 `maxChainDepth()` 层被拒。
- `CommandManager_HistoryIsBounded`：超出上限丢最旧。
- `CommandManager_CommandExceptionBecomesFailedResult`：命令挂起后抛异常 ⇒ 同步与 detached 两条路径
  都得到 `Failed` 且进程存活。
- `CommandManager_RegistrySurvivesConcurrentAccess`：两线程并发注册/注销/列举/执行。
- `CommandManager_TopLevelCallInsideLongRunningParentIsRefused`：LongRunning 父命令内部调顶层入口
  被门拒绝（子结果为 `Failed`）；门随父命令结束释放。
- `CommandManager_AsyncTopLevelCallInsideCommandOpensNewChain`：命令内部 `co_await` 异步顶层入口 ⇒
  子命令在新链上运行（深度 1）、运行期间它就是前台链；子链结束后前台**不恢复**成父链
  （父命令仍在跑但 `currentCommand()` 为 nullptr、`runningCount()` 为 0）。
- `CommandManager_NestedSyncExecuteCommandDoesNotDeadlock`：命令/协程内同步调 `executeCommand` 不自锁（但仍建议用 `executeChild()`）。
- `CommandManager_UndoableSnapshotHandlerRunsBeforeExecution`：快照先于命令体；快照抛异常 ⇒ 命令不执行且返回 Failed；
  清空回调后不再被调用。
- `CommandManager_CommandExceptionBecomesFailedResult` 内含空 `what()` 的兑底消息验证。
- 既有用例（`RegistrationOwner`、`LongRunningCreatesAmbientHost`、`BusyGateRejectsNewCommand`、
  `NestedProgressRunsChild`）全部保持通过。

## 有意**未做**的最佳实践项（留档）

- **合并 `Context` 与 `CommandExecutionContext`（用 friend）**：不合理，理由见下节。
- **`Impl` 改用 `make_unique`**：仓库的 PImpl 风格是 `d(new Impl(...))`（`ConfigRegistry`/`ThreadPool`/`DynamicLibrary` 等一致）。
- **`V_DISABLE_COPY_MOVE(CommandManager)`**：`unique_ptr<Impl>` 已隐式删除拷贝/移动，appfw 同类类多数不用该宏。
- **公共 bool 接口加 `[[nodiscard]]`**：appfw SDK 无先例，且插件侧存在故意忽略返回值的注册调用。
- **给加锁的访问器标 `noexcept`**：`std::mutex::lock` 理论上会抛（仅无锁的 `application()` 标了）。
- **私有嵌套类标 `final`**：外部本就无法继承，无收益。

## 约定：类级常量统一为 `static constexpr` 函数

本模块与 `EventBus` 的类级常量统一采用**小写 camelCase 的 `static constexpr` 访问器**：
`CommandManager::exclusiveDrainTimeout()` / `maxHistoryEntries()` / `maxChainDepth()`、
`EventBus::gracefulShutdownTimeout()`。

选择该形态的理由：

1. 调用点读起来像 API，而不是一个可 `odr-use`、可取地址的公开数据实体（导出类里尤其不该）。
2. 保留演化能力：将来若要改成实例级/可配置（例如从 Config 读超时），只需去掉 `static`，
   调用形状（`x.exclusiveDrainTimeout()`）不变；数据成员形态则连名字都得改。
3. 两种形态无法同名：类静态**数据**成员按约定是 `s_snake_case`，静态**成员函数**按约定必须小写 camelCase。
   既然选定函数形态，就不要再改回数据成员（否则调用点全部要再改一遍）。
4. `constexpr` 访问器仍是常量表达式，可用于默认参数（`shutdownGracefully()`）、数组边界、`static_assert` 等。

注：base 层的 `String::npos`（模仿 `std::string`）等既有数据成员常量属于 stdlib 镜像，不在此约定范围内。

## 已评估但**未采纳**（留档，避免重复建议）

- **Exclusive 无限等待旧链**：会与"命令可能在被单线程执行器驱动的主线程上启动"冲突，
  存在死锁风险；采用有界等待 + 超时拒绝。
- **`historyAt()` 返回 `Command*`（旧行为）**：任何返回裸指针的方案都要求 manager 延长命令生命周期，
  与"命令执行完即销毁"的现有所有权模型冲突，故改为值语义。
- **资源租约（Lease）/零信任 `CommandExecutionContext`**：需要把 `application()->...`、文档/撤销栈/进度
  全部改成必须经 Context 门禁，属于对所有现有命令的破坏性 API 变更，且 appfw 并不拥有文档层；
  另外对“纯 CPU 且不访问任何 Context 的死循环”无效（租约只能在资源访问点绊线）。当前不采用。
- **影子写入 / 事务性命令执行**：需要文档层支持（appfw 外），且会改变所有命令的写路径。当前不采用。
- **OS 级强制终止（`pthread_cancel`/`TerminateThread`/信号注入 unwind）**：会跳过析构、破坏
  互斥与 STL 内部状态，且 C++ 标准下属未定义行为。明确不做。
- **“校验频率自适应”系列课题**：前置依赖资源租约层，该层未采纳，因此当前无实现对象。
- **注册表不加锁**：已改为加锁（`registry_mutex`，叶子锁，锁内不调用户代码）。
