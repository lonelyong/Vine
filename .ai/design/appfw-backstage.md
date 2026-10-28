# Backstage 与 ApplicationButton

> **状态**：现行
> **最后核对**：2026-10-28
> **范围**：应用按钮的点击事件、菜单/Backstage 二选一、Backstage 顶层覆盖窗口与页面导航；不含最近文件、导入导出等具体页面内容。
> **权威来源**：`src/fw/appfw/sdk/vine/appfw/gui/Backstage.hpp`、`RibbonBar.hpp`、`MainWindow.hpp`；用例 `tests/test_gui/test_gui.cpp`。
> **待办**：`../backlog/appfw-ribbon.md`

## 1. 概要设计

### 1.1 问题与背景

Ribbon 左上角的应用按钮（“File”）现在只挂一个扁平 `QMenu`：没有点击事件、没有页面模型，也没有类似 Office Backstage 的整窗入口。
SARibbon 自带 `SARibbonApplicationButton` 和 `applicationButtonClicked` 信号，但 `SARibbonApplicationWidget` 只是一个跟随父窗口大小、
Esc 隐藏的 `QFrame` 壳——没有页面、导航、内容，且作为**子控件**会被中央 3D 的原生渲染窗口盖住（`.ai/bugs/docking-flyout-under-render.md`）。

### 1.2 目标

- 暴露应用按钮点击事件（`RibbonBar::applicationButtonClicked()`），让宿主/插件可响应。
- 提供 Backstage 组件：页面（id/标题/图标）、左侧导航、命令条目、打开/关闭/Esc、跟随主窗口几何。
- 应用按钮在“菜单”与“Backstage”之间二选一，不出现两者同时弹出的状态。
- 复用现有命令：Backstage 条目就是 `RibbonAction`（命令名即身份），不引入新的命令体系。
- 新组件不影响既有 ABI：只加新类、新方法，事件放 `Impl`。

### 1.3 非目标

- 不做最近文件（MRU）、导入导出、账户等页面内容（没有存储/需求，见 §9.2）。
- 不做页面内自定义内容布局（v1 页面只放命令条目）。
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
| F7 | 页面自定义内容 | 任意 Control 作为页面主体 | 未做，见 backlog |
| F8 | 最近文件 / MRU | 最近文件页 | 未做，见 backlog |

### 3.1.1 F1 应用按钮点击事件

- **行为**：按钮 `clicked` 转发为 `Event<RibbonBar, EventArgs>`。
- **约束**（C1）：事件在 `RibbonBar` 线程（UI 线程）触发。
- **验收**（T1）：`Backstage_ApplicationButtonOpensThePage`。
- **注意**（P1）：事件与菜单是并行的；用 Backstage 时先 `setBackstage()` 摘掉菜单。

### 3.1.2 F2 菜单/Backstage 模式

- **行为**：`setBackstage(bs)` 把按钮菜单置空并订阅事件打开 `bs`；`setBackstage(nullptr)` 断开订阅并恢复菜单。
- **约束**（C2）：`RibbonBar` 不拥有 Backstage。
- **验收**（T1、T2）。
- **注意**（P2）：`clearApplicationMenu()` 不删除动作对象。

### 3.1.3 F3 页面与导航

- **行为**：页面按添加顺序进入左侧导航与右侧 `QStackedWidget`；页面 id 在非空时唯一；第一页成为当前页。
- **约束**（C3）：id/标题/图标在 `addPage` 前设置。
- **验收**（T2）：`Backstage_PagesAndNavigation`。
- **注意**（P3）：空 id 页面可用，但 `findPage("")` 返回 nullptr。

### 3.1.4 F4 命令条目

- **行为**：`addItem(RibbonAction*)` 建一个 `QToolButton`，`setDefaultAction(action->impl<QAction>())`；点击走命令执行。
- **约束**（C4）：条目对象由调用方持有，页面不删除。
- **验收**（T3）：`Backstage_PageItemsAreCommandBound`。
- **注意**（P4）：条目先于页面销毁会让按钮的 action 悬空。

