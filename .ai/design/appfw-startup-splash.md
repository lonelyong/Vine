# appfw 启动框与启动进度（`BootSplash` / `StartupProgress`）设计（2026-09-18）

## 目标与定位

启动期没有进度可看：`GuiApplication::init()` 建主窗口（Ribbon、停靠面板）与 `PluginManager::loadAll()`
都在应用线程上跑，用户面对的是一个"还没长齐的窗口"，而且插件加载快慢完全不可见。
本次加的是**框架级**的启动框：任何 app 通过 `AppConfig::splash` 开一个，框里显示应用身份
（logo/标题/副标题）+ 当前在做什么（"正在加载插件 app_shell (2/3)"）+ 进度条。

"通用"落在两件事上：

- **应用只为外观配置一次**（`AppConfig::splash`：`enabled`/`title`/`subtitle`/`logo`），不写任何呈现代码；
- **启动期进度是应用级状态**，框架自己上报（界面初始化 + 插件逐个大报），应用可以插入自己的阶段
  （Vine 的 `main.cpp` 报了一行"正在初始化日志"）。

## 组成

| 部件 | 位置 | 职责 |
| --- | --- | --- |
| `SplashConfig` | `AppConfig::splash`（`AppBuilder.hpp`） | 应用侧配置：开关 + 标题/副标题/logo |
| `StartupProgress` | `sdk/vine/appfw/StartupProgress.hpp` | 启动期上报口：阶段、状态文字、比例；进程内至多一个 |
| `gui::BootSplash` | `sdk/vine/appfw/gui/BootSplash.hpp` | 呈现：无边框自绘圆角框 + 状态行 + 进度条 |
| `Application` | `beginStartupProgress()` / `startupProgress()` / **`finishStartup()`** | 生命周期与销毁 |
| `GuiApplication` | `init()` / `finishStartup()` 覆写 | 建框、显示、关闭 |
| `PluginManager::loadAll()` | 上报"正在查找插件"、"正在创建/加载/收尾插件 x (i/n)" | 启动里最长、最不可预测的一段 |

**为什么上报口在 appfw 而不是 base**：与 `ProgressHost` 同源——"应用此刻在干什么"是应用状态，
且要往 `ProgressHost` 里报（Core 的 Signal）。上报口**自己持一个前台 `ProgressHost`**，
所以启动进度不是启动框私有的：状态栏的 `ProgressPresenter`、无头模式的 `ConsoleProgressReporter`
不需要任何额外接线就能显示同一份状态。

## 两种阶段，以及为什么不是"整体 ETA"

- **可计数阶段** `stage(name, total)`：有总量，进度条按 `done/total` 推进（插件加载就是这样，
  每个插件的 `load()` 回来算一份）。
- **不确定阶段** `stage(name)`：只知道在做什么，进度条显示"进行中"。

**每个可计数阶段从零开始**（等于该阶段自己的比例），而不是整次启动的剩余。
理由：上报方不可能知道还没走到的阶段有多长，硬凑一个全局百分比要么在阶段切换处跳回 0
（骗人），要么得让应用预先声明权重表（把框架的复杂度转嫁给应用）。
用户真正要看的是"正在加载哪个插件"和"这段还要多久"，前者由状态文字精确回答
（`setLabel("正在加载插件 app_shell (2/3)")`），后者由当前阶段的真实比例回答。

## 生命周期

```
config.splash.enabled = true
createGuiApplication()          → init(): 建上报口 → stage("正在初始化界面") → 建框并 show() → 建主窗口并 show()
main: 应用自己的阶段            → stage("正在初始化日志") …
pluginManager()->loadAll()      → stage("正在查找插件") → stage("正在加载插件", n) → 逐个 setLabel/advance
app->finishStartup()            → 关框（窗口还不能露时推迟到渲染视图出帧）+ 结束上报口（complete() 补满进度条）
app->run()                      → 主循环
```

- **`finishStartup()` 必须由宿主显式调用**（用户拍板）：框架不知道应用的启动工作何时结束，
  不猜超时、不自动关。忘记调用时：框一直盖着主窗口，`run()` 与 `~GuiApplication()` 各记一条 warning
  （不静默兜底，也不替宿主做决定）。
