# Backstage 与 ApplicationButton

> **状态**：现行
> **最后核对**：2026-10-29
> **范围**：应用按钮的点击事件、菜单/Backstage 二选一、Backstage 顶层覆盖窗口与页面导航；不含最近文件、导入导出等具体页面内容。
> **权威来源**：`src/fw/appfw/sdk/vine/appfw/gui/Backstage.hpp`、`RibbonBar.hpp`、`MainWindow.hpp`；用例 `tests/test_gui/test_gui.cpp`。
> **待办**：`../backlog/appfw-ribbon.md`

## 1. 概要设计

### 1.1 问题与背景

Ribbon 左上角的应用按钮（“File”）现在只挂一个扁平 `QMenu`：没有点击事件、没有页面模型，也没有类似 Office Backstage 的整窗入口。
SARibbon 的 `SARibbonApplicationWidget` 只是个跟随父窗口、Esc 隐藏的 `QFrame` 壳——没有页面/导航/内容，且作为**子控件**会被中央 3D 原生窗口盖住（`.ai/bugs/docking-flyout-under-render.md`）。

### 1.2 目标

- 暴露应用按钮点击事件（`RibbonBar::applicationButtonClicked()`），让宿主/插件可响应。
- 提供 Backstage 组件：页面（id/标题/图标）、左侧导航、命令条目、打开/关闭/Esc、跟随主窗口几何。
- 应用按钮在“菜单”与“Backstage”之间二选一，不出现两者同时弹出的状态。
- 复用现有命令：Backstage 条目就是 `RibbonAction`（命令名即身份），不引入新的命令体系。
- 新组件不影响既有 ABI：只加新类、新方法，事件放 `Impl`。

### 1.3 非目标

- 不做最近文件（MRU）、导入导出、账户等页面内容（没有存储/需求，见 §9.2）。
- 不做页面的多段布局（一页只有一块主体：命令列或一个控件，见 F7）。
- 不改 `SARibbonApplicationWidget` 上游代码。

### 1.4 一句话结论

**应用按钮是触发器，Backstage 是独立组件；覆盖层必须是顶层 Tool 窗口，页面/条目沿用 id 与命令词汇。**

## 2. 设计要点

### 2.1 应用按钮只负责“触发”，Backstage 是独立组件（G1）

- 是什么：`RibbonBar` 暴露点击事件，并负责菜单/Backstage 的模式切换；`Backstage` 独立负责覆盖窗口与页面。
- 为什么：按钮输入、窗口层级、页面模型是三个职责，混在一个类里会重演“一个类什么都知道”的问题。
- 边界：`MainWindow` 拥有 Backstage 的生命周期；`RibbonBar` 只借用指针。

### 2.2 覆盖层是顶层 Tool 窗口，不是子控件（G10）

- 是什么：`Backstage` 的 native widget 是 `Qt::Tool | FramelessWindowHint` 的顶层窗口，**父窗口设为主窗口**（保证叠在主窗口与其原生子窗口之上，但不是全局 topmost），几何跟随主窗口客户区。
- 为什么：中央 3D 是 `createWindowContainer` 的原生子窗口，Windows 上永远画在 Qt 子控件之上；子控件覆盖层在文档区会缺失一块。飞窗的修复已经证明“顶层 Tool 窗口”是可行方向。
- 代价：需要自己同步 move/resize/minimize/restore、激活与 Esc；不如子控件省事。

### 2.3 页面是 id 平铺列表，条目复用命令（G1、G8）

- 是什么：`Backstage` 按添加顺序持有一组 `BackstagePage`；页面 id 唯一，空 id 不参与查找；页面条目是 `RibbonAction`。
- 为什么：命令名已经是动作身份，Backstage 不再发明一套 item id/role；页面顺序今天没有排序需求，append 足够（`G8`）。
- 边界：页面 id 只用于查找/去重；页面标题/图标在 add 之前设置。

### 2.4 所有权：窗口拥有 Backstage，栈拥有页面 widget（G10）

- 是什么：`MainWindow::setBackstage()` 接管 Backstage 指针；`Backstage::addPage()` 把页面 widget 交给内部 `QStackedWidget`，并把页面包装对象设为不拥有 impl。
- 为什么：避免“包装对象与 Qt 父对象都删除同一个 widget”；与 Ribbon 的 tab/group 所有权约定一致。
- 边界：页面内的 `RibbonAction` 由调用方持有，页面只创建按钮并 `setDefaultAction()`。

