# async 模块设计（`src/base/async`）

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：纯 header-only 协程运行时的生命周期、组合子、取消与阻塞驱动。
> **权威来源**：`src/base/async/sdk/vine/async/**`、`tests/test_async`。
> **待办**：`../backlog/async.md`

## 1. 概要设计

### 1.1 问题与背景

`vn::async` 是纯 header-only 协程运行时（`vn::Async` 是 INTERFACE 目标，只依赖 `vn::Global` + `vn::Core`），
约 28 个头文件、6.9k 行。它要提供任务、组合子、同步原语和调度器，同时把“一个完成唤醒多个等待者”这类
生命周期窗口写死成不可违反的规则。

### 1.2 目标

- `Task<T>`（惰性、单消费者）、`SharedTask<T>`（可复制、结果缓存）、`DetachedTask`（eager、自毁）、`Scope`。
- 组合子：`whenAll` / `whenAny` / `withTimeout` / `retry` / `transform` / `andThen` / `finally`。
- 同步原语：`AsyncEvent`、`AsyncLatch`、`AsyncMutex`、`AsyncConditionVariable`、`AsyncSemaphore`、
  `AsyncReaderWriterLock`、`AsyncQueue`、`TaskCompletionSource`、`sleepFor`、`yield`。
- 调度：`InlineScheduler`、`resumeOn`、`scheduleOn`、`ThreadPoolScheduler`。

### 1.3 非目标

- 不拥有线程池（`vn::ThreadPool` 在 base/core）、不拥有取消令牌（`vn::CancellationToken` =
  `std::stop_token`）、不拥有 Qt 事件循环调度（`appfw::async::Scheduler`）。
- 不提供隐式全局泵；阻塞驱动由调用方传入 pump。
- 不把 `Task<T&>`、混合 void 的变参 `whenAll` 做成核心形状。

### 1.4 一句话结论

协程帧的生命周期只有一条铁律：**一次完成唤醒多个等待者时，每轮锁内只出队一个、锁外 resume**；
其余设计（对称转移、两道阻塞门、显式取消策略）都是为这条铁律服务的。

## 2. 设计要点

### 2.1 生命周期契约

- 挂起中的协程**不能**在它 awaited 的对象正在完成它的过程中被另一个线程销毁。
- **放弃等待是支持的**：销毁等待中的协程前，awaiter 在析构里自我注销；库侧保证“已销毁的等待者不会被 resume”。
- 每个 awaiter 自带注销逻辑（`AsyncEvent::Awaiter::queued_`、`CvWaiter::queued_`、
  `AsyncMutex::LockAwaiter::waiter_`、`TcsAwaiter::handle_`、`SharedTaskAwaiter::handle_`、
  `SleepAwaiter::scheduled_` 等）。

### 2.2 一次完成唤醒多个等待者：每轮锁内只出队一个，锁外 resume

被 resume 的等待者可能同步销毁它的兄弟（`whenAny` 赢家完成时销毁仍挂起的落败者），也可能重新登记新的等待者。
因此：

- 不能跨 `resume()` 持有任何 waiter 指针/handle；
- 不能“先把整个链表换出来再逐个 resume”——换出去之后兄弟再也无法自我注销，循环会 resume 已释放的帧
  （2026-09-17 的 `SharedTask` 缺陷就是这一条）；
- 边界判定用**世代号**（`AsyncEvent::set_epoch_`），不用“队尾指针”——被销毁的 waiter 可能是边界节点。

遵守这条的：`AsyncEvent::set`、`AsyncConditionVariable::notify_one/notify_all`、`AsyncSemaphore::release`、
`AsyncReaderWriterLock::resume`、`AsyncQueue::close`、`TaskCompletionSource::resumeWaitersOneByOne`、
`TimerService::run`、`SharedTask::runShared`。

### 2.3 对称转移

`Task` 的 awaiter、`TaskFinalAwaiter`、`WhenChild::FinalAwaiter`、`WhenAnyChild::FinalAwaiter`、
`YieldAwaiter::await_suspend` 都 **return handle** 而不是 `h.resume()`：内联 resume 会让每层 await 在机器栈上
多留一帧（实测 `yield` 336 B/次 @-O0，20 万次即爆栈）。新增 awaiter 时先问“这里能不能 return handle”。

### 2.4 阻塞取结果：两道门

- `Task::result()`：在调用线程上把 body 驱动完（惰性照旧）；**只阻塞，不派发**。对应 C# `task.Result`，
  只在“该线程上没有别人需要转循环”时安全（工具、测试、headless）。
