# appfw CommandManager 设计（2026-09-10 重写）

代码：`src/fw/appfw/sdk/vine/appfw/CommandManager.hpp`、`src/fw/appfw/src/CommandManager.cpp`；
`CommandFlags::Exclusive` 语义见 `src/fw/appfw/sdk/vine/appfw/Command.hpp`。
测试：`tests/test_gui/test_gui.cpp`（`CommandManager_*` 共 41 例）。

## 入口点语义（顶层 vs 嵌套）

四个公开入口全部是**顶层入口**（`executeCommandAsyncImpl` 传 `nested=false`）；只有
`CommandExecutionContext::executeChild()` 走 `nested=true`。

| 入口 | 链 | 前台归属 | 串联门 | 取消源 | 阻塞调用线程 |
| --- | --- | --- | --- | --- | --- |
| `executeCommandAndWait(Command*)` / `(name)` | 新建 | 成为前台链 | 受门 | 新源 | 是（`Task::result()`） |
| `executeCommandAsync(Command*)` / `(name)` | 新建 | 成为前台链 | 受门 | 新源 | 否（惰性，await 后才跑） |
| `executeDetached(name)` | 新建 | 成为前台链 | 受门 | 新源 | 否（立即跑到首次挂起） |
| `context->executeChild(name)` / `(instance)` | **共享父链** | 不变（同一条链） | **绕过** | **共享父链源** | 否（co_await） |

由此产生的实际后果（均已有用例固化）：

- **异步入口的链是“首次 await 时才建立”的**：`Task` 是惰性的（`initial_suspend = suspend_always`），
  所以 `cm->executeCommandAsync(...)` 只是拿到一个未启动的 task，**那一刻不建链、不抢前台**；
  真正 `co_await`（或 `Task::result()`）时子协程才进入 `Impl::admit`，此时才新建链并写 `foreground`。
  丢掉不 await = 命令永不执行（帧被析构），也不会留下任何链。
- 在命令内部调顶层入口 = 开一条**独立链**，不是嵌套：它会**抢走前台**
  （`currentCommand()`/`runningCount()` 随后描述新的链，`cancelCurrent()`/Esc 也只能取消它），
  且 `maxChainDepth()`（64 层）**拦不住**这种写法的递归。
- **前台不会“恢复”**：子链跑完后 `foreground` 仍指向那条已 drain 的子链，
  于是父命令还在跑而 `currentCommand()` 返回 nullptr、`runningCount()` 返回 0
  （`cancelAll()` 仍能触及父链，因为它在活跃链注册表里）。
- 父命令是 `LongRunning`（持有门）时，内部调顶层入口会被拒（`Failed("另一个操作正在进行中，请稍候。")`）
  ⇒ 想嵌住跑子命令必须用 `executeChild()`。
- 被门拒绝现在会记一条 warning（`Command refused by the serialization gate`）：
  `executeDetached` 的调用方拿不到返回值，否则拒绝会完全无声。
- 命令内部**不要**用同步 `executeCommandAndWait()`：它阻塞当前线程，而该线程可能是 UI 线程或定时器线程
  （`Task::result()` 自己的文档就警告：若该线程的事件循环是任务恢复所必需的，就会死锁）。
  命令内部应 `co_await executeCommandAsync(...)`，或直接用 `executeChild()`。

## 最佳实践整理（第三轮）

1. **日志不改变行为，且不靠本地封装保证**：调用点直接写裸 `VN_LOGI/W/E`，参数一律传
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
   剩余仅是全仓 `VN_OBJECT_META_DECL;` 多一个分号的 `-Wextra-semi`（宏末尾自带 `;`，且仓库里两种写法并存），
   要统一得单独一轮、全仓改，不在本模块范围（项目并未启用该警告）。

## 第八轮：API / 架构 / 线程安全三轴复核（2026-09-17）

复核方式：读 `CommandManager`/`Command`/`UserIO` 三份头 + 实现 + 本设计文档 + UserIO 文档，写探针实测
（`/tmp/async_probe2/probe_command_flags.cpp`、`probe_signal_threads.cpp`、`bench_signal.cpp`），基线
`test_gui` 158/158。结论：**manager 自身的同步面是干净的**（`mutex`/`registry_mutex`/`Chain::mutex`/
`cancel_generation` + "锁内不跑用户代码" + `admit()` 在临界区外采样 progress 宿主），三处需要动作的地方如下。