### 2.5 只封装 SARibbon 的应用按钮（G2、G12）

- 是什么：使用 `SARibbonApplicationButton` 与 `SARibbonBar::applicationButtonClicked`；Backstage 的窗口与页面模型自建。
- 为什么：SARibbon 的 Backstage 壳不能满足原生表面叠放要求；强行使用会把上游控件的父窗口语义带进来（它要求 `SARibbonMainWindow*` 父窗口）。
- 边界：`SARibbonApplicationWidget` 不作为依赖；其 resize/Esc 行为由 Backstage 自己实现。

### 2.6 菜单与 Backstage 二选一（G4）

- 是什么：`RibbonBar::setBackstage(bs)` 时把应用按钮的 `QMenu` 摘掉；`setBackstage(nullptr)` 恢复菜单。
- 为什么：按钮同时弹菜单和打开页面是两种相互矛盾的行为；模式必须显式。
- 边界：`clearApplicationMenu()` 只清空菜单项，不删除 `RibbonAction`。

### 2.7 外观：导航轨 + 内容面（G4、G12、G6）

- 是什么：240 px 导航轨（顶部返回按钮 + 页面行）与 `palette(base)` 内容面（大号标题、1 px 分隔线、封顶 760 px 的条目列）；颜色一律用 `palette(...)` 角色，跟随明/暗主题（同 `detail::blendIntoSurface()`）。返回箭头按 `palette(window-text)` 现画：平台 `SP_ArrowBack` 是拟物位图，文本字形在带边框的样式表按钮里不居中。
- 为什么：默认控件平铺读起来像一个空的安全对话框，而 Office Backstage / VS Code 设置页都靠“一列导航 + 一块内容面”和两级字号建立层次；返回箭头属于“退出这一层”而非“这一页”，所以放导航轨顶部；条目列封顶是因为一行拉满 4K 后不再是一条条目。
- 边界（C15）：版式按 8 px 网格——导航行与条目行同为 40 px、2 px 间距、12 px 内边距、20 px 图标位，标题与条目列共用 32 px 左缩进，页眉上下 24 / 16 px，条目列距分隔线 24 px，行间一条 1 px 半透明细线；悬停/按下用中性半透明灰，不依赖某个调色板角色恰好不是白色；图标位属于行布局，无图标的行也用透明图标占位，否则该行文字比相邻行左移。条目列放在封顶 760 px 的滚动区里（细滚动条，纵向），滚动条就落在列表末尾而不是窗口边缘；body 页不套滚动区，滚动归 body 自己。

### 2.8 入口只有一个开关：应用按钮的可见性（G1、G8）

- 是什么：`RibbonBar::setApplicationButtonVisible(bool)` 是“提不提供 Backstage/应用菜单入口”的唯一控制；`Backstage` 本身没有 enabled 状态，`open()` 对任何持有者都有效。
- 为什么：应用按钮是进入菜单与 Backstage 的唯一入口，再给 Backstage 一个“禁用”就是同一件事的第二个表示：宿主得配对维护两处状态，而 `applicationButtonVisible()` 会同时被两种原因置假，读的人分不清是“宿主不要”还是“功能被禁用”（`G1` 一个事实一个所有者）。
- 边界：隐藏入口是呈现层选择，不是权限边界——命令/快捷键仍可调 `open()`，是否拒绝由宿主自己决定；想彻底拿掉 Backstage 用 `MainWindow::setBackstage(nullptr)`（见 D6）。

### 2.9 页面与 tab 同构：id 是身份，role 是语义锚，placement 是意图（G1、G6、G9）

