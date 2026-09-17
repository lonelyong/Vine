# SharedTask 唤醒等待者时 resume 已释放的帧（use-after-free）

**日期**：2026-09-17 ／ **状态**：已修复 ／ **涉及**：`src/base/async/sdk/vine/async/SharedTask.hpp`、
`tests/test_async/AsyncTest.cpp`

## 现象

`SharedTask` 完成时，如果它的等待者多于一个，并且第一个被唤醒的等待者在**它的 resume 过程里**销毁了另一个
等待者，进程就会在 `detail::runShared()` 的 resume 循环里崩掉：

- -O1 + ASan：`heap-use-after-free`，读发生在 `runShared` 的 `h.resume()`，释放发生在
  `~WhenChild() → composeChild() 的 Task::Awaiter::destroy()`。
- 无 sanitizer 的 -O2 构建：多数运行直接 SIGSEGV（exit 139）。
- 最小复现：一个 `SharedTask`，两个 waiter 都在 `whenAny` 里，源任务先挂起 30 ms——
  `whenAny` 的赢家完成时会销毁仍挂起的落败者，正是模块文档里写明的场景。

## 根因

`runShared()` 收尾时把整张等待者链表**整体换出**再逐个 resume：

```cpp
to_resume.swap(state->waiters);
for (auto h : to_resume) h.resume();
```

被换出去的 handle 已经不在共享链表里，兄弟等待者被销毁时无法自我注销（`~SharedTaskAwaiter` 的 `std::erase`
作用在一张空表上），循环于是继续 resume 一个已经释放的协程帧。

这违反了模块自己明文规定的规则：**一个完成唤醒多个等待者时，每轮锁内只出队一个，绝不跨 resume 持有 handle**
（`AsyncEvent::set`、`TaskCompletionSource::resumeWaitersOneByOne`、`AsyncSemaphore::release`、
`AsyncReaderWriterLock::resume`、`AsyncQueue::close`、`TimerService::run` 都遵守）。

## 修复

`runShared()` 改成与 `resumeWaitersOneByOne` 同形的循环：每轮加锁出队一个（队列空即结束，`completed`
在第一次 resume 前发布），锁外 resume。

## 验证

- 新增回归用例 `AsyncDefectRegressionTest.SharedTaskResumesWaitersOneAtATime`（135 条中的一条）。
- 变异反证：`git checkout -- SharedTask.hpp` 重建后，该用例 **10/10 段错误**；恢复修复后 **0/10 失败**。
- 对照：同样的场景换成 `TaskCompletionSource` / `AsyncEvent`（同为"一次完成、多个等待者"）本来就通过，
  证明根因只在 `runShared` 的批量换出。
- 让这类缺陷变确定的手法（可复用）：等待者协程帧做厚到走独立 mmap（帧内 256 KiB padding + 防优化 asm 屏障），
  并在 `syncWait` 之后留一小段 settle —— 过期 resume 发生在 `syncWait` 返回**之后**，进程先退出就看不到了。
  实测：不加固 19/20 命中，加固后 20/20。

## 教训

- 并发容器里"先换出再遍历"看起来更简单，但它**丢掉了被销毁等待者的自我注销机会**；这在本模块是硬性禁忌。
- 现有用例 `SharedTaskTest.WaiterDestroyedBeforeCompletion` 的注释写的就是这条不变式，但场景里只有 1 个
  等待者 ⇒ 不变式要有"≥2 个等待者 + 赢家销毁落败者"的用例才算被钉住。