| # | 问题 | 证据 | 处理 |
| --- | --- | --- | --- |
| E1 | `CommandFlags` 是位标志枚举却没有位运算 ⇒ 用户写不出"长时间且改数据"这种组合 | 编译探针：clang 报 `invalid operands to binary expression ... no implicit conversion for scoped enum`；加上 `VN_ENABLE_ENUM_FLAGS` 后 `Undoable\|LongRunning == 5`、`vn::testFlag` 可判位（仓库已在 `RenderApi`/`MessageBoxButton`/`DockAreas`/`DockFeatures`/`ModifierKey` 上这么做，`CommandFlags` 是唯一漏网的） | `Command.hpp` 加 `VN_ENABLE_ENUM_FLAGS(CommandFlags)`；删掉 `CommandManager.cpp` 里的本地 `hasFlag`，改用仓库的 `vn::testFlag`（5 处） |
| E2 | `isRegistered()` 与 `isCommandEnabled()` 实现完全相同，且名字与语义相反（禁用的命令 `isRegistered` 返回 false，却仍在 `names()`/`commandInfos()` 里） | 两者实现是同一段；既有用例断言禁用后 `isRegistered == false` | `isRegistered()` 改为**存在性**（只查注册表：不解析别名、不看 enabled）；`isCommandEnabled()` 保留"能执行"语义并补齐文档。生产代码没有 `isRegistered` 调用者，只有 test_gui 的断言按新语义改写 |
| E3 | `CommandExecutedEventArgs` 每次执行深拷贝一份 `CommandResult`（含 `std::any` 载荷） | 读码：值成员 `result_` | 改为持 `const CommandResult*`（同步通知，manager 在整个通知期间持有结果），`result()` 仍返回 `const CommandResult&`；文档写明只在通知期间有效。历史仍按值存（它要活得更久） |
| E4 | `VisualUserIO::currentPrompt_` 跨线程读写：写在**读发起线程**（命令可在定时器/IO 线程恢复后调用 `getXxxAsync`），读在应用线程（`repromptError`） | 读码 + 新用例把"读在非应用线程发起"钉成事实（命令先 `sleepFor(1ms)` 再提示，断言发起线程 ≠ 应用线程） | 提示记帐改成 `PromptState`（`shared_ptr`，避免 posted 回调捕获 `this`），写入挪进"显示提示"那条编组调用 ⇒ 只在应用线程读写；`onApplicationThread` 加断言守住"有事件循环时 inline 路径必须在应用线程" |
| E5 | `Signal` 本体无同步 ⇒ `executing/executed/commandsChanged` 三个**公开**事件表必须"订阅与命令完成错开" | TSan 探针：一边 `trigger` 一边 `add/removeHandler` ⇒ **12 条 data race**，进程不崩（静默 UB） | 见下节。这是本轮唯一"跨模块"的改动 |

### E5 落实：`Signal` 线程安全化（`src/base/core/sdk/vine/Signal.hpp`）

**做法**：handler 表是**不可变快照**（`std::vector<std::shared_ptr<Slot>>`，`Slot : Connection::State = {callback, atomic<bool> alive}`），
`connect()` 返回 RAII 句柄 `Connection`（`disconnect()`/`isActive()`/`detach()`；句柄是独立于 Signal 的**非模板类**，
形状与 appfw 的 `EventBus::Subscription` **同形**——移动语义、析构即取消——入口名则对齐 Qt 的 `QObject::connect()`）。
`connect/disconnectAll` 持一把 mutation mutex 复制-修改-发布；`trigger`
**不加任何锁**：原子 load 出已发布的表，靠 `shared_ptr` 引用计数把表和槽稳稳持有到遍历结束，逐个检查 `alive`。
取消一个订阅 = 一次原子写（O(1)，不再复制表）；已取消的槽在下一次 `connect()` 复制时顺手 `remove_if` 剔除。
这与 Qt 的连接表一致（原子指针 + 引用计数的 connection data + 每条连接的失效标记），
发火线程永远不会等一个订阅操作。
原有语义全部保留（订阅顺序、发火期间新增的下一轮才生效、发火期间注销的不再被调用、handler 内可增删/
可重入/可再发火），"一边订阅一边发火"变成有定义。

| 场景（TSan） | 改前 | 改后 |
| --- | --- | --- |
| 单线程重入（handler 内 add/remove/再 `trigger`） | 干净，`outer=2 added=1` | **完全相同** |
| 一边 `trigger` 一边 `add/remove` | **12 条 data race** | **0 条**（把 `alive` 变异成 `bool` 会回到 1 条） |

代价与收益（`bench_signal.cpp`，2e6 次发火，-O2，`taskset` 绑核交替测量）：

| handler 数 | trigger 改前（裸表，有竞争） | trigger 改后 | 订阅+注销 改前 → 改后 |
| --- | --- | --- | --- |
| 1 | 12.5 ns | 13.2 ns | 16.3 → 92 ns |
| 3 | — | 13.6 ns | — → 100 ns |
| 5 | 27.8 ns | **15.5 ns** | 20.8 → 106 ns |
| 20 | 104.6 ns | **25.8 ns** | 20.1 → 147 ns |

发火路径不再"每次分配一个 id vector"，遍历连续内存且不取锁；订阅路径按表大小复制（信号本就该在装配期
订阅），这是刻意取舍。受益方是所有 `Signal` 使用者：`CommandManager` 三个事件、`ConsolePanel` 的
`lineEntered`/`escapePressed`、`WindowContext` 的 mouse/key/resized、`GuiApplication::theme_changed` 等。

#### 为什么行里存的是 `shared_ptr<Slot>` 而不是裸 `handler`

这个指针不是为了省一次间接访问，它承载的是"订阅"这个实体本身（早期版本叫 `HandlerNode`，但表已经是平铺的
`vector<Entry>`，没有图可谈，改名后能自解释）：**订阅的生命周期必须长于任何一次快照**，
而表只是一份索引。三个替代方案都试过（探针 `/tmp/async_probe2/probe_{b,c}.cpp`）：

