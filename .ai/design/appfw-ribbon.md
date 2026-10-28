# Ribbon 身份、角色与放置

> **状态**：现行
> **最后核对**：2026-10-28
> **范围**：Ribbon 的 tab/group 身份（id）、语义角色（role）、放置（placement）、终止区与顺序解析；不含用户自定义布局与上下文 tab。
> **权威来源**：`src/fw/appfw/sdk/vine/appfw/gui/RibbonPlacement.hpp`、`RibbonBar.hpp`、`RibbonTab.hpp`、`RibbonGroup.hpp`；用例 `tests/test_gui/test_gui.cpp`。
> **待办**：`../backlog/appfw-ribbon.md`

## 1. 概要设计

### 1.1 问题与背景

功能区现在的身份是**标题字符串**，顺序是**追加**：`RibbonBar::addTab()` 只能排在末尾，`RibbonTab`/`RibbonGroup` 没有 id。
app_shell 在 `load()` 里建“插件/帮助”，model_viewer 在 `postLoad()` 里追加“模型”，于是“模型”落在“帮助”之后。
插件无法表达“我在帮助之前”，也无法稳定地找到“帮助”往里加东西；标题一改（翻译、重命名）一切引用都断。

### 1.2 目标

- tab 有**稳定 id**（引用、去重、持久化的键），标题只用于显示。
- 框架定义一份**常用角色词表**（`RibbonTabRole`，Qt `UserRole` 式可扩展），插件可用角色做**宿主无关**的锚点。
- tab/group 支持 `before/after` 锚点与 `append`；顺序由框架解析，插件不需要算 index。
- “帮助”这类 tab 用 `Terminal` 策略保证**永远最后**；任何插入都被夹到终止区之前。
- 插件能按角色/ id 找到标准 tab，并往它里面加 group。
- 现有 `addTab(tab)` / `addGroup(group)` 调用保持原语义，不破坏插件 ABI。

### 1.3 非目标

- 不做用户自定义（隐藏/拖动/保存布局）——没有消费者（G8），见 backlog。
- 不做上下文 tab（按条件显示/隐藏）。
- 不引入 XML/manifest 声明；插件是进程内 C++，代码 API 足够（G8）。
- 不给 button/action 强制加第二套 id：它们的身份是命令名（见 F6）。

### 1.4 一句话结论

**身份用 id，语义用 role，顺序用 placement；顺序由框架从声明解析出来，终止策略是解析器保证的不变量，而不是约定。**

## 2. 设计要点

### 2.1 身份、角色、命令三者分开（G1）

- 是什么：`id` 回答“是哪一个”，`role` 回答“这个位置是干什么的”，`command` 回答“点了做什么”。
- 为什么：一个概念一个定义。把显示标题当身份（现状）会在翻译/改名时失效；给 button 再发一个 id 会与命令名形成第三套身份。
- 边界：组只有 id（作用域是所属 tab）；item 只有命令名（可选 id 留给无命令控件，未落地）。

### 2.2 顺序是解析出来的，物理顺序只是镜像（G1、G11）

- 是什么：`RibbonBar` 保存注册项（tab + placement + 注册序），每次结构变化重算逻辑顺序，再把 SARibbon 的 category 顺序 `moveCategory()` 对齐。
- 为什么：`tabAt(i)`、`currentIndex()`、SARibbon 的物理顺序必须只有一个真相；直接 `insertCategoryPage(index)` 加一份私有 vector 很容易漂移。
- 边界：重排后恢复原来的当前 tab；SARibbon 之外不要直接动 category 顺序。

### 2.3 锚点只向已注册的节点解析（G3、G8、G11）

- 是什么：注册序（`next_seq`）决定解析方向：新项只能以**已经注册**的 tab/group 为锚点；找不到就回退。
- 为什么：单向解析天然无环，不需要依赖图与环检测；宿主先建标准 tab、插件后注册，正好满足这个方向。
- 代价：锚到“后加载插件”的 tab 会回退（`PlacedByFallback` + 日志）。这是有意的：不让顺序依赖插件扫描顺序之外的未知信息。

### 2.4 终止区是不变量，不是约定（G4）

- 是什么：`RibbonTabPolicy::Terminal` 的 tab 形成后缀；非终止 tab 的插入位置一律 `min(index, firstTerminal)`。
- 为什么：`after(Help)` 这类请求必须“可解释地失败”：tab 仍然出现（不丢 UI），状态为 `ClampedBeforeTerminal`，并记日志。
- 边界：`Terminal` 由宿主在目录里显式声明，**不由 role==Help 隐式推导**——隐式约定正是要消灭的东西。

### 2.5 失败有状态、有日志（G4、G11）

