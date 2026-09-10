# appfw CommandManager 设计（2026-09-10 重写）

代码：`src/fw/appfw/sdk/vine/appfw/CommandManager.hpp`、`src/fw/appfw/src/CommandManager.cpp`；
`CommandFlags::Exclusive` 语义见 `src/fw/appfw/sdk/vine/appfw/Command.hpp`。
测试：`tests/test_gui/test_gui.cpp`（`CommandManager_*` 共 36 例）。

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

1. **日志不改变行为，且不靠本地封装保证**：调用点直接写裸 `V_LOGI/W/E`，参数一律传
   `std::string_view`（`toUtf8View`，调用侧零分配）；“日志失败不会影响命令”由 **Logging 模块的契约**
   承担——`Logger` 的级别函数/`log()`/`defaultLogger()` 全部 `noexcept`，失败经
   `reportLoggingFailure()` 一次性报告到 stderr（见 `logging-module` 内存笔记）。
   因此本模块早期的 `logNoThrow`/`logInfoNoThrow`/`logWarnNoThrow`/`logErrorNoThrow` 封装族与
   `LogSeverity` 已删除（当初加它们是因为当时日志会抛：`vformat` 的 `bad_alloc`、
   `defaultLogger()` 首次构造、spdlog 对非 `std::exception` 的 sink 异常会重抛）。
   约定：**noexcept 路径上不要在建参数时做分配/拼接**（这正是仍保留 `toUtf8View` 的原因）。
2. **异常消息有兜底**：`failedResultFromException()` 内部 try 拷贝 `what()`，失败或空串时用
   `command threw an exception` 兜底，UI 不再可能拿到空失败消息。
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

## 第七轮：内部结构重构（2026-09-11，行为不变）

目标：简洁、去重、贴 C++20 最佳实践；公开 API 与行为不变（111 个 GUI 用例 + 117 async + 11 progress 全绿）。

1. **结果工厂**：`cancelledResult()` / `failedResult(String)` / `failureFromException()` 集中状态与消息，
   调用点不再散落 `CommandResult(CommandStatus::X, String(u8"..."))`；`messageFromException` 并入
   `failureFromException`（空 `what()` 的兜底只写一次）。
2. **单一异常收口** `failSafe(body)`：两个顶层异步入口共用一个协程包装，不变量 7 的策略只存在一处。
   协程 lambda 以**参数**形式传入（闭包被拷贝进协程帧），这正是 `async_global.hpp` 建议的安全形态。
3. **事件触发策略** `fireEvent(event, owner, args)`：`executing`/`executed` 共用"处理器抛错只记日志"，
   加上 `logOutcome()`；`Impl::report()` 因此只剩三步：`logOutcome` → `fireEvent(executed)` → `recordHistory`。
4. **去重**：`foregroundChain()`（三处"锁内取前台链"合一）、`Impl::cancelLiveChains()`（`cancelAll()` 与
   `cancelAllAndWait()` 共用的"停一切"原语，含代际自增）、`updateDisabledList()`（旧名 `applyPreference`，
   禁用列表增删只写一次）。
5. **可读性**：`ChainScope{TopLevel, Nested}` 取代 `bool nested`（布尔陷阱）；`Context` 去掉冗余的
   `Application*`（改从 manager 取）；统一使用 `std::ranges::` 算法；删掉未使用的 `<string>`。
6. **两处顺带修好**：
   - `executeCommandAsync(const String&)` 改为**非协程转发**到 `executeNamedCommand(String)`（旧名
     `executeRenamedCommand`）：
     名字在调用点就拷贝进任务，惰性任务不可能再读到调用方已销毁的临时量（原来 `cm->executeCommandAsync(String(...))`
     拿到的任务若稍后再 await 就是悬垂引用）。
   - `executeDetached()` 把"创建并启动任务"也纳入 try：启动期的分配失败原来会抛给 UI 调用方，
     与"fire-and-forget 不向调用方抛异常"的契约不符。

验证：本模块在 `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual` 下 **0 警告**
（剩余警告来自既有的 core/math 头）。

## 第六轮审查（重入 + 多线程，2026-09-11）