- `runToCompletion(task, pump)`：同一条驱动，每片（默认 200 µs）调一次 `pump()`。appfw 的门传
  `MainThreadDispatcher::deliverPostedCalls()`；async 不装“全局泵”。
- 钉子：挂起时 pump 确实被调用且只一次；不挂起的任务 pump 一次都不该发生；`result()` 只能读一次，
  再读一次 `throw std::logic_error`。

### 2.5 与其它协程库的差异

| 主题 | 本模块 | 原因 |
| --- | --- | --- |
| `Task<T&>` | 不支持，`StorableValue` 在模板边界拒绕 | 结果用 `std::optional` 缓存；支持引用要改三处，收益低。 |
| `Awaitable` 概念 | 额外校验 `await_ready` 可转 bool、`await_suspend` 只能是 `void`/`bool`/handle 值 | 只有“像 co_await 一样查找”的概念才能接受本模块自己的 awaitable。 |
| `Generator` 的 ranges 契约 | 补齐 `iterator_concept` + 后置 `++` + `operator->` | C++20 ranges 只要求后置 `++`，缺它 `weakly_incrementable` 失败。 |
| 取消模型 | 显式 `CancellationToken` + `CancelPolicy`（`StopAndWait` / `Destroy`） | P2300 环境 token 与 C# 显式令牌两个阵营都提供；默认值按适用面选。 |
| 无等待者时的 notify | `AsyncConditionVariable` 用粘性标记保留 | “释放 AsyncMutex + 登记等待”无法原子，丢掉会死锁。 |

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 任务类型 | `Task` / `SharedTask` / `DetachedTask` / `Scope` | 已落地 |
| F2 | 组合子 | `whenAll` / `whenAny` / `withTimeout` / `retry` / `transform` / `andThen` / `finally` | 已落地 |
| F3 | 同步原语 | event/latch/mutex/cv/semaphore/rwlock/queue/TCS/sleep/yield | 已落地 |
| F4 | 调度与生成器 | `InlineScheduler` / `resumeOn` / `scheduleOn` / `ThreadPoolScheduler` / `Generator` | 已落地 |
| F5 | 取消策略 | `CancelPolicy::StopAndWait` / `Destroy` | 已落地 |
| F6 | 阻塞驱动 | `Task::result()` / `runToCompletion(task, pump)` | 已落地 |

### 3.1.1 F1 任务类型

- **行为**：`Task<T>` 惰性、单消费者；`SharedTask<T>` 可复制、结果缓存；`DetachedTask` eager、自毁；
  `Scope` 管动态子任务。
- **约束**（C1）：`Task` 必须被 `co_await` 或 `result()` 驱动；`SharedTask` 的结果只能缓存一次。
- **要求**（R1）：`Task<T&>` 不支持；需要引用时返回指针/`std::reference_wrapper`。
- **验收**（T1）：`test_async` 的 Task/SharedTask/Scope 用例。
- **注意**（N1）：`Scope` 析构不等待/不取消孩子，离开作用域前必须 `co_await join()`。

### 3.1.2 F2 组合子

- **行为**：`whenAll` 等全部，`whenAny` 等第一个，`withTimeout` 超时取消，`retry`/`transform`/`andThen`/`finally`
  组合结果。
- **约束**（C2）：`whenAll` 默认 `StopAndWait`；`whenAny` / `withTimeout` / `Scope::join` 默认 `Destroy`。
- **要求**（R2）：不读环境令牌的孩子会拖住 `StopAndWait`；要“绝不挂住”显式选 `Destroy`。
- **验收**（T2）：`TaskTest.WhenAllStopAndWaitIsHeldByAChildThatIgnoresTheToken` 等。
- **注意**（N2）：`Destroy` 不做“先请求再销毁”；请求只会在几微秒后销毁帧时触发自己的 `stop_callback`。

### 3.1.3 F3 同步原语

- **行为**：event/latch/mutex/cv/semaphore/rwlock/queue/TCS/sleep/yield。
- **约束**（C3）：一次完成唤醒多个等待者必须按 §2.2；析构里自我注销且 `noexcept`。
- **要求**（R3）：不在锁内 resume；不在取消者的栈上 resume；跨线程唤醒走 `detail::deferWake()`。
- **验收**（T3）：`test_async` 的各个原语用例 + ASan/TSan。
- **注意**（N3）：`AsyncConditionVariable` 的粘性标记是刻意偏离，不要“修”回丢失唤醒。

