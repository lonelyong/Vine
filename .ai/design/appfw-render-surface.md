# appfw 渲染表面生命周期（2026-09-18）

## 问题

启动顺序是"窗口先生效，插件后布 UI"：`GuiApplication::init()` 里 `main_window->show()`（GuiApplication.cpp:250），
之后 `main.cpp` 才 `pluginManager()->loadAll()`，app_shell 的 `buildAppShellDock()` 才
`new RenderControl()` + `setCentralWidget()`。

后端 attach 需要"窗口存在、已布局、尺寸可用"，而这比 UI 组装晚。旧做法是插件猜一个延迟：

```cpp
QTimer::singleShot(100, [render_control] { render_control->init(); });   // 返回值被丢弃
```

两个后果：

1. **透明窗口期**：未呈现的原生窗口在屏幕上是一个洞，合成器填什么都行（用户看到"渲染区是透明的"）。
2. **可能永不恢复**：`init()` 里那三个重查定时器挂在 `if (d->backend_live)` 之下——**只有第一次就成功才会重查**；
   100ms 时若尺寸还是 0 或 `engine->initialize()` 返回 false，就再也没有人重试（表面事件早已在
   `wired` 之前被有意忽略），直到用户缩放窗口触发 resize 才恢复。

结论：这段窗口期必然存在（present 要求窗口已映射），要做的是让它**有界、可观察、不可见**，
而不是把时机交给宿主去猜。

## 文件划分（2026-09-19 拆分）

- **`RenderControl`**（公开，`sdk/vine/appfw/gui/RenderControl.hpp` + `src/gui/RenderControl.cpp`）只剩封装：
  构造 host QWidget、`createWindowContainer` 嵌表面、容器 `installEventFilter`，然后把
  `engine()/view()/init()/renderFrame()/fitToScreen()/devicePixelRatio()/state()/failureReason()`
  直接转发给表面。`stateChanged` 是唯一需要中继的一处：控件订阅表面的 `on_state_changed` 再 trigger 自己的
  信号，宿主仍然只订阅这一个入口。公开 API 与行为不变，`.cpp` 从 ~1060 行降到 ~100 行。
- **`SurfaceWindow`**（私有，`src/fw/appfw/src/gui/SurfaceWindow.hpp` + `.cpp`）装“表面 + 会话”的全部逻辑：
  引擎与 SceneView 的创建、诊断 sink、attach/重新公告、延迟 resize、settle 帧、可见性规则、状态机、
  Qt 事件翻译与 `view()->pushEvent()`、右键菜单。它是私有头、不导出（只在 appfw DLL 内使用），
  不参与公开 ABI。
- 日志前缀仍是 `[RenderControl]`：表面就是那个控件的表面，日志门禁与设计笔记都按它检索。
- `SurfaceWindow` 构造收一个 `QWidget* host`："能不能 present"要问宿主控件（表面自己的
  `isVisible()/isExposed()` 在祖先窗口隐藏时也可能为真，见“Qt 事实”），右键菜单也以它为 parent。
- 本文下方 2026-09-19 之前的条目里出现的 `handleSurfaceUpdate()` / `onSurfaceDestroyed()` /
  `initializeBackend()` / `RenderSurface` 都已经是 `SurfaceWindow` 的成员（分别是 `handleUpdate()` /
  `handleDestroyed()` / `initializeBackend()` / `SurfaceWindow`），只是当时还叫 `RenderControl.cpp` 里的名字。

## 机制：宿主给时机，控件自维护（2026-09-19 改）

- **首次 attach 只有宿主能做**：`init()` 是唯一入口，控件不排任何定时器、也不监听表面事件去替宿主
  attach。表面还没布局好时 `init()` 返回 `false`（不猜延时），宿主再调一次就是重试（幂等；已绑到
  当前句柄时立刻返回 true）。
