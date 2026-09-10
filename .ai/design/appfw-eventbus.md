# appfw EventBus 设计（2026-09-10 重写）

代码：`src/fw/appfw/sdk/vine/appfw/EventBus.hpp`、`src/fw/appfw/src/EventBus.cpp`、
`src/fw/appfw/sdk/vine/appfw/MainThreadDispatcher.hpp`、`src/fw/appfw/src/MainThreadDispatcher.cpp`；
依赖 `Type::interfaces()`（`src/base/core/sdk/vine/Type.hpp`）。
测试：`tests/test_gui/EventBusTest.cpp`（42 例）。

第八轮审查（2026-09-11）结论见文末“第八轮审查”。

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
  状态机在 `Impl` 里只有**一处**实现：`beginStop()` / `finishStop(clean)` / `stoppingOnThisThread()` /
  `awaitStopped()` / `awaitStoppedUntil()`；`~EventBus()`、`shutdown()`、`shutdownGracefully()`
  三个入口只是组合这五个动作，所以“落后者不得误报 true”“停止线程不得等自己”两条规则
  不再各叉一份。
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
  释放锁后派发。因此派发中的 subscribe/unsubscribe 只影响后续 publish。快照本身在
  `Impl::collectPlan()` 里（含锁外销毁被回收的闭包），`publish()` 只剩“快照 → 线程策略 → 派发”三步。
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
- **关停结果是共享的**：`Impl::shutdown_result` 由“**发起**那次关停的调用”写入，之后每个到达的
  `shutdownGracefully()`（无论并发还是更晚）都返回同一个值。“先做 drain 的调用写结果，后来的只是
  读”这条旧规则有一个洞：如果别的线程用 `shutdown()`/`~EventBus()` 抢先停掉了总线（这两条路径
  **从不 drain**，直接取消剩余投递），后来的 `shutdownGracefully()` 会读到默认值 `true`，等于
  宣称“所有投递都跑过了”——实际全被丢弃。因此 `shutdown_result` 默认 `false`，两条非优雅路径
  （`shutdown()` 的第一个分支、`~EventBus()` 的第一个分支）都显式写 `false`，只有真正完成了
  等待 + 泵队列的优雅关停才可能写 `true`。
- **停止线程上的重入不得等自己**：`Impl::stopper` 记录执行 `Stopping → Stopped` 转换的线程。
  `cancelSubscriptions()` 会在锁外销毁订阅闭包，而闭包析构里再调 `shutdown()`/`shutdownGracefully()`
  是完全合法的用法——此时 `state == Stopping` 且 `stopper` 就是本线程，等下去只能死锁（外层
  的 `Stopped` 转换要等这次调用返回）。两条路径都直接返回（`shutdown()` 返回空句柄语义的 no-op，
  `shutdownGracefully()` 返回 `false`，因为这次关停确实没有 drain）。
  `Application::run()`/`GuiApplication::run()` 用 `EventBus::gracefulShutdownTimeout()`（200ms）优雅关停，
  `Application::shutdown()` 在其返回 `false` 时记一条警告（与命令链 drain 超时的警告对称）。

## 必须由调用方保证

1. 销毁 `EventBus` 前推荐先停下：`shutdownGracefully()` 返回 `true` 即证明没有线程在内；
   `shutdown()` 只停止接纳、**不等**在飞调用，但 `~EventBus()` 自己会等到在飞调用退出，
   所以未停就并发析构不再悬垂，代价是可能阻塞。
2. `shutdownGracefully()` 只在**应用线程**上能真正 drain（`deliverPostedCalls()` 对其他线程返回
   false 且不执行任何调用）；它不杀线程，超时后如实返回 false。拿到 `true` 的**唯一**含义是
   “这一次关停等了在飞调用、也把排队的投递跑完了”。若这期间别人用 `shutdown()`/析构抢先停掉
   总线（它们不 drain），结果同样是 `false`——不要把它读成“总线没停”。
3. 一个 `Subscription` 句柄只由一个线程持有/销毁。
4. Main/Auto 的 handler 必须自持捕获；不要按引用捕获订阅者栈对象（取消不等待在飞投递）。
5. 接口订阅依赖事件类在元数据里声明了该接口（`V_OBJECT_META_IMPL(..., Itf)`）；声明了却不真继承
   会让 `obj_cast/dynamic_cast` 失败（记日志）。

## 测试映射（tests/test_gui/EventBusTest.cpp，42 例）

- **派发/多态（8）**：PublishDeliversToSubscribers、DifferentTypesAreIsolated、PolymorphicDispatch、
  PublishWithNoSubscribersIsNoOp、PublishNullEventIsIgnored、InterfaceSubscriptionReceivesImplementingEvents、
  InterfaceWalkDeliversEachSubscriptionOnce、MultiInterfaceVisitOrderAndDedup。
