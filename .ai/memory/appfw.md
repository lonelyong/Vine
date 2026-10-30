# appfw 模块要点（`src/fw/appfw`）

> **最后核对**：2026-10-28。
> 现役设计按模块拆在 `.ai/design/` 下的 appfw 文档里；本文件只放跨模块、干活时必须立刻想起的规则。
> 历史、已否决的方案与逐轮审查记录看 git；开放项看 `.ai/backlog/appfw-startup.md`。

## 启动

- 三拍：`startupStart()` → `startup()` → `startupEnd()`，都是 `vn::async::Task<void>`，protected，声明在类末尾。
- `run()` 先 `exec()`，再推一个启动步；`startupSequence()` 把三拍平铺，每拍之后垫一次
  `MainThreadDispatcher::resumeOnMainThread()`（`co_await` 之后跑在唤醒线程上）。
- 上报口由驱动建/收：`beginStartupProgress()` 在第一拍前，`endStartupProgress()` 在最后一拍后；
  `startupEnd()` 那一拍仍能上报。
- 启动异常致命：`failStartup()` = 记日志 + 收上报口 + `exit(1)`，**不走** `startupEnd()`。
  插件 `loadAll()` 返回 false 只是“某个插件没装上”，记告警，应用照常启动。
- 框架不替宿主拥有线程：宿主重写 `startup()`，先 `co_await Application::startup()`（插件先加载），
  重活再 `co_await vn::async::run(...)` 丢到池上；异常从那一拍抛出，同样致命。
- 启动可取消：`StartupProgress::stopToken()` / `requestCancel()`；`cancelStartup()` 卸已装插件 + 收上报口 +
  `exit(0)`，取消不是失败，主窗不上屏。
- 进度按相位更新，不做连续动画/心跳；启动框只有状态行 + 转圈，不显示百分比或进度条。
- 启动期屏幕上只有启动框；主窗在 `startupEnd()` 第一次 `show()`，启动框同时收掉并 `raise()`/`activate()`。
- Windows 的“主窗未 show 时内嵌表面已有有效 HWND”仍未实测，见 `.ai/backlog/appfw-startup.md`。

## 插件

- 生命周期三趟：`preLoad()` 全员 → `load()` 全员 → `postLoad()` 全员；声明依赖只决定 `load` 趟内先后。
- `loadAllAsync()` 是启动门（协程；管理器自己不转循环）；`loadAll()` 是同步门，给不跑循环的工具/测试/自驱宿主。
- 同步门 = `MainThreadDispatcher::runToCompletion()`（pump 只派发 `deliverPostedCalls()`）；
  **不能用 `Task::result()`**，它只阻塞不派发，钩子“回到应用线程”那一步会死锁。
- 钩子返回 `Task<void>`，**每条出口都要 `co_return;`**；缺了它函数不是协程，返回空 `Task`，`co_await` 必 `ud2`。
- `VN_APPFW_PLUGIN_ABI_VERSION` = **8u**。
- `uuid` 是身份：同名不同 uuid（第一个位置胜）、不同名同 uuid（都留下）、注册文件与库不符（库为准）都只 warning。
- `Application::shutdown()` 是 protected：只有 `run()` 的收尾路径调它；宿主/命令/插件停进程用 `exit()`。
  循环还在跑或从非应用线程调 `shutdown()`，只警告不拒绝。

## 文档模型

- 框架只定义**文档身份与生命周期**：`Document` + `DocumentManager`；进程一份挂在 `Application::documentManager()`，
  建在 `initialize()` 里，早于插件加载，所以插件能在 `load()` 里注册类型。
- 打开 = `open(payload)`：按载荷的 C++ 类型注册打开器；框架不解释载荷、不拷贝、不持有载荷对象。
  刷新是类型的 `reload()` 动作位，不是“重放 `open(payload)`”。
- 位置是 **`Vfs` + 虚拟路径**，不是裸 `path`；core 不认识 `Vfs`（appfw 不链 iobase）。
- `DocumentSource` 是值类型、不派生；“没有来源”= 空值；`save()` 无参写回来源，基类默认拒绝。
- 三条契约：`Vfs` 借用且必须比文档活得久；`Vfs` 无锁（同一实例不可两线程同碰）；载荷只在 `open()` 期间有效。
- 视图层只在 `GuiApplication`：`DocumentView` + `DocumentViewRegistry` + `CentralDocumentHost`。
- 视图按**文档实例**；`content()` 是“放进中央区的东西”。2D 视图返回自己，3D 视图也返回自己
  ⇒ 宿主只有“一实例一页”一条规则。
- 中央区是单槽，`setCentralWidget()` 会重父化（Qt 重建原生窗口）⇒ 放一个容器、只装一次、内部用
  `QStackedWidget` 切页。
- 三处显示只有一个来源（2026-10-28）：`DocumentManager::current()` ⇒ tab 选中 / 中央区内容 / `activate()` 的视图，
  全在 `CentralDocumentHost::syncToCurrentDocument()` 一处派生。浮动窗口激活只是 `setCurrent()` 请求。
