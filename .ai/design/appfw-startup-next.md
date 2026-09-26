# 启动流程下一步：启动事件 / 主窗后显示 / 渲染先就绪（待办，2026-09-26）

> **命名演变（同日稍后）**：本文里的 `beginStartup` / `finishStartup` / `runStartup(work)` 即后来的
> `startupStart(next)` / `startupEnd()` / `run(work)`；待办 1–4 已落地，三者实际形状见 `appfw-startup-phases.md`
> （`run(work)` 后来删了：宿主的重活由它自己重写那一拍、自己丢到线程池上）。

> 用户 2026-09-26 定的方向（原话）："`Application` 与 `GuiApplication` 要统一初始化，`init()` 是否需要；
> 先跑 `exec()`，某个方法 push 一个 event 用于调用 `beginStartup`，或者 startup 内调用虚的 `beginStartup`/`finishStartup` 等；
> 主窗口在初始化完成后再 `show()`，渲染的话也要在窗口显示前把资源初始化好。"
>
> 上一轮（`0ad0a10` + `e39373d`）落地的是"**创建与上屏分家**（`init()` 只建不 show）+ 框架排顺序 + 宿主只交 work"，
> 见 `appfw-startup-splash.md` 的"统一启动流程"一节。
> 本轮落地了待办 1（`init()` 折进构造，见下），剩下三条是在它之上再往前一步。

## 目标形状（①②③④一起满足时的样子）

```
main: app->run()                         宿主只声明配置，其余全归框架（日志由 main 自己 initDefault）
run():  exec()                           先把循环跑起来（此前屏幕上什么都没有）
        └─(启动步，posted) 框架：建上报口 + stage("正在启动") + beginStartup()
                     (首帧门)           GUI 等启动窗口画出首帧，然后才允许跑工作
                     loadAll()          插件由框架加载（AppConfig::load_plugins 默认开）
                     work()              宿主自己的阶段（有的话；跑在插件之后）
                     finishStartup()     关框 + 结束上报口（todo 3 后：主窗在这里第一次 show）
```

- **启动期屏幕上只有启动框**（todo 3 后）；主窗在 `finishStartup()` 里第一次上屏 ⇒ 用户永远看不到"先冒出一个窗口、再变样"。
- **没有启动框时**：启动期屏幕上什么都没有（主窗也还没 show），主窗在 `finishStartup()` 才出现。
- **首帧门只剩一个作用**：启动框画过第一帧之前不许跑 work（X11 上只 `repaint()` 不派发就是空白）。
  今天那道门还是"两个窗口都画过"（启动框 + 主窗）；主窗晚 show 之后它退化成"启动框画过" ⇒
  `bootWindows()` / `allPainted()` 的两窗逻辑可以删掉。

顺序建议：**待办 1（已完成）→ 2（启动事件已落地，虚函数部分待做）→ 4 → 3**（4 卡着 3）。

## 待办 1：`init()` 是否还需要 → 折进构造函数 —— **已落地（2026-09-26）**

今天已无 `init()`：`Application` / `GuiApplication` 的**构造函数做完全部初始化**，
`setSplashConfig()` 删除，两个 builder 各剩一句 `make_unique<...>(config, argc, argv)`。

形状：

- `Application(const AppConfig&, int, char**)`：设身份（`config.name` / `config.organization`，缺省组织回落）→ 建 managers
  → 建 Qt 应用对象（无头 `QCoreApplication`）→ `initialize(config)`（建 UserIO + 装配置文件）。
  `Application(int, char**)` 转调它，缺省 `AppConfig`（无头工具的入口）。
- `GuiApplication(const AppConfig&, int, char**)`：先走 base（身份 + managers），然后自己建 `QApplication`
  （WSLg 的 `selectX11UnderWslg()` 在这里跑）→ `initialize(config)` → `createWindows(config.splash)`
  （主题、启动框、主窗口；**仍然不 show**）。`GuiApplication(int, char**)` 同样转调。
- **为什么分成两半**：进程只能有一个 Qt 应用对象，而 base 构造时还不知道叶子要 `QCoreApplication` 还是
  `QApplication`（虚调用在 base 构造里不会派发到叶子）⇒ 叶子建好、存进数据，再调 `initialize()` 收尾。
