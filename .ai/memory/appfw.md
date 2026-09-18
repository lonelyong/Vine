# appfw 模块要点（`src/fw/appfw`）

> 详细设计按模块拆在 `.ai/design/`：`appfw-command-manager.md`（含 Command/执行链/历史/禁用）、
> `appfw-userio.md`（UserIO/ConsoleUserIO/VisualUserIO）、`appfw-progress.md`（ProgressHost 与两个呈现者）、
> `appfw-render-surface.md`（RenderControl 的表面生命周期）、`appfw-eventbus.md`、`appfw-plugin-system.md`、
> `appfw-config.md`。
> 本文件只放"跨这几篇、干活时必须立刻想起的规则"。

## 三条横切规则

1. **线程**：命令在它**恢复时所在的线程**上继续（定时器/IO/线程池），所以
   - `UserIO::putString`/`clear`/`cancelPendingInput` 可以被任意线程调用（GUI 实现自己编组，`VisualUserIO`
     用 `onConsolePanel` + `QPointer` 守卫，面板没了就是空操作）；
   - 命令可能在任意线程结束 ⇒ 事件 `executing`/`executed` 的**处理函数**必须自己编组（它们跑在结束线程上）；
     要回到应用线程：命令体里 `co_await app->mainThreadDispatcher()->resumeOnMainThread()`（协程式，无事件循环时不挂起），
     纯回调场景用 `postToMain()`；
   - `Signal` **本身已经是线程安全的**（2026-09-17：不可变快照 `vector<Entry>` + 原子发布，`trigger` **不取任何锁**、
     不分配，与 Qt 连接表同构），
     所以"订阅必须放在启动期"这条老限制**已作废**——任何线程都可以随时 `connect`/`disconnect`。
     实测：一边发火一边增删，TSan 从 12 条竞争降到 0；20 个 handler 时发火 104.6 → 25.8 ns；
     订阅+注销 147 ns/对（订阅要复制整表，仍属于装配期动作）。
   - 订阅成员推荐用 **`Connection` 句柄**：`theme_handler_ = app->theme_changed.connect(...)`，析构/赋值自动取消；
     不需要管理时显式 `.detach()`（`connect` 是 `[[nodiscard]]`，丢掉返回值 = 订阅完立刻取消）。
     句柄只持 `weak_ptr`，Signal 先死也安全，所以拆除路径里不再需要 `removeHandler` + `Application::current()` 查找。
     2026-09-18：句柄从 `Signal::Subscription` 外提为独立**非模板**类 `vine::Connection`；入口改名
     `subscribe/unsubscribe/release/unsubscribeAll` → `connect/disconnect/detach/disconnectAll`（对齐 Qt），
     `Signal::Slot` 改为继承 `Connection::State`。
2. **锁内不跑用户代码**：`CommandManager` 的 `mutex`/`registry_mutex`、`Chain::mutex` 里只做容器操作与值拷贝；
   命令虚函数、工厂、快照回调、事件处理函数、`ProgressHost::current()`（progress 全局锁）都在锁外。
   `registry_mutex` 是叶子锁；`admit()` 在临界区**外**采样 progress 宿主。
3. **生命周期是宿主契约**：`~CommandManager` 只告警不阻塞（析构里等协程 = 死锁风险），所以宿主必须先
   `UserIO::cancelPendingInput()`（唤醒停在用户输入上的命令）再 `CommandManager::cancelAllAndWait()`；
   活着的命令帧里存着 `Impl*` 与 `CommandManager*`。`Application::shutdown()` 已经这么做。

## 容易记错的 API 语义

- `isRegistered(name)` = **存在性**（只查注册表：不解析别名、不看 enabled）；`isCommandEnabled(name)` =
  **能不能执行**（解析别名链 + enabled）。禁用是标记而不是移除（`names()`/`commandInfos()` 仍列出）。
- 嵌套必须走 `CommandExecutionContext::executeChild()`：它共享父链的栈与取消源、绕过串联门；
  在命令里调顶层入口 = 新开一条链、会抢前台、`maxChainDepth` 拦不住。
  子命令要么给名字（走注册表建实例），要么**直接传实例** `executeChild(std::unique_ptr<Command>)`
  ——参数由父命令给、无法预注册的那类子命令用后者；实例所有权转到执行帧，子命令结束即析构，
  拒绝规则与顶层按实例入口一致（null / 名称已注册且被禁用 ⇒ Failed）。
- `CommandFlags` 现在支持位运算（`Undoable | LongRunning`），判位用 `vine::testFlag()`。
- **历史不留结果载荷**（2026-09-17）：`CommandHistoryEntry::result` 是 `CommandResult(status, message)`，
  `data()` 恒为空——条目数有上界（1024）时字节数才有上界，否则"最近 1024 次运行的载荷之和"可以到 GB 级；
  载荷属于发起那次执行的调用方。用例 `CommandManager_HistoryDoesNotRetainTheResultPayload` 钉住。
