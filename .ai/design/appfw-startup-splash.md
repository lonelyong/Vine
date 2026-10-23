# appfw 启动框与启动进度（`BootSplash` / `StartupProgress`）设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：启动框、启动进度与呈现；三拍、线程契约、失败/取消路径见 `appfw-startup-phases.md`。
> **权威来源**：`StartupProgress.hpp`、`gui/BootSplash.hpp`、`AppConfig.hpp`、`tests/test_gui/BootSplashTest`。
> **待办**：`../backlog/appfw-startup.md`

## 1. 概要设计

### 1.1 问题与背景

启动期没有进度可看：`GuiApplication` 构造函数建主窗（Ribbon、停靠面板），插件加载在应用线程上跑，用户面对的是
一个“还没长齐的窗口”，而且加载快慢完全不可见。启动框要给应用身份和当前阶段，但不能假装知道总量。

### 1.2 目标

- 应用只配外观（`AppConfig::splash`），不写呈现代码。
- 启动期只显示启动框；主窗在 `startupEnd()` 第一次上屏，用户看不到“先冒出一个窗口、再变样”。
- 进度按相位更新，不显示百分比/进度条；状态行 + 18 px 转圈。
- 上报口是应用级状态，状态栏/控制台/启动框共享同一份。

### 1.3 非目标

- 不做整体 ETA、进度条、百分比。
- 不在相位内做动画/心跳；不切开会话 `init()`，不让启动期回转循环。
- 不替宿主拥有启动线程；宿主的重活由它自己在 `startup()` 那一拍丢到池上。

### 1.4 一句话结论

启动框是框架级呈现：`Application::startupSequence()` 建/收上报口，`GuiApplication::startupStart()` 上框并等首帧，
`startupEnd()` 收框并第一次显示主窗；进度按相位推，框只显示状态行 + 转圈。

## 2. 设计要点

### 2.1 组成

| 部件 | 位置 | 职责 |
| --- | --- | --- |
| `SplashConfig` | `AppConfig::splash`（`AppConfig.hpp`） | 开关 + 标题/副标题/logo |
| `StartupProgress` | `sdk/vine/appfw/StartupProgress.hpp` | 启动期上报口：阶段、状态文字、比例；进程内至多一个 |
| `gui::BootSplash` | `sdk/vine/appfw/gui/BootSplash.hpp` | 无边框自绘圆角框 + 状态行 + 转圈指示器 |
| `Application` | `startupSequence()` 驱动三拍；建/收上报口 | 生命周期 |
| `GuiApplication` | 构造函数建框；`startupStart()` / `startupEnd()` 覆写上屏/收框 | 呈现 |
| `PluginManager::loadAllAsync()` | 上报“正在查找/加载/收尾插件 x (i/n)” | 启动里最长、最不可预测的一段 |

**为什么上报口在 appfw 而不是 base**：它要往 `ProgressHost` 里报（Core 的 `Signal`），而 `ProgressHost` 是应用状态。
上报口自己持一个前台 `ProgressHost`，所以启动进度天然被状态栏/控制台共享。

### 2.2 两种阶段

- **可计数阶段** `stage(name, total)`：`setDone()` 按 `done/total` 推进（插件加载就是这样，每个插件回来算一份）。
- **不确定阶段** `stage(name)`：只知道在做什么。
- **每个可计数阶段从零开始**（等于该阶段自己的比例），不是整次启动的剩余。上报方不可能知道还没走到的阶段有多长；
  硬凑全局百分比要么在阶段切换处跳回 0，要么让应用预先声明权重表。
- 启动框不把可计数阶段变成数字；状态栏呈现者与控制台照旧显示它们。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 启动框呈现 | 应用身份 + 状态行 + 转圈；无进度条/百分比 | 已落地 |
| F2 | 相位进度 | 每个相位开始报一次，收到上报就同步重绘 | 已落地 |
| F3 | 首帧门 | 启动框画出首帧后才跑启动工作；300 ms 兜底 | 已落地 |
| F4 | 主窗上屏 | `startupEnd()` 收框、主窗第一次 `show()`、`raise()`/`activate()` | 已落地 |
| F5 | 取消 | 启动框 `✕`/`startupStart` 的 token；取消不是失败 | 已落地 |
| F6 | 上报口生命周期 | 第一拍前建、最后一拍后收；没有上报口时全空操作 | 已落地 |

### 3.1.1 F1 启动框呈现

