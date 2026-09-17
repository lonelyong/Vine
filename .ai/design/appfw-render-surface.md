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
2. **可能永不恢复**：`init()` 里那三个重查定时器挂在 `if (d->init_ok)` 之下——**只有第一次就成功才会重查**；
   100ms 时若尺寸还是 0 或 `engine->initialize()` 返回 false，就再也没有人重试（表面事件早已在
   `wired` 之前被有意忽略），直到用户缩放窗口触发 resize 才恢复。

结论：这段窗口期必然存在（present 要求窗口已映射），要做的是让它**有界、可观察、不可见**，
而不是把时机交给宿主去猜。

## 机制：控件自驱，宿主零知识

- 构造时**隐藏原生表面**并装表面事件过滤器；之后由两个触发点驱动：事件循环第一拍（`singleShot(0)`）
  与每次表面事件（`onSurfaceResized`）。
- 重试只有一种：`ensureAttached()` 里分三支——表面**还没可用**（没尺寸）⇒ 不排定时器，等表面事件
  （不轮询，日志里会有一行 `waiting for a usable surface`）；表面可用但后端**拒绝** ⇒ 每 `100 ms` 重试，
  累计 `kMaxAttachAttempts=3` 次转 `Failed`；没有注册任何后端插件 ⇒ 直接 `Failed`。
  `init()` 仍可显式重新开始一轮预算。
- **先 attach，再显示，后 present**：attach 只需要句柄+尺寸（不需要表面可见），所以表面一直隐藏到
  后端绑上；present 要求窗口真的在屏幕上，判据用容器控件的 `isVisible()`（见"Qt 事实"）。
- 对外接口：`SurfaceState { Pending, Attached, Presenting, Failed }` + `state()` + `stateChanged` 信号
  + `failureReason()`；`setAutoInitialize(false)` 给"要自己掌握时机/先配置好再渲染"的宿主。
- app_shell 删掉了那句 `singleShot(100)`，现在只剩 `new RenderControl()` + `setCentralWidget()`。

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
  未映射的 surface 通常拿不到 extent。所以首次隐藏 attach 失败时，控件**显示表面并重试**
  （日志：`attaching to a hidden surface failed: this platform wants a visible window…`，并置
  `needs_visible_surface`），此后走"先显示再 attach"。
- **未验证**：本机只有 xcb/offscreen，Wayland 的回退路径**没有在真 Wayland 上实测**（仅按 WSI 契约实现）；
  在 Wayland 上跑时先看那条日志是否出现。

## Qt 事实（probe 实测，offscreen 与 xcb 结论一致）

1. `QWindow::winId()` 会**创建**平台窗口，不要求显示；未布局时表面尺寸是 `0x0`，要等布局/事件循环才给大小
   （`/tmp/probe_surface_visible.cpp` 阶段 A–F）。
2. `QWidget::createWindowContainer` 把内嵌 QWindow 的可见性**绑在容器上**：每次 container（或祖先）Show
   都会把它重新显示（探针 G、K）⇒ "隐藏"是意图，必须在 Show 之后**重申**（本实现的 `on_shown`），
   否则"窗口已显示后插件才建控件"的场景下，未呈现的原生窗口照样上屏。
3. 隐藏期间 QWindow 的**尺寸仍随容器布局变化**（`378x136 → 398x146`，探针 H/I）⇒ 隐藏时 attach
   拿到的是最终尺寸，不需要猜大小。
4. QWindow 的 `isVisible()`/`isExposed()` 在"祖先窗口没显示"时也可能为真（offscreen 实测 `exposed=1`）
   ⇒ 判断"能不能 present"要看**容器控件**的 `isVisible()`，不能只看表面自己的 flag。

## 测试

`tests/test_gui/RenderControlTest.cpp`（6 例，假后端 `RenderBackendRegistry` 语义，无需 GPU）：
自驱 attach（布局后自动）、`stateChanged` 的 `Pending→Attached→Presenting` 序列且不重复、
`setAutoInitialize(false)` 后等宿主调 `init()`、后端连败到上限转 `Failed` 且**不再重试**、
`Failed → init()` 可恢复、窗口未显示时先热起来但状态停在 `Attached`（不 present）。

## 为什么没有更复杂的机制（曾实现过，已删）

- **6 级退避梯子（16/50/100/200/400/900ms）**：实测在真 app 里根本没被用上（第一次事件循环拍时表面
  已经可用），而"表面还没布局"本来就会被表面事件唤醒 ⇒ 换成单一 100ms 重试 + 一个计数。
- **为"等表面"排定时器**：那就变成轮询（100ms 一圈）；现在的规则是"没尺寸就不排定时器"。

## 第二步（已被数据支持，尚未做）

**预热**：`VsgRenderer` 支持无窗口初始化（`native_handle == nullptr` / `host_window == nullptr` 的路径，
device-free 自测就是这么跑的），仓库也设计过"窗口重建时设备与已编译管线不动、只把后端搬到新表面"
（`VsgProgramSlot`/`windowBuildCount()` 平坦）。因此可以把"设备 + 管线构建"提前到窗口可见之前，
把那 143 ms 压到 swapchain + 一次 present。做法是在同一状态机上加 `setPrewarm`，并按本节的数字决定默认值。

## 已评估但未采纳

- **延后创建/插入 RenderControl 直到就绪**：自锁——控件不在已显示窗口的布局里就没有可用表面
  （`surfaceWidth()==0` 或句柄为空），`initializeBackend()` 会一直 defer，"就绪"永远不成立。
- **延后 show QWindow 到引擎就绪之后**：因果反了。引擎"就绪"指 swapchain 建好，而它需要窗口存在；
  present 又要求窗口已映射。本实现取的是它的可行内核：**隐藏时 attach（设备/swapchain），显示后才 present**。
- **在 QWindow 背后画背景当占位**：未呈现的原生窗口露出什么取决于平台/合成器，画在后面不保证可见；
  控件选择"保持隐藏，让容器自己的背景露出来"。