### 3.1.5 F5 打开/关闭/几何

- **行为**：`open()` 同步几何、显示、置顶、激活；Esc 或返回按钮 `close()`；主窗口 move/resize/minimize 时跟随/隐藏。
- **约束**（C5）：窗口覆盖主窗口**客户区**，不覆盖系统标题栏。
- **验收**（T4）：`Backstage_FollowsMainWindowSize`、`Backstage_ClosesOnEscapeOrClose`。
- **注意**（P5）：多屏/分数缩放下的 1px 偏差属于平台取整。

### 3.1.6 F6 生命周期

- **行为**：`MainWindow::setBackstage(bs)` 接管所有权，替换时先解除 Ribbon 的订阅再删除旧对象。
- **约束**（C6）：一个 MainWindow 只有一个 Backstage。
- **验收**（T5）：`Backstage_IsOwnedByMainWindow`（析构不崩溃、指针清空）。

## 4. 公开接口

### 4.1 RibbonBar

| 接口 | 契约 | 备注 |
|---|---|---|
| `applicationButtonClicked()` | 应用按钮点击事件 | 完整签名见头文件 |
| `clearApplicationMenu()` | 清空菜单项 | 不删除动作 |
| `setBackstage(Backstage*)` | 菜单/Backstage 二选一 | 借用指针 |
| `backstage()` | 当前 Backstage | 可为 nullptr |

### 4.2 Backstage

| 接口 | 契约 | 备注 |
|---|---|---|
| `Backstage(MainWindow*)` | 建顶层覆盖窗口并跟随主窗口 | 不接管页面 |
| `addPage(BackstagePage*)` | 追加页面 | 重复非空 id 返回 false |
| `findPage/pageAt/numPages` | 页面查找 | 逻辑顺序 |
| `setCurrentPage/currentPage` | 切换页面 | 同步导航与内容栈 |
| `open/close/isOpen` | 显隐 | Esc/返回按钮关闭 |

### 4.3 BackstagePage

| 接口 | 契约 | 备注 |
|---|---|---|
| `setId/id` | 页面身份 | add 前设置 |
| `setTitle/title` | 导航标题 | add 前设置 |
| `setIcon/icon` | 导航图标 | add 前设置 |
| `addItem(RibbonAction*)` | 命令条目 | 调用方持有动作 |
| `numItems/itemAt` | 条目查询 | 逻辑顺序 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- C1：应用按钮事件在 UI 线程触发。
- C2：`RibbonBar` 不拥有 Backstage；`MainWindow` 拥有。
- C3：页面 id/标题/图标在 add 前设置。
- C4：Backstage 不删除页面条目（`RibbonAction`）。
- C5：覆盖主窗口客户区，不覆盖系统标题栏。
- C6：一个 MainWindow 只有一个 Backstage。

### 5.2 要求（R#）

- R1：应用按钮点击必须可观测（事件）。
- R2：菜单与 Backstage 不能同时弹出。
- R3：Backstage 不能被原生 3D 表面遮挡。
- R4：页面 id 非空时唯一；重复添加被拒绝并记日志。
- R5：关闭/隐藏后主窗口恢复可用。
- R6：替换/销毁 MainWindow 时不得留下悬空订阅。

### 5.3 不变量（N#）

- N1：Backstage 是顶层窗口，不是主窗口的子控件。
- N2：导航行与内容栈索引始终一致。
- N3：页面包装对象在 add 后不拥有其 widget（栈拥有）。
- N4：菜单项清空后 `RibbonAction` 仍归调用方。

## 6. 注意事项与陷阱

### 6.1 用 SARibbonApplicationWidget 当子控件
- 会怎么错：文档区（原生 3D）盖住 Backstage，看起来“只显示了一部分”。
- 怎么判：打开 Backstage 后中央区被渲染面覆盖。
- 怎么防：顶层 Tool 窗口；见 `../bugs/docking-flyout-under-render.md`。

### 6.2 忘记摘菜单
- 会怎么错：点应用按钮同时弹菜单和 Backstage。
- 怎么判：`setBackstage` 后按钮仍有 menu。
- 怎么防：`setBackstage` 内 `setMenu(nullptr)`；`setBackstage(nullptr)` 恢复。