- **行为**：`SplashConfig` 配开关/标题/副标题/logo；`BootSplash` 自绘圆角面板，状态行 + 18 px 转圈。
- **约束**（C1）：不显示百分比/进度条；空标题回落到应用名；读不到的 logo 按“没有 logo”处理。
- **要求**（R1）：深浅主题自动跟随；居中于主屏；不改 appfw 现有控件样式。
- **验收**（T1）：`BootSplashTest` 的 `statusText()` 断言。
- **注意**（N1）：状态文字按框宽 `elidedText`，长插件名不能把窗口撑宽。

### 3.1.2 F2 相位进度

- **行为**：每个相位开始时报一次（“正在启动” → “正在显示启动画面” → “正在查找/加载/收尾插件” → “正在准备主窗口”）；
  框每收到一次上报就 `repaint()`。
- **约束**（C2）：相位内部不动画；不做心跳/按时间插值。
- **要求**（R2）：相位边界一定看得见；状态栏/控制台与框读同一份 `StartupProgress`。
- **验收**（T2）：`BootSplashTest` 的 `statusText()` 跟着 `stage()/setLabel()` 变。
- **注意**（N2）：给相位内加动画只能切开会话 `init()` 或让启动期回转循环，两条都比“相位边界可见”贵。

### 3.1.3 F3 首帧门

- **行为**：`startupStart()` 上屏启动框，`co_await` 首帧（`Window::hasPainted()` + 300 ms 兜底）才继续；
  X11 上只 `repaint()`/`show()` 不派发就是空窗口。
- **约束**（C3）：启动工作（含插件 `load()`）必须在启动框画出首帧之后；主窗还没 show。
- **要求**（R3）：没有启动框时立即继续；首帧门不能变成固定 sleep。
- **验收**（T3）：`test_appfw` 的启动工作用例；`startup work starting N ms ... the startup frame is on screen`。
- **注意**（N3）：启动期不要替渲染组件 `processEvents()`；只让启动框自己重绘。

### 3.1.4 F4 主窗上屏

- **行为**：`startupEnd()` 收启动框、主窗第一次 `show()`；有框时先 `raise()`/`activate()`。
- **约束**（C4）：主窗在 `startupEnd()` 之前不 show；启动期屏幕上只有启动框。
- **要求**（R4）：主窗第一次出现时已经是成品；渲染区在 `Presenting` 之前容器隐藏，露出主窗背景。
- **验收**（T4）：X11 `scripts/xwinmap.py` 读启动期主窗 `IsUnmapped`、结束后 `IsViewable`。
- **注意**（N4）：`Qt::SplashScreen` 置顶且不激活进程；Windows 只把前台激活给新进程，必须 `raise()`/`activate()`。

### 3.1.5 F5 取消

- **行为**：启动框右上角 `✕` 往 `StartupProgress::stopToken()` 上写；框架每拍之后、每个插件之前看一眼。
- **约束**（C5）：取消不是失败；`cancelStartup()` 卸已装插件 + 收上报口 + `exit(0)`，主窗不上屏。
- **要求**（R5）：插件从 `PluginLoadContext::stopToken()` 拿同一个 token；不在启动里的插件拿到永不停的 token。
- **验收**（T5）：`test_appfw` 的取消用例。
- **注意**（N5）：取消不走 `startupEnd()` 也不走 `failStartup()`。

### 3.1.6 F6 上报口生命周期

- **行为**：`beginStartupProgress()` 在第一拍前，`endStartupProgress()` 在最后一拍后；`startupEnd()` 那一拍仍能上报。
- **约束**（C6）：不在构造函数里建；从不跑启动的进程会多一个活着的宿主，`isBusy()` 永远为真，顶层命令全被拒。
- **要求**（R6）：没有上报口时所有调用都是空操作；销毁是刻意的（不是只 `complete()`）。
- **验收**（T6）：`test_appfw` 无头启动进度用例。
- **注意**（N6）：活着的宿主会一直待在前台栈上，挡掉之后所有命令的进度。

## 4. 公开接口

### 4.1 `StartupProgress`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `stage(name)` / `stage(name, total)` | 开始一个不确定/可计数阶段 | 每个可计数阶段从零开始 |
| `setDone(double)` | 推进当前阶段 | 值是绝对值，不是增量 |
| `setLabel(String)` | 改状态文字 | 逐插件上报用 |
| `fraction()` | `optional<double>` | 是否可计数只看它 |
| `stopToken()` / `requestCancel()` | 启动取消 | 取消不是失败 |

### 4.2 `gui::BootSplash`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `isBusyIndicatorRunning()` | 转圈是否在动 | 没有百分比/进度条 |
| `statusText()` | 当前画出来的状态行 | 测试断言用 |

### 4.3 `SplashConfig`