| 方案 | 结果 |
| --- | --- |
| B：`map<id, handler>` 值存 + 复制-修改-发布（无 `alive`） | **语义被破坏**：发火中注销的 handler 仍被调用（快照里存的是 `std::function` 的副本，删表动不了它）。探针实测 `YES <-- documented semantics violated`；这正是 `RemoveAnotherHandlerDuringEmitSkipsIt` / `ClearDuringEmitStopsTheRest` 钉住的行为 |
| C：`map<id, {handler, atomic<bool> alive}>` 值存（表地址稳定，靠标记注销） | **编译不过**：`std::atomic` 不可复制/赋值 ⇒ `make_shared<Map>(*cur)` 这一步根本无法表达（`std::is_copy_constructible_v` 在 libstdc++ 上仍报 true，别信它，错误在实例化时才出现） |
| D：`map<id, handler>` 原地增删 + `trigger` 全程持锁 | **用仓库自己的 `SignalTest.cpp` 实测过**：`std::mutex` 版在 `RemoveSelfDuringEmitIsSafe`（handler 注销自己）**挂死**（`timeout` = 124）；换成 `std::recursive_mutex` 后不死锁，但 `AddDuringEmitTakesEffectNextEmit` **失败**（`{1,100,2,200,200}` vs `{1,2,200}`：发火期间新增的 handler 在本轮就被走到了），`ClearDuringEmitStopsTheRest` **段错误**（139，原地 `clear()` 使遍历迭代器失效）。即：handler 内不允许增删/再发火才可能，而那是仓库已有用例钉住的契约 |

所以：`Slot` = 订阅的身份（`alive` 标志在快照之外，异步可见）+ 让每次改表的复制退化成 N 次
`shared_ptr` 引用计数自增（不分配、不拷贝 `std::function`）。代价是每次订阅多一次小分配，
在装配期路径上，可接受。

同理，`Slot::alive`（实际定义在基类 `Connection::State` 里，句柄只认这一层）必须是 `std::atomic<bool>`：读它的 `trigger()` 在遍历时**不持锁**，而写它的是
`disconnect()`（纯原子写，连 mutation mutex 都不取，所以取消是 O(1)）与 `disconnectAll()`（持锁批量标记）——
一把锁只保护"双方都拿它"的访问。实测（只把 `alive` 改成普通 `bool`，其余不动）：
TSan 在 `probe_signal_threads` mode1 报 **1 条 data race**（`trigger` 读 / `disconnect` 写），改回 atomic 后 **0 条**；
而 -O2 基准两者相同（20 handler：60.6 vs 60.0 ns/次）——1 字节 lock-free，acquire load 在 x86 上就是普通 `mov`，
去掉它换不来性能，只换来 UB。"加锁"两条路都不可行：锁包住整次遍历 ⇒ handler 自注销时自死锁
（`probe_lock_during_trigger.cpp`，3 秒超时 = 124），锁只包住每次标志检查 ⇒ 每次发火 N 次 `lock/unlock`，
且与 `add/remove` 抢同一把锁（发火线程之间也随之串行化）。

#### 表的形状（`vector`）与发布方式（Qt 式原子发布）

两种选择分别实测过，全部在同一台机上交替测量（`/tmp/async_probe2/`：`bench_qt` = 现方案，`bench_final` = 锁内取快照，
`bench_old` = 第一版 `map` + `atomic<shared_ptr>`，变异脚本 `make_variant_{vec,vec2}.py`）：

| 变体 | 发火 1 / 5 / 20 handler | 订阅+注销（20） | 说明 |
| --- | --- | --- | --- |
| `std::map` + `atomic<shared_ptr>`，发火零锁 | 13.1 / 17.7 / 59.7 ns | 562 ns/对 | 第一版：语义正确，但表是红黑树 |
| `std::vector<Entry>` + 锁内取快照 | **6.6** / 11.2 / 27.2 ns | 110 ns/对 | 单线程最快，但发火要抢 `add/remove` 那把锁 |
| `std::vector<Entry>` + `atomic<shared_ptr>` 发布（**现方案，Qt 同构**） | 13.2 / 15.5 / **24.8** ns | 147 ns/对 | 发火不取锁；空表/1 handler 多花 ~7 ns（libstdc++ 的 `atomic<shared_ptr>` 内部有自旋锁），20 handler 反而比锁内取快照快 2.4 ns |
| `std::vector<SubscriptionPtr>`（id 放进 Subscription） | 6.8 / 12.0 / 27.6 ns | 108 ns/对 | 与 Entry 版等价（发火略差、订阅略好），按发火优先选 Entry |

- **表用 vector**：id 单调递增，表只会被扫描和追加，红黑树（每次访问跳节点、每次复制重建整树）没有收益。
  20 handler：发火 62.6 → 24.8 ns，订阅+注销 538 → 147 ns/对。`removeHandler` 退化成线性查找，
  但那一次复制本来就是 O(H)，复杂度不变。
- **发布用 Qt 同构的原子发布**（不用锁内取快照）：换来"发火永不取锁、永不等订阅"，代价是 1-5 handler 多 ~7 ns，
  20 handler 反而更快。真实事件（1-3 个订阅者）两条路径都是十几 ns 量级，取语义与可预测性。