- **RAII/move（4）**：SubscriptionUnsubscribesOnDestruction、MoveTransfersOwnership、
  SubscriptionHandleReflectsCancellation、MoveAssignmentCancelsPreviousSubscription。
- **取消与快照一致性（4）**：UnsubscribeInsideHandlerCancelsNotYetVisited、
  ShutdownCancelsInFlightCurrentDispatch、SubscribeDuringDispatchAffectsOnlyLaterPublications、
  UnsubscribedBeforeQueuedDeliveryIsSkipped。
- **排队/事件生命周期（7）**：MainModeQueuesDelivery、AutoModeOnMainIsSynchronous、
  AutoModeOffMainQueuesToMain、MainWithoutMarshallerRunsOnPublishingThread、
  EventOutlivesPublishViaSharedPtr、ShutdownReleasesPendingEvent、
  UnsubscribedHandlerIsReleasedWhileDeliveryQueued。
- **teardown（3）**：BusDestroyedBeforeQueuedDeliveryIsDropped、TokenOutlivingBusIsInert、
  ShutdownStopsDeliveryAndDropsPending。
- **优雅关停/准入（10）**：GracefulShutdownDeliversParkedDelivery、GracefulShutdownWaitsForPublishingThread、
  GracefulShutdownFromWorkerReportsDroppedWork、GracefulShutdownFromHandlerDoesNotWaitForItself、
  ConcurrentGracefulShutdownsShareCompletion、PublishRacingShutdownIsSafe、
  GracefulShutdownDoesNotCountAnotherBusCallAsItsOwn、GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork、
  DestructionWaitsForAdmittedCall、ReentrantShutdownFromReleasedHandlerDoesNotWaitForItself。
- **异常观测（6）**：SubscriberExceptionIsContained、ErrorHandlerReceivesCurrentFailure、
  ErrorHandlerReceivesDeferredFailure、ThrowingErrorHandlerIsIgnored、ErrorHandlerCanBeRemoved、
  ErrorHandlerReportsSubscriptionTag。

语义要点（可直接引用）：
- `unsubscribe()`/`shutdown()` 之后，**尚未通过取消点的调用**不会再开始；已开始的跑完。
- `shutdownGracefully()` 返回后总线永久停止；`true` 表示所有已准入调用都已离开（可安全析构），
  并发调用者拿到同一结果；`false` 表示 drain 未在 deadline 内完成，但总线同样已停止。
- `true` 只可能由**真正执行了 drain 的那次调用**产生：若 `shutdown()`/`~EventBus()` 抢先
  （它们不 drain），随后的 `shutdownGracefully()` 一律返回 `false`。
- 订阅闭包析构里重入关停（`shutdown()`/`shutdownGracefully()`）不会等自己：停止线程上的
  重入返回空操作 / `false`，不会再把自己锁住。
