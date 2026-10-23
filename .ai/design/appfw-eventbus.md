# appfw EventBus 设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：事件总线、订阅/取消、投递与关停；不覆盖 `Signal` 本体的实现细节。
> **权威来源**：`EventBus.hpp`、`tests/test_gui/EventBusTest.cpp`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

appfw 需要一条进程级事件总线：订阅者可以活到 bus 之后，投递可以跨线程/排队，关停要能证明“没有线程还在总线内”。
过去 `stopped` 检查与 `active_calls++` 分开、排队投递持有 bus 指针，导致 UAF 与“关停结果说谎”。

### 1.2 目标

- 出圈对象（`Subscription`、排队任务）不持有指向 bus 的指针或强引用。
- 取消语义一致：`unsubscribe()`/句柄析构/`shutdown()`/bus 析构都取消“尚未开始调用”的 handler。
- `shutdown()`/析构立即释放排队事件；异常不穿出 `publish()`/队列回调/Qt 事件循环。
- 锁内不跑用户代码。

### 1.3 非目标

- 不做“关停时杀掉正在跑的 handler”；已开始的调用跑完。
- 不保证 `shutdown()` 等待在飞调用（只有 `shutdownGracefully()` 与析构等待）。
- 不把事件循环的定时器/绘制/输入一起泵（只派发 `QEvent::MetaCall`）。

### 1.4 一句话结论

`EventBus` 的每一条出圈路径都只持 `shared_ptr` 状态/投递，不持 bus 指针；准入计数、关闭状态机和投递注册表
一起保证“graceful 返回 true ⇒ 没有线程还在总线内”。

## 2. 设计要点

### 2.1 结构

```
EventBus ── Impl ─┬─ map<TypeId, EventChannel>   (channels, std::shared_mutex)
                  ├─ shared_ptr<DeliveryRegistry>  (排队的投递，与 bus 解耦)
                  └─ atomic<bool> stopped
EventChannel ──── vector<SubscriptionEntry{mode, shared_ptr<SubscriptionState>}> + std::mutex
SubscriptionState  const id / const Handler / atomic<bool> active
Subscription     ── shared_ptr<Control>  (Control 持 SubscriptionState，析构即 deactivate)
Payload          ── weak_ptr<SubscriptionState> + shared_ptr<const Object> event
Delivery         ── shared_ptr<DeliveryRegistry> + weak_ptr<Payload>；析构时从注册表注销
队列 λ            ── 只捕获 shared_ptr<Delivery>（不捕获 bus/channel/this）
```

`MainThreadDispatcher` 由 `Application` 创建并注入 `EventBus`；`ApplicationData` 中 `main_dispatcher`
先于 `event_bus` 声明，析构时后销毁。

### 2.2 准入与关闭状态机

- `publish()`/`subscribe()` 先通过 `Impl::CallGuard` 进入总线；`state` 与 `active_calls` 在同一把
  `call_mutex` 下，所以“总线已开始停止”与“有调用者进来了”不会交叉。
- `Running → Stopping → Stopped`；进入 `Stopping` 时同时置锁外可见的 `stopped`。
- 并发 `shutdown()`/`shutdownGracefully()` 的非持有者等到 `Stopped` 并拿到同一次关闭的结果。
- 状态机只在 `Impl` 一处实现：`beginStop()` / `finishStop(clean)` / `stoppingOnThisThread()` /
  `awaitStopped()` / `awaitStoppedUntil()`。

### 2.3 投递规则

- 一个订阅恰好收到那些 `obj_cast<TEvent>(event)` 会成功的类型：事件本身、派生类、实现的接口。
- 计划顺序：最派生类 → 该类声明的接口（递归、声明序）→ 基类及其接口；同一类型只计划一次。
- 接口遍历用显式栈 DFS，`visited` 在遍历层维护；消除递归深度与菱形重复遍历。
- `publish()` 在 map 共享锁内收集订阅快照，释放锁后派发；派发中的 subscribe/unsubscribe 只影响后续 publish。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 订阅/取消 | `subscribe()` 返回 RAII 句柄；析构/`unsubscribe()` 取消未开始调用 | 已落地 |
| F2 | 派发/多态 | `publish()` 按 `obj_cast` 规则派发；接口订阅 | 已落地 |
| F3 | 排队投递 | Main/Auto 模式；`DeliveryRegistry` 与 bus 解耦 | 已落地 |
| F4 | 关停 | `shutdown()` / `shutdownGracefully(timeout)` / 析构 | 已落地 |
| F5 | 异常观测 | `setErrorHandler()`；handler 异常不穿出 | 已落地 |