- 是什么：`BackstagePageRole`（Home/New/Open/Info/Print/Export/Plugins/Help/Account/Options/Close/UserRole）+ `BackstagePlacement`（append/before/after，锚点是 id 或 role）+ `BackstagePlaceResult`，与 `RibbonTabRole`/`RibbonPlacement`/`RibbonPlaceResult` 同形同义；`addPage(page)` 仍是 append。
- 为什么：页序此前等于添加顺序（也就是加载顺序），插件无法表达“放在 Info 后面”。与 tab 共用一套词汇，插件学一次用在两处；解析规则也是同一张表——终止区（`BackstagePagePolicy::Terminal`，`Close` 用它）+ role 序 + 单一锚 + 越界夹回。
- 代价与边界：解析器在 `Backstage.cpp` 与 `RibbonBar.cpp` 各有一份（判定同一张表，两边用例同构，分叉即 bug）；不抽模板，是因为一个被两个枚举实例化的模板比两份同形代码更难读（G11）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 应用按钮点击事件 | `RibbonBar::applicationButtonClicked()` | 已落地 |
| F2 | 菜单/Backstage 模式 | `setBackstage()` / `clearApplicationMenu()` / `backstage()` | 已落地 |
| F3 | Backstage 页面 | `addPage/findPage/currentPage/pageAt/numPages` | 已落地 |
| F4 | 命令条目 | `BackstagePage::addItem(RibbonAction*)` | 已落地 |
| F5 | 打开/关闭/几何 | `open/close/isOpen`、Esc、跟随主窗口 | 已落地 |
| F6 | 生命周期 | `MainWindow::setBackstage()` 接管所有权 | 已落地 |
| F7 | 页面主体 | `setBody(Control*)`；主体唯一（命令列或一个控件） | 已落地 |
| F8 | 最近文件 / MRU | 最近文件页 | 未做，见 backlog |
| F9 | 页面显隐 | `setPageVisible/isPageVisible`；导航行隐藏、序号不变 | 已落地 |
| F10 | 条目显隐 | `setItemVisible/isItemVisible`；按钮不参与布局 | 已落地 |
| F12 | 页面放置 | `BackstagePlacement` / `addPage(page, placement)` / `findPage(role)`；终止区与角色序 | 已落地 |
| F13 | 贡献者身份与成组撤销 | `setProvider` / `removePages(provider)`；插件卸载/重载时撤自己的页 | 已落地 |
| F14 | 条目列滚动 | 条目超过一页时页面列可滚动（细滚动条）；body 页不套滚动区 | 已落地 |

### 3.1.1 F1–F6 基础能力（行为细节以头文件为准，约束见 §5.1 的 C1–C6）

| # | 行为 | 验收 |
|---|---|---|
| F1 | 按钮 `clicked` 转发为 `Event<RibbonBar, EventArgs>`（P1：事件与菜单并行，用 Backstage 时先 `setBackstage()` 摘菜单） | T1 `Backstage_ApplicationButtonOpensThePage` |
| F2 | `setBackstage(bs)` 摘掉按钮菜单并订阅事件打开 `bs`；`nullptr` 断开订阅并恢复菜单（P2：`clearApplicationMenu()` 不删动作） | T1、T2 |
| F3 | 页面按添加顺序进入左侧导航与 `QStackedWidget`；非空 id 唯一；第一张可见页成为当前页（P3：空 id 页可存在，但 `findPage("")` 找不到） | T2 `Backstage_PagesAndNavigation` |
| F4 | `addItem(RibbonAction*)` 建一个 `QToolButton` 并 `setDefaultAction()`，点击走命令执行（P4：条目先于页面销毁会让按钮的 action 悬空） | T3 `Backstage_PageItemsAreCommandBound` |
| F5 | `open()` 同步几何、显示、置顶、激活；Esc 或返回按钮 `close()`；主窗口 move/resize/minimize 时跟随/隐藏（P5：多屏/分数缩放的 1 px 偏差属平台取整） | T4 `Backstage_FollowsMainWindowSize`、`Backstage_ClosesOnEscapeOrClose` |
| F6 | `MainWindow::setBackstage()` 接管所有权，替换时先解除 Ribbon 的订阅再删除旧对象（析构不崩溃、指针清空） | T5 `Backstage_IsOwnedByMainWindow` |

### 3.1.2 F9、F10、F12、F13 页面的显隐、放置与贡献者

| # | 行为 | 验收 |
|---|---|---|
| F9 | `setPageVisible` 隐藏/恢复导航行；隐藏当前页时选下一可见页，全隐藏则无当前页且 `open()` 拒绝（R7） | T6 `Backstage_PagesCanBeHidden` |
| F10 | `setItemVisible` 隐藏/恢复条目按钮，序号与动作不变（C9） | T7 `BackstagePage_ItemsCanBeHidden` |
| F12 | `addPage(page, placement)` 按放置意图插入；终止区 + 角色序 + 单锚 + 越界夹回，导航与栈随之镜像（C12） | T9 `Backstage_PageRolesAndPlacement`、`Backstage_PagePlacementEdgeCases` |
| F13 | `setProvider` + `removePages(provider)` 整组撤除；被撤的当前页由后一张接替，撤掉的页可重新加入（C13） | T10 `Backstage_PagesAreRemovedByProvider` |
| F7 | `setBody(Control*)` 把命令列换成一块控件；已有条目时拒装、有 body 时拒收条目，换 body 时旧的交回调用方（C14） | T12 `BackstagePage_BodyReplacesTheCommandColumn` |

