# async 模块设计（`src/base/async`）

> 模块笔记：`.ai/memory/async.md`。缺陷记录：`.ai/bugs/shared-task-batch-resume-uaf.md`。
> 本文写"为什么这样设计 / 与其它协程库的差异 / 改动时必须守住什么"。

## 1. 定位

纯 header-only 协程运行时（`vn::async`，`vn::Async` 是 INTERFACE 目标，只依赖 `vn::Global` + `vn::Core`），
27 个头文件、约 6.7k 行（含 `WaiterList.hpp`：等待者注册的 no-throw 助手）

| 类别 | 内容 |
| --- | --- |
| 任务类型 | `Task<T>`（惰性、单消费者）、`SharedTask<T>`（可复制、结果缓存）、`DetachedTask`（eager、自毁）、`Scope`（动态子任务） |
| 组合子 | `whenAll` / `whenAny`（vector 与变参两种形式）、`withTimeout`、`retry`、`transform`、`andThen`、`finally` |
| 同步原语 | `AsyncEvent`、`AsyncLatch`、`AsyncMutex`+`AsyncLockGuard`、`AsyncConditionVariable`、`AsyncSemaphore`、`AsyncReaderWriterLock`、`AsyncQueue`、`TaskCompletionSource`、`sleepFor`、`yield` |

另有：`Generator<T>`（同步单遍生成器，非 awaitable）、`Concepts.hpp`（`Awaitable` / `Schedulable` /
`StorableValue`）、`Scheduler.hpp`（`InlineScheduler`、`resumeOn`、`scheduleOn`）、
`ThreadPoolScheduler.hpp`（`runOn` / `run`，C# `Task.Run` 对应物）。

**不属于**本模块：线程池（`vn::ThreadPool`，base/core）、取消令牌（`vn::CancellationToken` =
`std::stop_token`，base/core）、Qt 事件循环调度（`vn::appfw::async::Scheduler`，fw/appfw）。

## 2. 四条必须守住的规则

### 2.1 生命周期契约（`async_global.hpp` 里写明）

- 挂起中的协程**不能**在它 awaited 的对象正在完成它的过程中被另一个线程销毁（完成线程换出等待者链表 →
  逐个 resume 之间，就是危险窗口）。
- **放弃等待是支持的**：销毁等待中的协程前，awaiter 会在析构里自我注销；库侧保证"已销毁的等待者不会被
  resume"。
- 每个 awaiter 都自带注销逻辑（`AsyncEvent::Awaiter::queued_`、`CvWaiter::queued_`、`AsyncMutex::LockAwaiter::waiter_`、
  `TcsAwaiter::handle_`、`SharedTaskAwaiter::handle_`、`SleepAwaiter::scheduled_` 等），这是"放弃"能成立的全部原因。

### 2.2 一次完成唤醒多个等待者时：**每次锁内只出队一个，锁外 resume**

原因：被 resume 的等待者可能**同步销毁它的兄弟**（`whenAny` 赢家完成时销毁仍挂起的落败者），也可能重新
**登记**新的等待者。因此：

- 不能跨 `resume()` 持有任何 waiter 指针/handle；
- 不能"先把整个链表换出来再逐个 resume"——换出去之后兄弟再也无法自我注销，循环会 resume 已释放的帧
  （2026-09-17 的 `SharedTask` 缺陷就是这一条，见 `.ai/bugs/shared-task-batch-resume-uaf.md`）；
- 边界判定用**世代号**而不是"队尾指针"（`AsyncEvent::set_epoch_`）：被销毁的 waiter 可能是边界节点。

遵守这条的：`AsyncEvent::set`、`AsyncConditionVariable::notify_one/notify_all`、`AsyncSemaphore::release`、
`AsyncReaderWriterLock::resume`、`AsyncQueue::close`、`TaskCompletionSource::resumeWaitersOneByOne`、
`TimerService::run`（除 deadline 与 `early_` 外还多一条 `posted_` 队列：跨线程的唤醒先入队，由定时线程逐个出队 resume）、`SharedTask::runShared`。

### 2.3 对称转移（symmetric transfer）

`Task` 的 awaiter、`TaskFinalAwaiter`、`WhenChild::FinalAwaiter`、`WhenAnyChild::FinalAwaiter`、
`YieldAwaiter::await_suspend` 都 **return handle** 而不是 `h.resume()`：内联 resume 会让每层 await 在机器栈上
多留一帧（实测 `yield` 336 B/次 @-O0，20 万次即爆栈）。新增 awaiter 时先问"这里能不能 return handle"。