- **已建立的会话自己维护**：平台窗口被 Qt 重建（换屏 / reparent / 把 dock 拖出去）后新句柄出现时，
  控件自己重新公告给后端（`handleUpdate()` 发现 `h != session_handle`，`renderFrame()` 里
  那条守卫同理），宿主不用察觉 —— 这是“控件拥有自己的表面”，不是“控件猜时机”。
- **两个标志各回答一个问题**（Impl 里就叫这两个名字）：`backend_live` = 最近一次 attach 尝试成功
  （后端活着）；`has_session` = 有会话（设备+管线建在 `session_handle` 上）。单向蕴含
  `backend_live ⇒ has_session`；重建后重新公告失败时前者 false、后者仍 true，这正是控件继续跟着新
  窗口的原因。
- **重试阶梯已删**：`setAutoInitialize(auto)`、`ensureAttached()`、`100 ms × kMaxAttachAttempts` 那条
  阶梯，连同构造里的 `singleShot(0)` 首触发一起删了。`Failed` 现在只剩一种来源：**没有注册任何后端
  插件**（等下去也不会变）；后端拒绝只是让 `init()` 返回 `false`，何时再试由宿主决定。
- **没建立过的会话不被接管**：`handleUpdate()` 只在 `has_session == true`（宿主真的 attach 成功过）
  时才重新 attach；从没 attach 过的表面只等宿主的 `init()`。
- **先 attach，再显示，后 present**：attach 只需要句柄+尺寸（不需要表面可见），所以表面一直隐藏到
  后端绑上；present 要求窗口真的在屏幕上，判据用容器控件的 `isVisible()`（见“Qt 事实”）。
- **句柄换了也一样“初始化好后才可见”**（用户 2026-09-19 要求）：`handleDestroyed()` 清掉
  `surface_shown`（只清 flag，不动正在销毁的窗口）——新窗口随容器一起被 Qt show，但 `SurfaceWindow::showEvent`
  按这个 flag 把它重新隐藏；后端绑上新句柄（`initializeBackend()` 成功）时才再显示。
- **状态跟着现实走**：`handleDestroyed()` 同时把状态打回 `Pending`——`Presenting` 的语义是“已经往可见
  表面出过帧”，窗口在换的时候不成立；重建后重新走一遍 `Pending → Attached → Presenting`（会话本身没变）。
  订阅 `stateChanged` 的宿主（占位图/缓存暂停之类）拿到的是真话。
- **表面由控件自己建、自己持有**（2026-09-19 简化）：`RenderControl` 在构造体里 `new SurfaceWindow(host_widget)` 并交给 window container（容器即持有者），没有工厂函数、也没有“把指针挂在 widget 的 property 上再取回来”的往返。`Control` 的 widget 是一个普通 host QWidget，里面一个 `QVBoxLayout` 装着
  `QWidget::createWindowContainer(surface, host)` 出来的容器；容器把 resize/show 报给表面（`installEventFilter`），因为 maximize 时嵌入窗口收不到自己的 resize。
- 对外接口：`SurfaceState { Pending, Attached, Presenting, Failed }` + `state()` + `stateChanged` 信号
  + `failureReason()`。
- **首帧与启动框**（2026-09-19）：`Attached` 之后的第一帧要等窗口系统把最终尺寸交给事件循环（布局几拍），
  所以它必然落在 `finishStartup()` 之后 —— 那一刻窗口还不能露时，`GuiApplication::finishStartup()`
  **不立刻关框**，而是订阅本类的 `stateChanged`、等 `Presenting`/`Failed`（`hasPresented()` 成立）才关，
  另加 2000 ms 一次性定时器兜底；跑循环的是 `run()`，不是嵌套循环。实测（本机 xcb + lavapipe）：
  `Attached` 后 21 ms 武装，首帧自身 ~180 ms（其间建 `shadow_map`/`gbuffer`/`composite` 与 3 个 program
  slot），框与 `Presenting` 同毫秒关；Windows + RTX 4060 基准：等待 759 ms，框晚 2 ms 关。
  见 `.ai/design/appfw-startup-splash.md`。