- **并发**（20 handler）：2 线程 115 → 89 ns（原子发布更好），4 线程 106 → 123 ns（锁内取快照更好），
  1 handler：2 线程 61 → 77、4 线程 67 → 117（锁内取快照更好）—— 两边的差异都在可容忍范围，没有单向胜负。
- **试过但否决**：`make_shared<HandlerTable>` 换成 `shared_ptr(new HandlerTable)`（控制块与 vector 元数据不同 cache line）
  ——2 线程 h=20 改善约 10%，但每次订阅 +12 ns，不划算。
- **不用 `atomic<shared_ptr>` 的理由不成立**（上一轮曾据此改过一次，本轮按"与 Qt 一致"改回）：
  `is_lock_free()` 确实是 false，但那只是"内部有自旋锁"，它不会阻塞在用户代码上、不会与 mutation mutex 互相等待，
  也就不会死锁；Qt 的 `ConnectionDataPointer` 本质上也是同一套（原子指针 + 引用计数）。

#### 与 Qt 的对应关系（"跟 Qt 保持一致"的落地清单）

| Qt（`QObject` 内部） | 这里 |
| --- | --- |
| `QObjectPrivate::ConnectionData`：原子指针 + `ref` 引用计数 | `std::atomic<std::shared_ptr<const HandlerTable>>`：标准库形式的同一套（原子指针 + 引用计数），值语义更安全 |
| `Connection` 自带 `ref`，发火期间被引用计数钉住 | `shared_ptr<Slot>`，快照持有它直到遍历结束 |
| `disconnect()` 置 `c->receiver = nullptr`；发火中尚未走到的槽被跳过，正在跑的跑完 | `alive.store(false, release)`；`RemoveAnotherHandlerDuringEmitSkipsIt`/`ClearDuringEmitStopsTheRest` 钉住同一语义 |
| `connect`/`disconnect` 取 `signalSlotLock`；`QMetaObject::activate` **不取锁** | `connect`/`disconnect`/`disconnectAll` 取 `mutation_mutex_`；`trigger` **不取锁** |
| `blockSignals()` / `signalsBlocked()`（返回旧值） | `setBlocked()` / `isBlocked()`（同样返回旧值） |
| 发火期间 `connect` 的新连接何时生效：实现定义、无文档保证 | 钉死为"下一轮才生效"（`AddDuringEmitTakesEffectNextEmit`） |
| 连接句柄 `QMetaObject::Connection`（`isValid()`、可 `disconnect()`） | `Connection`（`isActive()`、`disconnect()`、`detach()`）—— 入口与 Qt 同名，句柄形状与本仓库 `EventBus::Subscription` 同形 |

#### RAII 断连：`connect()` 返回 `Connection`（形状同 appfw 的 `EventBus::Subscription`）

只有一个入口 `connect()`，它返回**移动语义的 RAII 句柄** `Connection`：析构即取消，`disconnect()` 幂等且可从
handler 内部调用，`isActive()` 查询，`detach()` 放弃管理但保留订阅（对象自己的内部接线用）。因为没有 id 了，
**取消不再需要复制表**（旧 `removeHandler(id)` 要 `find_if` + 复制 + erase），现在只是一次原子写。

| 行为 | 实现 | 钉住它的用例 |
| --- | --- | --- |
| 离开作用域即取消 | `~Connection()` → `alive = false` | `SubscriptionCancelsOnDestruction` |
| 可移动、**不可拷贝**（只有一个所有者会取消） | 手写 move + `weak_ptr<Connection::State>` 成员 | `ConnectionIsMovableAndDisconnectIsIdempotent` |
| 给成员重新赋值 = 取消旧订阅（`handler_ = sig.connect(...)`） | move-assign 先 `disconnect()` | `AssigningASubscriptionCancelsThePreviousSubscription` |
| `detach()`：不管理但不取消 | `state_.reset()` | `DetachedSubscriptionStaysSubscribed` |
| **Signal 先死也安全**：句柄只持 `weak_ptr<Connection::State>`，不持 Signal 指针 | `state_.lock()` 失败即无操作 | `SubscriptionOutlivingTheSignalIsInert`（ASan 下跑） |
| Signal 释放后**地址被新 Signal 复用**也无害：句柄认的是槽，不是地址 | 新 Signal 的槽是新的控制块，句柄仍 inert | `SubscriptionOfADestroyedSignalStaysInertOnAReusedAddress` |
| **发火过程中 Signal 被销毁**（连 handler 里 `delete signal` 也算）：`trigger` 取到快照后不再碰 `this` | 表与槽由快照的引用计数保活，剩下的 handler 照常跑完 | `DestroyingTheSignalFromInsideAHandlerIsSafe`（ASan 下跑；把成员读取挪进遍历的变异版会报 heap-use-after-free） |
| 反复订阅/取消不会让表变长 | `addSlot()` 复制表时顺手 `remove_if(!alive)` | — |

**没被覆盖的**：另一个线程正在 `connect()`/`trigger()` 时析构 Signal——那是普通的对象生命周期 UB，与 Qt 相同：谁拥有对象谁负责协调
（应用里的做法是订阅放成员 + 宿主析构前先 `setBlocked(true)` 并停线程）。

三个变异验证（删掉析构里的 `disconnect()` / 删掉 move-assign 里的 `disconnect()` / 让 `trigger` 在遍历中再读成员）
各自只打红对应的那一条用例。Signal 用例 8 → **16**，TSan 0 告警，ASan+LSan PASS。

