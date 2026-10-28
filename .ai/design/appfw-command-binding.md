# 命令条目的显示状态

> **状态**：现行
> **最后核对**：2026-10-28
> **范围**：命令条目（按钮、下拉/应用菜单项、Backstage 条目）的 enabled / tooltip / 勾选态如何从
> `CommandManager` 与本次派发目标派生；不含 Ribbon 的 tab/group 身份与放置（见 `appfw-ribbon.md`）。
> **权威来源**：`src/fw/appfw/sdk/vine/appfw/CommandManager.hpp`、`gui/RibbonButton.hpp`、`gui/RibbonAction.hpp`、
> `src/fw/appfw/src/gui/CommandBinding.hpp`；用例 `tests/test_gui`。
> **待办**：`../backlog/appfw-ribbon.md`

## 1. 概要设计

### 1.1 问题与背景

命令可以按名字执行，但"这个按钮现在能不能点、提示写什么、当前是不是这一档"过去由每个宿主/插件各自维护：
有人订阅 `commandsChanged`，有人干脆在启动时写死。用户禁用一条命令后按钮照样能点、`NeedsDocument`/
`NeedsView` 的命令在缺目标时点了才被拒绝、`CommandInfo::description` 写好了却没人当 tooltip 用、五档着色
按钮看不出当前在哪一档。重复实现必然漂移，而把 `NotApplicable` 写进"禁用偏好"又会让临时状态变成永久设置。

### 1.2 目标

- 一条规则：`setCommand(name)` 的条目是命令的**视图**，显示状态只有一处派生。
- 覆盖所有命令条目：Ribbon 按钮、下拉项、应用菜单项、Backstage 条目、快捷访问项。
- 目标要求（`NeedsDocument`/`NeedsView`）按**本次派发目标**判断，缺目标时只灰显条目，不改偏好。
- tooltip 跟命令描述；勾选态用可选的 provider，不引入"单选组"模型。

### 1.3 非目标

- 不写 `CommandManager::setCommandEnabled()`：那是用户的持久化偏好，不是当前上下文。
- 不做 `LongRunning` 期间的全局忙碌灰显（没有消费者，且会闪烁）。
- 不做多窗口目标（今天是单窗口：刷新/派发都用 `MainWindow::current()`）。
- 不做无命令控件的 id、上下文 tab、用户自定义布局（见 `../backlog/appfw-ribbon.md`）。

### 1.4 一句话结论

**命令条目只显示命令的状态：注册/偏好/目标要求由 `CommandManager` 回答，描述就是 tooltip，勾选态由 provider
回答；规则只有 `CommandBinding` 一份实现。**

## 2. 设计要点

### 2.1 条目是视图，不是状态的第二个主人（G1、G5、G7）

- 是什么：条目保存命令名（身份），其余状态每次从命令管理器重新算。
- 为什么：同一条"能不能点"如果有条目自己的一份缓存，撤销注册、禁用、目标变化都会漏更新。
- 边界：命令名仍是身份；条目自己的 `setEnabled`/`setTooltip`/`setChecked` 只对未绑定命令的条目有效。

### 2.2 规则只有一份实现（G2、G6）

- 是什么：`RibbonButton` 与 `RibbonAction` 各自持有一个内部 `detail::CommandBinding`（`src/gui/CommandBinding.hpp`），
  由它订阅、计算、写回。二者只提供"把状态写到原生控件"的回调。
- 为什么：按钮和菜单项/Backstage 条目如果各写一份，`NeedsDocument` 这类规则迟早只在一处更新。
- 边界：这是 GUI 层内部类型，不新增公开 API；命令层（`CommandManager`）不认识控件。

### 2.3 派生的是显示态，不是持久化偏好（G1、G4）

- 是什么：`enabled = isCommandEnabled(name)`，再扣掉 `NeedsDocument`/`NeedsView` 在当前目标上不满足的情形。
- 为什么：禁用偏好属于用户（写在配置里）；当前目标缺失属于上下文，两者混用会让"暂时不能点"变成"永久禁用"。
- 边界：刷新只调用 `QWidget/QAction::setEnabled`，从不回写管理器；`NotApplicable` 也不是禁用。