- app_shell：`new RenderControl()` → `setCentralWidget()` → demo.install() → `init()`。

## 实测（本机 xcb + lavapipe，2026-09-18）

```
[VsgHostWindow] attached to the host window 0x60004c (640x480, mapped=true)
[RenderControl] Pending -> Attached after 1783 ms (surface visible=false, exposed=false)
[RenderControl] surface shown after 1784 ms
[RenderControl] Attached -> Presenting (first frame, 143 ms after attach)
```

- **隐藏状态 attach 在 X11 上确实可行**（attach 时 `visible=false, exposed=false`）。
- `1783 ms` 是 app 自身启动（插件加载 + demo 装配）到事件循环第一拍，**与渲染路径无关**。
- 渲染侧从"表面可用"到"画面出来"约 `300 ms`：swapchain/视图 159 ms + 首帧 143 ms（后者主要是 demo
  的管线构建，见 `[VsgProgramSlot] … attached` 那几行）。
- 剩余可见窗口期 = "show → 首帧" 的 143 ms。要再缩短就是预热（见"第二步"）。

## 平台契约

- **attach-while-hidden**：X11/Windows 接受未映射窗口；**Wayland** 需要已 configure 的 `xdg_surface`，
  未映射的 surface 通常拿不到 extent。所以在**隐藏表面上 attach 失败**时控件把表面显示出来，让下一次
  尝试能看见它（日志 `attaching to a hidden surface failed: this platform wants a visible window…`，
  状态位 `needs_visible_surface`，日志只说一次）。`needs_visible_surface` **不是“以后就一直显示”**：
  首次 attach 时下一次尝试是宿主的 `init()`，已建会话跟新窗口时是控件自己的 `init()`；
  **重建后表面又是隐藏的，回退会再触发一次**（必须如此，否则那个平台上重建之后永远绑不上）。
- **未验证**：本机只有 xcb/offscreen，Wayland 的回退路径**没有在真 Wayland 上实测**（仅按 WSI 契约实现）；
  在 Wayland 上跑时先看那条日志是否出现。

## Qt 事实（probe 实测，offscreen 与 xcb 结论一致）

1. `QWindow::winId()` 会**创建**平台窗口，不要求显示；未布局时表面尺寸是 `0x0`，要等布局/事件循环才给大小
   （`/tmp/probe_surface_visible.cpp` 阶段 A–F）。
2. `QWidget::createWindowContainer` 把内嵌 QWindow 的可见性**绑在容器上**：每次 container（或祖先）Show
   都会把它重新显示（探针 G、K）⇒ "隐藏"是意图，必须在 Show 之后**重申**（本实现的
   `SurfaceWindow::showEvent` / 它装在宿主上的 eventFilter），
   否则"窗口已显示后插件才建控件"的场景下，未呈现的原生窗口照样上屏。
3. 隐藏期间 QWindow 的**尺寸仍随容器布局变化**（`378x136 → 398x146`，探针 H/I）⇒ 隐藏时 attach
   拿到的是最终尺寸，不需要猜大小。
4. QWindow 的 `isVisible()`/`isExposed()` 在"祖先窗口没显示"时也可能为真（offscreen 实测 `exposed=1`）
   ⇒ 判断"能不能 present"要看**容器控件**的 `isVisible()`，不能只看表面自己的 flag。

## 测试

`tests/test_gui/RenderControlTest.cpp`（8 例，假后端，无需 GPU）：没人调 `init()` 就一直 `Pending`
（后端一次都没被碰过、表面不显示）、`init()` 后 attach 且表面才显示、`stateChanged` 的
`Pending→Attached→Presenting` 序列且不重复、后端拒绝 ⇒ `init()` 返回 `false` 且**没有人在背后重试**、
宿主再 `init()` 可恢复、没注册后端插件 ⇒ `Failed` + 原因、窗口未显示时先热起来但状态停在 `Attached`
（不 present）、**平台窗口重建后控件自己跟过去**（新句柄被重新公告，宿主零调用；`stateChanged` 序列
钐住 `Pending → Attached → Presenting`，即重建期间状态会退回 `Pending`）。