重入面：管理器调用的
用户代码有命令虚函数（`name`/`flags`/`group`/`description`/`getType`）、工厂、快照回调、
`executing`/`executed` 处理函数、`UserIO`；它们全部在所有锁之外被调用（`Impl::report` 连历史条目
都在锁外先建好）。多线程面：`mutex`（前台/活链/门/历史/快照回调）、`registry_mutex`（叶子）、
`Chain::mutex`、`cancel_generation`（原子），外加 ConfigManager 自己的读写锁。

| # | 情况 | 结论 | 处理 |
| --- | --- | --- | --- |
| D16 | 两个排他命令从不同线程并发提交 | **真缺陷**（探针：`a=0 b=0 max_concurrent=2`）：排他命令本来就绕过串联门（这正是「接管」的含义），两个都过了门就并发跑，排他性失效 | 新增 `Impl::exclusive_busy`：与门检查同处一个临界区，运行中的顶层 Exclusive 持有、由它的 `ChainGuard` 释放。改后峰值并发 = 1（用例 `CommandManager_ExclusiveCommandsAreSerialized`；改前为 2，可判别）。**接管仍正常**：后到者先等前者收尾，再被放行——用例里两种结局都断言了 |
| D17 | 工厂里重入注册表（注册/注销/列举/查询） | **安全**（探针 + 用例）：工厂在 `registry_mutex` 之外调用 | 已加用例 `CommandManager_ReentrantFactoryIsSafe` 钉住 |
| D18 | `executed` 处理函数里再跑命令、注销刚跑完的命令、增删处理函数 | **安全**（探针 + 用例）：通知在所有锁之外；`Signal` 先取 id 快照再查找，所以同线程内重入增删是安全的（新增的本轮不生效、注销的不再调用） | 已加用例 `CommandManager_ReentrantEventHandlerIsSafe` 钉住 |
| D19 | `executing`/`executed` 的事件表本身不是线程安全的（`Signal` 无锁，`trigger` 快照 + 查找） | **真约束**（读代码）：命令在任意线程结束，而订阅方在主线程 `addHandler` ⇒ 数据竞争 | 属仓库级 `Signal` 约定，本轮**只文档化**：两个事件的注释 + 本节明确「同线程内重入增删安全；跨线程且可能有命令正在结束时不行，订阅请放在启动期」。要根治得改 `Signal`（全仓）或改成总线事件（改语义） |
| D20 | `setCommandEnabled` 的读-改-写（读数组 → 改 → 写数组）不是原子的 | **真但极难触发**（只有 UI 线程调它；`ConfigManager` 自身读写是加锁的，所以无数据竞争，只有丢失更新） | **有意不加**管理器侧互斥：`ConfigManager::setStringArray()` 会在释放自身锁后触发 `changed`，处理器可能重入 `setCommandEnabled`，加一把跨该调用的锁会自锁。留档，建议将来在 ConfigManager 侧提供「原子增删数组元素」 |

结论：**命令的虚函数、工厂、事件处理函数、快照回调在同线程内重入是安全的**（无锁 + Signal 快照语义），
跨线程只有两条约束：事件表的增删要在无并发完成时进行（D19），禁用偏好的读写不要两个线程同时写（D20）。

## 第五轮审查（边界情况，2026-09-11）

重点是"各种情况"下新老机制的交界：等待用户输入、在自己身上排空、以及第 4 轮改动带来的新边界。