### 2.4 刷新由管理器事件驱动，跨线程先编组（G11）

- 是什么：订阅 `commandsChanged`（注册/偏好/描述）、`executed`（provider 的答案可能变了）、
  `DocumentManager::currentChanged`（目标变了）。
- 为什么：状态没有轮询点；少订阅一个事件就是"偶尔不更新"的 bug。
- 边界：`executed` 可能在工作线程触发（命令在定时器/IO 线程恢复），刷新经 `MainThreadDispatcher` 回到应用线程
  后才碰控件；`setCommand()` 时立即求值一次，避免首次显示是错的。

### 2.5 tooltip 归绑定所有，直到调用方接管（G4）

- 是什么：绑定写入命令描述，并记住自己写的文本；发现控件当前 tooltip 与它不同，就认为调用方接管，从此不再覆盖。
- 为什么：描述是命令的文档，应该默认出现；但条目也可能需要更具体的提示，二者不能互相静默覆盖。
- 边界：描述变空且当前文本仍是绑定写的，就把 tooltip 清掉。

### 2.6 勾选态用 provider，不建"单选组"模型（G8）

- 是什么：`setCheckedProvider(std::function<bool()>)` 回答"这一档现在是不是开着的"；有 provider 时条目变为
  checkable，每次刷新重新求值。
- 为什么：互斥关系是业务事实（当前着色模式），不是 Ribbon 的通用概念；五档按钮各自的 provider 读同一个目标，
  天然只有一个为真。
- 边界：provider 抛异常只记警告并保留原状态；没有 provider 时勾选完全归调用方。

### 2.7 未绑定命令的条目不订阅、不改状态（G8）

- 是什么：`setCommand("")` 或从未设置命令时，绑定不持有任何订阅，也不写 enabled/tooltip/checked。
- 为什么：没有消费者的监听只是噪声；调用方自己管理的按钮不该被框架"顺手"复位。
- 边界：重新绑定命令时恢复订阅。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 启用态跟随命令 | 注册、用户偏好、目标要求共同决定 | 已落地 |
| F2 | 目标要求灰显 | `NeedsDocument`/`NeedsView` 缺目标时只灰显条目 | 已落地 |
| F3 | 描述作为 tooltip | 调用方可接管 | 已落地 |
| F4 | 勾选态 provider | 每次刷新重新求值 | 已落地 |
| F5 | 覆盖所有条目 | 按钮、下拉项、应用菜单、Backstage | 已落地 |

### 3.1.1 F1 启用态跟随命令

- **行为**：`setCommand(name)` 后立即求值，并在 `commandsChanged` / `currentChanged` 时重算。
- **约束**（C1）：未注册与用户禁用都表现为不可执行；只显示，不写偏好。
- **要求**（R1）：命令重新注册/启用后条目自动恢复。
- **验收**（T1）：`GuiTest.RibbonItemsFollowTheCommandState`。
- **注意**（N1）：构建 UI 时命令尚未注册的条目会先灰显，注册事件到达后恢复。

### 3.1.2 F2 目标要求灰显

- **行为**：`CommandFlags` 由 `CommandManager::commandFlags()` 读出；目标用 `resolveCommandTarget(MainWindow::current())`。
- **约束**（C2）：只降低条目的可点性，不改变命令状态、不写配置。
- **要求**（R2）：`NeedsDocument` 看 `target.document`，`NeedsView` 看 `target.view`（视图有 3D 区才算）。
- **验收**（T2）：`GuiTest.RibbonItemsFollowTheResolvedTarget`。
- **注意**（N2）：没有视图的当前文档解析出的目标是空的（`resolveCommandTarget` 从视图取文档）。

### 3.1.3 F3 描述作为 tooltip

- **行为**：`commandDescription(name)` 写入 tooltip；描述变化时跟着改。
- **约束**（C3）：调用方设置的不同文本优先，绑定从此不再覆盖。
- **要求**（R3）：描述为空时清掉绑定写过的 tooltip，而不是留旧文本。
- **验收**（T3）：`GuiTest.RibbonItemsFollowTheCommandState` 的 tooltip 断言；
  `GuiTest.RibbonMenusShowCommandTooltips` 钉住菜单真的会显示它。