## 为什么没有更复杂的机制（曾实现过，已删）

- **6 级退避梯子（16/50/100/200/400/900ms）**：实测在真 app 里根本没被用上（第一次事件循环拍时表面
  已经可用），先换成单一 100ms 重试 + 一个计数，**2026-09-19 连这个也删了** —— 首次 attach 是宿主的
  `init()`，控件只维护已建立的会话。
- **为“等表面”排定时器**：那就变成轮询（100ms 一圈）；规则是“要 attach 的人自己决定下一次调用”。

## 第二步：把设备/管线构建挪进 load()（2026-09-18）

**动机**：设备 + 管线构建原来落在事件循环第一拍，也就是启动框关闭之后：那 ~1 s 主线程被占着，
主窗口一帧都画不出来（实测：`finishStartup` 时 `visible=true, active=true` 但屏幕空白，直到 `Presenting`）。

**做法：宿主侧一行，框架零新增**：控件先丢进窗口，然后立刻 `init()`：

```cpp
// src/plugins/app_shell/src/AppShellUi.cpp
auto* render_control = new gui::RenderControl();
manager->setCentralWidget(render_control);   // 先进窗口；表面此时的尺寸是退化值，布局稍后给真尺寸
AppShellDemo demo(render_control);
demo.install();                              // 内容 + 管线；相机 vantage 必须在 init() 之前（orbit 的 home 取它）
render_control->init();                      // 设备 + 管线在这个同步调用里建好（load() 内）
```

- attach 只要“句柄 + 尺寸 > 0”，而新 QWindow 的退化尺寸（本机实测 **1x1**，不是 0x0）就满足；
  真实尺寸随后由布局给出，走既有的 resize / settle 路径重建 swapchain（用例钉住 `resize_calls > 1`）。
- **不需要**给控件加“初始尺寸”类 API：曾加过 `setInitialSurfaceSize()`（以及“未上屏允许 attach、
  失败不计预算、不提前显示表面”那套预热语义），用户判定是把业务/策略塞进框架层，已全部删掉。
- 平台差异：Wayland 上未映射 surface 拿不到 extent ⇒ 那里的 `init()` 返回 `false`（静默不生效），
  控件显示表面后由宿主再调一次 `init()`（“先显示再 attach”回退），不会更差。

**实测（沙箱，VSG 因环境无法初始化）**：后端 `initialize()` 已在 `load()` 内跑（~1.1 s）；在渲染可用的
机器上这段应变成 `Pending -> Attached` 落在 `load()` 内。

**测试**：`RenderControlTest.HostCanAttachRightAfterEmbeddingBeforeTheLayoutSettles`
（嵌入后立刻 `init()` → 不等事件循环就 attach；布局落下后按真实尺寸重建并 `Presenting`）。

## 已评估但未采纳

- **延后创建/插入 RenderControl 直到就绪**（原版）：自锁——控件不在已显示窗口的布局里就没有可用表面
  （`surfaceWidth()==0` 或句柄为空），`initializeBackend()` 会一直 defer，“就绪”永远不成立。
  ⚠️ 2026-09-18 补：“先给尺寸再 attach、最后嵌入”的变形（`setInitialSurfaceSize()`）也试过，已删：
  退化尺寸（1x1）本来就够 attach，先嵌入再 `init()` 更简单，框架不需要尺寸策略。
- **延后 show QWindow 到引擎就绪之后**：因果反了。引擎"就绪"指 swapchain 建好，而它需要窗口存在；
  present 又要求窗口已映射。本实现取的是它的可行内核：**隐藏时 attach（设备/swapchain），显示后才 present**。
- **在 QWindow 背后画背景当占位**：未呈现的原生窗口露出什么取决于平台/合成器，画在后面不保证可见；
  控件选择"保持隐藏，让容器自己的背景露出来"。