| # | 情况 | 结论 | 处理 |
| --- | --- | --- | --- |
| D9 | 命令停在 `UserIO::getXxxAsync()` 上时排空 | **真缺陷**（探针：`drained=0 elapsed_ms=202 still_parked=1`）。读操作没有取消令牌，`cancelAll()` 传不进去；`Application::shutdown()` 只能白等满上界，然后带着活链销毁管理器 | 新增 `UserIO::cancelPendingInput()`（默认空实现，声明放在**虚表末尾**以缩小 ABI 影响面）+ `VisualUserIO` 实现（复用 Escape 的 `cancelInteraction()`）；`Application::shutdown()` 在排空前先取消挂起交互。`ConsoleUserIO` 是阻塞式 `std::getline`，外部无法唤醒，保持默认空实现并在头文件注明 |
| D10 | 命令内部调用 `cancelAllAndWait()`（例如"退出"命令调 `Application::shutdown()`） | **无法修复的语义限制**（探针：必定 `false`，且白等满上界）。命令自己所在的链正是要排空的对象，"检测当前是否在命令里"用 thread_local 计数是**错的**：协程会在别的线程恢复（`sleepFor` 定时器线程），计数会漂移，反而让宿主永远排不掉 | 不改代码，改为在 API 文档与本节明确：命令要停应用请 `Application::quit()`，由宿主收尾；并加一条用例把"必定 false"钉住，避免被误当可用路径 |
| D11 | `Chain::leaveChain()` 的 `--runs` 无下界保护 | 真（防御性）：一旦计数下溢，`liveRuns() > 0` 恒成立，排空永远失败 | 加 `assert(runs > 0)` |
| D12 | Exclusive 等待期间有新链被别的线程准入（快照之后） | 真（窗口收窄但未消除）：接管时看不到"稍后启动"的链 | 已知窗口，记档；要彻底消除需要"关门"状态（管理器拒绝新顶层命令），属于后续设计 |
| D13 | `cancelAll()` 无法中断正在等待排空的 Exclusive 命令 | 真：等待方还没建链，不在活跃链注册表里，只能等满上界（关停时白等 2s） | **已修**：新增 `Impl::cancel_generation`（原子计数，`cancelAll()`/`cancelAllAndWait()` 自增）。接管在发出停止请求之后采样它，等待循环发现变化就以 `TakeOverOutcome::Cancelled` 提前返回（命令结果为 `Cancelled`），而不是 `StillStopping` 的 `Failed` |
| D14 | `VisualUserIO::pending_` 跨线程读写（命令线程写、宿主/UI 线程读） | 真（既有竞争，非本轮引入）：`cancelPendingInput()` 只是多了一个读点 | 记档；修它要把 `pending_` 改成原子类型并改若干 switch，单独一轮做 |
| D15 | 贴出的失败消息在关停期间投递 | 经核实**安全**：`ApplicationData` 的声明顺序保证 `main_dispatcher` 最后销毁、`user_io` 先于 `command_manager` 销毁，而消息只可能被应用线程在 `EventBus::shutdownGracefully()` 派发时消费，那时两者都还活着 | 无需改动 |

## 第四轮审查（8 点：缺陷 → 修复，2026-09-10）

审查方式：先跑通全部既有用例（25/25），再用临时探针用例复现可疑行为（跑完即删除），
最后把能复现的固化成正式用例；两处属于契约收敛的问题没有稳定复现手段，只做静态判定。

