# Bug: AsyncQueue 的 noexcept 路径遇到分配或元素移动异常会 terminate

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `AsyncQueue.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

AsyncQueue 创建共享状态或入队时若分配失败，或者元素移动构造抛异常，异常可能越过 `noexcept` 边界并终止进程，
而不是作为操作失败传播。

## 根因

- `AsyncQueue` 构造函数声明 `noexcept`，但成员初始化使用 `std::make_shared`，分配失败会抛 `std::bad_alloc`。
- `QueuePushAwaiter` 构造函数声明 `noexcept`，但按值保存 `T` 时移动构造可能抛异常。
- 有界 push 的 `await_suspend()` 声明 `noexcept`，并在其中调用 `enqueueLocked()`；队列的
  `deque::push_back` 分配或 `T` 移动构造可能抛异常。

## 影响与验证

- 这是基于异常规格和调用路径的代码审查发现，未通过分配故障注入或 throwing-move 运行时探针验证。
- `T` 的公开约束没有要求 nothrow 移动构造；构造函数也没有说明分配失败会终止。
- 普通非挂起路径 `await_resume()` 中的入队没有相同的 `noexcept` 边界，但仍可能因 awaiter 构造失败而终止。

## 修复方向

统一队列的失败契约：移除不成立的 `noexcept`，或将可能失败的入队操作改成可从 awaiter 正常报告的错误通道；
同时明确是否要求 `T` 为 nothrow-movable。对分配失败和 throwing-move 路径增加测试或编译期约束。

## 修复

- `AsyncQueue` 构造函数去掉 `noexcept`：`make_shared` 的分配失败可以正常抛给调用者。
- `QueuePushAwaiter` 构造函数去掉 `noexcept`：`T` 的移动构造可以抛。
- `QueuePushAwaiter::await_suspend` 仍必须 `noexcept`：入队失败（移动抛、deque 分配失败）捕获进 `failure_`，
  由 `await_resume()` 在协程自己的栈上重抛，与 `detail::tryRegisterWaiter` 的约定一致。
- `T` 不要求 nothrow-movable；移动抛异常会作为 task 的错误报告。

## 验证

- `AsyncQueueTest.AThrowingMoveFailsThePushInsteadOfTerminating`：把 `T` 的每一次移动依次设为抛异常，
  每次都必须作为 task 的错误交给调用者（修复前会 `std::terminate`）。
- `test_async` Debug 与 ASan 全量通过。
