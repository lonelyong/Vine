# appfw 模块要点（`src/fw/appfw`）

> 详细设计按模块拆在 `.ai/design/`：`appfw-command-manager.md`（含 Command/执行链/历史/禁用）、
> `appfw-userio.md`（UserIO/ConsoleUserIO/VisualUserIO）、`appfw-progress.md`（ProgressHost 与两个呈现者）、
> `appfw-render-surface.md`（RenderControl 的表面生命周期）、`appfw-startup-splash.md`（启动框与启动进度）、
> `appfw-eventbus.md`、`appfw-plugin-system.md`、`appfw-config.md`。
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
     2026-09-18：句柄从 `Signal::Subscription` 外提为独立**非模板**类 `vn::Connection`；入口改名
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
- `CommandFlags` 现在支持位运算（`Undoable | LongRunning`），判位用 `vn::testFlag()`。
- **历史不留结果载荷**（2026-09-17）：`CommandHistoryEntry::result` 是 `CommandResult(status, message)`，
  `data()` 恒为空——条目数有上界（1024）时字节数才有上界，否则"最近 1024 次运行的载荷之和"可以到 GB 级；
  载荷属于发起那次执行的调用方。用例 `CommandManager_HistoryDoesNotRetainTheResultPayload` 钉住。
- **用户可见消息用中文，编程错误用英文**（2026-09-17 统一）：门拒绝是
  `另一个操作正在进行中，请稍候。` / `另一个操作仍在收尾，请稍后再试。`，禁用与未注册也是中文；
  `Command is null` 这类保持英文。
- `UserIO::parseInt()` 现在委托 `String::toInt()`（范围规则只有一处实现），它比 `toInt` 多出的唯一契约是
  **失败时不写 `value`**（重提示要保留用户已输入的值）。
- **嵌入渲染表面：宿主给时机，控件自维护**（2026-09-19 改）：`RenderControl` 不自己 attach 了 —— 首次
  attach 只有 `init()`（幂等；表面还没布局好就返回 `false`，不猜延时），宿主（app_shell）在
  `new` + `setCentralWidget()` + `demo.install()` 之后立刻调一次。**已建立的会话自己维护**：Qt 重建
  平台窗口（换屏/reparent/拖 dock）后新句柄由控件自己重新公告、自己重新显示，宿主零调用；重建期间
  状态退回 `Pending` 再走 `Attached → Presenting`（`Presenting` 只在真的往可见表面出过帧时成立）。
  表面**由控件持有的容器控制可见性**（容器藏着直到首帧 present，句柄换了也重新藏，`handleDestroyed()`
  只把状态打回 `Pending`）。**2026-09-19 拆分**：会话逻辑全在私有 `SurfaceWindow`（`src/gui/SurfaceWindow.hpp/.cpp`），
  `RenderControl` 只剩封装（嵌 surface + 转发公开 API，`state_changed` 用 `on_state_changed` 回调中继；
  2026-09-21 信号由 `stateChanged` 改名而来，对齐 `theme_changed`/`name_changed`），
  日志前缀仍是 `[RenderControl]`；公开 API 与用例不变，详见 `.ai/design/appfw-render-surface.md` 的“文件划分”。
  `setAutoInitialize`/重试阶梯/构造里的首触发已删；`Failed` 现在只有“没注册后端插件”一种来源。
  要知道“什么时候才能出画面”就订阅 `state_changed`（`Pending/Attached/Presenting/Failed`），别用
  `QTimer::singleShot` 猜。
- **渲染后端的初始化可以挪进 `load()`**（2026-09-18）：控件先丢进窗口、再立刻 `init()` ——
  attach 只要求“句柄 + 尺寸 > 0”，而新 QWindow 的退化尺寸（实测 1x1，不是 0x0）就够，真实尺寸随布局
  由 resize/settle 路径补齐。否则设备/管线构建会掉到事件循环第一拍（启动框关掉后那 ~1 s 空屏）。
  框架不该为这事加尺寸策略（曾加的 `RenderControl::setInitialSurfaceSize()` 已删）。