- `finishStartup()` 是**幂等**的：没有上报口时它什么都不做。
- 上报口的销毁是刻意的（不是只 `complete()`）：活着的宿主会一直待在前台栈上，
  把之后所有命令的进度挡在后面。
- 没有上报口时（`StartupProgress::current() == nullptr`，例如未启用启动框、或测试）**所有调用都是空操作**，
  所以启动代码可以无条件上报。

## 渲染

- **自绘圆角面板**：`QWidget` 子类 `paintEvent` 画圆角矩形，颜色取 `palette(window)` / `palette(mid)`，
  深浅主题自动跟随；窗口 `Qt::SplashScreen | Qt::FramelessWindowHint` + `WA_TranslucentBackground`，
  居中于主屏（启动期还没有用户选过的显示器）。**不改 appfw 现有控件样式**，也不引第三方。
- **logo**：`.svg` 走 `QSvgRenderer`（按框等比缩放，带 DPR），其余走 `QPixmap`；读不到的图按"没有 logo"
  处理——启动不该因为一张装饰图失败。空标题回落到 `AppConfig::name`（由 `createGuiApplication()` 解析：
  `init()` 时 `QCoreApplication::applicationName()` 还没被 application builder 设上）。
- **重绘用 `repaint()`，不 `processEvents()`**：启动期加载占着应用线程，不主动重绘则首帧要等启动结束；
  但 pump 整个事件队列会顺手把**别的**组件的定时器/事件也跑了——嵌入式渲染表面的 resize/settle 更新
  就是这样被提前唤醒的
  （见下）。`repaint()` 只同步画这一帧，不替别人推进状态机。
- 状态文字按框宽手工 `elidedText`（插件名可能很长，QLabel 会自己把窗口撑宽）。

## ⚠️ 主窗口照旧在 `init()` 里 `show()`（实测结论）

一开始的实现是"有启动框就不显示主窗口，等 `finishStartup()` 再显示"，**实测崩在渲染后端**：

```
[VsgRenderer] initialize FAILED at 'creating Vulkan window (instance/device/swapchain)':
  vsg::Win32_Window::Win32_Window(...) GetClientRect(..) failed : 无效的窗口句柄 (VkResult 1400)
[RenderControl] surface Pending -> Failed after 1216 ms: the render backend would not initialize
```

- 原因：嵌入式渲染表面（`RenderControl` + VSG 后端）要用**顶层控件的原生窗口**建交换链，
  而没被 `show()` 过的窗口没有可用的 HWND（`appfw-render-surface.md` 的
  "延后创建/插入 RenderControl 直到就绪：自锁"正是同一件事）。
  试过 `show()` 后立刻 `setVisible(false)`（想"先实现再隐藏"）：**同样失败**——隐藏的顶层窗口一样给不出 swapchain。
- 所以：**主窗口保持与原行为一致地显示**，启动框作为 stay-on-top 的 splash 盖在它上面。
  代价是"框后面的窗口已经可见"，收益是不碰渲染路径的启动时序（那套时序很脆，见该设计文档的"实测"一节）。
- 插一句同源提醒：**启动期不要替渲染组件 pump 事件**（本节第一条日志就是 pump 提前唤醒 attach 的样子）。
  这条规则有一个例外，见"框要上屏，先派发'窗口已可见'通知"一节：闪屏 `show()` 之后必须派发到它画出第一帧，
  而那一刻主窗与插件都还不存在。

### 框关掉后必须把主窗口提到前面（2026-09-18 用户实测报“框没了，主窗口没出来”）

- 机制：`Qt::SplashScreen` 既**置顶**又**不激活进程地显示**，而 Windows 只把“前台激活”给新进程的
  **第一个**窗口 —— 这个名额被启动框占掉，于是随后 `show()` 的主窗口**显示在框下面、且一直没成为前台窗口**，
  压在启动它的终端/IDE 后面。日志侧对照：主窗口 1.4 s 就已 `mapped=true`，所以它一直在，只是被压着。
- 修法（与 `QSplashScreen::finish()` 同一套）：`GuiApplication::finishStartup()` 先 `delete` 框，再
  `native->raise()` + `activate()`。只在**确实有框**时做：无框应用不该在长启动结尾抢别人的焦点。
- 诊断行（那一刻打）：`Startup frame going away: main window visible=…, active=…`，
  用来把“被压在后面”（visible=true）与“根本没显示”（visible=false）分开。

