# Bug: runOn 无法转发 move-only 值参数

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `ThreadPoolScheduler.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

将 move-only 值参数（例如 `std::unique_ptr`）传给 `runOn()` 时，调用无法编译，即使 callable 能按值接收该参数。

## 根因

`runOn(vn::ThreadPool&, F f, Args... args)` 按值保存 callable 和参数，但在协程恢复后以
`std::invoke(f, args...)` 调用。具名局部变量是左值，因而不能移动到需要按值接收 move-only 对象的 callable。
返回类型却以 `std::invoke_result_t<F, Args...>` 推导，形成了类型推导与实际调用类别不一致。

## 影响与验证

- 独立最小编译探针以 `std::unique_ptr` 作为值参数，确认调用 `runOn()` 编译失败。
- 现有 `runOn` 测试未覆盖 move-only 值参数。
- 同一问题也影响默认线程池重载，因为它转发到相同的调用实现。

## 修复方向

以与 API 按值接收语义一致的值类别调用保存的 callable 和参数，并补充 move-only 参数回归测试；
同时验证普通值参数、引用包装及 void/非 void 返回路径。

## 修复

- `runOn()` 以 `std::move(f), std::move(args)...` 调用任务帧里的副本，与 `std::invoke_result_t<F, Args...>`
  描述的值传递语义一致。
- `run()` 转发到同一实现，默认线程池重载因而一并修复。

## 验证

- `ThreadPoolSchedulerTest.RunOnMovesMoveOnlyArgumentsIntoTheCallable`：`std::unique_ptr` 值参数可编译并运行。
- 既有 `RunOnPool` / `RunOnPoolVoid` / `RunUsesDefaultPool` / `RunVoidUsesDefaultPool` 仍通过。