- **用户可见消息用中文，编程错误用英文**（2026-09-17 统一）：门拒绝是
  `另一个操作正在进行中，请稍候。` / `另一个操作仍在收尾，请稍后再试。`，禁用与未注册也是中文；
  `Command is null` 这类保持英文。
- `UserIO::parseInt()` 现在委托 `String::toInt()`（范围规则只有一处实现），它比 `toInt` 多出的唯一契约是
  **失败时不写 `value`**（重提示要保留用户已输入的值）。
- **嵌入渲染表面不要猜延迟**（2026-09-18）：`RenderControl` 自己管生命周期（构造时隐藏表面 → 布局后自驱
  attach，16/50/100/200/400/900ms 退避 → 首帧呈现后才显示表面），宿主只要 `new` + 放进布局；
  要知道“什么时候才能出画面”或想做自己的占位/错误提示，订阅 `stateChanged`（`Pending/Attached/Presenting/Failed`），
  不要用 `QTimer::singleShot` 猜。要自己掌握时机用 `setAutoInitialize(false)` + 幂等 `init()`。
- **无头模式已经有进度显示了**（2026-09-18）：`ConsoleUserIO` 构造时挂一个 `ConsoleProgressReporter`，
  订阅 `ProgressHost::changed()` 后按"500ms 后首次出字、最小行距 200ms、百分比变 5% 才重画"出**一行一条**的
  `[进度] 42% 阶段名`，宿主结束后补一行 `[进度] 已结束`；要推自己的节奏就调 `poll()`（不需先 `start()`）。
- **`ProgressHost` 现在属于 appfw**（2026-09-18 从 `src/base/progress` 搬来）：注册表/前台栈/label/变更信号是
  应用状态；base 只留零依赖的 `ProgressIndicator`/`ProgressRange`/`ProgressScope`（`vi::Progress`）。
  插件写 `<vine/appfw/ProgressHost.hpp>` + `vine::appfw::ProgressHost`，`V_APPFW_PLUGIN_ABI_VERSION` **3u**。
- **进度是推送而不是轮询**：`ProgressHost::changed()` 是进程级 `Signal<>`，在注册/注销/前台栈/label/
  整百分点时发火；位置合流在 `ProgressIndicator::setPositionCallback`（每条目一次的热路径上只多一次比较，
  -O3 实测无代价，2000 万条目 101 次通知）。GUI 呈现器与控制台消费者各自订阅（不再是单观察者回调）。
- 命令不能在自身上 `cancelAllAndWait()`（必定失败，用例钉住）；要退出应用用 `Application::quit()`。

## 命令（`CommandManager.cpp`）

- 入口点/前台链/串联门/Exclusive 接管/取消语义/历史与深度的上界：见设计文档「入口点语义」「关键机制」「不变量」。
- `LongRunning` = 占串联门 + 建 ambient `ProgressHost`；`Exclusive` = 停**所有**活链并等收尾（超时则
  `Failed("另一个操作仍在收尾，请稍后再试。")`），并且两个 Exclusive 之间靠 `exclusive_busy` 串行。
- 等待链收尾一律 5ms 切片轮询 `runs`，不用 `AsyncEvent`（有界等待销毁等待者会踩 async 生命周期契约）。

## 测试

- `tests/test_gui/test_gui.cpp`：`CommandManager_*` 37 例、`UserIOTest.*` 6 例（含工作线程读与历史去载荷）。
- `tests/test_core/SignalTest.cpp`：16 例（含"发火期间注销/清空"与"并发订阅+发火"）。
- `tests/test_asyncqt/QtAsyncTest.cpp`：2 例（`async::Scheduler` 与 `MainThreadDispatcher::resumeOnMainThread`，
  都用到 appfw 私有头）；同目录原来的 QTimer 版 `async::Sleep` 已删除（与 `vine::async::sleepFor()` 重复且无调用者）。
- `tests/test_progress/ProgressIndicatorTest.cpp`：9 例，只链 `vi::Progress vi::Core`（无 Qt）；
  `ProgressHost` 的 15 例搬到了 `tests/test_gui/ProgressHostTest.cpp`（宿主属于 appfw，测试跟着走）。
- `ConsoleProgressReporter` 的 3 例在 `test_gui`（含一例直接构造私有 `ConsoleUserIO` 抽 stdout 的端到端）。
- `tests/test_gui/RenderControlTest.cpp`：6 例，用假后端（无需 GPU）钉住渲染表面的自驱/退避/失败/状态机。
- GUI 用例需要 `QT_QPA_PLATFORM=offscreen`；全量 GUI 套件约 8 s。
