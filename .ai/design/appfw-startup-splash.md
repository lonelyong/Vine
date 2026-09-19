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

### 框关掉后必须把主窗口提到前面（2026-09-18 用户实测报“框没了，主窗口没出来”）

- 机制：`Qt::SplashScreen` 既**置顶**又**不激活进程地显示**，而 Windows 只把“前台激活”给新进程的
  **第一个**窗口 —— 这个名额被启动框占掉，于是随后 `show()` 的主窗口**显示在框下面、且一直没成为前台窗口**，
  压在启动它的终端/IDE 后面。日志侧对照：主窗口 1.4 s 就已 `mapped=true`，所以它一直在，只是被压着。
- 修法（与 `QSplashScreen::finish()` 同一套）：`GuiApplication::finishStartup()` 先 `delete` 框，再
  `native->raise()` + `activate()`。只在**确实有框**时做：无框应用不该在长启动结尾抢别人的焦点。
- 诊断行（那一刻打）：`Startup frame going away: main window visible=…, active=…`，
  用来把“被压在后面”（visible=true）与“根本没显示”（visible=false）分开。

### 框关掉时窗口必须已经能画（2026-09-19：关框推迟到渲染视图出帧）

- 实测现象（保留作证据）：`finishStartup()` 那一刻 `main window visible=true, active=true`，但渲染区空白，
  直到 `Attached -> Presenting after ~3300 ms`。差的那 ~0.7–0.9 s 不是“渲染后端初始化慢”，而是**首帧的时机**：
  原生窗口的最终尺寸来自窗口系统（要事件循环），所以 `Attached` 之后的第一帧只能落在事件循环跑起来之后，
  而 `finishStartup()` 比它早 —— 框关在了还没画的那一瞬。
- 一度在框架里加的是 `SplashConfig::wait_for_first_frame`（按**固定时长**等主渲染视图首帧），**仍然删掉**：
  那是猜时长，也是把具体业务塞进框架。
- **现在的机制**（`GuiApplication.cpp`（2026-09-19）：`windowCanBeSeen()` / `deferStartupFrameClose()` /
  `closeStartupFrame()` 三个私有方法就是这条路径）：`finishStartup()` 只在“窗口已经能露”时才立刻关框，
  判据是 `windowCanBeSeen()` —— 主窗口没有主渲染视图（`MainWindow::primaryRenderControl()` 为 `nullptr`），
  或该视图 `hasPresented()`（`Presenting`/`Failed`）。还不能露时走推迟：订阅 `RenderControl::stateChanged`，
  由视图自己的上报（`Presenting`/`Failed`）触发关框；另加一个 `kSurfaceWaitMs = 2000 ms` 的一次性 `QTimer`
  兜底 —— 一个什么都不报的视图不能把框永远举着，到点 warning 后照旧关框（永远举着框比露一块空区更糟）。
  **没有嵌套事件循环**：跑循环的是 `run()`，等待方不在里面插一脚（启动期重入其它组件的事件是另一类坑，
  见上面“重绘用 `repaint()`”一条）。框全程不动，只是结束在“有东西可露出来”的那一刻。
  三条日志：武装时 `the startup frame stays up until the render view shows a frame (or 2000 ms pass)`，
  关框时 `Startup frame going away: main window visible=…, active=…`，到点仍未上屏则
  `the render view has not shown a frame after 2000 ms: closing the startup frame anyway`。
- **“init 时把管线建好”不能替代这次等待**（2026-09-19 复核）：`RenderControl::init()` 确实在插件的 `load()` 里
  就 attach，并让 `RenderEngine::initialize()` 预热（主内容在 `backend_->initialize()` 里编译，随后每个
  enabled、不清屏的 pass 跑一遍，见 `RenderEngine.cpp:118` 的注释），但：
  1) 预热建在**退化尺寸**上（首帧冲出来的 `build profile (pre-frame)` 实测 extent 只有 `100x30`）；
  2) `Attached` 的语义只是“后端绑上了”，**一帧都没提交**，而首帧只能由事件循环提交（最终尺寸与曝光都来自
     窗口系统）⇒ target / pass graph / program slot 仍是在首帧里建的，真实尺寸更是首帧之后才到（见下面实测）。
- **契约没变的部分**：**插件从 `load()` 返回就表示它的子系统已经可用**；`finishStartup()` 仍是
  “宿主自己的活 + 所有插件的活都干完了”。变的只是“框什么时候消失”：它等的是**窗口**（框架拥有它），
  不是插件的子系统（那仍是插件自己的责任）；`Pending`（宿主还没让它 attach）不等待。
- 不放在插件里的原因：`load()` 跑在 `loadAll()` 中段，在那儿等会拖住**其它插件**的加载与进度上报。
- **实测（本机 Windows + RTX 4060，2026-09-19）**：`Pending -> Attached after 2281 ms` → 首帧的图在 **752x480**
  上只建一次 → `Attached -> Presenting after 3282 ms` → 框比 `Presenting` 晚 2 ms 关（从武装到关 759 ms）。
  修前同一段：框先关，756–903 ms 后首帧才上屏。
- **实测（本机 xcb + lavapipe，2026-09-19）**：`Pending -> Attached after 1783 ms`（+`surface shown`）→ 21 ms
  后 `finishStartup()` 武装推迟（那一刻仍是 `Attached`）→ 首帧在 `run()` 的循环里建
  `shadow_map 1024x1024`、`gbuffer`/`composite` `100x30` 与 3 个 program slot →
  `Attached -> Presenting after 2005 ms`，框同一毫秒关。首帧自身 `build profile (frame)`：`rebind compiles 1`、
  `program slots 2 (view compiles 9.5 ms)`；真实尺寸 27.148 ms 才到（`composite resized 100x30 -> 378x247`），
  32.966 ms 还在变（→ `1178x479`）。删掉这次等待，露出来的就是这 ~180–220 ms 的空区。

## 平台注意

- 圆角靠 `WA_TranslucentBackground`；没有合成的 X11 会话里圆角会退化成黑角（Windows/WSLg 正常）。
  要绝对稳就把 `paintEvent` 的面板改成满矩形（一处改动）。
- Wayland 上"未映射窗口 attach"本就不成立，`RenderControl` 会走"先显示表面再 attach"的回退
  （见 `appfw-render-surface.md` 的平台契约）；启动框与它的关系只是"框也在最上层"。

## ABI

`V_APPFW_PLUGIN_ABI_VERSION` **不需要 +1**：插件可见面（`PluginAbi`/`PluginInfo`/`Plugin`/
`PluginLoadContext`/入口签名/命令注册）一个都没动；`StartupProgress` 是新增 API，`Application`
新增的 `finishStartup()` 是**追加在虚表末尾**（声明在所有既有虚函数之后）。

## 顺带修复（不修就没法跑 GUI 用例）

`tests/test_gui/test_gui.cpp` 里的 `ConsoleProgressReporterTest.ConsoleUserIOPrintsTheProgressOfAForegroundOperation`
直接构造 **appfw 私有**类 `ConsoleUserIO`（`src/ConsoleUserIO.hpp`），但该类没有导出宏，
`viAppfwd.lib` 里根本没有它的符号 ⇒ `test_gui` 在 HEAD 上**链接失败**（LNK2019 ×2，`6ec0e4d` 引入）。
修法：给 `ConsoleUserIO` 加 `V_APPFW_API`（类仍私有，只多两个导出符号），保留那个端到端用例。

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