- **主窗口的启动尺寸是显式写下的**（2026-09-25）：`MainWindow` 构造里 `resize(800×600)`（与
  `setMinimumSize` 同值）。不写它的话 Qt 首 show 会**按布局 sizeHint 定尺寸**，而 hint 跟着
  ribbon/dock/日志内容走——实测六次启动六个渲染区（最大 3418×1110，见 §11.16cs/dc）；门禁因此一直
  **自己**先把窗口 resize 到 800×600 再判图（`scripts/vsg_rewrite_gate.sh`），写上之后那一步从“补偿”
  变“确认”。钉子 `MainWindowTest.TheOpeningSizeIsStatedInsteadOfInheritedFromTheLayout`（去掉 resize 行 ⇒ 红）。
- **无头模式已经有进度显示了**（2026-09-18）：`ConsoleUserIO` 构造时挂一个 `ConsoleProgressReporter`，
  订阅 `ProgressHost::changed()` 后按"500ms 后首次出字、最小行距 200ms、百分比变 5% 才重画"出**一行一条**的
  `[进度] 42% 阶段名`，宿主结束后补一行 `[进度] 已结束`；要推自己的节奏就调 `poll()`（不需先 `start()`）。
- **`ProgressHost` 现在属于 appfw**（2026-09-18 从 `src/base/progress` 搬来）：注册表/前台栈/label/变更信号是
  应用状态；base 只留零依赖的 `ProgressIndicator`/`ProgressRange`/`ProgressScope`（`vn::Progress`）。
  插件写 `<vine/appfw/ProgressHost.hpp>` + `vn::appfw::ProgressHost`，`VN_APPFW_PLUGIN_ABI_VERSION` **3u**。
- **进度是推送而不是轮询**：`ProgressHost::changed()` 是进程级 `Signal<>`，在注册/注销/前台栈/label/
  整百分点时发火；位置合流在 `ProgressIndicator::setPositionCallback`（每条目一次的热路径上只多一次比较，
  -O3 实测无代价，2000 万条目 101 次通知）。GUI 呈现器与控制台消费者各自订阅（不再是单观察者回调）。
- **启动框（2026-09-18）**：`AppConfig::splash` 开（`enabled`/`title`/`subtitle`/`logo`），框架自己上报
  "初始化界面 + 逐个插件"，应用插入自己的阶段用 `app->startupProgress()->stage("正在初始化日志")`（无框时是空操作）。
  **宿主必须在进主循环前调 `Application::finishStartup()`**：框不自动关（忘了会盖着主窗口，`run()` 记 warning）；
  它是幂等的，也是销毁上报口的地方。阶段语义是**阶段内比例**（可计数 `stage(name,total)`+`advance`；
  不确定 `stage(name)`），不是全局 ETA。
- ⚠️ **有启动框时主窗口照旧在 `init()` 里 show()**：嵌入式渲染表面要用顶层窗口的原生句柄建 swapchain，
  没 show 过（或隐藏）的窗口给不出来 → VSG `GetClientRect failed: 无效的窗口句柄` + `surface Failed`。
  启动框是 stay-on-top splash，盖在窗口上面。
- ⚠️ **启动期不要 `processEvents()`**：会顺手跑别的组件的定时器/事件（渲染表面的 resize/settle 更新
  就是这样被提前唤醒的）。启动框只 `repaint()` 自己那一帧。
- ⚠️ **唯一例外（2026-09-26）：闪屏与主窗都要“show 之后派发到它画出第一帧”**。X11 上 Qt 要先收到服务端的
  expose 才把 backing store flush 进窗口，而 expose 只能由事件队列派发送来 ⇒ 不派发时 `repaint()` 是空转，
  闪屏整个启动期全透明、主窗整个启动期是一块黑板（用户实测报的就是这两条）。机制：`GuiApplication::init()`
  经 `showAndWaitForFirstPaint()` 把 show 与派发成对（上限 300 ms，超时 `VN_LOGW`），条件是 SDK 级的
  `Window::hasPainted()`（`WindowData` 里的 `PaintWatcher`：窗口自己 + 已有子控件 + 后加的子控件），
  助手）**内部**用一条 `assert(d->main_window == nullptr || d->main_window->primaryRenderControl() == nullptr)`
  把安全前提写死（同一句覆盖两个调用点：闪屏那次主窗还不存在，主窗那次插件还没加载）。实测：
  闪屏 `5–11 ms`、主窗 `17–24 ms`（轮询步进 2 ms）。
  测试：`tests/test_gui/WindowPaintTest.cpp`（3 例）；Qt 自己的 `QSplashScreen::repaint()` 也是调 `processEvents()`
  （文档："even when there is no event loop present"）——即这是 Qt 级行为，不是 WSL 缺陷。
