# Bug: withCancellation 未将 token 传给被包装任务

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `Cancellation.hpp`、`StopToken.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

`withCancellation(token, task)` 只在包装任务开始时检查 token；被包装任务无法从
`currentStopToken()` 读取该 token，运行中请求停止也不会自动到达被包装任务。

## 根因

包装函数调用 `throwIfCancelled(token)` 后直接 await 原任务，没有通过 `TaskEnvironment::setToken()` 或
`withStopToken()` 将 token 注入其 promise。函数注释称 token 会“passed down”，但实现只提供启动前检查。

## 影响与验证

- 独立探针用可取消 token 包装一个读取 `currentStopToken()` 的任务；任务观察到
  `stop_possible() == false`。
- 现有 `WithCancellation` 用例只测试已取消时拒绝启动及空 token 时正常完成，未测试 token 是否进入子任务或运行中取消。
- 因此调用者若认为该组合子会传播取消，子任务将继续运行，除非另有显式 token 通道。

## 修复方向

明确并统一 API 语义：若它是环境包装器，应在子任务启动前注入 token，并定义运行中取消如何表现；
若只负责启动前检查，则改名/改文档，避免暗示传递取消。补充环境可见性和运行中请求停止的测试。

## 修复

- `withCancellation()` 在 `throwIfCancelled()` 之后调用 `detail::TaskEnvironment::setToken(task, token)`，
  与 `withStopToken()` 和组合子的注入方式一致；随后 await 原任务。
- 注释改为：token 成为任务的运行环境，运行中停止只能被读过该令牌的代码观察到（协作式）。

## 验证

- `TaskTest.WithCancellationHandsItsTokenToTheTaskEnvironment`：任务体内 `currentStopToken()` 的
  `stop_possible()` 为 true。
- `TaskTest.WithCancellationCancelsTheTaskWhileItRuns`：任务在 sleep 中，外部 request_stop 后 1 s 内醒来
  （未注入时会跑满 2 s 的 deadline）。
- 既有 `WithCancellationThrowsWhenCancelled` / `WithCancellationRunsTask` 仍通过。
