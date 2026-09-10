# appfw EventBus 设计（2026-09-10 重写）

代码：`src/fw/appfw/sdk/vine/appfw/EventBus.hpp`、`src/fw/appfw/src/EventBus.cpp`、
`src/fw/appfw/sdk/vine/appfw/MainThreadDispatcher.hpp`、`src/fw/appfw/src/MainThreadDispatcher.cpp`；
依赖 `Type::interfaces()`（`src/base/core/sdk/vine/Type.hpp`）。
测试：`tests/test_gui/EventBusTest.cpp`（28 例）。

## 目标与不变量

1. **出圈者不指向 bus**：任何离开 bus 的对象（`Subscription` 句柄、事件循环里的任务）都不得持有
   指向 `EventBus`/`Impl`/`EventChannel` 的指针或强引用。bus 先死只让它们变哑，不能悬垂。
2. **取消语义一致**：`unsubscribe()` / 句柄析构 / `shutdown()` / bus 析构 = 取消所有"尚未开始调用"
   的 handler（Current 与 Main 一致）；已开始的调用跑完，且任何 API 都不阻塞等待在飞投递。
3. **不泄漏事件**：`shutdown()` 与 bus 析构立即释放排队投递持有的事件，不等事件循环。
4. **异常不外逃**：handler 异常与投递入队失败都不得穿出 `publish()`/队列回调/Qt 事件循环。
   日志异常不再需要在这里预算——“日志发射路径永不抛”是 Logging 模块的契约（`Logger` 的级别函数、
   `log()`、`defaultLogger()` 均 `noexcept`，失败一次性报告到 stderr），所以 `reportHandlerError`
   里的日志调用不再包 try/catch。
5. **锁内不跑用户代码**：handler 调用、handler 闭包析构、事件析构都在所有锁之外执行。

## 结构

```
EventBus ── Impl ─┬─ map<TypeId, EventChannel>   (channels, std::shared_mutex)
                  ├─ shared_ptr<DeliveryRegistry>  (排队的投递，与 bus 解耦)
                  └─ atomic<bool> stopped
EventChannel ──── vector<SubscriptionEntry{mode, shared_ptr<SubscriptionState>}> + std::mutex
SubscriptionState  const id / const Handler / atomic<bool> active   (唯一可变项是 active)
Subscription     ── shared_ptr<Control>  (Control 持 SubscriptionState，析构即 deactivate)
Payload          ── weak_ptr<SubscriptionState> + shared_ptr<const Object> event  (全 const)
Delivery         ── shared_ptr<DeliveryRegistry> + weak_ptr<Payload>；析构时从注册表注销
队列 λ            ── 只捕获 shared_ptr<Delivery>（不捕获 bus/channel/this）
```

- `MainThreadDispatcher` 由 `Application` 创建并**注入** `EventBus` 构造函数（bus 不再查
  `Application::current()`）。`ApplicationData` 中 `main_dispatcher` 先于 `event_bus` 声明 →
  析构时后销毁 → dispatcher 比 bus 长命。
- `MainThreadDispatcher::isMainThread()` = "创建 QCoreApplication 的线程"（不再在无 app 时返回 true）；
  `hasEventLoop()` = QCoreApplication 是否存在；`postToMain()` 返回 bool，不会在调用线程内联执行。

## 关键机制

- **准入协议（admission）**：`publish()`/`subscribe()` 先通过 `Impl::CallGuard` 进入总线。
  `state` 与 `active_calls` 在同一把 `call_mutex` 下，因此“总线已开始停止”与“有调用者进来了”
  不会交叉：一旦 `state` 离开 `Running` 就不再准入，而已经准入的调用一定对等待者可见。
  这正是“graceful 完成 ⇒ 没有线程还在总线内”可证明的前提（旧版把 `stopped` 检查与
  `active_calls++` 分开，存在后者凌越前者的窗口）。
- **关闭状态机**：`Running → Stopping → Stopped`。进入 `Stopping` 时同时置锁外可见的
  `stopped`（供 `isShutDown()` 与准入快路径使用）。并发调用 `shutdown()`/`shutdownGracefully()`
  时，非持有者会等到 `Stopped` 并拿到**同一次关闭的结果**，不会误以为“已经停好了”而提前析构。