| # | 缺陷 | 证据 | 修复 |
| --- | --- | --- | --- |
| D1 | `executeDetached()` 的失败回写发生在命令结束的那个线程上（定时器/异步 IO 线程），GUI 的 `VisualUserIO::putString()` → `ConsolePanel::append()` 直接写 QWidget | 探针：`executed` 回调线程 ≠ 应用线程 | 新增 `reportToUser()`：`MainThreadDispatcher::isMainThread()/hasEventLoop()` 为假时 `postToMain()` 编组，否则内联；`VisualUserIO` 的控制台回写同样处理（`appendOnApplicationThread()`） |
| D2 | Exclusive 只取消 `foreground` 链 ⇒ 被别的顶层命令顶出前台的后台链与之并发（不变量 8 失效） | 探针：`takeover=Success` 且后台链仍在跑 | `takeOverForeground()` 改为取活链快照、对**全部**活链 `request_stop()` 并等待全部收尾；超时仍以 `Failed` 拒绝 |
| D3 | 被取消的嵌套子命令在 `throw` 上抛前不 `report()` ⇒ 没有 `executed` 事件、不进历史（可 `executing` 已经发出） | 探针：child `executing=1 executed=0`、`history=1` | 上抛前先 `report(..., Cancelled)` |
| D4 | 关停时既不取消也不等待命令链；`~CommandManager` 也无保护 ⇒ 活帧在管理器销毁后恢复即 UAF | 代码：全仓无 `cancelAll()` 生产调用；`Application::shutdown()` 不碰管理器 | 新增 `cancelAllAndWait(timeout)`；`Application::shutdown()` 第一步调用（超时只记 warning）；`~CommandManager` 若发现活链则告警（不在析构里阻塞） |
| D5 | `Impl::admit()` 持 `mutex` 调 `ProgressHost::current()`（取 progress 模块的**全局**锁），与"锁内不跑外部代码/不与 progress 锁嵌套"的注释矛盾 | 代码 | 进临界区前采样 `ambient_host_busy` |
| D6 | `waitDrained` 用 `withTimeout(AsyncEvent)` 做有界等待：超时会**从定时器线程销毁**仍排队的 waiter，而 `AsyncEvent::set()` 在弹出 waiter 后**锁外** resume ⇒ 可能 resume 已释放的帧（`async_global.hpp` 把该窗口明确划给调用方） | 代码（未复现，窗口为微秒级） | 删除 `Chain::drained`，改为 5ms 切片轮询 `runs`（`Impl::waitChainsDrained`） |
| D7 | `setCommandEnabled()` 不解析别名 ⇒ 按别名禁用无效（执行路径会解析），而 `isCommandEnabled(别名)` 仍返回 true（API 自相矛盾） | 代码 | `resolveName()` 后再写偏好与标记 |
| D8 | `executeCommandAsync(Command*)` 的 `command->name()`（虚函数 + `String` 拷贝）与 `isDisabledRegistration()` 在 try 之外 ⇒ 与不变量 7 冲突（会抛给 UI 调用方） | 代码 | 移入 try 块 |

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
   都在所有锁之外；`ProgressHost::current()`（progress 模块的全局锁）也在 `mutex` 之外采样。
   锁内只做容器操作与存储值的拷贝（`String`/`std::any` 的值拷贝，属数据而非回调）。
5. **链只有在自身帧彻底收尾后 `runs` 才归零**：先弹栈项、再析构 `ProgressHost`，最后才
   `leaveChain()`；门标志在归零前释放，所以轮询到零的等待者看到的是干净状态。
6. **前台链是唯一**：`d->foreground` 指向最近一次顶层执行建立的链；`cancelCurrent()` 只作用于它，
   而 `cancelAll()` 作用于活跃链注册表。
7. **异常不外逃**：顶层入口（`executeCommandAsync(Command*)`、`executeCommandAsync(name)`）
   把一切逃逸异常（工厂、分配失败、命令体）转为 `Failed` 结果；快照回调抛错 ⇒ 命令不执行；
   `executing`/`executed` 回调抛错只记日志。原因是调用方是 UI 事件处理器与 `DetachedTask`
   （后者未捕获异常即 `std::terminate()`）。
8. **排他性不可静默丢失**：Exclusive 只要么接管成功，要么以 `Failed` 拒绝；
  不存在的状态是“两个链并发跑”。这条对**并发提交**的 Exclusive 同样成立：
  `exclusive_busy` 保证两个排他命令不同时运行（后到者要么接管前者、要么被拒）。
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
      ├─ int runs                     (未结束的运行数, mutex 保护; 归零 = 本链已收尾)
      └─ std::stop_source stop_source (只停不换)