- 是什么：`RibbonPlaceResult{status,index}` 返回 `Placed / PlacedByFallback / ClampedBeforeTerminal / DuplicateId / AlreadyAdded / InvalidAnchor`。
- 为什么：Blender 的字符串类别、Eclipse 的 locationURI 都会静默落到错误位置；可定位性必须是 API 的一部分。
- 边界：日志只记一次一件事，不抛异常（UI 构建不应因布局请求失败而崩溃）。

### 2.6 机制归框架，目录归宿主（G2）

- 是什么：appfw 只提供 id/role/placement/policy 与解析器；`插件/帮助` 这些 tab 的 id、标题、role、policy 由 app_shell 声明。
- 为什么：appfw 是宿主框架，不应该知道某个应用有哪些 tab；插件只依赖 appfw，用 role 指向标准槽位，不依赖 app_shell 的头文件。
- 边界：`RibbonTabRole` 是框架词表（值稳定，只增不改）；具体 tab 的 id 是宿主命名空间。

### 2.7 只为有消费者的层引入扩展点（G8）

- 是什么：v1 只做 tab 级 role + tab/group 级 id/placement；`order`、item id、用户自定义、上下文 tab 都不进代码。
- 为什么：没有消费者的字段会变成要维护的契约；顺序的并列项今天用注册序就够（插件加载序是确定的）。
- 边界：新需求出现时按 §9.2 的触发条件重开，不预先占位。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | tab 身份与策略 | id、role、policy 三个属性；set 在 add 之前 | 已落地 |
| F2 | tab 放置 | append / before / after（id 或 role）；终止区夹取 | 已落地 |
| F3 | tab 查找 | 按 id 或 role 找 tab | 已落地 |
| F4 | group 身份与放置 | group id + before/after/append；往标准 tab 加组 | 已落地 |
| F5 | 旧接口兼容 | `addTab(tab)` / `addGroup(g)` 仍为追加 | 已落地 |
| F6 | item 身份 | button/action 的身份 = 命令名，不新增 id | 已落地（契约） |
| F7 | 用户自定义布局持久化 | 按 id 隐藏/移动/保存 | 未做，见 backlog |
| F8 | 上下文 tab | 按条件显示/隐藏 | 未做，见 backlog |

### 3.1.1 F1 tab 身份与策略

- **行为**：`RibbonTab::setId/setRole/setPolicy`；空 id 表示“只能追加、不能被锚点引用”；`RibbonTabRole::None` 表示无语义角色。
- **约束**（C1、C2、C4）：属性在 add 之前设置，add 之后的修改不影响本次解析；role 值进 ABI，只增不改。
- **要求**（R1）：`Terminal` tab 必须在终止区。
- **验收**（T1）：`RibbonBar_RolesAndPlacement`。
- **注意**（N1）：同 id 重复注册被拒绝。

### 3.1.2 F2 tab 放置与终止区

- **行为**：`addTab(tab, placement)`；`Append` 排在非终止区末尾；`Before/After` 按 id 或 role 找锚点；终止区之后的请求被夹回。
- **约束**（C3）：锚点必须是已注册的 tab；找不到回退并返回 `PlacedByFallback`。
- **要求**（R2、R3）：任何非终止 tab 都在终止区之前；失败返回状态并记日志。
- **验收**（T2、T3、T4）：`RibbonBar_TerminalTabStaysLast`、`RibbonBar_UnknownAnchorFallsBack`、`RibbonBar_AfterHelpIsClamped`。
- **注意**（P1）：宿主忘记给“帮助”标 `Terminal`，终止区就不存在。

### 3.1.3 F3 tab 查找

- **行为**：`findTab(id)` 精确查找；`findTab(role)` 返回该角色在逻辑顺序中的第一个 tab；`role == None` 与空 id 一律返回 `nullptr`。
- **约束**（C4）：id 在 bar 内唯一。
- **要求**（R4）：查找走逻辑顺序，不受 SARibbon 物理索引影响。
- **验收**（T5）：`RibbonBar_FindByRoleAndId`。

### 3.1.4 F4 group 身份与放置

- **行为**：`RibbonGroup::setId`；`RibbonTab::addGroup(g, placement)`、`findGroup(id)`；锚点是 group id（角色锚点在 group 级返回 `InvalidAnchor`）。
- **约束**（C5）：group id 在所属 tab 内唯一；空 id 允许重复。
- **要求**（R5）：插件可以 `findTab(RibbonTabRole::Help)->addGroup(...)` 往标准 tab 贡献内容。
- **验收**（T6）：`RibbonTab_GroupPlacementAndContribution`。
- **注意**（P2）：group 级 placement 不要传 role。

### 3.1.5 F5 旧接口兼容