- **投递规则 = `obj_cast` 可行**：一个订阅恰好收到那些 `obj_cast<TEvent>(event)` 会成功的类型，
  即事件本身、其派生类，或它实现的接口。计划顺序：最派生类 → 该类声明的接口（递归、声明序）→
  基类及其接口；同一类型只计划一次（`planned` 去重），因此冗余/多路径的接口声明不会重复投递。
  接口订阅需要 `subscribe<TEvent>` 的约束是 `TypeDescribed`（而非 `ObjectBased`）。
- **接口遍历是迭代的、且边走边剪枝**：`forEachInterface(roots, visited, visit)` 用显式栈做
  DFS 前序（栈内反序入栈，保证先声明的接口先访问），并在**遍历层**维护 `visited`（所有访问过的
  类型，不只是命中 channel 的）。这两点分别消除：（a）递归深度随接口嵌套深度增长；（b）菱形/嵌套
  菱形下同一子树被反复遍历（路径数个 § 指数）。访问顺序与旧递归版一致，由
  `InterfaceSubscriptionReceivesImplementingEvents`（{ITaggedEx, ITagged, EventArgs}）与
  `MultiInterfaceVisitOrderAndDedup`（{IBoth, ILeft, IRight, EventArgs}）钉住。
- **订阅者异常观测**：`setErrorHandler(EventBusErrorHandler)` 可选安装，上下文含事件类型、订阅 id、
  **订阅标签**、`exception_ptr`、线程模式与 `deferred` 标志。标签由 `subscribe(handler, mode, tag)` 在
  订阅时传入（`String`，可空，存于不可变的 `SubscriptionState`，而 `EventBusError` 里是拷贝），
  便于把失败直接映射到模块/窗口。钩子在**所有锁外**调用，自身抛异常被丢弃（noexcept 语义），
  未安装时只在错误路径多一次 `has_error_handler` 加载。排队投递在 **post 时快照**钩子+标签，
  因此之后更换/移除不影响已排队的投递；两者以 `shared_ptr<const Handler>`/值存于 `Payload`，
  不引入任何指向 bus 的引用。
- **一致计划（consistent plan）**：`publish()` 在 map 共享锁内一次性收集所有匹配 channel 的订阅快照，
  释放锁后派发。因此派发中的 subscribe/unsubscribe 只影响后续 publish。
- **逐次取消检查**：每个 handler 调用前检查 `state->isActive()`。这就是**取消线性化点**：
  通过检查的调用算“已开始”并可跑完；未到的被跳过。`unsubscribe()` 永不等待在飞调用。
- **自排除必须按 bus 计数**：`Impl::callDepth()` 是 per-thread、**per-bus** 的（TLS 里一组
  `{Impl*, depth}`，降到 0 时删除）。它回答的是“本线程在这个 bus 里占了几次已准入调用”，
  不能用全局 thread-local 嵌套深度：否则本线程嵌在另一个 bus 的调用里时，会把不属于本 bus 的
  深度当作自己的，从而在还有别的线程在飞时误报“没人了”（→ 可提前析构 = UAF）。
- **析构必须等在飞调用**：`~EventBus()` 先拒绝准入 + 取消订阅/丢弃投递（让在飞调用尽快结束），
  再无限期等待已准入调用退出（返回 `true` 的 `shutdownGracefully()` 也有同样保证；
  单纯的 `shutdown()` **不等待**）。契约仍是“析构时无并发调用”，这一步把违约从 UAF 变成延迟；
  因此禁止在 handler 内销毁 bus，handler 也不能依赖销毁线程推进。
- **锁外回收**：`reapLocked()`/`takeAll()` 把被回收条目交给调用方（`garbage`），调用方在释放所有锁之后
  再销毁，避免 handler 闭包析构回调 bus 造成自死锁。