- ⚠️ **启动框关掉时要把主窗口 `raise()` + `activate()`**：`Qt::SplashScreen` 置顶且不激活进程地显示，
  Windows 的前台激活名额被它占掉，随后 show() 的主窗口就压在终端/IDE 后面（看起来像“没显示出来”）。
  `finishStartup()` 只“确实有框”时做，那一刻会打一行 `main window visible=…, active=…` 供区分。
- ⚠️ **“框关掉时窗口必须已经能画”这条责任 2026-09-20 挪回渲染视图**：框架那套等待
  （`windowCanBeSeen()` / `deferStartupFrameClose()` / `closeStartupFrame()` + 2000 ms 定时器）**已整个删除**
  （`GuiApplicationData` 的两个字段、常量也一并删），`finishStartup()` 无条件关框。取代它的是 `RenderControl`
  的规则：**窗口容器藏着，直到 `state_changed` 报到 `Presenting`** —— 可见的容器会被 Qt 用
  `CompositionMode_Source` + `Qt::TRANSPARENT` 抹成洞（嵌入窗口的洞），隐藏的容器不被 paint，所以那一格是
  主窗口自己的背景；表面自己不再管可见性（`surface_shown`/`setSurfaceShown()`/`handleShown()`/`showEvent()`
  全删，容器 `setAutoFillBackground(true)` 是错的机制、已删）。首帧提前：`initializeBackend()` 里控件不在屏上
  就 `prewarmFrame()`（按当时尺寸渲一帧，付掉设备/管线开销，不发布），上屏后的首帧走就地改尺寸。
  同时删掉 `init()` 里 `singleShot(150/400/900)` 的重试梯子：“表面现在能画了”由容器上屏
  （`eventFilter` 的 `QEvent::Show` 补 `scheduleUpdate()`）/ 容器 resize / SurfaceCreated 三个事件上报。
  实测（Windows + RTX 4060，2026-09-20）：`Pending -> Attached` 在 `load()` 里（2642 ms）→ 预热帧
  `extent 320x320, targets 3, program slots 3, total 142.3 ms` → 69 ms 后关框（2711 ms）→ 392 ms 后
  `Attached -> Presenting`，这一帧 `total 3.8 ms`（旧版 183.6 ms）→ 稳定帧 `extent 752x480, program slots 2,
  total 43.8 ms`。**框到 Presenting：759 ms → 392 ms**。像素证据：品红窗口垫在主窗口背后 + `PrintWindow`
  （与 z 序无关）—— 那一格是主题背景色，`Presenting` 后是画面，品红没露过。契约不变：
  **插件从 `load()` 返回即表示其子系统可用**，`finishStartup()` 仍是“宿主 + 插件的活都干完了”。
- ⚠️ **`ConsoleUserIO` 现带 `VN_APPFW_API`**（类仍私有，头在 `src/`）：`test_gui` 直接构造它抽 stdout，
  不导出就 LNK2019（`6ec0e4d` 起 `test_gui` 一直链不上，2026-09-18 修）。
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
  都用到 appfw 私有头）；同目录原来的 QTimer 版 `async::Sleep` 已删除（与 `vn::async::sleepFor()` 重复且无调用者）。
- `tests/test_progress/ProgressIndicatorTest.cpp`：9 例，只链 `vn::Progress vn::Core`（无 Qt）；
  `ProgressHost` 的 15 例搬到了 `tests/test_gui/ProgressHostTest.cpp`（宿主属于 appfw，测试跟着走）。
- `ConsoleProgressReporter` 的 3 例在 `test_gui`（含一例直接构造私有 `ConsoleUserIO` 抽 stdout 的端到端）。
- `tests/test_gui/RenderControlTest.cpp`：8 例，用假后端（无需 GPU）钉住渲染表面的“宿主给时机 + 已建会话
  自己维护 + 隐藏到绑上为止”状态机（含平台窗口重建后自己跟过去、状态退回 `Pending`）。
- GUI 用例需要 `QT_QPA_PLATFORM=offscreen`；全量 GUI 套件约 8 s。
