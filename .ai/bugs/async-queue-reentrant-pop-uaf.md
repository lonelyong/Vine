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

- 交接规则：交出一个槽位/一个元素的操作**先挂起自己**（`detail::WakePeerAndContinue`），再由 timer 线程上的
  一个动作先唤醒同伴、后恢复自己。同伴赢得 `whenAny` 时，`Destroy` 销毁的是一个**已挂起**的协程帧
  （以及同样挂起的祖先帧），这是合法销毁点。
- 只把唤醒投递给 timer 线程不够：操作自己仍在它的线程上执行，同伴的 `Destroy` 会销毁一个正在运行的帧
  （2026-11-02 复验：worker 线程执行 `pop()`、生产者由 timer 唤醒，ASan 报 `heap-use-after-free`）。
- 等待者醒来后重新检查条件，不满足就重新登记（`QueuePopAwaiter` / `QueuePushAwaiter` 可重复 await）。
- `WakePeerAndContinue` 的析构会注销自己的 handle：操作先被销毁时，动作只唤醒同伴、不恢复已亡的帧。
  剩下无法自我挂起的 `tryPush` / `tryPop` / `close` 只能投递唤醒，头文件写明了这一点。

## 验证

- `AsyncQueueTest.PopWakingAProducerDoesNotDestroyTheExecutingPop`：容量 1、满队列的 `push` 与延迟 `pop`
  在 `whenAny` 中竞争（50 轮）；ASan 下不再报错，修复前同场景（worker 线程 + `AsyncEvent` 唤醒 popper）
  报 `heap-use-after-free`，读发生在 popper 的 resume 上。
- `AsyncQueueTest.PopHandsAFreedSlotOverBeforeItReturns` / `PushHandsTheItemOverBeforeItReturns`：交接是
  有顺序的——`pop()` / `push()` 返回时，被唤醒的同伴已经运行过。
- `test_async` 在 Debug 与 ASan 下全量通过。
- 语义变化（已写进头文件）：`pop()` / `push()` 要等同伴先跑完才返回；若它在 `Destroy` 组合里是落败者，
  会在这个挂起点被销毁（`pop()` 已取走的值随之丢弃）。