### 框关掉时窗口必须已经能画（2026-09-20：这条责任挪回渲染视图，框架不再等）

- 实测现象（保留作证据，2026-09-19）：`finishStartup()` 那一刻 `main window visible=true, active=true`，
  但渲染区空白，直到 `Attached -> Presenting after ~3300 ms`。差的那 ~0.7–0.9 s 不是“渲染后端初始化慢”，
  而是**首帧的时机**：`Attached` 的语义只是“后端绑上了”，一帧都没提交，而首帧要等事件循环把窗口推到屏幕上，
  再按窗口系统的最终尺寸建图和提交。
- 框架一度为此等待（`windowCanBeSeen()` / `deferStartupFrameClose()` / `closeStartupFrame()` + 2000 ms 一次性
  定时器，2026-09-19，见 git 历史）。**2026-09-20 三个方法连同常量、订阅、定时器整个删掉**：
  `finishStartup()` 现在无条件关框，框架不再知道“首帧什么时候到”。
- **取代它的规则在渲染视图里：容器控件藏着，直到有帧落进去**（`RenderControl`，见 `appfw-render-surface.md`）。
  未 present 的原生窗口是一块洞（合成器随便填），而 Qt 的 window container 在 paint 里把自己的矩形抹成透明
  （`CompositionMode_Source` + `Qt::TRANSPARENT`），所以“可见的容器 + 还没有帧”就是一块洞；
  隐藏的容器根本不被 paint，那一格于是是**主窗口自己的背景**。`RenderControl` 因此把容器隐藏着，
  `state_changed` 报到 `Presenting` 才显示它；表面自己的可见性不碰（容器管嵌入窗口的几何与可见性，
  自己 `show()/hide()` 是 Qt 文档里不推荐的做法），`surface_shown` / `setSurfaceShown()` / `handleShown()` /
  `showEvent()` 都删了。
- **首帧提前：attach 时先预热一帧**（2026-09-20）。`SurfaceWindow::initializeBackend()` 成功后看控件在不在屏上：
  不在（还停在启动框后面）就走 `prewarmFrame()` —— 按当时的表面尺寸（实测 320x320，布局还没跑）
  渲染一帧，把设备与管线的一次性开销付掉，不发布、不上屏、不改状态。窗口上屏后的第一帧由此变成**就地改尺寸**
  的一帧（`target resizes 2`、`program slots 0`），不再重付那 183.6 ms。
- 顺带把首帧的时机本身改成事件驱动（2026-09-20）：`init()` 里那串 `singleShot(150/400/900)` 重试梯子删了，
  “表面现在能画了”由三个事件报告 —— 容器的 show（`eventFilter` 的 `QEvent::Show` 里补 `scheduleUpdate()`）、
  容器的 resize、平台窗口重建（SurfaceCreated）。
- **实测（本机 Windows + RTX 4060，2026-09-20）**：`Pending -> Attached` 在插件 `load()` 里（2642 ms），
  预热帧 `build profile (frame): extent 320x320, targets 3, program slots 3, total 142.3 ms` → 69 ms 后
  `Startup frame going away: main window visible=true, active=true`（2711 ms，框立刻走，没有武装、没有等待）
  → 392 ms 后 `Attached -> Presenting`，这一帧 `total 3.8 ms`（旧版同一帧 183.6 ms，因为那一帧是拿窗口最终
  尺寸冷建的）；稳定帧 `extent 752x480, target resizes 2 (0.2 ms), program slots 2 (40.7 ms), total 43.8 ms`。
- **这一格是背景而不是洞**（2026-09-20，像素证据）：品红全屏窗口垫在主窗口背后 + `PrintWindow`
  （`PW_RENDERFULLCONTENT`，截的是窗口自己的合成，与 z 序无关）：关框那一刻渲染区是主题背景色，
  `Presenting` 后同一区域是画面，品红一次都没露过。旧版这里写成“露出来的是控件底色
  （容器 `setAutoFillBackground(true)`）”，机制说错了：那个背景和洞在同一趟 paint 里被抹掉。
  渲染器一侧的完整链条见 `appfw-render-surface.md`。
- **契约没变的部分**：**插件从 `load()` 返回就表示它的子系统已经可用**；`finishStartup()` 仍是
  “宿主自己的活 + 所有插件的活都干完了”。`Pending`（宿主还没让它 attach）与这条无关。