### 3.1.4 F4 调度与生成器

- **行为**：`InlineScheduler` / `resumeOn` / `scheduleOn` / `ThreadPoolScheduler`；`Generator<T>` 同步单遍生成器。
- **约束**（C4）：`Generator` 不是 awaitable；`ThreadPoolScheduler` 的 `run` 对应 C# `Task.Run`。
- **要求**（R4）：调度器只提供 `schedule()`/`resumeOn`，不替调用方决定线程策略。
- **验收**（T4）：`test_async` 的 Scheduler/Generator 用例。
- **注意**（N4）：`sleepFor` 的定时线程恢复与最终检查之间的窗口归调用方契约。

### 3.1.5 F5 取消策略

- **行为**：外部取消触发 `CancelPolicy`；`StopAndWait` 先 `request_stop()` 再等最后一个孩子收尾；
  `Destroy` 直接销毁。
- **约束**（C5）：取消是**请求**，由操作上报；`Destroy` 不做“先请求再销毁”。
- **要求**（R5）：注入的令牌只在被组合的那一层可见；包装层不转发就切断通道。
- **验收**（T5）：`TaskTest.WhenAllCancellation`、`ScopeTest.JoinCancellation…`。
- **注意**（N5）：`std::stop_callback` 内不直接 resume；统一走 `TimerService::postWake()` + `deferWake()`。

### 3.1.6 F6 阻塞驱动

- **行为**：`Task::result()` 同步驱动；`runToCompletion(task, pump)` 每片调 pump。
- **约束**（C6）：`result()` 只阻塞、不派发；`runToCompletion` 的 pump 由调用方给。
- **要求**（R6）：应用线程上阻塞必须用 `runToCompletion` + `deliverPostedCalls()`，不能用 `result()`。
- **验收**（T6）：`test_async` 的 `result()`/`runToCompletion` 用例；appfw 的同步插件门。
- **注意**（N6）：`Task::result()` 只能读一次，再读 `throw std::logic_error`。

## 4. 公开接口

### 4.1 头文件与模块

| 接口/头 | 契约 | 备注 |
| --- | --- | --- |
| `Task.hpp` / `SharedTask.hpp` / `DetachedTask.hpp` / `Scope.hpp` | 任务类型 | 见 F1 |
| `When.hpp` / `WithTimeout.hpp` / `Retry.hpp` / `TaskCombinators.hpp` / `Finally.hpp` | 组合子 | 见 F2 |
| `AsyncEvent.hpp` / `AsyncLatch.hpp` / `AsyncMutex.hpp` / `AsyncConditionVariable.hpp` / `AsyncSemaphore.hpp` / `AsyncReaderWriterLock.hpp` / `AsyncQueue.hpp` / `TaskCompletionSource.hpp` | 同步原语 | 见 F3 |
| `Scheduler.hpp` / `ThreadPoolScheduler.hpp` / `Generator.hpp` / `Concepts.hpp` | 调度/生成器/概念 | 见 F4 |
| `StopToken.hpp` / `CancelPolicy` | 取消 | 见 F5 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | `Task` 必须被驱动；`SharedTask` 结果只缓存一次；`Task<T&>` 不支持。 |
| C2 | `whenAll` 默认 `StopAndWait`；`whenAny`/`withTimeout`/`Scope::join` 默认 `Destroy`。 |
| C3 | 一次完成唤醒多个等待者：每轮锁内只出队一个，锁外 resume。 |
| C4 | 不在锁内 resume；不在取消者栈上 resume；跨线程唤醒走 `deferWake()`。 |
| C5 | 取消是请求；`Destroy` 不做“先请求再销毁”。 |
| C6 | `result()` 只阻塞不派发；`runToCompletion` 的 pump 由调用方给。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 每个 awaiter 自带注销逻辑，析构 noexcept；已销毁的等待者不会被 resume。 |
| R2 | 能对称转移就 return handle，不内联 resume。 |
| R3 | 应用的阻塞门用 `runToCompletion` + `deliverPostedCalls()`。 |
| R4 | 注入的取消令牌只对被组合的那一层可见。 |
| R5 | `Scope` 析构不等待/不取消孩子；离开作用域前显式 `join()`。 |
| R6 | 不给 `Task` 加隐式派发，不装全局泵。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 协程帧不能在完成线程换出等待者链表与逐个 resume 之间被另一线程销毁。 |
| N2 | `AsyncEvent` 的边界判定用世代号，不用队尾指针。 |
| N3 | `AsyncConditionVariable` 的粘性标记保留无等待者时的 notify。 |
| N4 | `TimerService` 故意不析构（进程级单例 + worker 线程）。 |
| N5 | `SharedTask` 的等待者逐个出队，绝不跨 resume 持有 handle。 |