- **Qt 类型不进 SDK**：`initialize(const AppConfig&)` 不带 Qt 参数，Qt 对象由叶子写进私有 `ApplicationData::app`
  （`dptr()->app = new QCoreApplication/QApplication(dptr()->argc, dptr()->argv)`）。
  中间版本曾把 `QCoreApplication*` 放进受保护的 SDK 签名（并在 `Application.hpp` 里前置声明 Qt 类型）——**已撤回**：
  core SDK（`Application.hpp`/`AppConfig.hpp`/`AppBuilder.hpp`）里不出现任何 Qt 类型，只有文档提到 Qt。
- **ABI（坑 1 的结论）**：`init()` 是公开虚函数且位于 vtable 中间 ⇒ 删除让后面所有槽位整体前移 ⇒
  `VN_APPFW_PLUGIN_ABI_VERSION` **3u → 4u**（不留 deprecated 空槽）。
  `beginStartup()` 已声明在所有其它虚函数之后，之后的追加不必再动版本。
- **`setSplashConfig()` 与那条标题补丁一起删**：空标题由 `BootSplash` 自己解析成应用名，而身份在**建窗口之前**
  已经应用（构造函数顺序），所以 builder 不需要再补 `splash.title = config.name`。
- **顺带修掉的潜伏缺陷**：`GuiApplicationData` 曾用 `QApplication* app` **遮蔽** `ApplicationData::app`，
  于是 `Application::run()` 里 `d->app->exec()` 走的一直是空指针（没炸只因为 `QCoreApplication::exec()` 是静态成员）。
  两份合成一份（base 的 `QCoreApplication* app`）后，GUI 路径也真的存上了。
- `AppConfig`/`SplashConfig` 搬到 `sdk/vine/appfw/AppConfig.hpp`（`AppBuilder.hpp` 转 include，旧包含路径仍可用）；
  `AppBuilderSupport.hpp` 与 `applyAppConfig()` 删除，逻辑进 `Application::initialize()`。
- 钉子（会红）：`test_gui` 的 `GuiApplicationConstructionTest.TheBuilderReturnsAFinishedApplication`
  与 `BootSplashTest.AnEmptyTitleShowsTheApplicationName`、`test_vsg` 的
  `VsgBackendPluginTest.TheConstructorBuildsTheApplicationFromItsConfig`（`persist_config=false` ⇒ `configFile()` 为空）。
- 本轮验证：Debug + Release 两棵树 `ninja` 全绿；`test_gui` 219（217+2）、`test_vsg` 452（451+1）、`test_asyncqt` 2 全绿；
  真机 `bin/Vine` 启动日志与改动前一致（`startup work starting 10 ms …`、`Pending -> Attached -> Presenting`）。

## 待办 2：启动阶段由“启动事件 + 虚函数”推进 —— **已落地（2026-09-26）**

形状（见 `appfw-startup-splash.md` 的“生命周期”）：`run()` 先 `exec()`，再用 `Qt::QueuedConnection` 推一个
启动步（就是 `run()` 里那两行：建上报口 + `stage("正在启动")`，然后把画面交给 `beginStartup(work)` 的回调），
所以无头与有窗口的启动阶段都在循环里跑（以前无头的启动工作在 `exec()` 之前）。插件加载也进来了：
`AppConfig::load_plugins`（默认开）⇒ 宿主的 `main` 不再写 `loadAll()`，而且它跑在宿主的 work 之前。
钉子：`test_appfw` 里“启动前 post 的一帧回调，工作里快照它已经到过” ⇒ 变异（启动步改回 `exec()` 之前直接调）即红。

**两个扩展点的名字改了，但仍然两个**（原计划是“一个 `beginStartup()`”）：`showUserInterface()` →
`beginStartup()`（它只上屏启动框，而“用户界面”——主窗——归 `finishStartup()`：旧名字会骗人）、
`whenUserInterfaceIsUp()` → `beginStartup()`（它等的是启动框首帧，不是“界面就绪”）。
为什么不能合成一个 `beginStartup()`：**“它真的在屏上了吗”这个问题只有带窗口的那一层答得出**——
base `Application` 在 core SDK 里，命名不了 `gui::Window`（`Application.hpp` 里连 Qt 类型都不许出现），
所以“等首帧”必须是 gui 层实现的虚函数。分工因此是：`beginStartup()` 摆画面（无头：空实现）、
`beginStartup(then)` 报告它到底上没上屏（无头：立刻 `then()`），框架只管顺序与 `finishStartup()`。
（移除/改名这两个 protected 虚函数**不需要**动 `VN_APPFW_PLUGIN_ABI_VERSION`：它们在 vtable **末尾**、
且插件拿不到（protected），插件能碰的 `finishStartup()`/`run()` 槽位没动。）