`enabled` / `title` / `subtitle` / `logo`；应用只配外观，不写呈现代码。

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 启动框不显示百分比/进度条；状态行 + 转圈。 |
| C2 | 相位内部不动画；不做心跳/按时间插值。 |
| C3 | 启动工作在启动框首帧之后；主窗在 `startupEnd()` 之前不 show。 |
| C4 | 主窗在 `startupEnd()` 第一次 show；启动期屏幕上只有启动框。 |
| C5 | 取消不是失败；不走 `startupEnd()`/`failStartup()`。 |
| C6 | 上报口不在构造函数里建；由 `startupSequence()` 建/收。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 深浅主题自动跟随；logo 读不到不影响启动。 |
| R2 | 相位边界可见；状态栏/控制台/框共享同一份进度。 |
| R3 | 首帧门有 300 ms 兜底；没有启动框立即继续。 |
| R4 | 主窗第一次出现时是成品，不露洞。 |
| R5 | 插件拿同一个 stop token；不在启动里的插件拿永不停的 token。 |
| R6 | 没有上报口时所有调用空操作。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 启动期屏幕上只有启动框。 |
| N2 | 主窗第一次 `show()` 发生在 `startupEnd()`。 |
| N3 | 上报口生命周期 = 启动阶段生命周期。 |
| N4 | 启动框的可计数阶段不变成数字。 |

## 6. 注意事项与陷阱

### 6.1 相位内动画的代价

- 会怎么错：以为加个定时器就能让转圈更顺；实际要切开会话 `init()` 或让启动期回转循环。
- 怎么判：应用线程最长连续占用实测 ~184 ms（会话 init）。
- 怎么防：只做相位边界更新；相位内保持静止。

### 6.2 启动期 pump 事件

- 会怎么错：`processEvents()` 把渲染表面的 resize/settle 定时器提前唤醒，拿不到有效句柄。
- 怎么判：attach 在布局没稳时发生；启动框全透明/空白。
- 怎么防：启动框只 `repaint()`；首帧门用 `Window::hasPainted()`。

### 6.3 Windows 未验

- 会怎么错：主窗未 show 时 `VsgHostWindow` 拿不到有效 HWND，attach 失败。
- 怎么判：日志 `GetClientRect(..) failed` / `surface -> Failed`。
- 怎么防：回退“主窗早 show、门等两窗”，并记“Windows 上未采纳”；判据见 backlog。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 进度条 / 百分比 / 整体 ETA | 启动没有已知总量，条只能骗人。 | — |
| D2 | 相位内动画 / 心跳 / 按时间插值 | 要切开会话 `init()` 或回转循环，代价高于收益。 | — |
| D3 | 主窗与启动框一起早 show | 用户会看到“先冒出一个窗口、再变样”。 | — |
| D4 | 构造函数里建上报口 | 从不跑启动的进程会永远 `isBusy()`，顶层命令全被拒。 | — |
| D5 | 宿主自己排启动顺序（先结束启动再跑循环） | 顺序漏写就回到老故障；启动步归 `run()` 所有。 | — |
| D6 | 启动期 `processEvents()` pump | 会把别的组件的定时器/事件一起跑。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 默认关闭；关闭时 `startupEnd()` 幂等 | `BootSplashTest.DisabledByDefaultInTheTestApplication` |
| T2 | 框画出来的字跟着 `stage()` / `setLabel()` 变 | `BootSplashTest` 的 `statusText()` |
| T3 | 启动步在循环里、宿主 work 在别的线程、主线程仍在派发 | `test_appfw` 启动工作用例 |
| T4 | 主窗未 show 时 attach 成功、`startupEnd()` 后 `Presenting` | `scripts/xwinmap.py` + 门禁 app 阶段 |

## 9. 状态与演进

### 9.1 当前状态

F1–F6 已落地；X11/WSLg 已验证“启动期只有启动框”，Windows 未验。

### 9.2 已知缺口 / 待办

- Windows 的 HWND + client rect 探针；见 `../backlog/appfw-startup.md`。
- 相位内动画/进度条不做；如要做，先解决会话 `init()` 的线程与循环回转。

### 9.3 兼容性 / 迁移

旧名 `beginStartup` / `finishStartup` / `runStartup` 与已删除的 `run(work)` 重载只出现在 git 历史里；
当前只按 `startupStart` / `startup` / `startupEnd` 三拍写。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/StartupProgress.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/BootSplash.hpp`
- `src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`
- `tests/test_gui/BootSplashTest.cpp`

### 10.2 文档与 ADR

- `appfw-startup-phases.md`
- `appfw-render-surface.md`
- `appfw-progress.md`
- `../backlog/appfw-startup.md`

### 10.3 外部资料

- Qt `Qt::SplashScreen` / `WA_TranslucentBackground` 语义。
- X11 `XQueryTree` / `XGetWindowAttributes`（`scripts/xwinmap.py`）。