- **行为**：`addTab(RibbonTab*)` 等价于 `addTab(tab, RibbonPlacement::append())`；`addGroup(RibbonGroup*)` 等价于追加。
- **约束**（C6）：不改既有函数签名/返回类型；新能力只通过重载 + 新方法进入。
- **要求**（R6）：现有 app_shell / model_viewer / 测试不改也能编译并保持追加语义。
- **验收**（T7）：`RibbonBar_LegacyAddStillAppends` + 现有 Ribbon 用例全绿。

### 3.1.6 F6 item 身份

- **行为**：`RibbonButton::command()` / `RibbonAction::command()` 就是 item 的身份；没有命令的控件暂不提供放置 API。
- **约束**（C7）：不给 item 另造 id/role（G1、G8）。
- **要求**（R7）：将来要做自定义布局时，以命令名为键；无命令控件才补显式 id。
- **验收**（T8）：设计评审 + backlog 触发条件。
- **注意**（P3）：同一命令放在两个组是允许的（命令是动作，按钮是视图）。

## 4. 公开接口

### 4.1 RibbonPlacement（`RibbonPlacement.hpp`）

| 接口 | 契约 | 备注 |
|---|---|---|
| `RibbonPlacement::append()` | 追加到非终止区末尾 | 完整签名见头文件 |
| `before/after(id)` | 锚到同作用域的兄弟 id | 找不到回退 |
| `before/after(role)` | 仅 tab 级 | group 级返回 `InvalidAnchor` |
| `RibbonPlaceResult` | `status` + 最终逻辑 index | `index == -1` 表示未放入 |

### 4.2 RibbonBar

| 接口 | 契约 | 备注 |
|---|---|---|
| `addTab(tab)` | 旧的追加入口 | 保持原签名 |
| `addTab(tab, placement)` | 解析放置并重排 | 返回状态与 index |
| `findTab(id)` / `findTab(role)` | 逻辑顺序查找 | 空 id / None 返回 nullptr |
| `numTabs()/tabAt(i)` | 逻辑顺序 | 与物理顺序一致（N2） |

### 4.3 RibbonTab

| 接口 | 契约 | 备注 |
|---|---|---|
| `setId/id` | 稳定身份 | add 之前设置 |
| `setRole/role` | 语义角色 | 框架词表 + UserRole |
| `setPolicy/policy` | `Terminal` 等 | 宿主目录用 |
| `addGroup(g)` | 旧的追加入口 | 保持原签名 |
| `addGroup(g, placement)` | 组级放置 | 锚点仅 id |
| `findGroup(id)` | 组查找 | 空 id 返回 nullptr |

### 4.4 RibbonGroup

| 接口 | 契约 | 备注 |
|---|---|---|
| `setId/id` | tab 内唯一 | 空 id 允许重复 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- C1：id/role/policy 在 `addTab` 之前设置；解析器读取的是加入时的值。
- C2：`RibbonTabRole` 的数值是插件 ABI 的一部分，只增不改、留间隔。
- C3：锚点只向已注册（注册序更早）的兄弟解析。
- C4：tab id 在 bar 内唯一；id 是 ASCII、不翻译。
- C5：group id 在所属 tab 内唯一；group 级锚点只用 id。
- C6：既有公开签名与返回类型不变。
- C7：item 的身份是命令名。

### 5.2 要求（R#）

- R1：`Terminal` tab 永远处于终止区。
- R2：任何非终止 tab 不得出现在终止区之后。
- R3：失败必须返回状态并记日志，不抛异常、不静默。
- R4：重排后必须保持原来的当前 tab（若它还在）。
- R5：插件能按 role 找到标准 tab 并贡献 group。
- R6：旧调用点行为不变。
- R7：item 不引入第二身份。

### 5.3 不变量（N#）

- N1：`RibbonBar` 的 entry 列表就是逻辑顺序；SARibbon 的 category 顺序在每次结构变化后与之一致。
- N2：终止 tab 构成逻辑顺序的后缀；终止 tab 自身的相对 placement 被忽略，按注册序排在终止区。
- N3：同一个 tab 指针只登记一次；同一个组指针只登记一次。
- N4：空 id 不参与唯一性/锚点匹配。

## 6. 注意事项与陷阱

### 6.1 忘记声明终止策略
- 会怎么错：`after(Help)` 真的落到帮助之后。
- 怎么判：`RibbonBar_TerminalTabStaysLast` 用例；检查目录里 `setPolicy(Terminal)`。
- 怎么防：宿主目录模板；代码审查看 policy。

### 6.2 add 之后改 id/role
- 会怎么错：`findTab` 找不到、后续锚点解析用新值而先前 entry 用旧值。
- 怎么判：查找返回 nullptr、`PlacedByFallback` 日志。
- 怎么防：文档要求先设置再 add；v1 不在 wrapper 里加“变更通知”机制（G8）。

