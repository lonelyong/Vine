# async 模块要点（`src/base/async`）

> 详细设计、与 cppcoro/P2300 的差异、已知风险：`.ai/design/async-design.md`。
> 本文件只放"干活时需要立刻想起的东西"。

## 结构

- 纯 header-only，`vn::Async` 是 INTERFACE 目标（只依赖 `vn::Global` + `vn::Core`）。
- 26 个头文件；头文件都在 `sdk/vine/async/`，类在 `namespace vn::async`。
- 任务：`Task<T>`（惰性/单消费者）、`SharedTask<T>`（可复制/缓存结果）、`DetachedTask`（eager/自毁）、`Scope`。
- 组合：`whenAll` / `whenAny`（vector + 变参，含全 void 变参）、`withTimeout`、`retry`、`transform`、`andThen`。
- 原语：`AsyncEvent`、`AsyncLatch`、`AsyncMutex`(+`AsyncLockGuard`)、`AsyncConditionVariable`、`AsyncSemaphore`、
  `AsyncReaderWriterLock`、`AsyncQueue`、`TaskCompletionSource`、`sleepFor`（共享定时服务）、`yield`。
- 概念：`Awaitable`（像 co_await 一样展开 `operator co_await`）、`Schedulable`、`StorableValue`。

## 三条铁律

1. **一个完成唤醒 N 个等待者**：每轮锁内只出队一个，锁外 resume；绝不"整体换出再逐个 resume"
   （`SharedTask` 2026-09-17 的 UAF 就是这么来的）。世代号做边界（`AsyncEvent::set_epoch_`），不用队尾指针。
2. **能对称转移就 `return handle`**，不要 `h.resume()`（否则每层 await 多一帧，`yield` 循环会爆栈）。
3. **绝不跨 resume 持有锁、也不在取消者的栈上 resume**（后者见 `Sleep.hpp::requestAbort` 的 `early_` 队列设计；
   第四轮 `test_gui` 全量回归就是这条踩出来的）。

## 契约与边界

- 挂起中的协程**不能**与"正在完成它的线程"并发销毁（`async_global.hpp`）；**放弃等待**是支持的（awaiter 自我注销）。
- 取消是显式传 `CancellationToken`（= `std::stop_token`）；组合子在取消/超时时**直接销毁**未完成的孩子，
  不是 P2300 的"请求停止 + 等它收尾"。
- `AsyncConditionVariable` 的空等待通知用**粘性标记**保留（偏离 `std::condition_variable`，是刻意设计）。
- `Task`/`SharedTask`/`Generator` 的值类型受 `StorableValue` 约束：**不支持引用/数组/函数型别**（用指针或 `reference_wrapper`）。
- `whenAll`/`whenAny` 变参只支持"全 void"或"全非 void"，混合的用 `vector<AnyTask>` + `discard()`。
- `AsyncMutex::try_lock` / `AsyncSemaphore::try_acquire` 用 snake_case（对齐 std），是仓库 camelCase 规则的**刻意例外**。

## 重复实现（已处理）

- `vn::appfw::async::sleep()` **已删除**（2026-09-17）：与 `vn::async::sleepFor()` 重复且已无调用者，
  连同 `test_asyncqt` 的两个用例一起移除。`appfw::async::Scheduler`（Qt 事件循环调度）不重复，保留。

## 陷阱

- **协程帧的创建线程必须活到协程结束**：在会退出的线程上创建 `DetachedTask` 并让它挂起、再由别的线程恢复
  ⇒ 帧所在的那段栈随线程退出而消失，恢复即 SIGSEGV（2026-09-17 实测：`gdb` 里崩溃点的 `this` 落在
  已退出线程的栈区间）。要跨线程恢复，就让创建线程留在事件循环或 `syncWait` 里：
  `test_asyncqt` 的 `MainThreadDispatcherResumesOnApplicationThread` 是范式——应用线程创建，
  先被定时器线程恢复（断言"确实不在应用线程"），再跳回应用线程。

## 命令

- 构建+跑：`cmake --build build --target test_async && ./build/bin/test_async`（当前 135 条）。
- TSan/ASan 配方、以及"让过期 resume 缺陷变确定"的手法（帧内 padding + `syncWait` 后 settle）见设计文档 §7。