```

- `Chain` 是 `CommandManager` 的私有嵌套类型，定义在 `.cpp`（`struct CommandManager::Chain`），
  因此头文件只前向声明 `struct Chain;`。
- 顶层入口（`executeCommandAsync(Command*)`、`executeCommandAsync(name)`、`executeDetached`）
  各自建立新链并写 `d->foreground`；`nested=true` 的分支使用 `Context` 传入的父链。
- 每个 `LongRunning` 命令（无论嵌套与否）仍各自持有 `ProgressHost`，绑定**本链**的 `stop_source`。
- **等待链收尾一律轮询 `runs`**（`Impl::waitChainsDrained`，5ms 切片，`sleepFor` 驱动），
  不再有 `AsyncEvent`：有界等待若采用"超时就销毁等待者"的做法，会在 `AsyncEvent::set()`
  弹出 waiter 之后、锁外 resume 之前的窗口里销毁协程帧，违反 async 模块的生命周期契约
  （见 `async_global.hpp`）。链的 `runs` 归零前一定已完成弹栈与宿主析构，所以轮询读到的
  零值同样代表"整帧已收尾"。

## 命令管理器对话框（`gui::CommandManagerDialog`）

- 文件：`src/fw/appfw/src/gui/CommandManagerDialog.cpp`（窗口标题「命令管理器」），由 app_shell 的
  `show_commands` 命令打开。
- 布局：筛选框（带清除按钮）→ 命令表（名称 / **状态** / 别名 / 来源插件 / 分组 / 描述）→ 底部一行
  「左侧反馈文字 + `刷新` / `禁用命令`(选中禁用项时变 `启用命令`) / `关闭`」，与插件管理器对话框同形。
- 表格视觉与插件管理器**共用** `src/fw/appfw/src/gui/TableStyle.hpp` 的 `detail::blendIntoSurface()`
  （透明表体、表头取 `palette(window)`、去掉自身边框与行号列、显式补 `gridline-color` 与
  `::item:selected`），它是这两个对话框唯一的表格样式来源，别再各写一份 QSS。
- 被禁用的行**灰显斜体**（不隐藏、不删除），`状态` 列写 `已禁用`；切换按钮在未选中行时**置灰**
  而不是隐藏（选中行变化频繁，隐藏会闪）。按钮文案随选中行的状态在 `禁用命令` / `启用命令` 之间切。
- 对话框**不再提供"卸载命令"**：禁用是标记而非移除，语义更清楚也不会丢元数据/别名。

## 命令禁用（2026-09-10，用户拍板的设计）

**禁用是注册表里的一个标记，不是移除**：命令留在 registry 里（连同元数据、别名、owner），只是
不能执行，随时可以再启用。相比"卸载"，插件下次启动重新注册时不会被"复活"，也不丢展示信息。

- 数据结构：`RegisteredCommand::enabled`（默认 true）；`CommandInfo::enabled` 供列举方使用。
- `setCommandEnabled()` **先 `resolveName()`**：执行路径都解析别名，偏好只记在别名上就会
  静默失效（第 4 轮审查 D7）。别名调用 ⇒ 标记与偏好都落在命令的规范名上。
- 公开 API（新增，不影响既有调用）：
  - `setCommandEnabled(name, enabled)`：先写偏好、再翻标记；名字未注册时**只记偏好**（等它注册时生效）。
  - `isCommandEnabled(name)`：仅"已注册且启用"为 true。
  - `disabledCommands()` / `static disabledConfigKey()`（`commands.disabled` 字符串数组，
    与 `PluginManager::disabledConfigKey()` = `plugins.disabled` 对称）。
  - ⚠️ `isRegistered()` 语义收紧为"**能执行**"：禁用的命令返回 false（禁用项仍在 `names()` /
    `commandInfos()` 里，需要"存在性"请查这两者）。
- **两个入口都要把关**：
  - 按名字：`createCommandByName()`（顶层 `executeCommand(name)` 与 `context->executeChild(name)`
    都走它）返回 nullptr，并通过 out 参数 `disabled` 区分"禁用"与"未注册"。
  - 按实例：`executeCommand(Command*)` / `executeCommandAsync(Command*)` 本来完全绕过注册表，
    禁用形同虚设；现在先查 `Impl::isDisabledRegistration(command->name())`——**只有"注册了且被禁用"**
    才拒绝（实例名未注册的临时命令照旧能跑，否则一堆本地命令会被误伤）。
- **持久化路径**：`registerCommand()` 里读偏好（`Impl::isDisabled()`）决定新注册项的 `enabled`。
  插件每次启动都会重注册自己的命令，于是"上次禁用的"自动带着标记回来——不需要额外的
  "启动后统一应用禁用列表"步骤。没有宿主的 ConfigManager 时退化成 `Impl::disabled` 进程内列表。
- 消费方也要认这个标记：控制台补全（`VisualUserIO::refreshCompletion`）**跳过**禁用命令（补全一个
  跑不了的命令只会导致执行失败）；`list_commands` 在行尾加 `[已禁用]`。
- 测试：`GuiTest.CommandManager_DisableIsAFlagAndIsPersisted`（禁用后仍列举、`isRegistered` 为假、
  别名同样不可执行、执行消息为「命令“x”已被禁用；可在「命令管理器」中启用。」、偏好已记录、
  **注销后重新注册仍是禁用**、启用后立刻恢复执行且偏好被清掉）与
  `GuiTest.CommandManager_DisableBlocksCallerSuppliedInstance`
  （按实例的入口同样被拦；实例名未注册时不受影响）。
- **拒绝时的消息是给用户看的**（`refusalMessage()`）：带命令名 + 可执行下一步，"未注册"也在
  这里一起中文化（原来是 `Command not registered`，没有任何测试依赖它）。`Command is null`
  这类编程错误保持英文。
- ⚠️ **触发方式决定提示能不能被看到**：控制台输入走 `executeCommandAsync()`，由
  `VisualUserIO::onLineEntered` 把失败消息回写控制台；而 Ribbon 按钮/动作走
  `executeDetached()`（没人等结果）——禁用后点按钮原本**完全静默**。现在
  `executeDetached()` 也会把失败消息写进 `UserIO`（同控制台同一条消息），两条路都有提示
  且不会重复（控制台不走 executeDetached）。

## 关键机制

- **Exclusive 接管**：顶层 Exclusive 命令先取活链快照（`collectLiveChains()`），对**每一条**活链
  `request_stop()`，再 `co_await Impl::waitChainsDrained(...)`：等待是**协作式**的
  （每 5ms 切片挂起本协程而不是阻塞线程），因此被取消的链仍能在原线程继续收尾；等待有上界
  `CommandManager::exclusiveDrainTimeout()`（默认 2s）。
  超时 ⇒ 命令以 `Failed("Another operation is still stopping")` 拒绝，**不会**与旧链并发执行。
  接管成功则在自己的新链上运行并绕过串联门。
  **只停前台链是不够的**：一条被 `executeDetached()` 启动、随后被别的顶层命令顶出前台的链
  不再是前台，但它仍在跑；只取消前台会让排他命令与它并发（第 4 轮审查 D2，已有用例
  `CommandManager_ExclusiveStopsEveryLiveChain`）。
  **等待本身可被打断**：`cancelAll()`/`cancelAllAndWait()` 会自增 `Impl::cancel_generation`，
  等待中的接管在下一个轮询点（≤5ms）就放弃并以 `Cancelled` 收尾，不必等满 2s
  （第 5 轮审查 D13，用例 `CommandManager_CancelAllAbortsWaitingTakeOver`）。
- **串联门（原子）**：决定“是否准入”与“占用门”在同一临界区（`d->mutex`）内完成，
  占用标志为 `foreground_busy`，由拿走它的那次运行的 `ChainGuard` 在析构时释放。
  门条件 = `foreground_busy || ProgressHost::current() != nullptr`（后者覆盖外部手动挂的前台宿主）。
  必须这么做的原因：`ProgressHost` 的宿主是在临界区之后才创建的，
  若“检查—占用”分离，两个线程会同时通过。
- **异常策略**：`command->execute()` 抛出的非取消异常在当前层转成 `Failed(what())` 并记入历史、
  触发 `executed`；`TaskCancelledException` 仍按嵌套规则上抛（链起点转为 `Cancelled`），
  但**上抛前先上报自己**：子命令的 `executing` 已经发出，所以它必须补上对应的 `executed`
  与历史条目（`Cancelled`），否则"每个跑过的运行都有 executing/executed 一对"的契约在
  被取消的嵌套子命令上失效。
  快照回调失败 ⇒ 不执行该 Undoable 命令（否则后续 undo 会回到不存在的状态）。
- **嵌套深度**：`Context::executeChild` 在 `chain_->stackSize() >= maxChainDepth()` 时拒绝并返回 `Failed`，
  自我递归命令最多 64 层。
- **历史容量**：`deque` 满 `maxHistoryEntries()` 时 `pop_front()`，`historyAt(0)` 始终是“仍保留的最早一条”。
- **活跃链注册表**：只在 `mutex` 下维护 `weak_ptr`（不延长链寿命），每次登记/取快照前
  顺带清理已结束项，因此注册表大小 = 活链数。`cancelAll()`、`cancelAllAndWait()` 与
  Exclusive 接管共用 `Impl::collectLiveChains()`。
- **关停排空**：`cancelAllAndWait(timeout = exclusiveDrainTimeout())` = `cancelAll()` +
  轮询等待全部链收尾，返回是否真的排空；`Application::shutdown()` 先
  `UserIO::cancelPendingInput()`（唤醒停在用户输入上的命令），再调用它（超时只记 warning）。
  管理器被销毁时活着的命令帧仍指向 `Impl`（`ChainGuard::impl`）与 `CommandManager`
  （`Context::mgr_`），所以"先排空再销毁"是必须由宿主保证的前提（见下节）。
  命令**不能**在自己内部调用它（自己所在的链永远等不到），要停应用用 `Application::quit()`。
- **取消传播**：`catch (TaskCancelledException)` 里用 `chain->stackSize() > 1` 判定"我是嵌套子命令"，
  是则**先 `Impl::report()`（Cancelled）再 `throw`**，否则（链的起点）记录 `Cancelled`。
  判据取自**本链**，不再被其他链污染。
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
- **管理器必须长于所有命令执行**：活着的命令帧里存着 `Impl*`（`ChainGuard`）与
  `CommandManager*`（`Context`），销毁管理器时若还有链没收尾，这些帧一旦恢复就会触碰已释放的
  内存。宿主用 `cancelAllAndWait()` 建立这个前提（`Application::shutdown()` 已经这么做）；
  管理器自身不做排空（析构里阻塞线程等协程恢复本身就是死锁风险），只在返回 false 时由宿主
  决定是否继续销毁。
- **独占性由调用方按语义使用**：Exclusive 会取消**所有**活链（包括 `executeDetached()` 启动的
  后台链）。UI 上"切换工具/模式"这类命令适合用它；一个还需要别的命令继续跑的场景不能用。
- **不要在命令内部排空或关停**：`cancelAllAndWait()`/`Application::shutdown()` 会等待命令自己
  所在的链，永远等不到（只会在上界后返回 false，关停还会带着活帧往下走）。命令要退出应用时
  用 `Application::quit()`（停止主循环），由宿主在 `run()` 返回后收尾。

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
- 第 4 轮新增：`CommandManager_NestedCancelledChildIsReported`（被取消的嵌套子命令也要
  executed=1 且父子各一条历史）、`CommandManager_ExclusiveStopsEveryLiveChain`（后台链被顶出
  前台后仍被排他命令取消，且排他命令放行前它已收尾）、`CommandManager_CancelAllAndWaitDrainsChains`
  （空闲幂等、取消后有界排空、历史落定）、`CommandManager_DisablingAnAliasDisablesTheCommand`
  （按别名禁用/启用落到命令上）、
  `CommandManager_DetachedFailureIsReportedOnTheApplicationThread`（失败消息经
  `deliverPostedCalls()` 才出现在控制台面板，证明它被编组而不是从工作线程直写 QWidget）。
- 第 5 轮新增：`CommandManager_PendingUserInputBlocksDrainUntilCancelled`（只取消链排不掉，
  `cancelPendingInput()` 之后才成功，且结果落定为 Cancelled；再次取消幂等）、
  `CommandManager_CancelAllAndWaitFromInsideCommandCannotSucceed`（钉住 D10 的限制）、
  `CommandManager_CancelAllAbortsWaitingTakeOver`（等待接管的排他命令被 `cancelAll()` 立刻打断，
  结果为 Cancelled 且命令体未执行）。
- 第 6 轮新增：`CommandManager_ReentrantFactoryIsSafe`、`CommandManager_ReentrantEventHandlerIsSafe`（重入注册表/事件回调安全）、
  `CommandManager_ExclusiveCommandsAreSerialized`（并发提交的排他命令不重叠）。

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