- **排队投递的取消与释放**：`Payload` 由 `DeliveryRegistry`（与 bus 解耦的引用计数对象）强持有，
  队列任务只持其弱引用（`Delivery` 由队列回调的**副本**共同持有，最后一个副本释放时才注销 payload）。
  `shutdown()` 清空注册表 → payload（及其 event）立即释放，已投递的 Qt 回调仍留在队列里但退化为 no-op；
  bus 析构走同一路径。任务无论运行、被取消还是被事件循环丢弃，都由 `~Delivery` 注销。
- **降级策略**：无 dispatcher 或 QCoreApplication 不存在 → Main/Auto 在发布线程内联执行（文档化）；
  `postToMain` 失败 → 丢弃 + 警告日志。
- **优雅关停**：`shutdownGracefully(timeout)` = 置 stopped → 等其它线程离开 `publish()/subscribe()`
  （`CallGuard` 计数 + `thread_local` 排除自己，避免 handler 内调用自杀）→ 在应用线程上
  `MainThreadDispatcher::deliverPostedCalls()`（只派发 `QEvent::MetaCall`，即 `postToMain` 投的调用，
  不跑定时器/绘制/输入）循环泵队列直到 `pendingDeliveryCount()==0` 或超时 → 再 `cancelSubscriptions()`。
  返回 false 表示有工作被丢弃（超时、或调用线程不是应用线程）。
  `Application::run()`/`GuiApplication::run()` 用 `EventBus::gracefulShutdownTimeout()`（200ms）优雅关停。

## 必须由调用方保证

1. 销毁 `EventBus` 前推荐先停下：`shutdownGracefully()` 返回 `true` 即证明没有线程在内；
   `shutdown()` 只停止接纳、**不等**在飞调用，但 `~EventBus()` 自己会等到在飞调用退出，
   所以未停就并发析构不再悬垂，代价是可能阻塞。
2. `shutdownGracefully()` 只在**应用线程**上能真正 drain（`deliverPostedCalls()` 对其他线程返回
   false 且不执行任何调用）；它不杀线程，超时后如实返回 false。
3. 一个 `Subscription` 句柄只由一个线程持有/销毁。
4. Main/Auto 的 handler 必须自持捕获；不要按引用捕获订阅者栈对象（取消不等待在飞投递）。
5. 接口订阅依赖事件类在元数据里声明了该接口（`V_OBJECT_META_IMPL(..., Itf)`）；声明了却不真继承
   会让 `obj_cast/dynamic_cast` 失败（记日志）。

## 测试映射（tests/test_gui/EventBusTest.cpp，28 例）

派发/多态：PublishDeliversToSubscribers、DifferentTypesAreIsolated、PolymorphicDispatch、
PublishWithNoSubscribersIsNoOp、InterfaceSubscriptionReceivesImplementingEvents、
InterfaceWalkDeliversEachSubscriptionOnce；RAII/move：SubscriptionUnsubscribesOnDestruction、
MoveTransfersOwnership、MoveAssignmentCancelsPreviousSubscription、SubscriptionHandleReflectsCancellation；
取消：UnsubscribeInsideHandlerCancelsNotYetVisited、ShutdownCancelsInFlightCurrentDispatch、
UnsubscribedBeforeQueuedDeliveryIsSkipped；快照一致性：SubscribeDuringDispatchAffectsOnlyLaterPublications；
排队/事件生命周期：MainModeQueuesDelivery、AutoModeOnMainIsSynchronous、AutoModeOffMainQueuesToMain、
EventOutlivesPublishViaSharedPtr、ShutdownReleasesPendingEvent、UnsubscribedHandlerIsReleasedWhileDeliveryQueued、
MainWithoutMarshallerRunsOnPublishingThread；teardown：BusDestroyedBeforeQueuedDeliveryIsDropped、
TokenOutlivingBusIsInert、ShutdownStopsDeliveryAndDropsPending、GracefulShutdownDeliversParkedDelivery、
GracefulShutdownWaitsForPublishingThread、GracefulShutdownFromWorkerReportsDroppedWork；
优雅关停 / 准入：GracefulShutdownDeliversParkedDelivery、GracefulShutdownWaitsForPublishingThread、
GracefulShutdownFromWorkerReportsDroppedWork、GracefulShutdownFromHandlerDoesNotWaitForItself、
ConcurrentGracefulShutdownsShareCompletion、PublishRacingShutdownIsSafe、
GracefulShutdownDoesNotCountAnotherBussCallAsItsOwn、DestructionWaitsForAdmittedCall；
遍历：InterfaceSubscriptionReceivesImplementingEvents、InterfaceWalkDeliversEachSubscriptionOnce、
MultiInterfaceVisitOrderAndDedup；异常观测：SubscriberExceptionIsContained、
ErrorHandlerReceivesCurrentFailure、ErrorHandlerReceivesDeferredFailure、ThrowingErrorHandlerIsIgnored、
ErrorHandlerCanBeRemoved、ErrorHandlerReportsSubscriptionTag。