- 不放在插件里的原因（当时）：`load()` 跑在 `loadAll()` 中段，在那儿等会拖住**其它插件**的加载与进度上报。

### 框要上屏，先派发"窗口已可见"通知（2026-09-26：WSL 下启动框整只全透明）

**现象**（用户实测报"启动画面，wsl 下不显示"）：闪屏的 X 窗（440×152、depth 32 ARGB，外加 WM 的
504×216 装饰框）已映射、`MAP_STATE_VIEWABLE`、尺寸也对，**但整个生命期每个像素都是 `rgba(0,0,0,0)`** ——
既不是"被别的窗口盖住"，也不是"位置跑到屏外"，而是**窗口里一个像素都没有**。

**探针**（临时 `fprintf` 到 stderr，验后已删）：`show()` 之后只收到 `QEvent::Show(exposed=0)`，
16 次启动上报各调一次 `repaint()`，**全程没有 `QEvent::Expose`，`paintEvent` 一次都没跑**；
框在启动结束（~1.9 s）被 `delete`。

**分辨"服务端还是客户端"**（两个 X 连接的最小客户端：A 建窗+填色，B 独立 `XGetImage`）：B 在 0 ms 就读到了填充
⇒ X 服务端**立刻**保存新窗内容，卡住的不是服务端。

**机制**：X11 上 Qt 把内容画进窗口要先有"窗口已可见"的通知（expose 事件），而它只能由**事件队列派发**送达；
启动期不跑事件循环（这正是当初选 `repaint()` 的原因），于是 `repaint()` 只画进 backing store，
一个像素都到不了窗口。Windows 上 `repaint()` 直写 HWND，所以同一份代码在 Windows 正常。
（**夸平台口径**：Qt 自己在 `QSplashScreen::repaint()` 的说明里写的就是"标准 `repaint()` 也要调 `processEvents()`，
以保证更新显示，**even when there is no event loop present**"，"有些 X11 WM 不支持 stays-on-top，
办法是定时 `raise()`"——即这是"启动期不跑事件循环"的 Qt 级行为，不是 WSL/Weston 的缺陷。）

**修法**：`GuiApplication::init()` 把"show 窗口"与"派发到它画出第一帧"成对放在
`showAndWaitForFirstPaint()` 里（`processEvents(ExcludeUserInputEvents)` + 2 ms 步进，上限 300 ms，
超时 `VN_LOGW` 不静默），判断条件就是 `Window::hasPainted()`（见下节）。
位置刻意选在"主窗与所有插件都还不存在"那一刻，并把这个前提用 `assert` 写死在
**函数内部**（而不是只写在注释里，也不拆到两个调用点上）：
`assert(d->main_window == nullptr || d->main_window->primaryRenderControl() == nullptr)` ——
同一句覆盖两个调用点（闪屏那次主窗还不存在，主窗那次插件还没加载）。
所以本文件上面那条"不要替渲染组件 pump"的顾虑在这里不成立（那时还没有渲染表面）。

**实测**（本机 WSLg/Weston + Xwayland，2026-09-26）：`startup frame painted after 11 ms`（本次启动的第一条日志），
独立 X 读回 `440×152 painted=99.9% modal=rgba(245,245,245,255)`（主题面板色），从 ~0.4 s 一直覆盖到启动结束（~1.9 s）。

**反证（真机）**：去掉 `init()` 里的派发 ⇒ 闪屏回到 `painted=0.0% rgba(0,0,0,0)`；恢复 ⇒ 99.9%。

**顺带修正**：`BootSplash.hpp` 的类注释里"a notification repaints **and pumps the event queue**"是旧设计的残留
（实现从 6988216 起就只 `repaint()`），已改成与实现一致，并写清"把窗口系统那一半交给宿主"。

### 主窗在启动期也是一块黑板（2026-09-26，同一根因）

**现象**（用户实测报"splash 还没完，主窗口就显示出来了，只是开始全黑"）：闪屏按预期亮着，但主窗口是**全黑**的。

**实测**：主窗的 WM 框（`864x664`）在整段启动期（~0.35 s → 1.9 s）`XGetImage` 采样都是 `painted=0.0%`，
直到 `run()` 进事件循环才跳到 `83.5%`。与闪屏同根：Qt 把窗口画上屏要事件队列派发，而启动期不跑事件循环。