- 触发机制三选一，都不引新依赖：`QTimer::singleShot(0, d->app, …)`（今天 GUI 在用，最省事）/
  `QMetaObject::invokeMethod(d->app, …, Qt::QueuedConnection)` / 自定义 `QEvent`（要一个私有 QObject 来接，
  换来"命名事件 + 可用 `removePostedEvents` 撤销"）。
- ⚠️ **关键坑**：post 出去的用户事件与 Qt 自己 post 的绘制请求（`QEvent::UpdateRequest`）在**同一趟里按 FIFO 派发**
  ⇒ 事件早于绘制 ⇒ "先推启动"**不能**替代"等首帧"。那道门必须在**推事件之前**满足（GUI 侧：从首帧信号 / 上限里推启动事件）。
- 虚函数：`finishStartup()` 已经是虚的 ✓。新增 `virtual void beginStartup()`（默认空；GUI = 显示启动框）。
  **不要为了对称硬加** `startup()`：宿主的工作继续走 `runStartup(work)` 回调更贴合现状（`main.cpp` 不是 `Application` 子类）。
  将来若真要 OO 化（`class VineApp : public GuiApplication { void onStartup() override; }`），让 `beginStartup()` 的默认实现
  变成"调用登记的回调"即可，不必现在做。
- 位置提醒（2026-09-26 已变）：**启动上报口现在是 `Application::run()` 建的**（`stage("正在启动")`，见
  `appfw-startup-splash.md` 的"上报口属于启动阶段"），`GuiApplication` 构造里那句"建上报口 + 报'正在初始化界面'"
  已经删掉。所以 `beginStartup()` 落地时，"建上报口 + 报框架的第一阶段"这两行就是从 `run()` 搬进 `beginStartup()`
  （GUI 同时在那里显示启动框），不需要再动 `GuiApplication` 的窗口创建。

## 待办 3：主窗在初始化完成后再 `show()` —— **已落地（2026-09-26，X11 已验）**

形状：`beginStartup()` **只上屏启动框**（没框就什么都不上屏）；首帧门只等启动框
（`bootWindows()`/`allPainted()` 的两窗逻辑已删，没框 ⇒ 直接继续）；主窗由 `finishStartup()` 第一次 `show()`
（有框时先 `raise()`/`activate()`）。

- 前提是**待办 4 的 X11 半边成立了**（见下）：“未 show 的内嵌表面窗口有可用句柄”已在 WSLg/X11 实测。
- 硬证据（X 服务端读回，`map_state`）：启动期只有启动框 `IsViewable`、主窗 `800x600` 仍是 **IsUnmapped**；
  `finishStartup()` 后启动框消失、主窗（框 `864x664`）`IsViewable`。日志侧：`surface Pending -> Attached`
  发生在主窗未 show 时，`Attached -> Presenting` 紧随主窗上屏之后。细节与完整日志见 `appfw-startup-splash.md`
  的“启动期只有启动框”一节。
- ⚠️ **Windows 未验**（待办 4 的另一半）：若那边 `VsgHostWindow` 拿不到有效 HWND ⇒ `GetClientRect(..) failed` +
  `surface -> Failed`，就得回退今天的形状（主窗早 show、门等两窗）并记“Windows 上未采纳”。
  即使早期 attach 失败，主窗上屏会触发 layout/resize，控件自己会重试 attach（`handleUpdate()`）。

## 待办 4（前置，卡着 3）：渲染资源先就绪，窗口后显示 —— **X11 半边已完成（2026-09-26）**

现状（`.ai/design/appfw-render-surface.md`）：

- **attach 只需要“句柄 + 尺寸”**（不要求表面可见）；**present** 才要求窗口在屏上。
- **容器控件的可见性就是“露出开关”**：`RenderControl` 把窗口容器藏着，直到 `state_changed` 报到 `Presenting` 才 `setVisible(true)`
  ⇒ “主窗晚 show”也不会露出洞（那一格是主窗自己的背景）。