## 4. 公开接口

### 4.1 RibbonBar

| 接口 | 契约 | 备注 |
|---|---|---|
| `applicationButtonClicked()` | 应用按钮点击事件 | 完整签名见头文件 |
| `clearApplicationMenu()` | 清空菜单项 | 不删除动作 |
| `setBackstage(Backstage*)` | 菜单/Backstage 二选一 | 借用指针 |
| `backstage()` | 当前 Backstage | 可为 nullptr |
| `setApplicationButtonVisible(bool)` | File 入口显隐（也决定应用菜单入口） | 呈现层选择，不是权限 |
| `applicationButtonVisible()` | 入口当前是否显示 | 报告控件状态 |

### 4.2 Backstage

| 接口 | 契约 | 备注 |
|---|---|---|
| `Backstage(MainWindow*)` | 建顶层覆盖窗口并跟随主窗口 | 不接管页面 |
| `addPage(BackstagePage*)` | 追加页面 | 重复非空 id 返回 false |
| `addPage(BackstagePage*, const BackstagePlacement&)` | 按放置意图加页 | 返回状态 + 逻辑索引 |
| `findPage(const String&)` / `findPage(BackstagePageRole)` | 按 id / 按角色查找 | 逻辑顺序中的第一个 |
| `removePages(const String& provider)` | 整组撤除某贡献者的页 | 返回撤掉的页数；空 provider 不匹配 |
| `findPage/pageAt/numPages` | 页面查找 | 逻辑顺序 |
| `setCurrentPage/currentPage` | 切换页面 | 同步导航与内容栈 |
| `setPageVisible/isPageVisible` | 页面显隐 | 隐藏不删除，序号不变 |
| `open/close/isOpen` | 显隐 | Esc/返回按钮关闭；无可见页时 open 拒绝 |

### 4.3 BackstagePage

| 接口 | 契约 | 备注 |
|---|---|---|
| `setId/id` | 页面身份 | add 前设置 |
| `setTitle/title`、`setIcon/icon` | 导航标题与图标 | add 前设置 |
| `setRole/role`、`setPolicy/policy` | 语义锚与终止区 | add 前设置；Close 标 Terminal |
| `setProvider/provider` | 贡献者身份 | add 前设置；供 `removePages` 整组撤除 |
| `setBody/body` | 页面主体（一块控件） | 与 `addItem` 互斥；widget 由页面持有 |
| `addItem(RibbonAction*)` | 命令条目；一个动作一个条目 | 调用方持有动作 |
| `numItems/itemAt` | 条目查询 | 逻辑顺序；含隐藏条目 |
| `setItemVisible/isItemVisible` | 条目显隐 | 动作作身份，序号不变 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- C1：应用按钮事件在 UI 线程触发。
- C2：`RibbonBar` 不拥有 Backstage；`MainWindow` 拥有。
- C3：页面 id/标题/图标在 add 前设置。
- C4：Backstage 不删除页面条目（`RibbonAction`）。
- C5：覆盖主窗口客户区，不覆盖系统标题栏。
- C6：一个 MainWindow 只有一个 Backstage。
- C7：隐藏页面/条目不删除、不改序号；隐藏当前页时必须有可见页接替，一个都没有则没有当前页并拒绝 open。
- C9：条目的隐藏以动作为键（一个动作一个条目）；隐藏不是禁用命令。
- C10：页面里只有 `addItem()` 建的条目按钮，不添加装饰性按钮（用例按 `findChildren<QToolButton*>()` 断言条目，多一个即失败）。
- C12：页面顺序由 placement 解析，不等于添加/加载顺序；终止页始终在末尾，导航行与内容栈索引随逻辑顺序同步。
- C13：`removePages(provider)` 只撤匹配的页；被撤页的 widget 从栈里摘下并交回页面（可重新加入），空 provider 不匹配。
- C14：一页只有一块主体——命令列或 `setBody()` 装的控件；两者不共存，换 body 时旧 body 交回它的主人。
- C15：版式是网格——导航行与条目行同为 40 px 高、2 px 间距、20 px 图标位；页面标题与条目列共用 32 px 左缩进。改任何一个尺寸都要同时看另一列，否则两列节奏对不上。

