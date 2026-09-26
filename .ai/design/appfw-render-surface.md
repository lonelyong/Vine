# appfw 渲染表面生命周期（2026-09-18）

## 问题

启动顺序是"窗口先生效，插件后布 UI"：窗口由 `Application::run()` 上屏（`showUserInterface()`，2026-09-26 起
`init()` 只建不 show），之后 `main.cpp` 交出去的启动工作才 `pluginManager()->loadAll()`，app_shell 的 `buildAppShellDock()` 才
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
  直接转发给表面。`state_changed` 是唯一需要中继的一处：控件订阅表面的 `on_state_changed` 再 trigger 自己的
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
- **先 attach，不显示，首帧才显示**（2026-09-20 定稿）：attach 只需要句柄+尺寸（不需要表面可见），
  present 要求窗口真的在屏幕上（判据用容器控件的 `isVisible()`，见“Qt 事实”）。**“显示”这件事归容器控件**：
  `RenderControl` 把容器隐藏着，`initializeBackend()` 成功只到 `Attached`，等首帧 present 进表面、
  `setState(Presenting)` 报到 `on_state_changed` 时容器才 `setVisible(true)`。规则的实质是
  “**露出来 = 有东西可看**” —— 未 present 的原生窗口是一块洞（旧版在 attach 时就显示它，那 1 s 全靠启动框盖着），
  而可见的容器会被 Qt 把它的矩形抹成透明（见“可见性规则归容器控件”），所以容器就是那个开关。
  表面自己**从不**碰自己的可见性（Qt 文档：容器管嵌入窗口的几何与可见性）。
- **句柄换了照样成立**：`handleDestroyed()` 只把状态打回 `Pending`；新窗口随容器一起被 Qt 收进来，
  而容器因为状态不再是 `Presenting` 而被控件重新隐藏，新窗口里落下第一帧后才再显示。
- **预热帧**（2026-09-20）：attach 成功时控件还不在屏上（启动框还盖着）就渲一帧把设备/管线开销付掉。
- **“表面能画了”是三个事件报的，不是定时器**（2026-09-20）：容器的 show（`eventFilter` 的
  `QEvent::Show` 里 `scheduleUpdate()`）、容器的 resize（`handleResized()`）、平台窗口重建（`noteSurfaceUsable()`）。
  `init()` 里那串 `singleShot(150/400/900)` 的“布局可能还没稳”重试梯子已删 —— 和当年删 attach 的退避梯子
  同一个理由：定时器是猜，事件是事实。
- **状态跟着现实走**：`handleDestroyed()` 同时把状态打回 `Pending`——`Presenting` 的语义是“已经往可见
  表面出过帧”，窗口在换的时候不成立；重建后重新走一遍 `Pending → Attached → Presenting`（会话本身没变）。
  订阅 `state_changed` 的宿主（占位图/缓存暂停之类）拿到的是真话。
- **表面由控件自己建、自己持有**（2026-09-19 简化）：`RenderControl` 在构造体里 `new SurfaceWindow(host_widget)` 并交给 window container（容器即持有者），没有工厂函数、也没有“把指针挂在 widget 的 property 上再取回来”的往返。`Control` 的 widget 是一个普通 host QWidget，里面一个 `QVBoxLayout` 装着
  `QWidget::createWindowContainer(surface, host)` 出来的容器；容器把 resize/show 报给表面（`installEventFilter`），因为 maximize 时嵌入窗口收不到自己的 resize。
- 对外接口：`SurfaceState { Pending, Attached, Presenting, Failed }` + `state()` + `state_changed` 信号
  + `failureReason()`。
- **信号改名 `stateChanged` → `state_changed`**（2026-09-21，用户拍板）：与本目录早已存在的属性变更事件
  （`GuiApplication::theme_changed`、`UIElement::name_changed`）同一风格；内部中继 `on_state_changed` 是
  `std::function` **字段**、按字段规则本就是 snake_case，改名后同一个概念只有一个名字。
  `VN_APPFW_PLUGIN_ABI_VERSION` **不需要 +1**（它只描述插件握手面 `PluginAbi`/`PluginInfo`/入口签名/命令注册，
  见 `appfw-plugin-system.md`），但用了该信号的插件必须重编：仓内是 `app_shell` 与 `test_gui`，同批改。
- **可见性规则归容器控件**（2026-09-20，用户拍板）：`RenderControl` 把窗口容器**隐藏**着，直到 `state_changed` 报到
  `Presenting` 才显示它。理由是 Qt 的 window container 在每次 paint 里把自己的矩形用 `CompositionMode_Source`
  抹成 `Qt::TRANSPARENT`（给内嵌原生窗口挖洞），所以：**可见的容器 + 还没帧 = 一块透到桌面后面的洞**
  （给它 `setAutoFillBackground(true)` 也没用：先填的背景在同一趟里被抹掉）；**隐藏的容器则根本不被 paint** —— 
  没洞，那一格就是主窗口自己的背景。原生子窗口在容器显示/隐藏时会跟着 Qt 一起变化（文档：容器管嵌入窗口的几何
  与可见性，不要自己调 `show()/hide()`），所以 `SurfaceWindow` 不再管自己的可见性：`surface_shown` /
  `setSurfaceShown()` / `handleShown()` / `showEvent()` 全删。