**`[[nodiscard]]` 是刻意的**：句柄即所有权，丢掉返回值 = 订阅完立刻取消，所以每条 `connect` 必须要么绑定句柄、
要么显式 `detach()`。仓库里真实调用点已全部迁移：`MainWindow`/`ConsolePanel`/`VisualUserIO`/`ConsoleLogRouter` 用
成员或全局 `Connection`（删掉了拆除路径里的 `removeHandler` 与 `Application::current()` 查找），
`RibbonAction`/`RibbonButton`（给自己的信号接线）与 `test_window` 的各用例显式 `.detach()`。

**顺带发现的既有缺陷（未改）**：`MainWindowImpl::~MainWindowImpl()`、`ConsolePanel::~ConsolePanel()` 先
`if (auto* app = obj_cast<GuiApplication>(Application::current()))` 再 `removeHandler` —— 若此刻
`Application::current()` 已经为空（关闭顺序一变就会），handler 不会注销，而它捕获的正是正在析构的 `this`，
下次 `theme_changed` 发火就会调到悬垂对象。改成 `Connection` 成员可根治：弱引用不依赖
`Application::current()`，移除那两处 `removeHandler` 与 `obj_cast` 即可。
- **试过但否决**：`make_shared<HandlerTable>` 换成 `shared_ptr(new HandlerTable)`（控制块与 vector 元数据分属不同
  cache line）——2 线程 h=20 改善约 10%（121 → 104-117），但每次订阅 +12 ns（113 → 126），不划算。

### 本轮评估但**未改**（记档）

- **`commandsChanged` 不带载荷**：消费方（控制台补全）只能全量重取 `commandInfos()`；要增量更新就给
  EventArgs 加 `kind/name`，属 API 扩展，等有实际痛点再做。
- **`std::any` 作为命令结果**：跨插件 ABI 的类型擦除是刻意的（模板/variant 在 DLL 边界不友好），代价是运行时
  类型检查 + 拷贝；本轮已去掉事件里那份拷贝。
- **"manager 必须长于所有命令帧"仍是契约而非类型保证**：让 `ChainGuard`/`Context` 持 `shared_ptr<Impl>`
  可把它变成类型保证，但要求 `Impl` 与 `CommandManager` 对象寿命解耦，属单独一轮的所有权改造。
  **该轮已于 2026-09-25 落地实现后否决并回退**（共享 `Impl`、链上死亡标记、三个恢复点守卫、
  独占路径预建链；实现期 44 用例全绿、ASan 变异 5/5 命中预期 UAF）、维持契约形态，理由见
  "已评估但**未采纳**"。
- **`Command::name()/group()/description()` 值返回**：`String` 有 SSO，短名不分配；改成 `const String&`/view
  要动虚函数签名与插件 ABI，收益不抵成本。

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
| D19 | `executing`/`executed` 的事件表本身不是线程安全的（`Signal` 无锁，`trigger` 快照 + 查找） | **真约束**（读代码）：命令在任意线程结束，而订阅方在主线程 `subscribe` ⇒ 数据竞争 | 当时只文档化（「订阅请放在启动期」）。**已根治**：2026-09-17 把 `Signal` 本体改成线程安全（不可变快照 + 原子发布，与 Qt 连接表同构），见下节「第九轮」；事件注释里的启动期限制不再需要 |
| D20 | `setCommandEnabled` 的读-改-写（读数组 → 改 → 写数组）不是原子的 | **真但极难触发**（只有 UI 线程调它；`ConfigManager` 自身读写是加锁的，所以无数据竞争，只有丢失更新） | **有意不加**管理器侧互斥：`ConfigManager::setStringArray()` 会在释放自身锁后触发 `changed`，处理器可能重入 `setCommandEnabled`，加一把跨该调用的锁会自锁。留档，建议将来在 ConfigManager 侧提供「原子增删数组元素」 |

结论：**命令的虚函数、工厂、事件处理函数、快照回调在同线程内重入是安全的**（无锁 + Signal 快照语义），
跨线程的约束只剩一条：禁用偏好的读写不要两个线程同时写（D20）——事件表的增删**已安全**（第九轮把 `Signal`
本体改成线程安全，D19 根治）。

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
| D14′ | 同上 | 已在 UserIO 审查轮修掉（U6：`pending_`/`cancelled_` 改 `std::atomic`） | **本行已过时**，以上是当时的记档 |
| D15 | 贴出的失败消息在关停期间投递 | 经核实**安全**：`ApplicationData` 的声明顺序保证 `main_dispatcher` 最后销毁、`user_io` 先于 `command_manager` 销毁，而消息只可能被应用线程在 `EventBus::shutdownGracefully()` 派发时消费，那时两者都还活着 | 无需改动 |

## 第四轮审查（8 点：缺陷 → 修复，2026-09-10）

审查方式：先跑通全部既有用例（25/25），再用临时探针用例复现可疑行为（跑完即删除），
最后把能复现的固化成正式用例；两处属于契约收敛的问题没有稳定复现手段，只做静态判定。

