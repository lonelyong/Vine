# Bug: Scope 停止后重新添加任务仍收到已停止的令牌

- **模块**: `src/base/async`
- **状态**: 已修复
- **涉及**: `Scope.hpp`、`tests/test_async/AsyncTest.cpp`

## 现象

`Scope` 的当前批次停止后，后续 `add()` 开始的新批次仍收到已请求停止的 stop token。
孩子在开始运行时读取 `currentStopToken()`，会发现令牌已经停止，即使新批次并未被请求停止。

## 根因

`Scope::add()` 在 `pending == 0` 时重置完成事件和异常状态，将其作为新批次开始；但它没有重建
`source_`。此前 `requestStop()` 对 `source_` 的停止状态是持久的，故新孩子注入的仍是已停止令牌。

## 影响与验证

- 复现步骤：添加并完成一批任务，调用 `requestStop()`，再添加任务并读取 `currentStopToken()`；
  第二批任务观察到 `stop_requested() == true`。
- 独立最小探针已复现；`test_async` 现有测试未覆盖停止后的批次重用。
- 这使“新批次”重置了完成状态，却没有重置取消状态；调用者无法仅凭 `add()` 的现有行为区分新批次是否仍受旧停止请求影响。

## 修复方向

明确 `requestStop()` 与批次重用的契约：若新批次应独立运行，则为该批次提供新的 stop source；
若停止是整个 `Scope` 生命周期不可逆的，则应明确拒绝停止后的 `add()`，而不是悄然启动一个已取消的孩子。
补充回归测试覆盖停止后再次 `add()` 的令牌状态。
## 修复

- `Scope::add()` 在 `pending == 0` 开启新批次时一并执行 `source_ = std::stop_source{}`，与 `done` /
  `first_exception` 一起重置。停止请求从此只作用于它被提出的那个批次，也是 `requestStop()` 文档写的语义。

## 验证

- `ScopeTest.ABatchAfterRequestStopGetsAFreshUnrequestedToken`：新批次仍拿到 scope 的令牌
  （`stop_possible()`），但 `stop_requested()` 为 false。
- 既有 `ChildrenRunInTheScopesEnvironment...`、`JoinStopAndWait...`、`ADetachedScope...` 仍通过。