**修法**：`init()` 里主窗 `show()`（状态栏进度条先挂好，这样首帧就把框架放进窗口的东西都画上）
也走同一个 `showAndWaitForFirstPaint()`（守卫在函数里，见上一节）。

**实测**：`main window painted after 12–21 ms`，主窗框从 ~0.35 s 起就是 `83.5%`（余下 16.5% ≈ 378×247
就是渲染区那块"洞"——容器里的原生子窗还没有帧，见 `appfw-render-surface.md`；首帧呈递后就是画面）。

**反证（真机）**：去掉主窗派发 ⇒ 主窗回到 `painted=0.0%`、没有 `main window painted` 行；恢复 ⇒ `83.5%`。
`test_gui` **216/216**；两棵树门禁 `cases=451 failed=0 vuid=0 hazard=0`，应用阶段像素与基线**逐字节一致**
（额外派发没打扰渲染表面，进度条提前挂也不影响画面）。

### “已画过”是 SDK 级契约：`Window::hasPainted()`（2026-09-26 复核后重排）

两个窗口问的是同一个问题，所以机制只留一份、放在它该在的层：

- **契约**：`Window::hasPainted()`（公开 SDK，`noexcept`）。语义：**窗口自己或它里面任何东西画过**；
  窗口已不再是一个“空窗口”（X11 上全黑、透明框则全透明）。
- **实现**：`WindowData` 持一个 `Window.cpp` 内部的 `PaintWatcher`（事件过滤器）。安装时递归接上
  窗口与当时已有的全部子控件；**后加的**子控件（停靠面板、状态栏里的进度条）靠父控件的 `ChildAdded`
  通知接上。两条路径各有原因：顶层自己可能一个像素都不画（面积被不透明子控件盖住），
  而“谁先画”取决于布局；窗口里的东西也不必在观察之前就存在。
- **观察者随窗口销毁**（不挂在被观察的 widget 上：窗口不拥有 widget 时 widget 会先死）。
- **测试**（`tests/test_gui/WindowPaintTest.cpp`，3 例）：未 show ⇒ 未画；show + 派发 ⇒ 已画；
  已在里面的子控件画 ⇒ 已画；后加进来的子控件画 ⇒ 已画。
- **变异（4/4 各有用例负责）**：`Paint` 不置位 ⇒ 3 例全红；观察者不装到窗口自己 ⇒ 2 例红；
  忽略 `ChildAdded` ⇒ 只“后加进来的子控件”红；安装时不接已有子控件 ⇒ 只“已在里面的子控件”红。

**另一个好处**：这套机制让“等待”变成可验证的后置条件，而不是平台 hack——
没有 `#ifdef`，在派发即可画的平台上循环第一次就退出。

**没有改 z 序（当日量过，记下免得再查）**：怀疑"主窗压住闪屏"时量过 X 子窗顺序
（`XQueryTree` 自下而上）——主窗框始终列在闪屏框之后（=更靠上），而且在
`impl<QWidget>()->raise()` 前后**都不变**；而用户能看到闪屏 ⇒ 这个顺序在这台 WSLg 上反映不了合成结果。
所以**不加**这行（无实测效果的代码不写）。

## 平台注意

- 圆角靠 `WA_TranslucentBackground`；没有合成的 X11 会话里圆角会退化成黑角（Windows/WSLg 正常）。
  要绝对稳就把 `paintEvent` 的面板改成满矩形（一处改动）。
- **"窗口已映射"不等于"框上屏"**：还要窗口系统把"现在可见了"的通知派发进来（见下节）。
  WSLg 上框的 X 窗从建出来就是 `Viewable`，但只要不派发就一个像素都没有。
- Wayland 上"未映射窗口 attach"本就不成立，`RenderControl` 会走"先显示表面再 attach"的回退
  （见 `appfw-render-surface.md` 的平台契约）；启动框与它的关系只是"框也在最上层"。

## ABI

`VN_APPFW_PLUGIN_ABI_VERSION` **不需要 +1**：插件可见面（`PluginAbi`/`PluginInfo`/`Plugin`/
`PluginLoadContext`/入口签名/命令注册）一个都没动；`StartupProgress` 是新增 API，`Application`
新增的 `finishStartup()` 是**追加在虚表末尾**（声明在所有既有虚函数之后）。

