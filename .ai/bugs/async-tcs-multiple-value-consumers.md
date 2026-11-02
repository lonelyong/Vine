# Bug: TaskCompletionSource 多个 value task 会移动同一份结果

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `TaskCompletionSource.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

从同一个 `TaskCompletionSource<T>` 获取多个 `task()` 并等待时，只有第一个消费者取得完整结果；
后续消费者读取的是已被移动的对象。若多个已完成任务从不同线程同时读取，结果对象还可能发生数据竞争。

## 根因

`TcsState<T>` 只保存一个 `std::optional<T>`，而每个 `TcsAwaiter::await_resume()` 都执行
`std::move(*state_->value)`。但公开契约同时允许从同一 source 获取多个独立任务，并称它们会一起完成，
且 setResult 文档称结果交付给每个等待任务；实现却按单消费者移动。

## 影响与验证

- 独立探针创建两个 `TaskCompletionSource<std::string>::task()`，设置结果为 `"payload"` 后依次读取，
  输出为 `payload|`，第二个结果为空。
- 现有 `MultipleWaiters` 用例用的是 `TaskCompletionSource<int>`（`int` 的移动等于拷贝，恰好掩盖了该缺陷），
  void source 的用例叫 `VoidSource`；两者都没有检查多消费者读到的值是否完整。
- 尚未对并发 await-ready 后同时 `await_resume()` 的路径运行 TSan；现有无锁移动共享结果的实现存在该风险。

## 修复方向

先明确结果契约：多消费者共享只读值、对可复制类型复制、或将有值 source 定义为单消费者。
随后使状态类型、await_resume、API 文档及 move-only 类型约束一致，并为多个 value task 添加顺序与并发测试。

## 修复

- `TcsAwaiter::await_resume()` 返回 `T`：可拷贝构造的 `T` 每次拷贝一份，多个任务（含并发 await）都读到完整值；
  只可移动的 `T` 仍接单消费者移动，并在类注释 / `task()` / `setResult()` 文档里写明这一限制。
- `TcsState<T>` 的注释同步改为“拷贝给每个等待者；move-only 只能取一次”。

## 验证

- `TaskCompletionSourceTest.EveryValueTaskReceivesItsOwnCopy`：两个 `std::string` 任务都读到 `"payload"`。
- 既有 `MultipleWaiters` / `VoidSource` / 并发 `setResult` 用例仍通过。
