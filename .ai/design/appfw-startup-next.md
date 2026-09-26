# 启动流程下一步：`init()` 去留 / 启动事件 / 主窗后显示 / 渲染先就绪（待办，2026-09-26）

> 用户 2026-09-26 定的方向（原话）："`Application` 与 `GuiApplication` 要统一初始化，`init()` 是否需要；
> 先跑 `exec()`，某个方法 push 一个 event 用于调用 `beginStartup`，或者 startup 内调用虚的 `beginStartup`/`finishStartup` 等；
> 主窗口在初始化完成后再 `show()`，渲染的话也要在窗口显示前把资源初始化好。"
>
> 本轮（`0ad0a10` + `e39373d`）落地的是"**创建与上屏分家**（`init()` 只建不 show）+ 框架排顺序 + 宿主只交 work"，
> 见 `appfw-startup-splash.md` 的"统一启动流程"一节。下面四条是在它之上再往前一步。

## 目标形状（①②③④一起满足时的样子）

```
main: app->runStartup(work)              宿主只交"启动期要做的事"，其余全归框架
run():  exec()                           先把循环跑起来（此前屏幕上什么都没有）
        └─(启动事件) beginStartup()      虚：GUI = 启动框 show（启动期唯一在屏的窗口）
                     work()              宿主 stage + loadAll()；渲染表面要在"主窗未 show"下 attach
                     finishStartup()     虚：GUI = 关框 + 主窗**第一次** show（此后才有主窗）
```

- **启动期屏幕上只有启动框**；主窗在 `finishStartup()` 里第一次上屏 ⇒ 用户永远看不到"先冒出一个窗口、再变样"。
- **没有启动框时**：启动期屏幕上什么都没有（主窗也还没 show），主窗在 `finishStartup()` 才出现。
- **首帧门只剩一个作用**：启动框画过第一帧之前不许跑 work（X11 上只 `repaint()` 不派发就是空白）。
  今天那道门是"两个窗口都画过"（启动框 + 主窗）；主窗晚 show 之后它退化成"启动框画过" ⇒
  `bootWindows()` / `allPainted()` 的两窗逻辑可以删掉。
- 现有两个扩展点 `showUserInterface()` / `whenUserInterfaceIsUp()` **被"一个启动事件 + `beginStartup()`/`finishStartup()`"取代**，
  `run()` 变成"起循环 → 推启动 → 循环"。

顺序建议：**待办 1 → 2 → 4 → 3**（4 卡着 3）。

## 待办 1：`init()` 是否还需要 → 折进构造函数

今天的 `init()` 干两件事：建 Qt 应用对象（`QCoreApplication` / `QApplication`）+ `setupUserIO()`；
GUI 版还建闪屏与主窗、挂状态栏进度条、定主题。

它单独存在只是为了**让 builder 先塞设置**：`setSplashConfig()`（闪屏标题要 `QCoreApplication::applicationName()`，
而那个名字是 `applyAppConfig()` 在 init **之后**才设的 ⇒ `GuiAppBuilder.cpp` 里多了一句"空标题回落到 `config.name`"的补丁），
以及 `applyAppConfig()` 本身（名字 / 组织 / 配置文件）。

- **建议**：把 `init()` 折进构造 —— `Application(const AppConfig&, int argc, char** argv)`（GUI 同签名），
  构造里按"设身份 → 建 Qt 应用 → 建 UI"做完，两个 builder 变成一行，`setSplashConfig()` 与那条标题补丁一起删
  （`AppConfig::splash` 本来就是它的来源）。
- **坑 1（ABI）**：`init()` 是**公开虚函数**且位于 vtable 中间 ⇒ 直接删会让后面的槽位（`run()`/`finishStartup()`…）整体前移，
  老插件虚调就会打错。要么**保留一个 deprecated 的空实现槽位**，要么 `VN_APPFW_PLUGIN_ABI_VERSION` **+1**
  （这次是真 ABI 变更，不再是"追加在末尾"那种）。
- **坑 2**：保留一个"无配置"的构造（`Application(argc, argv)`，测试/工具在用），让它转调 `AppConfig` 版本、缺省即可。
- **调用面很小**：仓内只有 `AppBuilder.cpp` / `GuiAppBuilder.cpp` 调 `init()`（测试都经 builder）。

## 待办 2：启动阶段由"启动事件 + 虚函数"推进

- 触发机制三选一，都不引新依赖：`QTimer::singleShot(0, d->app, …)`（今天 GUI 在用，最省事）/
  `QMetaObject::invokeMethod(d->app, …, Qt::QueuedConnection)` / 自定义 `QEvent`（要一个私有 QObject 来接，
  换来"命名事件 + 可用 `removePostedEvents` 撤销"）。
- ⚠️ **关键坑**：post 出去的用户事件与 Qt 自己 post 的绘制请求（`QEvent::UpdateRequest`）在**同一趟里按 FIFO 派发**
  ⇒ 事件早于绘制 ⇒ "先推启动"**不能**替代"等首帧"。那道门必须在**推事件之前**满足（GUI 侧：从首帧信号 / 上限里推启动事件）。
- 虚函数：`finishStartup()` 已经是虚的 ✓。新增 `virtual void beginStartup()`（默认空；GUI = 显示启动框）。
  **不要为了对称硬加** `startup()`：宿主的工作继续走 `runStartup(work)` 回调更贴合现状（`main.cpp` 不是 `Application` 子类）。
  将来若真要 OO 化（`class VineApp : public GuiApplication { void onStartup() override; }`），让 `beginStartup()` 的默认实现
  变成"调用登记的回调"即可，不必现在做。

## 待办 3：主窗在初始化完成后再 `show()`