| # | 缺陷 | 证据 | 修复 |
| --- | --- | --- | --- |
| D1 | `executeDetached()` 的失败回写发生在命令结束的那个线程上（定时器/异步 IO 线程），GUI 的 `VisualUserIO::putString()` → `ConsolePanel::append()` 直接写 QWidget | 探针：`executed` 回调线程 ≠ 应用线程 | 新增 `reportToUser()`：`MainThreadDispatcher::isMainThread()/hasEventLoop()` 为假时 `postToMainThread()` 编组，否则内联；`VisualUserIO` 的控制台回写同样处理（`appendOnApplicationThread()`） |
| D2 | Exclusive 只取消 `foreground` 链 ⇒ 被别的顶层命令顶出前台的后台链与之并发（不变量 8 失效） | 探针：`takeover=Success` 且后台链仍在跑 | `takeOverForeground()` 改为取活链快照、对**全部**活链 `request_stop()` 并等待全部收尾；超时仍以 `Failed` 拒绝 |
| D3 | 被取消的嵌套子命令在 `throw` 上抛前不 `report()` ⇒ 没有 `executed` 事件、不进历史（可 `executing` 已经发出） | 探针：child `executing=1 executed=0`、`history=1` | 上抛前先 `report(..., Cancelled)` |
| D4 | 关停时既不取消也不等待命令链；`~CommandManager` 也无保护 ⇒ 活帧在管理器销毁后恢复即 UAF | 代码：全仓无 `cancelAll()` 生产调用；`Application::shutdown()` 不碰管理器 | 新增 `cancelAllAndWait(timeout)`；`Application::shutdown()` 第一步调用（超时只记 warning）；`~CommandManager` 若发现活链则告警（不在析构里阻塞） |
| D5 | `Impl::admit()` 持 `mutex` 调 `ProgressHost::current()`（取进度注册表的**全局**锁），与"锁内不跑外部代码/不与进度锁嵌套"的注释矛盾 | 代码 | 进临界区前采样 `ambient_host_busy` |
| D6 | `waitDrained` 用 `withTimeout(AsyncEvent)` 做有界等待：超时会**从定时器线程销毁**仍排队的 waiter，而 `AsyncEvent::set()` 在弹出 waiter 后**锁外** resume ⇒ 可能 resume 已释放的帧（`async_global.hpp` 把该窗口明确划给调用方） | 代码（未复现，窗口为微秒级） | 删除 `Chain::drained`，改为 5ms 切片轮询 `runs`（`Impl::waitChainsDrained`） |
| D7 | `setCommandEnabled()` 不解析别名 ⇒ 按别名禁用无效（执行路径会解析），而 `isCommandEnabled(别名)` 仍返回 true（API 自相矛盾） | 代码 | `resolveName()` 后再写偏好与标记 |
| D8 | `executeCommandAsync(Command*)` 的 `command->name()`（虚函数 + `String` 拷贝）与 `isDisabledRegistration()` 在 try 之外 ⇒ 与不变量 7 冲突（会抛给 UI 调用方） | 代码 | 移入 try 块 |

## 第二轮审查（12 点，逐条对照代码核实）

| # | 指控 | 核实结果 | 处理 |
| --- | --- | --- | --- |
| 1 | Exclusive 等待超时后静默放行 ⇒ 双链并发 | **真缺陷**：仅 `VN_LOGW` 就继续执行，排他契约被破坏 | 改为 **Fail-Safe 拒绝**：`Failed("另一个操作仍在收尾，请稍后再试。")` + 错误日志 |
| 2 | 串联门 `ProgressHost::current()` TOCTOU | **真缺陷**：检查与占用分离，两个顶层长任务可同时通过 | 门检查 + `foreground` 赋值 + 占用标志写入同一临界区 |
| 3 | 链式别名只解析一层 | **真**（功能性缺陷，且环路会死循环） | `resolveName` 迭代解析 + visited 防环；列举按最终目标挂别名 |
| 4 | `executeDetached` 异常导致 `std::terminate` | **真缺陷**（已核实 `DetachedTask::promise_type::unhandled_exception()` 无 handler 时直接 terminate；树内 `VisualUserIO::executeInput` 的 DetachedTask 无 try/catch） | 异常在**顶层入口收口**为 `Failed` 结果（含工厂、快照、事件回调），detached 包装再叠一层兵底 catch |
| 5 | history 无上限 | 真 | `std::deque` + `maxHistoryEntries()`（默认 1024），超出丢最旧 |
| 6 | detached 链被顶出前台后无法取消 | 真 | 新增活跃链注册表 + `cancelAll()`（`cancelCurrent()` 语义不变） |
| 7 | `Task::result()` 在协程内可能死锁 | **无法确认为框架缺陷**：`Task::result()` 在调用线程上驱动任务，`sleepFor` 在独立定时器线程恢复，因此嵌套同步调用不会自锁（已用 `NestedSyncExecuteCommandDoesNotDeadlock` 验证）。真正的约束是语义性：命令内部嵌套必须走 `context->executeChild()`，否则开新链会被串联门拒绝 | 保留 `Task::result()` 语义 + 用例验证 + 文档强调 `executeChild` |
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
   都在所有锁之外；`ProgressHost::current()`（进度注册表的全局锁，宿主现在属于 appfw）也在 `mutex` 之外采样。
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

## 调度内核转移表（2026-09-25 审计）

