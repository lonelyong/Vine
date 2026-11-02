# Bug: 销毁挂起的 AsyncConditionVariable::wait 会使 RAII 锁重复解锁

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `AsyncConditionVariable.hpp`、`AsyncMutex.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

持有 `AsyncLockGuard` 的协程调用 `AsyncConditionVariable::wait(mutex)` 后，在通知前被组合子
（例如 `whenAny`）销毁，guard 析构会对已经解锁的 mutex 再次调用 `unlock()`，Debug 构建触发 `locked_` 断言。

## 根因

`wait()` 在挂起前直接调用 `mutex.unlock()`，通知后再重新获取 mutex；但它无法更新调用方仍持有的
`AsyncLockGuard`。若等待期间协程帧被销毁，guard 仍以为自己拥有锁并再次释放。
因此，CV 等待允许的“放弃等待”路径与 RAII 锁的所有权状态不一致。

## 影响与验证

- 独立复现：使用 `co_await lockAsync(mutex)` 获取 guard，随后 `co_await cv.wait(mutex)`；
  以短定时任务作为 `whenAny` 赢家，在通知前销毁 CV waiter。
- Debug 探针触发 `AsyncMutex::unlock()` 中 `assert(locked_)`。
- 既有 `WaiterDestroyedWhileWaiting` 用例以手动 `lock()/unlock()` 方式测试，没有在取消时保留 RAII guard，
  因而没有覆盖此所有权失配路径。

## 修复方向

让 CV wait 的释放、等待、重获与 guard 的所有权状态保持一致；或者明确不支持持有 RAII guard 时放弃 CV 等待，
并使接口能阻止/检测该用法。补充 `lockAsync` + `whenAny` 销毁 waiter 的回归测试。

## 修复

- 新增 `AsyncConditionVariable::wait(AsyncLockGuard&)`：等待期间清空 guard，返回前重新加锁并重新装填同一个
  mutex。协程在等待中被销毁时 guard 为空，析构不会二次 `unlock()`。
- 新增 `AsyncLockGuard::mutex()`，让 `wait()` 在清空前记下 mutex。
- `wait(AsyncMutex&)` 保留给手动 `lock()/unlock()` 的调用者，文档写明“持有 guard 时不要用它”。

## 验证

- `AsyncConditionVariableTest.GuardWaitReacquiresAndReleasesOnce`：等待结束后 guard 重新持有锁，析构只释放一次。
- `AsyncConditionVariableTest.AbandonedGuardWaitDoesNotUnlockTwice`：`whenAny` 销毁等待者后 mutex 仍可加锁，
  Debug 构建不再触发 `AsyncMutex::unlock()` 的 `assert(locked_)`。
- `test_async` Debug 与 ASan 全量通过。