### 3.1.1 F1 订阅/取消

- **行为**：`subscribe(handler, mode, tag)` 返回 `Subscription`；析构/`unsubscribe()` 把 `active` 置 false。
- **约束**（C1）：每个 handler 调用前检查 `active`；这是取消线性化点。
- **要求**（R1）：未通过检查的调用不再开始；已开始的跑完；`unsubscribe()` 不等待在飞调用。
- **验收**（T1）：`SubscriptionUnsubscribesOnDestruction`、`UnsubscribeInsideHandlerCancelsNotYetVisited`。
- **注意**（N1）：一个 `Subscription` 只由一个线程持有/销毁。

### 3.1.2 F2 派发/多态

- **行为**：按 `obj_cast` 规则派发；接口订阅要求事件元数据声明该接口。
- **约束**（C2）：计划顺序固定；同一类型只计划一次。
- **要求**（R2）：`publish()` 空事件/已停止是 no-op。
- **验收**（T2）：`PolymorphicDispatch`、`InterfaceSubscriptionReceivesImplementingEvents`、
  `MultiInterfaceVisitOrderAndDedup`。
- **注意**（N2）：声明了接口却不真继承会让 `obj_cast/dynamic_cast` 失败（记日志）。

### 3.1.3 F3 排队投递

- **行为**：Main/Auto 模式；`Payload` 由 `DeliveryRegistry` 强持有，队列任务只持弱引用。
- **约束**（C3）：`shutdown()` 清空注册表 → event 立即释放；队列回调退化为 no-op。
- **要求**（R3）：任务运行/取消/被丢弃都由 `~Delivery` 注销。
- **验收**（T3）：`MainModeQueuesDelivery`、`ShutdownReleasesPendingEvent`、
  `BusDestroyedBeforeQueuedDeliveryIsDropped`。
- **注意**（N3）：无 dispatcher 或 QCoreApplication 不存在时，Main/Auto 在发布线程内联执行（文档化）。

### 3.1.4 F4 关停

- **行为**：`shutdown()` 停止接纳、取消订阅/丢弃投递、不等待；`shutdownGracefully(timeout)` 等待在飞调用 +
  泵队列；析构先拒绝准入再无限期等待已准入调用退出。
- **约束**（C4）：`shutdown_result` 默认 false；只有真正完成等待 + 泵队列的优雅关停才写 true。
- **要求**（R4）：停止线程上的重入不等自己；非应用线程的 `shutdownGracefully()` 返回 false。
- **验收**（T4）：`GracefulShutdownWaitsForPublishingThread`、
  `GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork`、
  `ReentrantShutdownFromReleasedHandlerDoesNotWaitForItself`。
- **注意**（N4）：拿到 `true` 的唯一含义是“这一次关停等了在飞调用、也把排队投递跑完”。

### 3.1.5 F5 异常观测

- **行为**：`setErrorHandler(EventBusErrorHandler)` 可选；上下文含事件类型、订阅 id、标签、`exception_ptr`、
  线程模式与 `deferred`。
- **约束**（C5）：钩子在所有锁外调用；钩子自身抛异常被丢弃（noexcept）。
- **要求**（R5）：排队投递在 post 时快照钩子 + 标签，之后更换/移除不影响已排队投递。
- **验收**（T5）：`SubscriberExceptionIsContained`、`ErrorHandlerReceivesDeferredFailure`、
  `ErrorHandlerReportsSubscriptionTag`。
- **注意**（N5）：未安装钩子时只在错误路径多一次 `has_error_handler` 加载。

## 4. 公开接口

### 4.1 `EventBus`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `subscribe<TEvent>(handler, mode, tag)` | 返回 RAII `Subscription` | `TEvent` 需 `TypeDescribed` |
| `publish(event)` | 按 `obj_cast` 规则派发 | 空事件 no-op |
| `unsubscribe(subscription)` | 取消未开始调用 | 不等待在飞 |
| `shutdown()` | 停止接纳、丢弃排队投递 | 不等待 |
| `shutdownGracefully(timeout)` | 等待在飞 + 泵队列 | 返回是否 drain 完成 |
| `isShutDown()` / `pendingDeliveryCount()` | 查询 | — |
| `setErrorHandler(...)` | 异常观测 | 锁外调用 |