对 `Impl::admit()` / `takeOverForeground()` / `waitChainsDrained()` / `ChainGuard` / `cancelLiveChains()`
的现状逐事件落表，作为"调度内核状态机化"（②）的复核交付：审计确认代码与不变量 4/5/8 一致，
**不做** `Scheduler` 类抽取（理由见"已评估但**未采纳**"）。
状态＝占用位（`foreground_busy` 门 / `exclusive_busy` 排他，均在 `Impl::mutex` 内读写，释放统一在
`ChainGuard` 析构）＋活链集合 R ＋ `cancel_generation` 代际 G。

| 事件 | 前置 | 结果 |
| --- | --- | --- |
| E1a 顶层非排他 admit | 门空闲且无环境进度宿主（`ProgressHost::current()`，进临界区前采样） | 建链并登记；`foreground` 指向它；LongRunning ⇒ 门置位 |
| E1a 同上 | 门被占或环境宿主忙（与来者自身 flags 无关） | 拒绝：`Failed("另一个操作正在进行中，请稍候。")` |
| E1b 顶层排他 admit（E2 成功后才到） | `exclusive_busy` 空闲 | 认领排他位（LongRunning 加置门位）；建链同 E1a |
| E1b 同上 | `exclusive_busy` 被占 | 拒绝——并发提交的两个排他只有一个过（D16） |
| E1c 嵌套 admit | 任意 | 无调度效果（入父链、绕门） |
| E2 排他接管 | 任意 | 快照 R → 全链 `request_stop()`；无活链 ⇒ 直接成功；否则采样 G 后 5ms 轮询：排空 ⇒ 成功；G 变化 ⇒ `Cancelled`（D13）；到期 ⇒ `StillStopping`，顶层以 `Failed` 拒绝 |
| E3 帧收尾（`ChainGuard`） | 持有门/排他位 | 位先清（锁内）→ `leaveChain()`（runs 归零）；位清在归零之前（不变量 5），且 `ChainGuard` 在 progress host/StackGuard 之前声明 ⇒ 最后析构 |
| E4 `cancelAll()`/`cancelAllAndWait()` | 任意 | G 先自增（打断等待中的 E2）→ 快照 R → 全链停；位由各帧 E3 释放 |
| E5 `~CommandManager` | 任意 | 只告警（宿主契约，见"必须由调用方保证"） |

**已知残余（本表不消除）**：排他是"启动时刻清场"，不是**入场锁**——运行期（或接管等待窗口内，D12）
被准入的普通顶层命令会与排他命令并存；彻底关门需要"管理器拒绝新顶层命令"的显式状态，属动 API 轮。
覆盖用例：`GateAdmitsAtMostOneTopLevelLongRunningCommand`（E1a 拒绝）、`ExclusiveCommandsAreSerialized`
（E1b 并发）、`ExclusiveStopsEveryLiveChain`（E2 全链）、`CancelAllAbortsWaitingTakeOver`（E2×E4）、
`ExclusiveIsRejectedWhenChainIgnoresCancellation`（E2 超时）。

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
  - `isRegistered(name)`：**存在性**（只查注册表，不解析别名、不看 enabled）。禁用项仍在
    `names()`/`commandInfos()` 里；"能不能执行"问 `isCommandEnabled()`。
- **两个入口都要把关**：
  - 按名字：`createCommandByName()`（顶层 `executeCommandAndWait(name)` 与 `context->executeChild(name)`
    都走它）返回 nullptr，并通过 out 参数 `disabled` 区分"禁用"与"未注册"。
  - 按实例：`executeCommandAndWait(Command*)` / `executeCommandAsync(Command*)` 本来完全绕过注册表，
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
  超时 ⇒ 命令以 `Failed("另一个操作仍在收尾，请稍后再试。")` 拒绝，**不会**与旧链并发执行。
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
  自我递归命令最多 64 层。两个重载（按名字 / 传入实例）走同一处检查。
- **嵌套形态两种**（2026-09-17 新增第二种）：按名字（注册表建实例）与**传入实例**
  （`executeChild(std::unique_ptr<Command>)`，参数由父命令给、无法预注册）。实例重载先拒 `nullptr`
  （`Command is null`）与"名称已注册且被禁用"的实例（与顶层按实例入口同规则，防止绕过禁用），
  并接过实例所有权：参数活在执行帧里，子命令结束后随帧析构。子命令以它自己报告的名字进
  事件与历史，未注册则不出现在 `commandInfos()` 里。
- **历史容量与内容**：`deque` 满 `maxHistoryEntries()` 时 `pop_front()`，`historyAt(0)` 始终是“仍保留的最早一条”。
  历史**不保留结果载荷**（`CommandResult::data()` 的 `std::any`）：条目数有界时字节数才会随之有界，
  否则"最近 1024 次运行的载荷之和"可以到 GB 级；载荷属于发起那次执行的调用方。
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
- **管理器生命周期只跟随 `Application`；退出必须优雅清空**：活着的命令帧里存着 `Impl*`
  （`ChainGuard`）与 `CommandManager*`（`Context`），销毁管理器时若还有链没收尾，这些帧一旦恢复
  就会触碰已释放的内存——**这是未定义行为，框架不做运行期兜底**（仅在析构里告警）。
  唯一受支持的形状：管理器与 `Application` 同生灭；`Application::shutdown()` 把命令排空放在第一步
  （`cancelPendingInput()` → `cancelAllAndWait()`，未收干净只告警不阻塞），随后才是插件卸载、
  事件总线排空与配置落盘；之后资源才随析构释放。管理器自身不排空（析构里阻塞线程等协程恢复
  本身就是死锁风险）。2026-09-25 决策（否决共享 `Impl` 的运行期兜底一揽子方案）见"已评估但**未采纳**"。