- **首帧与启动框**（2026-09-20）：框架不再为“窗口能不能露”做任何等待 ——
  `windowCanBeSeen()` / `deferStartupFrameClose()` / `closeStartupFrame()`（2026-09-19 那版）连同 2000 ms 定时器
  全删了，`GuiApplication::finishStartup()` 无条件关框。让它成立的是本类的可见性规则（上一节）：容器藏着就不上屏，
  所以窗口任何时候都可露，首帧之前那一格是**主窗口自己的背景** —— 不是洞，也不是“控件底色被画进去”：
  早期版把容器 `setAutoFillBackground(true)` 当成机制写在这里，是错的（那个背景与洞在同一趟 paint 里被抹掉，
  该调用已删）。
- **首帧提前（prewarm）**（2026-09-20）：attach 成功时若控件还不在屏上（还停在启动框后面），
  就按当时的表面尺寸渲染一帧（`prewarmFrame()`），把设备/管线/程序槽的一次性开销付在启动框还盖着的时候；
  这一帧不发布、不上屏、不改状态。窗口上屏后的那一帧走**就地改尺寸**
  （见 `vsg-target-resize-in-place.md`），所以预热的尺寸不是浪费。
- **实测（本机 Windows + RTX 4060，2026-09-20）**：`Pending -> Attached` 在插件 `load()` 里，
  紧随其后的预热帧 `build profile (frame): extent 320x320, targets 3, program slots 3 (132.1 ms: nodes 23.9,
  view compiles 108.2, overlay glslang 4), total 142.3 ms`（2642 ms）→ 69 ms 后
  `Startup frame going away: main window visible=true, active=true`（2711 ms）→ 392 ms 后
  `Attached -> Presenting`，这一帧 `extent 200x60, target resizes 2 (0.4 ms), program slots 0, total 3.8 ms` →
  随后稳定帧 `extent 752x480, target resizes 2 (0.2 ms), program slots 2 (40.7 ms), total 43.8 ms`。
  对照旧版：框到 `Presenting` 759 ms（现 392 ms），窗口上屏后的首帧 183.6 ms（现 3.8 ms）。
- **那一格是背景，不是洞**（2026-09-20，像素证据）：把一块品红全屏窗口放到主窗口正后方，再给主窗口
  `PrintWindow`（`PW_RENDERFULLCONTENT`，截的是窗口自己的合成，与 z 序无关）：关框那一刻渲染区是主题背景色，
  `Presenting` 之后同一区域是画面，品红一次都没露出来。
  见 `.ai/design/appfw-startup-splash.md`（那里有一节专门算这笔账）。
- `hasPresented()`：留在公开接口上（宿主问“画面出没出来”仍然合法），但它不再是“窗口能不能露”的前置条件 ——
  控件不会把空的原生窗口留在屏幕上。
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
（后端一次都没被碰过、表面不显示）、宿主上屏时一次 `init()` 就 attach **并在同一调用里出首帧**
（`Presenting` + 表面这时才可见）、`state_changed` 的 `Pending→Attached→Presenting` 序列且不重复、
后端拒绝 ⇒ `init()` 返回 `false` 且**没有人在背后重试**、宿主再 `init()` 可恢复、没注册后端插件 ⇒ `Failed` + 原因、
窗口未显示时先热起来但状态停在 `Attached` 且**表面不上屏**（`WarmsUpWhileInvisibleAndPutsTheSurfaceOnScreenWithItsFirstFrame`：
`show()` 之后靠 show 事件自己把首帧做出来）、**平台窗口重建后控件自己跟过去**（新句柄被重新公告，宿主零调用；
`state_changed` 序列钐住 `Pending → Attached → Presenting`，即重建期间状态会退回 `Pending`）。

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
（嵌入后立刻 `init()` → 不等事件循环就 attach；之后宿主改窗口尺寸，**没有定时器兜底**，靠容器 resize 事件
把后端对齐到表面真实尺寸）。

## 已评估但未采纳

- **延后创建/插入 RenderControl 直到就绪**（原版）：自锁——控件不在已显示窗口的布局里就没有可用表面
  （`surfaceWidth()==0` 或句柄为空），`initializeBackend()` 会一直 defer，“就绪”永远不成立。
  ⚠️ 2026-09-18 补：“先给尺寸再 attach、最后嵌入”的变形（`setInitialSurfaceSize()`）也试过，已删：
  退化尺寸（1x1）本来就够 attach，先嵌入再 `init()` 更简单，框架不需要尺寸策略。
- **延后 show QWindow 到引擎就绪之后**：因果反了。引擎"就绪"指 swapchain 建好，而它需要窗口存在；
  present 又要求窗口已映射。本实现取的是它的可行内核：**隐藏时 attach（设备/swapchain），显示后才 present**。
- **在 QWindow 背后画背景当占位**：未呈现的原生窗口露出什么取决于平台/合成器，画在后面不保证可见；
  控件选择"保持隐藏，让容器自己的背景露出来"。