就是把"上屏"拆成两半：启动框归 `beginStartup()`，**主窗归 `finishStartup()`（第一次 show，= 启动结束才让它出现）**。

- 今天**做不到**，因为插件 `loadAll()` 里的 `new RenderControl()` + `init()` 要靠主窗的**原生句柄**；
  主窗没 show 过就谈不上句柄 ⇒ 历史实测（见 `appfw-startup-splash.md`）：
  `[VsgRenderer] initialize FAILED … GetClientRect(..) failed : 无效的窗口句柄` + `[RenderControl] surface Pending -> Failed`。
  **所以 3 依赖 4。**
- 退路：若 4 拿不下来，就保持今天的形状（主窗早 show、门等两个窗口），把 3 记进"已评估但未采纳"并写清原因——
  不为对称硬上。

## 待办 4（前置，卡着 3）：渲染资源先就绪，窗口后显示

现状（`.ai/design/appfw-render-surface.md`）：

- **attach 只需要"句柄 + 尺寸"**（不要求表面可见）；**present** 才要求窗口在屏上。
- **容器控件的可见性就是"露出开关"**：`RenderControl` 把窗口容器藏着，直到 `state_changed` 报到 `Presenting` 才 `setVisible(true)`
  ⇒ **主窗早 show 也不会露出洞**（那一格是主窗自己的背景）。所以"晚 show"的收益是**"启动期屏幕上只有启动框"**（用户要的正是这个），
  而技术上卡住的是"**未 show 的顶窗没有可用的原生句柄**"。

要查清的一件事（先做最小探针，别再靠猜）：**"已创建但未映射"的原生窗口能不能拿来 attach？**

- Qt 侧：`QWidget::create(…, WA_NativeWindow)` / `winId()` 能在不 show 的情况下强制建平台窗口，
  而 `windowHandle()` 可能仍为 null——历史那次失败很可能就是拿到了 null，而不是"Qt 拒绝建窗"。
  探针要打三样：`winId()` / `windowHandle()` / 平台窗口映射状态，在 `show()` 前后各一次，**Windows 与 WSLg/X11 各跑一遍**。
- VSG 侧：`VsgHostWindow`（`plugins/gfx_backend_vsg/src/VsgHostWindow.cpp`）用 HWND / X 句柄建 `VkSurfaceKHR` + swapchain。
  Vulkan 对"未映射窗口"建 surface 是合法的（只要句柄有效）；要验证的是 `initialize()` 是否只依赖 HWND + client rect，
  以及**首次 present 在未映射窗口上的返回值**（大概率失败 ⇒ 那正好是"设备/管线/程序槽先建，swapchain 等映射"，
  接上既有的 `Pending → Attached → Presenting` 状态机与 prewarm 帧）。
- ⚠️ Release 树 `VSG_MAX_DEVICES=1` 是**有意的绊线**（仓库里任何地方都不设它）⇒ 两棵树都要跑。

交付物：

1. 一个用例钉住"主窗未 show 时 `RenderControl::init()` 能把设备/管线建起来（返回 true，或明确可重试的 false），
   首帧在 show 之后到"；
2. `SurfaceWindow` 的 attach 前置条件从"宿主控件可见"改成"句柄 + 尺寸 > 0"（若其实已经如此，就补用例 + 把文档口径统一）；
3. 真机（Windows 桌面）视觉确认：启动期**屏幕上只有启动框**，结束后主窗带画面出现。

## 不能忘的实测/硬约束

1. 窗口 show 之后必须**派发到首帧**才算"上屏"（X11 只 `repaint()` 是空转）⇒ 启动期唯一在屏的窗口（启动框）仍要过首帧门。
2. "上屏早于启动工作"不是顺序偏好：**渲染表面 attach 需要窗口的句柄**，而今天只有 show 过才有（= 待办 4 要拆掉的前提）。
3. 用户事件与绘制请求同趟 FIFO ⇒ "先推启动" ≠ "等首帧"。
4. `init()` 在 vtable 中间 ⇒ 删它要么保留槽位，要么 ABI +1。
5. 测试应用（从不跑循环）的窗口靠幂等的 `finishStartup()` 上屏；`finishStartup()` 语义改成"关框 + 主窗第一次 show"后，
   这条路径要一起复核（`test_gui` 的 `BootSplashTest.DisabledByDefaultInTheTestApplication`）。
6. 复现探针 `/tmp/xstack.py`（堆叠序 + 每窗 painted 比例）、`/tmp/xfirst.py` 会被 /tmp 清理，
   需要时按 `appfw-startup-splash.md` 的描述重建。

## 验收（这四条做完时的判据）

- **行为**：启动期独立 X 读回**只有启动框**（主窗不在 `XQueryTree` 里，或 `isVisible()==false`）；
  日志 `startup work starting … : the startup frame is on screen`；启动结束后主窗出现且 `painted ≈ 83.5%`。
- **"先黑后画"彻底消失**：主窗第一次出现在屏幕上时就已经有内容（渲染区至少是容器隐藏后的窗口底色，不是洞）。
- `test_gui`（现 217）/ `test_vsg`（现 451）全绿；两棵树 `vsg_rewrite_gate.sh` → `cases=451 failed=0 vuid=0 hazard=0`
  + 应用阶段像素与基线一致；`check_doc_symbols.py` / `check_diagnostic_formats.py` / `check_include_hygiene.py` 全绿；
  `cmake --install build --prefix dist` 后跑一遍真机。
- **反证**：把新加的门/事件拿掉，要能重现"启动期主窗可见但 `painted=0.0%`"这类故障
  （证明新机制承重，而不是巧合；本轮就靠这条证明了统一后的门仍然承重）。