## 6. 注意事项与陷阱

### 6.1 `std::stop_callback` 内同步 resume

- 会怎么错：正持有 callback 的协程帧在 callback 执行期间被析构，`~stop_callback` 触发标准留白（实测 abort）。
- 怎么判：`whenRace`/`whenAllImpl`/`whenAny`/`Scope::join` 直接 `done.set()` 会红。
- 怎么防：统一走 `TimerService::postWake()` + `detail::deferWake()`，resume 由定时线程做。

### 6.2 `TimerService` 的 OOM 暴露面

- 会怎么错：`postWake` 的 `deque::push_back` 在 `noexcept` 回调里失败。
- 怎么判：注入分配失败；与 `requestAbort()` 的 `early_.push_back` 同一处。
- 怎么防：要清零就换侵入式队列；目前挂账。

### 6.3 `Scope` 析构不等待

- 会怎么错：离开作用域时孩子还在跑，`~ScopeState` 的 `AsyncEvent` 断言炸。
- 怎么判：没有 `co_await join()` 的 `Scope`。
- 怎么防：离开作用域前显式 `join()`；析构只 `request_stop()` + 断言。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | `Task<T&>` | 结果用 `optional` 缓存；支持引用要改 Task/SharedTask/When 三处，收益低。 | — |
| D2 | 混合 void 的变参 `whenAll` | 要去掉结果 tuple 里的 void 位，属 API 形状改动，未做。 | — |
| D3 | 隐式全局泵 / `Task` 自动派发 | 谁该被推进是宿主的策略，不该由 async 猜。 | — |
| D4 | `SyncWait.hpp` / `syncWait()` | 与 `Task::result()` 重复；机制搬进 `Task.hpp`，只留一个名字。 <!-- drift-ok --> | — |
| D5 | `appfw::async::sleep()` | 与 `vn::async::sleepFor()` 重复，调用者为 0。 | — |
| D6 | 把 `try_lock`/`try_acquire` 改成 camelCase | 对齐 `std::mutex`/`std::counting_semaphore`；改名属破坏性改动。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 任务类型正常/放弃/销毁路径 | `test_async` 的 Task/SharedTask/Scope 用例 |
| T2 | 组合子取消与超时 | `TaskTest.WhenAllCancellation`、`WhenAllStopAndWaitIsHeldByAChildThatIgnoresTheToken` |
| T3 | 同步原语无丢失唤醒/无 UAF | `test_async` + ASan/TSan |
| T4 | 阻塞驱动只泵一次/不泵 | `test_async` 的 `result()`/`runToCompletion` 用例 |
| T5 | 变异反证：改回旧写法必须红 | `git checkout -- <头文件>` → 重建 → 跑目标用例 |

## 9. 状态与演进

### 9.1 当前状态

任务/组合子/同步原语/调度器均已落地；取消策略按 `StopAndWait`/`Destroy` 收口；P2 的三项见 backlog。

### 9.2 已知缺口 / 待办

- 取消的环境令牌（stop token 随任务走）建议排期；详见 `../backlog/async.md`。
- 结构化作用域小改（析构断言 + 显式 `join()` + `detach()`）。
- 帧分配不做，挂账；判据在 backlog §3。

### 9.3 兼容性 / 迁移

`SyncWait.hpp` / `syncWait()` 已删除，调用点改 `x.result()`； <!-- drift-ok -->
`appfw::async::sleep()` 已删除，改用 `vn::async::sleepFor()`。新代码按 §4 的头文件写。

## 10. 参考

### 10.1 代码

- `src/base/async/sdk/vine/async/**`
- `src/base/core/sdk/vine/ThreadPool.hpp`
- `tests/test_async`

### 10.2 文档与 ADR

- `.ai/memory/async.md`
- `../backlog/async.md`
- `../bugs/shared-task-batch-resume-uaf.md`

### 10.3 外部资料

- cppcoro / folly / P2300 的对应语义（见 §2.5）。
- `std::stop_token` / `std::stop_callback` 的标准留白。