### 6.3 删除顺序
- 会怎么错：页面包装对象先于 Backstage 栈销毁，widget 双删；或条目动作先删导致按钮悬空。
- 怎么判：ASan / 关闭窗口崩溃。
- 怎么防：add 时 `setOwnsImpl(false)`；条目由调用方持有。

### 6.4 Esc 不生效
- 会怎么错：导航列表有焦点，Esc 被列表吞掉。
- 怎么判：打开后按 Esc 不关闭。
- 怎么防：用 `QShortcut(QKeySequence::Cancel)` + `WidgetWithChildrenShortcut`，不依赖 `keyPressEvent`。

### 6.5 几何不同步
- 会怎么错：主窗口最大化/移动后 Backstage 仍在旧位置。
- 怎么判：最大化后覆盖不全。
- 怎么防：主窗口事件过滤器同步 `Move/Resize/WindowStateChange/Show/Hide`。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | `SARibbonApplicationWidget` 作为主窗口子控件 | 被原生 3D 表面遮挡（G10） | `../decisions/ADR-2026-10-28-backstage-overlay-window.md` |
| D2 | 打开 Backstage 时隐藏/重建 `RenderControl` | 表面生命周期代价、闪烁（G10） | 同上 |
| D3 | `WA_NativeWindow` 子窗口 + raise | 飞窗已证明会扰乱渲染表面（G10） | 同上 |
| D4 | Backstage 拥有页面包装对象 | 与 Qt 父对象双重所有权，双删风险（G10） | 同上 |
| D5 | 页面支持任意内容布局（v1） | 没有消费者；条目已覆盖当前需求（G8） | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 应用按钮事件打开 Backstage | `GuiTest.Backstage_ApplicationButtonOpensThePage` |
| T2 | 页面增删/查找/当前页 | `GuiTest.Backstage_PagesAndNavigation` |
| T3 | 条目是命令绑定的动作 | `GuiTest.Backstage_PageItemsAreCommandBound` |
| T4 | 跟随主窗口尺寸、关闭 | `GuiTest.Backstage_FollowsMainWindowSize`、`GuiTest.Backstage_ClosesOnEscapeOrClose` |
| T5 | MainWindow 所有权替换不崩溃 | `GuiTest.Backstage_IsOwnedByMainWindow` |

## 9. 状态与演进

### 9.1 当前状态

F1–F6 落地；app_shell 用两个页面（插件、帮助）演示命令条目；`test_gui` 292 例全绿（291 通过 + 1 既有 skip），
`test_appfw` 33 例全绿。

### 9.2 已知缺口 / 待办

见 `../backlog/appfw-ribbon.md`：最近文件/MRU、页面自定义内容、Backstage 项的角色/放置、与命令状态绑定联动。

### 9.3 兼容性 / 迁移

- 新类 `Backstage`/`BackstagePage`；`RibbonBar` 只加方法与 `Impl` 事件，无类布局变化。
- 旧 `appendApplicationMenu()` 保持；不使用 `setBackstage()` 的宿主行为不变。
- 迁移：把 `appendApplicationMenu` 的条目改成 `BackstagePage::addItem`，调用 `MainWindow::setBackstage()`。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/gui/Backstage.hpp`
- `src/fw/appfw/src/gui/Backstage.cpp`
- `src/fw/appfw/src/gui/RibbonBar.cpp`（事件与模式切换）
- `src/fw/appfw/src/gui/MainWindow.cpp`（所有权）
- `src/plugins/app_shell/src/AppShellUi.cpp`（页面装配）

### 10.2 文档与 ADR

- `../decisions/ADR-2026-10-28-backstage-overlay-window.md`
- `../bugs/docking-flyout-under-render.md`

### 10.3 外部资料

- SARibbon：`SARibbonApplicationButton`、`SARibbonBar::applicationButtonClicked`、`SARibbonApplicationWidget`。
- Office Backstage：整窗文件页 + 左侧导航。