- `publish()` 的空事件与“总线已开始停止”都是 no-op；`subscribe()` 在停止后返回惰性句柄。
- 若句柄分配失败（`std::bad_alloc`），`subscribe()` 会先取消刚建立的注册再抛出：不会留下
  “没人能取消的活跃订阅”。
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
scripts/asan_check.sh                          # EventBusTest.*（默认，含 42 例）
VINE_ASAN_LEAKS=1 scripts/asan_check.sh        # 再加 LeakSanitizer
VINE_ASAN_FILTER='*' scripts/asan_check.sh     # 整个 test_gui（68 例）
```

它在独立目录 `build-asan` 里用 `-fsanitize=address` 构建（不改动主 build 目录），
并强制 `QT_QPA_PLATFORM=offscreen`：继承桌面的 `xcb` 插件时，libxkbcommon-x11 会在
`strndup` 上报读越界并在任何用例之前中止（库侧问题，非框架代码）；要对着真实显示
跑就用 `VINE_ASAN_QPA_PLATFORM=xcb`。脚本会先验证二进制确实链了 ASan runtime
（否则报错退出，避免“门禁”静默跑未插桩的二进制），失败时保留完整日志并打印报告。

当前结果（2026-09-11，第八轮修复后）：`VINE_ASAN_FILTER='EventBusTest.*'` 下 **42 例全过、
AddressSanitizer 错误为零**（UAF/越界/释放后使用均无），默认跑法输出 `RESULT: PASS`。
加 `VINE_ASAN_LEAKS=1` 时 LeakSanitizer 报 15 处 / 5864 字节，栈**全部**经过
`GuiTest::SetUp`（`tests/test_gui/test_gui.cpp:108`）→ `createGuiApplication()` →
`GuiApplication::init()` → `MainWindow`/`SARibbonMainWindow`（第三方），也就是测试夹具与
QApplication 的既有保留，与 EventBus 无关；这与早先整跑 test_gui 时出现的 `GuiTest::buildDock`
是同一类问题（测试夹具自己持有对象），只是不同跑法命中的不同用例。

## 第八轮审查（2026-09-11）

标准与 CommandManager 那轮一致：每条缺陷要么可复现、要么可证伪，修复必须带证据。

已修：

| 编号 | 缺陷 | 修复与证据 |
|------|------|-----------|
| E1 | `reportHandlerError` 是 `noexcept`，但内部要构造 `EventBusError`（`String tag` 拷贝会分配）并调用用户钩子：分配失败会在**错误路径**上炸成 `std::terminate` | 把“构造上下文 + 填充 tag + 调用钩子”整体放进同一个 `try { ... } catch (...) {}`。观测路径不能反过来成为新的失败源 |
| E3 | `subscribeErased()` 中 `make_shared<Subscription::Control>` 若抛异常，订阅**已经注册进 channel** 却没有句柄：活跃、永远无法取消、也没人知道 | `state` 按值传给 `make_shared`，`catch (...)` 里先 `state->deactivate()` 再 `throw;`：失败要么完全没订阅，要么订阅已变哑 |
| E7 | `Impl::shutdown_result` 默认 `true`。`shutdown()`/`~EventBus()`（两者都**不 drain**，直接丢弃排队投递）抢先停掉总线后，后来的 `shutdownGracefully()` 读到默认值 `true`，等于报告“排队的投递都跑过了” | 默认改 `false`，两条非优雅路径显式写 `false`；只有真正等完在飞调用 + 泵完队列的调用才可能写 `true`。复现用例 `GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork`：修前 `Actual: true / Expected: false`，修后通过 |
| E11 | **停止线程上的重入关停会等自己**：`cancelSubscriptions()` 在锁外析构订阅闭包，闭包析构里再调 `shutdown()`/`shutdownGracefully()` 时 `state == Stopping`，而 `Stopped` 要等外层这次转换——死锁 | 新增 `Impl::stopper`（受 `call_mutex` 保护，只在 `Stopping` 期间有意义）；`state == Stopping && stopper == std::this_thread::get_id()` 时 `shutdown()` 直接返回、`shutdownGracefully()` 返回 `false`（这次关停确实没 drain）。两条入口都验过：闭包析构入口用**常驻**用例 `ReentrantShutdownFromReleasedHandlerDoesNotWaitForItself`（工作线程 + 3s 有界等待，回归时失败而非挂住）：去守卫失败（3068ms 后断言失败）、带守卫 10ms 通过；drain 泵入口（Main handler 在 `shutdownGracefully()` 的泵里再调 `shutdown()`，外层正卡在该线程上）用一次性用例验过：带守卫 2ms 通过、去守卫 `timeout 30` 被 SIGTERM 杀掉（143，连结果都没打印）——即真死锁。该一次性用例因其"回归即挂住"的失败形态未留在套件里 |

文档精度（不改行为）：`EventBusError::subscriber_id` 只在同一事件类型内唯一（`Channel::last_id_`
每个 channel 一份）；`isShutDown()` 从**开始停止**那刻即为 `true`（不是等停下来）；
`subscribe()` 的 `std::bad_alloc` 语义、`publish()` 的 null 事件语义写进头文件。

仅记录、本轮**未改**：

| 编号 | 观察 | 为什么不改 |
|------|------|-----------|
| E5 | `isShutDown()` 与 `shutdown()`/`shutdownGracefully()` 命名不成体系（`isShutdown()` 更一致） | 公开 API：改名会影响库外调用者。仓内波及 = 头文件 + 9 处测试调用 + 本文档。需要用户决定 |
| E9 | `shutdownGracefully(0)` 一轮 drain 都不做（deadline 已过，`drainPendingDeliveries()` 在 `pendingCount()==0` 之外直接返回 false） | “0 = 不等待”是自洽语义；改成“至少跑一轮”会让 0 与“立刻取消”的直觉分离，风险大于收益 |
| E10 | `subscriber_id` 唯一性范围 | 不是缺陷，是文档缺口，已补 |

门禁自身修复：`scripts/asan_check.sh` 推导 C 驱动时只处理了裸名（`g++` → `gcc`），
遇到缓存里的绝对路径 `/usr/bin/g++` 会原样传给 `CMAKE_C_COMPILER`；CMake ≥ 4
因此直接配置失败（"The CMAKE_C_COMPILER is set to a C++ compiler"）。现按路径安全的方式
替换，且在找不到配套 C 驱动时从 `PATH` 取 `gcc`/`cc`/`clang`，不再退化把 C++ 编译器交给 C。