### 6.3 锚到后注册的 tab
- 会怎么错：`PlacedByFallback`，tab 落在非终止区末尾而不是目标旁边。
- 怎么判：返回状态 + 日志。
- 怎么防：宿主先建标准 tab；插件之间不要互相锚定，除非加载序明确。

### 6.4 重复 id
- 会怎么错：两个 tab 抢同一个 id，查找与持久化歧义。
- 怎么判：`DuplicateId`，第二次注册被拒。
- 怎么防：id 命名用 `<vendor>.<plugin>.<name>`；空 id 只用于一次性 tab。

### 6.5 物理顺序被外部改动
- 会怎么错：`tabAt(i)` 与实际显示不一致。
- 怎么判：现有用例断言 `tabAt(currentIndex())`。
- 怎么防：不要在 SARibbon 层直接 move/remove；走 `RibbonBar`。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 只用全局数字优先级/序号 | 表达不了“帮助之前”；插入标准 tab 要重排所有数字（G4、G9） | `../decisions/ADR-2026-10-28-ribbon-roles-and-placement.md` |
| D2 | 只支持 id 锚点（IntelliJ 式） | 宿主没有该 id 时静默跑偏；插件跨宿主不成立（G4、G11） | 同上 |
| D3 | 只支持 role 锚点 | 无法精确指向某个自定义 tab/group（G4） | 同上 |
| D4 | 每个 item 也强制 id | 与命令名形成第三套身份（G1、G8） | 同上 |
| D5 | 声明式 manifest/XML | 没有进程外插件消费者（G8） | 同上 |
| D6 | 完整依赖图 + 环检测 | 注册序单向解析已足够；多出的机制没有消费者（G8、G11） | 同上 |
| D7 | `role == Help` 自动终止 | 隐式约定；终止应是显式策略（G4） | 同上 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | role/id/policy 生效，顺序符合放置 | `GuiTest.RibbonBar_RolesAndPlacement` |
| T2 | 终止 tab 永远最后 | `GuiTest.RibbonBar_TerminalTabStaysLast` |
| T3 | 未知锚点回退且仍在终止区之前 | `GuiTest.RibbonBar_UnknownAnchorFallsBack` |
| T4 | `after(Help)` 被夹回并报告 | `GuiTest.RibbonBar_AfterHelpIsClamped` |
| T5 | 按 id/role 查找 | `GuiTest.RibbonBar_FindByRoleAndId` |
| T6 | group 放置与向标准 tab 贡献 | `GuiTest.RibbonTab_GroupPlacementAndContribution` |
| T7 | 旧追加语义不变 | `GuiTest.RibbonBar_LegacyAddStillAppends` + 现有 Ribbon 用例 |
| T8 | item 身份是命令名 | 设计评审（F6） |

## 9. 状态与演进

### 9.1 当前状态

F1–F6 落地；`test_gui` 286 例全绿（285 通过 + 1 既有 skip），`test_appfw` 33 例全绿；SARibbon 顺序镜像由
`RibbonBar` 负责。

### 9.2 已知缺口 / 待办

见 `../backlog/appfw-ribbon.md`：显式 order、item id、用户自定义持久化、上下文 tab、manifest 贡献。

### 9.3 兼容性 / 迁移

- `addTab(tab)` / `addGroup(g)` 保持原行为；新能力全部是重载/新方法。
- 无虚函数变化，`RibbonTab`/`RibbonGroup`/`RibbonBar` 的新状态都在 `Impl` 里，不改变类布局。
- 迁移建议：标准 tab 设 id/role/policy；插件 tab 用 `before(RibbonTabRole::Help)` 或 `append()`。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/gui/RibbonPlacement.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/Gui.hpp`（`RibbonTabRole`、`RibbonTabPolicy`）
- `src/fw/appfw/src/gui/RibbonBar.cpp`（解析器与物理镜像）
- `src/plugins/app_shell/src/AppShellUi.cpp`（标准目录）
- `src/plugins/model_viewer/src/ModelViewerPlugin.cpp`（`before(Help)`）

### 10.2 文档与 ADR

- `../decisions/ADR-2026-10-28-ribbon-roles-and-placement.md`

### 10.3 外部资料

- Qt `Qt::ItemDataRole` / `Qt::UserRole`：预定义角色 + 自定义角色区间的做法。
- Office 自定义 UI XML：`insertBeforeMso` / `insertAfterMso` 的相对定位。
- Visual Studio VSCT：稳定命令 id + `priority`。
- IntelliJ Platform：`group-id` + `anchor` + `order`。
