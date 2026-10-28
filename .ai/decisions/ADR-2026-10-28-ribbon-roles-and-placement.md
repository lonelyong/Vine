# ADR-2026-10-28：Ribbon 用角色 + id + 锚点放置，不用全局序号

- 状态：已采纳
- 日期：2026-10-28
- 相关：`../design/appfw-ribbon.md`、`tests/test_gui/test_gui.cpp`

## 背景

功能区只能追加 tab，且 tab 的身份是显示标题。app_shell 在 `load()` 里建“插件/帮助”，model_viewer 在
`postLoad()` 里追加“模型”，结果“模型”排在“帮助”之后。插件无法表达“在帮助之前”，也无法稳定地找到
标准 tab 往里加 group；标题一改（翻译/重命名）引用全断。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 全局数字优先级（`order` / `priority`） | 实现最简单，排序是总序 | 表达不了“帮助之前”；插入标准 tab 要重排所有数字；插件之间只能靠猜 |
| B. 只支持 id 锚点（IntelliJ 式） | 精确、相对定位 | 宿主没有该 id 时静默跑偏；插件与宿主 id 强耦合 |
| C. 只支持 role 锚点 | 插件跨宿主/版本 | 无法精确指向某个自定义 tab/group |
| D. role + id + placement（本决定） | 语义默认 + 精确指向；锚点缺失有回退；终止区可保证 | 多一个词表与一个解析器；role 数值进 ABI |
| E. 声明式 manifest / XML | 外部插件可声明 UI | 今天只有进程内 C++ 插件（G8 违反）；解析/校验成本大 |

## 决定

采用 D：

- **id** 是节点身份（tab 在 bar 内唯一，group 在 tab 内唯一）；标题只显示。
- **role** 是框架定义的语义词表（`Home/View/Tools/Plugins/Help`，自定义从 `UserRole + n` 起），
  用作宿主无关的锚点与查找；role 数值只增不改，进插件 ABI。
- **placement** 是 `append / before / after`，锚点可以是兄弟 id 或（tab 级的）role。
- **终止策略显式声明**：`RibbonTabPolicy::Terminal` 形成后缀；非终止插入被夹到第一个终止 tab 之前，
  返回 `ClampedBeforeTerminal` 并记日志（不丢 UI、不静默）。
- **锚点只向已注册的兄弟解析**（注册序更早）；找不到就回退到非终止区末尾并返回 `PlacedByFallback`。
  单向解析天然无环，不需要依赖图/环检测。
- **v1 不引入 `order` 字段**：并列用注册序（插件加载序是确定的：目录排序 + 依赖序）。
- **item 身份 = 命令名**：不给 button/action 强制第二套 id；无命令控件与自定义布局等有消费者时再加。

## 后果

- 好的：
  - 插件不依赖宿主版本/语言即可说“在帮助之前/往帮助里加东西”。
  - 顺序是声明式的，删除锚点后依赖项可以通过重算回流。
  - 失败可定位：状态 + 日志，测试可以断言“被夹回/回退”。
  - 旧 `addTab(tab)` / `addGroup(g)` 语义不变；无虚函数变化，不 bump 插件 ABI。
- 坏的 / 代价：
  - 多一份 role 词表与解析器（约 150 行）。
  - 锚到后注册的 tab 会回退——插件之间的相互锚定需要明确的注册序。
  - 宿主必须记得给“帮助”标 `Terminal`；否则终止区不存在（用例与目录模板缓解）。
  - 同一锚点的并列顺序依赖插件加载序；需要业务排序时再加 `order`（见 backlog 触发条件）。
- 触发条件（什么时候重开）：
  - 两个插件在同一锚点上要求稳定业务顺序，而不是加载序；
  - 出现用户自定义布局（需要 item id 与持久化）；
  - 出现进程外插件（需要声明式 manifest）。

## 证据

- 用例：`GuiTest.RibbonBar_RolesAndPlacement`、`RibbonBar_TerminalTabStaysLast`、
  `RibbonBar_UnknownAnchorFallsBack`、`RibbonBar_AfterHelpIsClamped`、`RibbonBar_FindByRoleAndId`、
  `RibbonTab_GroupPlacementAndContribution`、`RibbonBar_LegacyAddStillAppends`。
- 代码：`src/fw/appfw/src/gui/RibbonBar.cpp`（解析与 SARibbon 镜像）、
  `src/fw/appfw/sdk/vine/appfw/gui/RibbonPlacement.hpp`。
- 门禁：`test_gui`、`check_public_docs.py`、`check_include_hygiene.py`、`check_ai_docs.py --strict`、
  `check_design_docs.py`。