探针结论（WSLg/X11）：**“已创建但没 show 的顶层窗口”不影响 attach**，因为 `SurfaceWindow::nativeHandle()` 用的是
**容器里那个独立 `QWindow` 自己的 `winId()`**，不是顶层的句柄；顶层没 show 时它照样有有效句柄
（实测 `[VsgHostWindow] attached to the host window 0x60002a (160x160, mapped=true)`）。历史失败来自旧渲染器拿顶层句柄 +
没人建原生窗口两件事叠加，不是当前形状的性质。

仍待做：**Windows 一侧同一探针**（HWND + client rect）。判据：启动期日志里出现 `attached to the host window 0x…`
与 `surface Pending -> Attached`（而不是 `GetClientRect(..) failed` / `-> Failed`）。
- ⚠️ Release 树 `VSG_MAX_DEVICES=1` 是**有意的绊线**（仓库里任何地方都不设它）⇒ 两棵树都要跑。

交付物：

1. 一个用例钉住"主窗未 show 时 `RenderControl::init()` 能把设备/管线建起来（返回 true，或明确可重试的 false），
   首帧在 show 之后到"；
2. `SurfaceWindow` 的 attach 前置条件从"宿主控件可见"改成"句柄 + 尺寸 > 0"（若其实已经如此，就补用例 + 把文档口径统一）；
3. 真机（Windows 桌面）视觉确认：启动期**屏幕上只有启动框**，结束后主窗带画面出现。

## 不能忘的实测/硬约束

1. 窗口 show 之后必须**派发到首帧**才算"上屏"（X11 只 `repaint()` 是空转）⇒ 启动期唯一在屏的窗口（启动框）仍要过首帧门。
2. ~~"上屏早于启动工作"不是顺序偏好：**渲染表面 attach 需要窗口的句柄**，而今天只有 show 过才有~~
   **已推翻（2026-09-26，X11）**：句柄来自容器里那个独立 `QWindow` 自己的 `winId()`，顶层不必 show。
   仍然成立的是：**插件加载与宿主的活要等在“启动框已画出首帧”之后**（X11 上只 show 不派发就是空窗口）。
3. 用户事件与绘制请求同趟 FIFO ⇒ "先推启动" ≠ "等首帧"。
4. ~~`init()` 在 vtable 中间 ⇒ 删它要么保留槽位，要么 ABI +1~~ **已决**：删掉并 ABI **3u → 4u**（待办 1）。
5. 测试应用（从不跑循环）的窗口靠幂等的 `finishStartup()` 上屏；`finishStartup()` 语义改成"关框 + 主窗第一次 show"后，
   这条路径要一起复核（`test_gui` 的 `BootSplashTest.DisabledByDefaultInTheTestApplication`）。
6. 探针：**`scripts/xwinmap.py`（已入库）**——列出根顶层窗口的名字/尺寸/`map_state`，
   “启动期只有启动框在屏”就靠它读（启动期读一次、结束后再读一次）。它替代了早先放在 /tmp 的
   `xstack.py`/`xfirst.py`（会被清理）。⚠️ 手写 `XWindowAttributes` 偏移时 `map_state` 在 **92**，
   不是 100（读错会让所有窗口都长得像 IsUnmapped）。

## 验收（这四条做完时的判据）

- **行为**：启动期独立 X 读回**只有启动框**（主窗不在 `XQueryTree` 里，或 `isVisible()==false`）；
  日志 `startup work starting … : the startup frame is on screen`；启动结束后主窗出现且 `painted ≈ 83.5%`。
- **"先黑后画"彻底消失**：主窗第一次出现在屏幕上时就已经有内容（渲染区至少是容器隐藏后的窗口底色，不是洞）。
- `test_gui`（现 219，待办 1 加了 2 例）/ `test_vsg`（现 452，待办 1 加了 1 例）全绿；
  两棵树 `vsg_rewrite_gate.sh` → `cases=452 failed=0 vuid=0 hazard=0`
  + 应用阶段像素与基线一致；`check_doc_symbols.py` / `check_diagnostic_formats.py` / `check_include_hygiene.py` 全绿；
  `cmake --install build --prefix dist` 后跑一遍真机。
- **反证**：把新加的门/事件拿掉，要能重现"启动期主窗可见但 `painted=0.0%`"这类故障
  （证明新机制承重，而不是巧合；本轮就靠这条证明了统一后的门仍然承重）。