### 2.4 阻塞取结果：两道门，风险写在调用点

`Task::result()` 在调用线程上把 body 驱动完（惰性照旧：body 从调用线程开始、跑到第一次挂起）：**只阻塞，
不派发**任何东西。`runToCompletion(task, pump)` 是同一条驱动，但每片（默认 200 µs）调一次 `pump()`。

- `result()` 对应 C# 的 `task.Result`，**连坑都一样**：body 的下一步若需要调用线程继续转（把投递给应用线程的
  调用执行掉），而调用线程正卡在 `result()` 里——死锁。所以它只在"该线程上没有别人需要转循环"时安全
  （工具、测试、headless）；应用线程上要阻塞就该用下面那道门。
- `runToCompletion()` 是 C# 里用 `DispatcherFrame` 排空队列的同款绕法：泵**由调用者传**（appfw 的门传
  `MainThreadDispatcher::deliverPostedCalls()`）。**async 不装"全局泵"**：谁该被推进是宿主的策略，不该由
  async 猜；因此也不给 `Task` 加隐式派发。
- 泵推到什么程度由 pump 决定：appfw 那个只跑 `QEvent::MetaCall`，不跑定时器/绘制/输入，所以它不是
  "重入事件循环"。
- 钉子（`test_async`，两条都变异验过）：挂起时 pump 确实被调用且只一次；**不挂起的任务 pump 一次都不该发生**
  （body 在 `resume()` 里跑完 ⇒ 第一个 `waitFor` 立即返回 ⇒ 零开销）；`result()` 只能读一次——再读一次当场
  `throw std::logic_error`（去掉那道守卫就不是报错而是踩已销毁的帧，实测 SIGSEGV）。

## 3. 与其它协程库的对照（差异都写在这里，避免重复讨论）

| 主题 | cppcoro / folly / P2300 | 本模块 | 原因 |
| --- | --- | --- | --- |
| `Task` 惰性、单消费者 | 相同（`cppcoro::task` 亦为 `operator co_await() &&`） | 同 | — |
| `Task<T&>` | cppcoro 支持 | **不支持**，`StorableValue` 在模板边界拒绕（报错清晰，不再落进 `std::optional`） | 结果用 `std::optional` 缓存；支持引用要改 Task/SharedTask/When 三处，收益低。需要引用时返回指针/`std::reference_wrapper` |
| `Awaitable` 概念 | cppcoro `awaitable_traits` 用 `get_awaiter` 展开成员/非成员 `operator co_await` | 同（`detail::getAwaiter`），并额外校验 `await_ready` 可按语言规则上下文转 bool、`await_suspend` 只能是 `void`/`bool`/`coroutine_handle<>` **值**（引用形式的 handle 被 clang 判错、g++ 目前放过，本概念按标准拒绝） | 只有"像 co_await 一样查找"的概念才能接受本模块自己的 awaitable（Task/SharedTask/AsyncEvent 都靠 `operator co_await`）；返回型别也要查，否则概念会放过编译器会拒绝的写法 |
| `Generator` 的 ranges 契约 | `std::generator`（C++23）是 input_range | 已补齐 `iterator_concept` + 后置 `++` + `operator->`，实测 `std::ranges::input_range` = true | C++20 ranges 只要求后置 `++`，缺它 `weakly_incrementable` 即失败 |
| 取消模型 | P2300：环境 stop token（`get_stop_token`），取消必须**请求**后由操作上报 `set_stopped` | 显式传 `CancellationToken`（同 cppcoro），组合子在取消/超时时**直接销毁**未完成的孩子 | 与 cppcoro 一致；但"强制销毁"比 P2300 激进，见 §4 |
| 无等待者时的 notify | `std::condition_variable`/folly `Baton` 记为丢失 | `AsyncConditionVariable` 用**粘性标记**保留，下一个 waiter 消费 | "释放 AsyncMutex + 登记等待"无法原子，丢掉会死锁；谓词循环写法下多余唤醒无害。这是**刻意偏离**，不要"修"回去 |
| 调度器 | `schedule()`/`resumeOn` 语义一致 | 同 | — |
| `whenAll`/`whenAny` 变参 | P2300 `when_all` 接受 void 子操作（结果里不含 void） | 支持**全 void** 的变参重载（返回 `Task<void>`），混合 void/非 void 仍不支持（用 `vector<AnyTask>` + `discard()`） | 一致地支持混合 void 需要去掉结果 tuple 里的 void 位，属 API 形状改动，未做 |
| 命名 | — | `AsyncMutex::try_lock` / `AsyncSemaphore::try_acquire` 用 snake_case（对齐 `std::mutex` / `std::counting_semaphore`），其余实例方法 camelCase（仓库规则） | 刻意例外；改名属破坏性改动，未做 |