- 当前文档浮动时中央区显示说明页（`shownContent()`），`currentView()` 仍是它；tab 右键菜单提供浮动/停靠/关闭。
- 关掉当前文档：管理器先发 `currentChanged(nullptr)` 再发 `closed`；宿主在 `closed` 里请求“接替那一格”成为当前，
  因为 tab 条表达不了“无选中”（Qt 6.10 实测：有 tab 时 `setCurrentIndex(-1)` 是空操作）。只剩一份/零份时条隐藏。
- 3D 文档各自一块 `RenderControl`（自己的面/会话/设备/管线）；应用级共享的只有 DockPanel/Ribbon 外壳。
- `VSG_MAX_DEVICES` = **4**（`gfx_backend_vsg/CMakeLists.txt` 显式设）。第 5 块由后端拒：
  `vsg::Device` 构造抛异常，`deviceFailureMessage()` 转成带“at most 4 simultaneous render devices”的理由，
  `RenderControl::init()` 返回 false 并进 `Failed`。
- 多格/共享会话那套已删：`RenderControl::present()`、`SceneView::removeWindowPass()` 公开、宿主“借页”都不存在。
- 输入进到视图只有一条通道：`SceneView` 的操纵器。拾取在 SDK，选择/高亮/框选是业务，框架没有选中态。

## 渲染控件与查看器

- 构造只组装；attach 由 `init()` / `initAsync()` 或第一拍兜底。`init()` 同步、幂等；表面还没布局好时返回 false，
  宿主再调一次就是重试。
- `initAsync()` 只是 `co_return init()` 的协程拼法，**不挂起、不下池**；设备/会话/管线与 warm-up 帧都在应用线程上
  一次付掉（实测最长一段 ~184 ms）。
- attach 读场景图（管线由 pass 建、warm-up 要 record）⇒ 宿主不要在 attach 进行中从别的线程改场景；
  当前没有 `isAttaching()` 旗子，attach 与应用线程上的内容装配天然串行。
- `Viewer` 是一块渲染区的**请求半边**：`setViewerOptions()` / `setShadingMode()` / `release(view)` / `view()` /
  `isActive()` / `takeArea()`。一块区只画一幅画，多格已删。
- 宿主在区上声明自己的主 pass ⇒ `isActive()` 为 false，`takeArea()` 不动；要接管就先声明或先 `release(view)`。
- 中央区 `setCentralWidget()` 重父化会重建原生窗口 ⇒ `RenderControl` 走一遍 `Pending -> Presenting`。

## 功能区

- tab/group 的身份、角色与顺序见 `.ai/design/appfw-ribbon.md`；标准目录在 app_shell，插件 tab 用 role 锚点。
- `setCommand()` 的条目（按钮、菜单项、Backstage）状态是**派生**的：enabled 跟 `isCommandEnabled()` 加
  `NeedsDocument`/`NeedsView` 对派发目标的要求，tooltip 跟描述，勾选态用 `setCheckedProvider()`；
  实现只有一处 `src/gui/CommandBinding.hpp`。`setEnabled()` 对绑定条目只是暂时效果。
- `CommandManager::commandFlags()` / `commandDescription()` 读注册时缓存，别名照执行入口解析。
- 行高别交给样式表：`min-height` 只约束内容盒，加 1 px 下边线会把行变高（Backstage 条目行 44 px ≠
  导航行 40 px）；尺寸用 `setFixedHeight()`，样式表只管颜色与内边距。
- `border-*-color: palette(...)` 不一定解析成想要的灰度（`palette(midlight)` 在本机是白色，线画了看不见）；
  分隔线用中性半透明灰 `rgba(127,127,127,90)`，明/暗底都成立。
- `palette(midlight)` 在默认浅色调色板里就是 `#ffffff`（与 `Base` 同色），拿它当 hover 底色等于没有反馈；
  hover/按下用 `rgba(127,127,127,40)` / `rgba(127,127,127,70)`。
- `QScrollArea::setWidget()` 会给被滚动的控件打开 `autoFillBackground`（画成 `Window` 色的一块）；要在
  setWidget 之后再 `setAutoFillBackground(false)`，否则透明页面上会出现一块灰底。

## 公共面

- SDK 文本一律 `vn::String`（字面量写 `u8"…"`）；base 的 progress 模块用 `std::string`，在 appfw 边界
  `as_std_str()` 转换一次。
- 阶段推进叫 `setDone(double)`；是否可计数只由 `std::optional<double> fraction()` 回答。
- 跨线程助手在 `MainThreadDispatcher` 的静态面上：`isMainThread` / `hasEventLoop` / `postToMainThread` /
  `invokeOnMainThread` / `resumeOnMainThread`；实例上只剩 `deliverPostedCalls()`。
- `Application::d` 已私有，叶子走 `dptr()`；`argv()` 返回 `char* const*`。

## 已知缺口

- `MeshLoader::load()` / `loadMerged()` 只吃路径与流，没有 `Vfs` 入口 ⇒ 文档模型说的
  “`Vfs` + 虚拟路径”今天读不进模型文件。
- 面板注册表不存在 ⇒ 面板只能插件自己建、自己挂；视图那一半已经落地。
- 同时最多 4 份 3D 文档；要更多得给 vsg 打补丁（本树保持 vsg 原版 + 门禁）。