## 顺带修复（不修就没法跑 GUI 用例）

`tests/test_gui/test_gui.cpp` 里的 `ConsoleProgressReporterTest.ConsoleUserIOPrintsTheProgressOfAForegroundOperation`
直接构造 **appfw 私有**类 `ConsoleUserIO`（`src/ConsoleUserIO.hpp`），但该类没有导出宏，
`viAppfwd.lib` 里根本没有它的符号 ⇒ `test_gui` 在 HEAD 上**链接失败**（LNK2019 ×2，`6ec0e4d` 引入）。
修法：给 `ConsoleUserIO` 加 `VN_APPFW_API`（类仍私有，只多两个导出符号），保留那个端到端用例。

## 测试映射

| 用例 | 覆盖 |
| --- | --- |
| `StartupProgressTest.NoSinkByDefault` | 默认没有上报口（未启用启动框的应用/测试不受影响） |
| `StartupProgressTest.SinkIsForegroundAndProcessWide` | `current()` 进程内唯一；宿主在前台栈上，销毁后前台恢复 |
| `StartupProgressTest.IndeterminateStageReportsLabelOnly` | 不确定阶段只出状态文字 |
| `StartupProgressTest.CountedStageReportsFraction` | 绝对值推进、忽略倒退、`setLabel` 细到插件一级、`complete()` 补满 |
| `StartupProgressTest.EveryCountedStageStartsOver` | 每个可计数阶段从零开始 |
| `StartupProgressTest.StageWithoutTotalIsIndeterminate` | `total <= 0` 退化成不确定阶段 |
| `BootSplashTest.ShowsReportedStageAndProgress` | 框的状态文字/比例/不确定态跟着上报口走 |
| `BootSplashTest.MissingLogoIsNotFatal` | 读不到的 logo 不致命 |
| `BootSplashTest.FrameSurvivesTheEndOfTheBoot` | 上报口先死（宿主先关框）时保留最后一帧，不读已销毁对象 |
| `BootSplashTest.DisabledByDefaultInTheTestApplication` | 默认关闭；`finishStartup()` 幂等 |
| `BootSplashTest.TheFramePaintsOnceTheWindowSystemHasShownIt` | `Window::hasPainted()` 直到窗口系统通知可见（派发事件队列）之后才为真 |
| `WindowPaintTest.AWindowHasNotPaintedUntilTheQueueHasBeenDispatched` | 未 show 未画；show + 派发后已画 |
| `WindowPaintTest.APaintOfAnythingThatIsAlreadyInsideTheWindowCounts` | 窗口构造时就已存在的子控件画了也算 |
| `WindowPaintTest.APaintOfAWidgetThatArrivesLaterCounts` | 窗口构造之后才加进来的子控件画了也算（`ChildAdded`） |

视觉验证（临时用例 + `grab()` 离屏渲染，验证后已删）：圆角面板/标题/副标题/进度条/状态行、
SVG logo 等比缩放、确定态与忙碌态两种进度条。

## 已评估但未采纳

- **隐藏主窗口、等 `finishStartup()` 再显示**：见上，渲染后端需要已显示的顶层窗口。
- **把上报口做成 base `Application` 自动创建**：没用启动框的无头应用/测试会平白多出一个前台宿主，
  把 `ProgressHost::current()` 的语义搅浑（现有用例断言"默认 nullptr"）。改成"启用启动框才建"，
  无头应用想报启动进度走 `beginStartupProgress()`（protected）。
- **全局加权 ETA**（把根标尺切成固定权重带）：需要在阶段开始前就知道全部阶段及其权重，
  而应用插入的阶段只有应用自己知道；退化成"应用要预先声明权重表"，不如现在的"阶段内比例 + 状态文字"诚实。
- **`QSplashScreen`**：只有一行文字 + 一张图，画不了"状态行 + 进度条 + 主题跟随"。
- **给启动框加动画/淡出**：需要在 `finishStartup()` 里跑一段事件循环（或定时器），
  与"显式结束、立刻进主循环"的契约冲突；需要时再说。
- **把 `SplashConfig` 放进配置文件**（`app.splash.*`）：用户明确选了"代码配置"，
  多个 app 各自在 `main.cpp` 里声明；要改成配置项时只需 `createGuiApplication()` 里读一次 `ConfigManager`。