### 4.2 `Subscription` / `EventBusError`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `Subscription::~Subscription()` | 取消订阅 | 移动语义 |
| `EventBusError` | 事件类型/id/标签/异常/线程模式/deferred | 值拷贝 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 每个 handler 调用前检查 `active`；这是取消线性化点。 |
| C2 | 计划顺序固定；同一类型只计划一次。 |
| C3 | `shutdown()` 清空注册表 → event 立即释放。 |
| C4 | `shutdown_result` 默认 false；只有真正 drain 的优雅关停写 true。 |
| C5 | 异常钩子在锁外调用；钩子异常被丢弃。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 未通过取消点的调用不再开始；已开始的跑完。 |
| R2 | `publish()` 空事件/已停止是 no-op；`subscribe()` 停止后返回惰性句柄。 |
| R3 | 排队任务无论运行/取消/丢弃都由 `~Delivery` 注销。 |
| R4 | 停止线程上的重入不等自己；非应用线程 graceful 返回 false。 |
| R5 | 排队投递在 post 时快照异常钩子 + 标签。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 出圈对象不持有 bus 指针/强引用。 |
| N2 | 锁内不跑用户代码；handler 闭包/事件析构都在锁外。 |
| N3 | `shutdownGracefully()` 返回 true ⇒ 没有线程还在总线内。 |
| N4 | 析构时无并发调用；违约从 UAF 变成延迟。 |

## 6. 注意事项与陷阱

### 6.1 在 handler 内销毁 bus

- 会怎么错：`~EventBus()` 等已准入调用退出，而当前 handler 正是其中之一 → 死锁/延迟。
- 怎么判：handler 内调 `delete bus` / 析构。
- 怎么防：禁止在 handler 内销毁 bus；要停进程用 `Application::exit()`。

### 6.2 非应用线程 graceful

- 会怎么错：以为 `shutdownGracefully()` 在任何线程都能 drain。
- 怎么判：返回 false 且 `deliverPostedCalls()` 未执行。
- 怎么防：只在应用线程调用；其它线程用 `shutdown()` 或让应用线程收尾。

### 6.3 订阅闭包析构里重入关停

- 会怎么错：停止线程上的重入等自己 → 死锁。
- 怎么判：`shutdown()`/`shutdownGracefully()` 在 `Stopping` 且 `stopper == this_thread`。
- 怎么防：`Impl::stopper` 守卫直接返回（`shutdown()` no-op / graceful false）。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | `DeliveryRegistry` 换 `unordered_map` + `extract` | 待处理投递通常 0–2，`push_back` 已是 O(1)，向量对缓存友好。 | — |
| D2 | `visited` 用 `unordered_set` | 尺寸小、指针比较快、无分配。 | — |
| D3 | `thread_local` 深度覆盖 detached async task | 异步任务调 `publish()` 是独立准入调用，不依赖逃离线程的深度。 | — |
| D4 | 让 `shutdown()` 等待在飞调用 | 语义是“停止接纳”，等待交给 `shutdownGracefully()`/析构。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 订阅/取消/RAII/移动 | `SubscriptionUnsubscribesOnDestruction`、`MoveTransfersOwnership` 等 |
| T2 | 多态/接口计划顺序 | `PolymorphicDispatch`、`MultiInterfaceVisitOrderAndDedup` |
| T3 | 排队/事件生命周期 | `MainModeQueuesDelivery`、`ShutdownReleasesPendingEvent` |
| T4 | 关停/准入 | `GracefulShutdownWaitsForPublishingThread`、`GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork` |
| T5 | 异常观测 | `SubscriberExceptionIsContained`、`ErrorHandlerReportsSubscriptionTag` |
| T6 | ASan 门 | `scripts/asan_check.sh`（`EventBusTest.*`） |

## 9. 状态与演进

### 9.1 当前状态

订阅/派发/排队/关停/异常观测均已落地；ASan 门禁通过。

### 9.2 已知缺口 / 待办

- 若事件循环长时间停顿、上万条 Main 投递同时待处理，drain 路径的 `remove` 是 O(N²)；
  需要时改注册表（见 D1）。
- LeakSanitizer 报的 15 处 / 5864 字节全部经过 `GuiTest::SetUp`，是测试夹具/QApplication 保留，与 bus 无关。

### 9.3 兼容性 / 迁移

新代码只按 §4 的接口写；`shutdown_result` 默认 false 是行为修正，不要读回旧默认值。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/EventBus.hpp`
- `src/fw/appfw/src/EventBus.cpp`
- `src/fw/appfw/sdk/vine/appfw/MainThreadDispatcher.hpp`
- `src/base/core/sdk/vine/Type.hpp`

### 10.2 文档与 ADR

- `appfw-command-manager.md`
- `appfw-progress.md`
- `../backlog/`

### 10.3 外部资料

- Qt `QMetaObject::invokeMethod` / `QEvent::MetaCall` 语义。
- `scripts/asan_check.sh` 的 ASan 构建约定。