### 5.2 要求（R#）

- R1：应用按钮点击必须可观测（事件）。
- R2：菜单与 Backstage 不能同时弹出。
- R3：Backstage 不能被原生 3D 表面遮挡。
- R4：页面 id 非空时唯一；重复添加被拒绝并记日志。
- R5：关闭/隐藏后主窗口恢复可用。
- R6：替换/销毁 MainWindow 时不得留下悬空订阅。
- R7：隐藏页仍可查找，恢复后能重新成为当前页；隐藏条目的序号与动作不变，可用同一动作恢复。

### 5.3 不变量（N#）

- N1：Backstage 是顶层窗口，不是主窗口的子控件。
- N2：导航行与内容栈索引始终一致。
- N3：页面包装对象在 add 后不拥有其 widget（栈拥有）。
- N4：菜单项清空后 `RibbonAction` 仍归调用方。
- N5：导航行索引与页面索引一致（隐藏行仍占位）；隐藏页不可被 `setCurrentPage()` 选中。
- N7：`pages`（逻辑顺序）是唯一真相，导航行与栈索引只是镜像；当前页按对象保持，不跟行号。

## 6. 注意事项与陷阱

### 6.1 用 SARibbonApplicationWidget 当子控件
- 会怎么错：文档区（原生 3D）盖住 Backstage，看起来“只显示了一部分”。
- 怎么判：打开 Backstage 后中央区被渲染面覆盖。
- 怎么防：顶层 Tool 窗口；见 `../bugs/docking-flyout-under-render.md`。

### 6.2 删除顺序
- 会怎么错：页面包装对象先于 Backstage 栈销毁，widget 双删；或条目动作先删导致按钮悬空。
- 怎么判：ASan / 关闭窗口崩溃。
- 怎么防：add 时 `setOwnsImpl(false)`；条目由调用方持有。

### 6.3 Esc 不生效
- 会怎么错：导航列表有焦点，Esc 被列表吞掉。
- 怎么判：打开后按 Esc 不关闭。
- 怎么防：用 `QShortcut(QKeySequence::Cancel)` + `WidgetWithChildrenShortcut`，不依赖 `keyPressEvent`。

### 6.4 几何不同步
- 会怎么错：主窗口最大化/移动后 Backstage 仍在旧位置。
- 怎么判：最大化后覆盖不全。
- 怎么防：主窗口事件过滤器同步 `Move/Resize/WindowStateChange/Show/Hide`。

### 6.5 隐藏后的计数
- 会怎么错：宿主把 `numPages()`/`numItems()` 当“可见数量”，把隐藏项也算进布局。
- 怎么判：用例断言隐藏后计数不变、`itemAt/pageAt` 仍返回原对象。
- 怎么防：需要可见数量时自己数 `isPageVisible()/isItemVisible()`。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | `SARibbonApplicationWidget` 作为主窗口子控件 | 被原生 3D 表面遮挡（G10） | `../decisions/ADR-2026-10-28-backstage-overlay-window.md` |
| D2 | 打开 Backstage 时隐藏/重建 `RenderControl` | 表面生命周期代价、闪烁（G10） | 同上 |
| D3 | `WA_NativeWindow` 子窗口 + raise | 飞窗已证明会扰乱渲染表面（G10） | 同上 |
| D4 | Backstage 拥有页面包装对象 | 与 Qt 父对象双重所有权，双删风险（G10） | 同上 |
| D5 | 页面支持任意内容布局（v1） | 没有消费者；条目已覆盖当前需求（G8） | — |
| D6 | 给 `Backstage` 加 `enabled`（禁用而不删除，入口自动隐藏） | 入口就是应用按钮，`setApplicationButtonVisible(false)` 已经表达同一事实；两个开关要配对维护，且 `applicationButtonVisible()` 会被两种原因置假（G8）。要“留着页面但不要入口”：隐藏按钮；要彻底拿掉：`setBackstage(nullptr)` | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 应用按钮事件打开 Backstage | `GuiTest.Backstage_ApplicationButtonOpensThePage` |
| T2 | 页面增删/查找/当前页 | `GuiTest.Backstage_PagesAndNavigation` |
| T3 | 条目是命令绑定的动作 | `GuiTest.Backstage_PageItemsAreCommandBound` |
| T4 | 跟随主窗口尺寸、关闭 | `GuiTest.Backstage_FollowsMainWindowSize`、`GuiTest.Backstage_ClosesOnEscapeOrClose` |
| T5 | MainWindow 所有权替换不崩溃 | `GuiTest.Backstage_IsOwnedByMainWindow` |
| T6 | 页面隐藏/恢复/无可见页 | `GuiTest.Backstage_PagesCanBeHidden` |
| T7 | 条目隐藏/恢复 | `GuiTest.BackstagePage_ItemsCanBeHidden` |
| T8 | File 入口隐藏/恢复落到控件上 | `GuiTest.RibbonBar_ApplicationButtonHidesTheEntry` |
| T9 | 页面放置：角色序/锚点/终止区/回退/夹回/角色查找 | `GuiTest.Backstage_PageRolesAndPlacement`、`GuiTest.Backstage_PagePlacementEdgeCases` |
| T10 | 按 provider 整组撤除、当前页接替、撤掉的页可重新加入 | `GuiTest.Backstage_PagesAreRemovedByProvider` |
| T11 | 标准目录：插件页按 role 锚定到终止的帮助页之前 | `GuiTest.Backstage_StandardRoleCatalog` |
| T12 | 页面主体：与条目互斥、换 body 交回旧的、null 不算 | `GuiTest.BackstagePage_BodyReplacesTheCommandColumn` |
| T13 | 版式：导航行留图标位、条目列与标题同左缩进、条目行 40 px | `GuiTest.Backstage_NavRowsReserveTheIconColumn`、`GuiTest.Backstage_PageRowsAlignWithTheTitle` |
| T15 | 条目超过一屏时可滚动；body 页不套滚动区 | `GuiTest.Backstage_ManyItemsScroll` |