## 4. 已知风险（未证实 / 未处理，改这里前先读）

1. **~~`std::stop_callback` 内同步 resume~~ 已修（2026-09-26）**：`whenRace`/`whenAllImpl`/`whenAny`（typed）/
   `Scope::join` 五处取消回调原先直接 `done.set()`，于是"正持有该 callback 的协程帧"在 callback 执行期间被析构
   （同线程销毁 `stop_callback` 正是标准留白的那一条）。现在统一走 **`TimerService::postWake(owner, action)` +
   `detail::deferWake(state)`**：回调只置 `cancelled` 并把唤醒入队，**resume 由定时线程做**，回调先返回，
   `~stop_callback` 于是变成标准认可的跨线程等待。
   变异实测：把任一处改回 `done.set()` ⇒ `test_async` **不是变红而是直接 abort**
   （`terminate called after throwing an instance of 'std::system_error' what(): Invalid argument`）——
   在这台机器（libstdc++/glibc）上旧写法不是"未定义但侥幸"，而是会死；libc++/MSVC 仍未知。
   钉子：`TaskTest.WhenAllCancellation`、`ScopeTest.JoinCancellationWakesOffTheCancellersStackAndLeavesTheChildRunning`
   （都以取消线程 id 做断言；后者还钉住"取消只停等待、孩子继续跑"）。
   残留暴露面：`postWake` 的 `deque::push_back` 在 OOM 时会在 `noexcept` 回调里失败——与 `requestAbort()` 的
   `early_.push_back` 是同一处暴露；要清零就得换侵入式队列（见 §8）。
2. **取消 = 强制销毁**：组合子在取消/超时时销毁未完成的孩子。孩子若正持锁或持有外部资源，就地销毁是危险的；
   P2300 语义要求先请求停止再等它收尾。头文件已声明是设计选择。
3. **`TimerService` 故意不析构**（进程级单例 + worker 线程），LSan 不报是因为线程让它可达；不要"顺手"加析构。
4. **`Scope` 析构不等待/不取消孩子**：孩子自持（`DetachedTask` + 共享状态），必须在 Scope 离开作用域前
   `co_await join()`，否则 `~ScopeState` 的 `AsyncEvent` 断言会炸。
5. **`SleepAwaiter` 定时线程的最终检查与 resume 之间的窗口**：头文件里写明归调用方契约。

## 5. 重复实现与收尾项

- **`SyncWait.hpp` 已删（2026-09-26）**：阻塞驱动的机制（`detail::WaitEvent`/`WaitTask`/`makeWaitTask`）
  搬到 `Task.hpp`（`result()` 要用它，而成员只能在自己头文件里定义），`syncWait()` 这个自由函数连同头文件
  一并去掉——同一个动作只留一个名字。全仓库 172 处调用点改成 `x.result()`。
- ~~`vn::appfw::async::sleep()`~~：**已于 2026-09-17 删除**（`src/fw/appfw/src/async/Sleep.hpp/.cpp`
  连同 `test_asyncqt` 的两个用例一起移除）：与 `vn::async::sleepFor()` 功能重复，且当时全仓库
  （含测试）已无调用者——"怕动导出符号"的顾虑在调用者为 0 时不成立。
  同目录的 `appfw::async::Scheduler` 与 base 模块不重复（Qt 事件循环调度），保留；它仍是私有头、
  只有 `test_asyncqt` 在用，"提到 `sdk/` 供命令作者做线程回归"见命令管理器模块的后续课题。
- 命名：`sleep` vs `sleepFor` 的不一致同上；`try_lock`/`try_acquire` 的 snake_case 见 §3。

## 6. 加一个新 awaitable 的检查单