- **注意**（N3）：菜单默认不显示 QAction tooltip，见 P4。

### 3.1.4 F4 勾选态 provider

- **行为**：`setCheckedProvider(fn)`；每次刷新调用 `fn()`，有值时把条目设为 checkable 并写 checked。
- **约束**（C4）：provider 属于绑定的命令；没有命令时不生效。异常只警告。
- **要求**（R4）：命令执行后（`executed`）重新求值，于是"点完这档"立刻显示为选中。
- **验收**（T4）：`GuiTest.RibbonItemsFollowTheCommandState`；`ModelViewerTest.TheShadingModeCommandsDriveTheCurrentModelView`
  钉住 provider 读的就是命令作用的那块 3D 区。
- **注意**（N4）：provider 读请求（`RenderControl::viewerOptions().mode`）而不是链，避免"表面还没 attach"时无答案。

### 3.1.5 F5 覆盖所有条目

- **行为**：Ribbon 按钮、按钮下拉项、应用菜单项、Backstage 条目、快捷访问项都是 `RibbonButton`/`RibbonAction`。
- **约束**（C5）：不新增条目类型；同一个命令可以出现在多个条目里，它们各自刷新。
- **要求**（R5）：Backstage 的条目不自己维护一份状态（`QToolButton::setDefaultAction` 自动跟随）。
- **验收**（T5）：`GuiTest.Backstage_PageItemsAreCommandBound` + 审查 `RibbonAction` 的绑定。
- **注意**（N5）：同一命令的两个条目各有一份订阅；数量小，换来的是互不依赖。

## 4. 公开接口

### 4.1 CommandManager

| 接口 | 契约 | 备注 |
|---|---|---|
| `commandFlags(name)` | 注册时缓存的 flags；别名照解析 | 未注册返回 `nullopt` |
| `commandDescription(name)` | 注册时缓存的描述 | 未注册返回 `nullopt` |
| `isCommandEnabled(name)` | 能否执行 | 禁用与未注册都为 false |

### 4.2 RibbonButton / RibbonAction

| 接口 | 契约 | 备注 |
|---|---|---|
| `setCommand(name)` / `command()` | 命令名即身份；绑定后状态派生 | 空名解除绑定 |
| `setCheckedProvider(fn)` | 可选的勾选态回答 | 无 provider 时勾选归调用方 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- C1：只写条目显示状态，不写 `setCommandEnabled()`。
- C2：目标要求在应用线程上解析；`executed` 跨线程时先编组。
- C3：tooltip 只更新绑定自己写过的文本。
- C4：provider 只随命名的命令生效；异常不传播。
- C5：不引入新的条目类型或第二套身份。

### 5.2 要求（R#）

- R1：注册/启用变化后条目自动恢复可点。
- R2：缺目标时条目灰显，命令本身不被改写。
- R3：描述变化会反映到 tooltip；调用方接管后保持其文本。
- R4：命令执行后勾选态重新求值。
- R5：所有命令条目的规则只有 `CommandBinding` 一份实现。

### 5.3 不变量（N#）

- N1：条目保存的只有命令名，不缓存 enabled/描述/勾选。
- N2：未绑定的条目没有订阅。
- N3：一次刷新最多读三次命令注册表（enabled/flags/description），不实例化命令。
- N4：刷新发生在应用线程。

## 6. 注意事项与陷阱

### 6.1 P1 对绑定条目调 setEnabled()

- 会怎么错：用户以为禁用了，下一次刷新又变回派生值。
- 怎么判：`RibbonItemsFollowTheCommandState` 中 enabled 只跟命令管理器走。
- 怎么防：长期禁用用 `CommandManager::setCommandEnabled()`；头文件已写明。

### 6.2 P2 构建 UI 时命令尚未注册

- 会怎么错：按钮先灰显，被误认为"坏了"。
- 怎么判：注册后 `commandsChanged` 会自动恢复；日志里能看到注册。
- 怎么防：宿主先注册再建条目（app_shell/model_viewer 的顺序就是这样）。

