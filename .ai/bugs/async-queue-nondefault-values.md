# Bug: AsyncQueue::pop 隐式要求元素可默认构造且可赋值

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `AsyncQueue.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

对合法的非默认构造、可移动类型使用 `AsyncQueue<T>::pop()` 会编译失败，即使队列中只需移动出一个已入队元素。

## 根因

`detail::popOne()` 声明 `T value;` 并将队首元素 move-assign 给它，额外要求 `T` 可默认构造且可移动赋值。
AsyncQueue 的模板/API 文档没有声明这些约束；其余队列操作主要以移动构造/移动入队表达值传递。

## 影响与验证

- 编译探针使用删除默认构造函数、提供移动构造和移动赋值的类型实例化 `AsyncQueue<T>::pop()`，
  在 `popOne()` 的默认构造处失败。
- 该类型满足常见队列元素所需的可移动语义，但无法使用 pop API。

## 修复方向

从队首构造返回值，避免无必要的默认构造和赋值要求；对支持的元素类型补充编译期/运行期测试，
包括不可默认构造且可移动构造的类型。

## 修复

- `detail::popOne()` 与 `QueuePopAwaiter::await_resume()` 改为 `std::optional<T>` +
  `emplace(std::move(front))`：只需要 `T` 可移动构造。
- `pop()` 的契约收紧为“`T` 可移动构造”；`tryPop(T&)` 仍需要移动赋值，因为值被写进调用方的 sink，
  这一点已写进类注释。

## 验证

- `AsyncQueueTest.PopSupportsAMoveOnlyTypeWithoutDefaultConstruction`：默认构造与移动赋值都删除的类型
  照样能 `push` / `pop`。