1. `await_ready/await_suspend/await_resume` 齐备，`await_suspend` 只返回 `void`/`bool`/handle；能对称转移就 return handle。
2. 注册进共享容器时：**出队与 resume 都按 §2.2**；析构里自我注销，`noexcept`。
3. 不在锁内 resume；不在取消者的栈上 resume（跨线程唤醒一律走 `detail::deferWake()`，实现在 `Sleep.hpp`）。
4. 文档写清：谁拥有帧、放弃等待怎么办、取消语义、需要调用方遵守什么。
5. 用例三件套：正常路径、放弃/销毁路径、变异验证（把实现改回旧写法，用例必须红）。

## 7. 验证配方（实测可用）

- 常规：`cmake --build build --target test_async && ./build/bin/test_async`（当前 145 条：含 9 条 `static_assert` 概念钉子、4 条并发/变异钉子）。
- 门禁：`ctest -R 'test_async|test_asyncqt'`、`QT_QPA_PLATFORM=offscreen ctest -R test_gui`、
  `scripts/check_{include_hygiene,doc_symbols,diagnostic_formats}.py`。
- 并发：TSan 手工编同一套用例（需要一起编 `src/base/core/src/ThreadPool.cpp`、gtest-all、gtest_main）；
  内存：`VINE_ASAN_TARGET=test_async VINE_ASAN_LEAKS=1 scripts/asan_check.sh`。
- **让"resume 已释放帧"这类缺陷变确定**：把等待者协程帧做厚（帧内 256 KiB padding + 不让编译器消掉的
  asm 屏障）→ 帧走独立 mmap → 销毁即 munmap → 过期 resume 必然踩空；再在 `Task::result()` 之后加一小段
  settle（过期 resume 发生在 `Task::result()` 返回**之后**，进程若先退出就看不到了）。
  实测：不加这两样 19/20 命中，加上 20/20；修好的版本 0/20。
- 变异反证：`git checkout -- <头文件>` → 重建 → 跑目标用例必须红 → 立刻恢复并重建（不要让工作区停在变异态）。



## 8. P2：下一步的结论（详细设计见 `.ai/design/async-next.md`）

三项的**形状、依据（含全仓 grep 实测）、验证配方与 go/no-go** 都在 `.ai/design/async-next.md`；
这里只留结论，避免两处说法漂移：

1. **取消的环境（stop token 随任务走）⇒ 建议排期**：appfw 早已把令牌装进上下文对象
   （`PluginLoadContext::stopToken()` / `CommandExecutionContext::stopToken()` / `StartupProgress::stopToken()`），
   而 `vn::async` 的组合子看不见它；建议给它一个"环境式令牌"（显式参数全部保留、不改 ABI 与用例）。
2. **结构化作用域 ⇒ 小改**：`Scope` 在生产代码**0 使用者**，且标准形状是"析构断言、同步点显式 await"
   （stdexec 的 `async_scope`/`counting_scope` 同理）⇒ 析构只做 `request_stop()` + 断言，另加 `detach()`，
   不做"析构里等待"（析构不是协程，等待要么阻塞线程、要么泵循环，那是宿主的策略）。
3. **帧分配 ⇒ 不做，挂账**：协程体只出现在 appfw/app_shell/test_plugin/vsg 插件各若干处，`src/viz`
   渲染路径 **0 个** ⇒ 帧分配不在任何热路径上；判据写在 `async-next.md` §3。

另记一条本轮量到的（不是风险，是"别写没用的守卫"）：`WhenAnyChild::promise_type::return_value()` 里的
`try/catch` 变异掉**不红**——语言本身会把 `return_value` 抛出的异常交给 `unhandled_exception`（今天这条
路径上没有 `noexcept` 边界）。真正承重的是 `WhenAnyChild::FinalAwaiter::await_suspend()` 里那个守卫
（删掉 ⇒ 直接 abort）。守卫暂时都留：`return_value` 那个是防"将来把它挪进 `noexcept` 的 `start()`"。

另：`Concepts.hpp` 的 `Awaitable` 有意窄化——`await_suspend` 只用类型擦除的 `coroutine_handle<>` 探测，所以
"在签名里点名 promise 型 handle"的 awaiter（正是本模块 `When.hpp` 的 final awaiter 形状）**不满足**该概念，
而语言是允许的。原因已写在头文件里："some `coroutine_handle<P>`" 无法表达为概念，用一个探针 promise 去
近似只会接受错误的类型。