### 6.3 P3 视图注册晚于文档

- 会怎么错：当前文档的视图在 `currentChanged` 之后才登记，目标不会重新解析。
- 怎么判：需要视图注册表提供变更事件；今天没有。
- 怎么防：插件在 `load()` 里登记视图（早于任何文档打开）；这是当前唯一支持的顺序。

### 6.4 P4 菜单不显示 tooltip

- 会怎么错：描述写好了，鼠标停在下拉/应用菜单项上没有提示。
- 怎么判：菜单需要 `setToolTipsVisible(true)`（框架创建菜单时统一打开）。
- 怎么防：`GuiTest.RibbonMenusShowCommandTooltips`；新增菜单创建点必须沿用同一设置。

### 6.5 P5 provider 抛异常

- 会怎么错：刷新中断、按钮状态停在半路。
- 怎么判：日志含 "checked-state provider threw"。
- 怎么防：provider 只读状态、不做 IO；异常会让条目保留原勾选态。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 每个宿主/插件自己订阅并刷新条目 | 同一规则 N 份实现，必然漂移（G5、G6、G7） | 本文 §2.2 |
| D2 | 用 `setCommandEnabled()` 表达"当前不适用" | 把临时上下文写成持久化偏好（G1、G4） | `appfw-command-manager.md` N3 |
| D3 | 全局"当前勾选项"表 / 单选组模型 | 互斥是业务事实，通用机制没有消费者（G8） | 本文 §2.6 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | enabled 跟随注册/偏好 | `GuiTest.RibbonItemsFollowTheCommandState` |
| T2 | 目标要求驱动灰显 | `GuiTest.RibbonItemsFollowTheResolvedTarget` |
| T3 | 描述成为 tooltip、调用方优先 | `GuiTest.RibbonItemsFollowTheCommandState` |
| T4 | provider 与命令作用同一块区 | `GuiTest.RibbonItemsFollowTheCommandState`、`ModelViewerTest.TheShadingModeCommandsDriveTheCurrentModelView` |
| T5 | 单条 flags/描述读取与别名 | `GuiTest.CommandManager_FlagsAndDescriptionResolveThroughAliases` |
| T6 | 菜单显示条目 tooltip | `GuiTest.RibbonMenusShowCommandTooltips` |
| T7 | 工作线程结束的命令经编组刷新 | `GuiTest.RibbonItemStateFromAWorkerThreadIsMarshalled` |

## 9. 状态与演进

### 9.1 当前状态

F1–F5 落地；`test_gui` 全绿；model_viewer 的五档着色按钮使用 provider。

### 9.2 已知缺口 / 待办

- 多窗口：刷新与派发都问 `MainWindow::current()`；出现第二个窗口时按窗口解析（见 `../backlog/appfw-ribbon.md`）。
- `LongRunning` 忙碌期的灰显、上下文 tab、无命令控件的 id：没有消费者，按 `../backlog/` 的触发条件重开。
- 视图注册表变化不会重解目标（P3），需要时给注册表加事件。

### 9.3 兼容性 / 迁移

- `CommandManager::commandFlags/commandDescription` 与 `setCheckedProvider` 都是新增的非虚接口，插件 ABI 不变。
- `setCommand()` 的点击语义不变；绑定后 enabled/tooltip/checked 变为派生显示态。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/src/gui/CommandBinding.hpp`（规则唯一实现）
- `src/fw/appfw/src/gui/RibbonButton.cpp`、`RibbonAction.cpp`（写回原生控件）
- `src/plugins/model_viewer/src/commands/ShadingModeCommands.hpp`（`isCurrentShadingMode` provider）

### 10.2 文档与 ADR

- `appfw-command-manager.md`（§4.1 的单条读取、N3）
- `appfw-ribbon.md`（条目身份与放置）
- `appfw-backstage.md`（条目是命令动作）

### 10.3 外部资料

- Qt `QAction` / `QToolButton::setDefaultAction`：动作携带 enabled/checkable/checked/toolTip。
- Office Ribbon：命令级启用与图标化状态。