- **独占性由调用方按语义使用**：Exclusive 会取消**所有**活链（包括 `executeDetached()` 启动的
  后台链）。UI 上"切换工具/模式"这类命令适合用它；一个还需要别的命令继续跑的场景不能用。
- **不要在命令内部排空或关停**：`cancelAllAndWait()`/`Application::shutdown()` 会等待命令自己
  所在的链，永远等不到（只会在上界后返回 false，关停还会带着活帧往下走）。命令要退出应用时
  用 `Application::quit()`（停止主循环），由宿主在 `run()` 返回后收尾。

- **跨线程碰 UI 的写法**：命令可能在任意线程恢复，要回到应用线程就用
  `co_await MainThreadDispatcher::resumeOnMainThread()`（2026-09-17 新增，见
  `MainThreadDispatcher.hpp`；协程式，与事件处理函数里的"自己编组"是同一条规则）；
  没有事件循环时它不挂起，直接在调用线程继续。回调式场景仍用 `postToMainThread()`。

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
- `CommandManager_NestedSyncExecuteCommandDoesNotDeadlock`：命令/协程内同步调 `executeCommandAndWait` 不自锁（但仍建议用 `executeChild()`）。
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
- 第 10 轮新增（2026-09-17）：`CommandManager_ExecuteChildRunsACallerSuppliedInstance`
  （实例进父链：深度 2 而非 1；执行期实例活着、回到父命令时已随帧销毁；未注册名字不进列举）、
  `CommandManager_ExecuteChildRefusesNullAndDisabledInstances`（null ⇒ `Command is null`；
  用户禁用了某名字 ⇒ 父命令拿同名实例也不能绕过）、
  `CommandManager_ExecuteChildBoundsInstanceNesting`（实例形态自我递归同样在 `maxChainDepth()` 处被拒）。
- 第 6 轮新增：`CommandManager_ReentrantFactoryIsSafe`、`CommandManager_ReentrantEventHandlerIsSafe`（重入注册表/事件回调安全）、
  `CommandManager_ExclusiveCommandsAreSerialized`（并发提交的排他命令不重叠）。

## 有意**未做**的最佳实践项（留档）

- **合并 `Context` 与 `CommandExecutionContext`（用 friend）**：不合理，理由见下节。
- **`Impl` 改用 `make_unique`**：仓库的 PImpl 风格是 `d(new Impl(...))`（`ConfigRegistry`/`ThreadPool`/`DynamicLibrary` 等一致）。
- **`VN_DISABLE_COPY_MOVE(CommandManager)`**：`unique_ptr<Impl>` 已隐式删除拷贝/移动，appfw 同类类多数不用该宏。
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
- **运行期"宿主违约"兜底（2026-09-25 实现后否决）**：一揽子方案 = `Impl` 改共享（帧持 `shared_ptr`）、
  链上死亡标记 `orphaned`、三个恢复点守卫（执行尾部的 `report` 前、`Context::executeChild()` 两处、
  接管等待循环）、独占路径"接管前预建链 + 停链扫掠里排除自己"；另有配套的 3 个违约场景用例与
  ASan 变异电池（M1–M5 全红，命中预期 UAF 位置）。整轮实现完工、44 用例全绿后**否决并回退**：
  收益只在"带着活动帧销毁管理器"这一违约场景，而契约已收紧为 manager 跟随 `Application`、退出先
  `cancelAllAndWait()`；代价是运行态跨对象耦合（帧要从链上读死亡标记、接管路径多一条必须在停链扫掠里
  排除自己的预建链——实现期内它就制造过一次"接管被自己取消"的缺陷）。复查条件：出现"管理器短于进程"
  的合法宿主（多管理器/嵌入宿主）时再评估。
- **调度内核 `Scheduler` 类抽取（②，2026-09-25 复核后降级为审计）**：把 `admit`／接管／drain／释放
  提炼为 Impl 私有嵌套类 + 显式状态转移表。**不做**：一是行为中性重排（既有 41 用例只能证明"没改坏"）；
  二是承诺的直接收益"不用 GUI 测转移"在"零签名变化"下不可达——嵌套私有类没有任何测试缝，加缝就是动 API。
  审计价值已由"调度内核转移表"一节承接。真正需要 `Scheduler` 形态的是带队列/优先级的整体重设计，
  等 API 轮一起做。
- **`collectLiveChains()` 快照分配复用（④A，2026-09-25 测量后否决）**：它只有 2 个调用者——
  `cancelLiveChains()`（生产路径仅 `Application::shutdown()` 的 `cancelAllAndWait()`，每进程一次）与
  `takeOverForeground()`（顶层 Exclusive 命令，按用户操作、自带 2s 有界等待）。快照大小 = 活链数
  （典型 0–3）。复用会引入一把锁保护的常驻缓冲，去优化一个每进程一次、O(活链数) 的分配——无对象可优化。