## 9. 状态与演进

### 9.1 当前状态

F1–F6 落地；F9/F10（页面与条目的显隐）随之落地；app_shell 用两个页面（插件、帮助）演示命令条目，两页标了 role（Plugins/Help，帮助页 `Terminal`）与 provider（`app_shell`），插件可按 role 锚定到它们前面；F7 第一个真实消费者是同一外壳的「关于」页（`setBody(AboutPanel)`，见 `appfw-app-info.md`）；条目是 `RibbonAction`，所以启用态/tooltip/勾选态跟着命令走（见 `appfw-command-binding.md`），页面本身不再写一份。

### 9.2 已知缺口 / 待办

见 `../backlog/appfw-ribbon.md`：最近文件/MRU（T8）、声明式清单（T5）。

### 9.3 兼容性 / 迁移

- 新类 `Backstage`/`BackstagePage`；`RibbonBar` 只加方法与 `Impl` 事件，无类布局变化。
- F12 只加新类型 `BackstagePlacement`/`BackstagePlaceResult` 与页面上的 `setRole/setPolicy`、`Backstage::addPage(page, placement)`/`findPage(role)`；`addPage(page)` 保留为 append，null 页仍返回 false（放置重载才抛，同 `RibbonTab::addGroup`）。
- F13 加 `setProvider/provider` 与 `removePages(provider)`；`app_shell` 的两页标了 `u8"app_shell"` 并在 `unload()` 里撤除。
- `RibbonPlacement::Where` 变为 `PlacementWhere` 的别名（源兼容，枚举值不变）；两套放置共用这个枚举。
- 旧 `appendApplicationMenu()` 保持；不使用 `setBackstage()` 的宿主行为不变。
- 迁移：把 `appendApplicationMenu` 的条目改成 `BackstagePage::addItem`，调用 `MainWindow::setBackstage()`。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/gui/Backstage.hpp`、`src/fw/appfw/src/gui/Backstage.cpp`（组件本体）
- `src/fw/appfw/src/gui/RibbonBar.cpp`（事件与模式切换）、`src/fw/appfw/src/gui/MainWindow.cpp`（所有权）
- `src/plugins/app_shell/src/AppShellUi.cpp`（页面装配）

### 10.2 文档与 ADR

- `../decisions/ADR-2026-10-28-backstage-overlay-window.md`
- `../bugs/docking-flyout-under-render.md`

### 10.3 外部资料

- SARibbon：`SARibbonApplicationButton`、`SARibbonBar::applicationButtonClicked`、`SARibbonApplicationWidget`。
- Office Backstage：整窗文件页 + 左侧导航。