语义要点（可直接引用）：
- `unsubscribe()`/`shutdown()` 之后，**尚未通过取消点的调用**不会再开始；已开始的跑完。
- `shutdownGracefully()` 返回后总线永久停止；`true` 表示所有已准入调用都已离开（可安全析构），
  并发调用者拿到同一结果；`false` 表示 drain 未在 deadline 内完成，但总线同样已停止。
- drain 会派发已排队的回调，因此可能产生**重入**的总线调用；此时 `publish()` 为 no-op、
  `subscribe()` 返回惰性句柄（准入已被拒绝），所以重入是有界且安全的。

运行：`QT_QPA_PLATFORM=offscreen ./build/bin/test_gui --gtest_filter='EventBusTest.*'`。

## 已评估但**未采纳**的优化（留档，避免重复建议）

- **DeliveryRegistry 换成 `unordered_map` + `extract` 做 O(1) 移除**：未采纳。待处理投递数通常为
  0–2（跑到就注销），`push_back` 已是 O(1)，向量对缓存友好且不额外分配；换成哈希表要在
  每次投递多一次节点分配/哈希，对真实负载无收益。只有在“事件循环长时间停顿、上万条 Main 投递
  同时待处理”时才会出现 drain 路径的 O(N²)（`remove` 每次 O(N)），如需消除可把注册表改成
  `unordered_map<const Payload*, shared_ptr<Payload>>` + `extract`（保留 key=指针，Payload 仍不可变）
  或将 Payload 存索引做 swap-and-pop（代价：Payload 不再 immutable）。目前无任何测量支持动它。
- **`visited` 用 `unordered_set` 代替 vector**：未采纳（尺寸小、指针比较快、无分配）。若将来出现
  上百类型的层级再换，代价可忽略。
- **`thread_local` 深度不覆盖 detached async task**：非问题。深度只记录“调用线程当下嵌套的
  已准入调用”；异步任务之后调 `publish()` 本身就是一次独立的准入调用，不依赖任何逃离线程的深度。

## ASan 门（scripts/asan_check.sh）

UAF 类结论只有插桩构建能确证，所以有一条可复跑的门禁：

```
scripts/asan_check.sh                          # EventBusTest.*（默认，含 28 例）
VINE_ASAN_LEAKS=1 scripts/asan_check.sh        # 再加 LeakSanitizer
VINE_ASAN_FILTER='*' scripts/asan_check.sh     # 整个 test_gui（68 例）
```

它在独立目录 `build-asan` 里用 `-fsanitize=address` 构建（不改动主 build 目录），
并强制 `QT_QPA_PLATFORM=offscreen`：继承桌面的 `xcb` 插件时，libxkbcommon-x11 会在
`strndup` 上报读越界并在任何用例之前中止（库侧问题，非框架代码）；要对着真实显示
跑就用 `VINE_ASAN_QPA_PLATFORM=xcb`。脚本会先验证二进制确实链了 ASan runtime
（否则报错退出，避免“门禁”静默跑未插桩的二进制），失败时保留完整日志并打印报告。

当前结果（2026-09-10）：EventBusTest 28 例在 ASan 下干净、**泄漏也为零**；
整个 test_gui 68 例无越界/UAF，但泄漏模式会报 `GuiTest::buildDock`
（`tests/test_gui/test_gui.cpp`）——测试夹具自己 `new` 了 DockPanel 而无人释放，
属测试/停靠面板归属问题，与 EventBus 无关，待单独处理。
