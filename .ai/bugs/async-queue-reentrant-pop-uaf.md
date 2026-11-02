# Bug: AsyncQueue::pop 唤醒生产者后可被重入销毁（use-after-free）

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `AsyncQueue.hpp`、`When.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

有界队列已满时，一个生产者等待 `push()`；另一个任务从队列 `pop()` 后，等待的生产者被同步恢复。
如果生产者在 `whenAny` 中获胜，`whenAny` 会销毁仍在执行 `pop()` 的任务帧，随后 `pop()` 继续访问已释放的协程帧。

## 根因

`detail::popOne()` 移除队列元素后，在返回值之前恢复等待中的生产者。该恢复可以沿 `whenAny` 完成路径重入并销毁
当前 `AsyncQueue::pop()` 协程帧；`pop()` 的 `co_return co_await` 后续代码仍会在已销毁的帧中运行。
在消费操作返回前恢复任意等待者，因而允许调用栈上的当前协程被其间接销毁。

## 影响与验证

- 最小复现：容量为 1 的队列先放入一个元素；`whenAny` 同时等待一个被队列满状态阻塞的 `push()`，
  以及延迟后执行的 `pop()`。
- Clang 23 + ASan 独立探针复现 heap-use-after-free，读发生于 `AsyncQueue::pop()`，
  释放路径经 `whenAny` 销毁仍在执行的 pop 任务。
- 当前 `test_async` 的 158 项通过，但没有覆盖“pop 唤醒生产者、生产者赢得组合、pop 任务同时被销毁”的场景。

## 修复方向

避免在 `pop()` 尚未完成自身状态转换和返回结果前恢复生产者；设计一个不会在当前操作仍执行时销毁该操作协程的唤醒时序，
并增加上述 `whenAny` + 有界队列回归测试及 ASan 验证。

## 修复

- 队列不再在调用者的栈上 resume 等待者：`push` / `pop` / `tryPush` / `tryPop` / `close` 改变队列后，
  把唤醒交给 `TimerService::postWake()`（`detail::wakeOnePopper` / `wakeOnePusher` / `wakeAll`）。
- 唤醒动作在 timer 线程上锁内取一个 handle、锁外 resume；等待者若已销毁，其 awaiter 已从表中注销。
- 唤醒成为“可能虚假”的：`pop()` / `push()` 醒来后重新检查条件，不满足就重新登记
  （`QueuePopAwaiter` / `QueuePushAwaiter` 可重复 await）。
- `close()` 只投递一个 `wakeAll` 动作；该动作在 timer 线程内逐个 resume，赢家销毁落败者时列表已经更新。

## 验证

- `AsyncQueueTest.PopWakingAProducerDoesNotDestroyTheExecutingPop`：容量 1、满队列的 `push` 与延迟 `pop`
  在 `whenAny` 中竞争，50 轮都要求 `pop()` 返回（修复前会释放仍在返回的 pop 协程帧）。
- `test_async` 在 ASan 下全量通过；修复前同场景 ASan 报 `heap-use-after-free`。
- 语义变化（已写进头文件）：`pop()` 返回表示“值已取走”，不再表示“生产者已经运行”。
